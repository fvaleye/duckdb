#include "duckdb/main/capi_v2/capi_v2_internal.hpp"

#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/main/prepared_statement.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/planner/operator/logical_explain.hpp"
#include "duckdb/planner/planner.hpp"

using namespace duckdb::capiv2;

//! A nested binder has no parent, so it cannot detect a view or macro that recursively reaches the explain call
static thread_local bool context_explain_in_progress = false; // NOLINT

DUCKDB_V2_ERROR duckdb_v2_context_explain_json(duckdb_v2_context_handle ctx, const char *sql,
                                               const duckdb_v2_identifier_t *parameter_names,
                                               const duckdb_v2_value_handle *parameter_values, idx_t parameter_count,
                                               duckdb_v2_value_handle *out_plan, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(out_plan);
	*out_plan = nullptr;
	DUCKDB_CHECK_ARG(ctx);
	DUCKDB_CHECK_ARG(sql);
	if (parameter_count > 0 && !parameter_values) {
		return NullArgumentError(err, __func__, "parameter_values");
	}
	return WithErrorHandler(err, [&]() {
		auto &callback_context = *Convert(ctx);
		if (!callback_context.binder) {
			throw duckdb::InvalidInputException(
			    "context_explain_json is only supported in scalar and table function bind callbacks");
		}
		if (context_explain_in_progress) {
			throw duckdb::InvalidInputException("context_explain_json calls cannot be nested");
		}
		struct InProgressGuard {
			explicit InProgressGuard(bool &in_progress_p) : in_progress(in_progress_p) {
				in_progress = true;
			}
			~InProgressGuard() {
				in_progress = false;
			}
			bool &in_progress;
		} in_progress_guard(context_explain_in_progress);
		duckdb::unique_ptr<duckdb::Value> output;
		callback_context.WithContext([&](duckdb::ClientContext &context) {
			if (context.HasExecutor()) {
				throw duckdb::InvalidInputException("context_explain_json is not supported during query execution");
			}
			callback_context.binder->SetAlwaysRequireRebind();
			duckdb::Parser parser(context);
			parser.ParseQuery(sql);
			if (parser.statements.size() != 1) {
				throw duckdb::InvalidInputException("context_explain_json requires exactly one statement");
			}
			if (parser.statements[0]->type == duckdb::StatementType::MULTI_STATEMENT) {
				throw duckdb::InvalidInputException(
				    "context_explain_json does not support statements requiring expansion, such as PIVOT without IN");
			}
			duckdb::Planner planner(context);
			BuildParameterMap(parameter_names, parameter_values, parameter_count, __func__, planner.parameter_data);
			duckdb::PreparedStatement::VerifyParameters(planner.parameter_data, parser.statements[0]->named_param_map,
			                                            &context);
			auto explain = duckdb::make_uniq<duckdb::ExplainStatement>(std::move(parser.statements[0]),
			                                                           duckdb::ExplainType::EXPLAIN_STANDARD,
			                                                           duckdb::ProfilerPrintFormat::JSON());
			explain->named_param_map = explain->stmt->named_param_map;
			planner.CreatePlan(std::move(explain));
			if (!planner.properties.bound_all_parameters) {
				throw duckdb::InvalidInputException("Not all parameters were bound");
			}
			planner.Optimize();
			duckdb::PhysicalPlanGenerator generator(context);
			generator.Plan(*planner.plan);
			output =
			    duckdb::make_uniq<duckdb::Value>(std::move(planner.plan->Cast<duckdb::LogicalExplain>().physical_plan));
		});
		*out_plan = Convert(output.release());
	});
}

DUCKDB_V2_ERROR duckdb_v2_connection_create(duckdb_v2_instance_handle instance, duckdb_v2_connection_handle *out_conn,
                                            duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(instance);
	DUCKDB_CHECK_ARG(out_conn);
	*out_conn = nullptr;
	return WithErrorHandler(err, [&]() {
		auto &instance_wrapper = *Convert(instance);
		auto connection = duckdb::make_uniq<CV2Connection>(instance_wrapper.GetDatabase());
		*out_conn = Convert(connection.release());
	});
}

DUCKDB_V2_ERROR duckdb_v2_connection_destroy(duckdb_v2_connection_handle *conn) {
	return WithErrorHandler(nullptr, [&]() {
		if (!conn) {
			return;
		}
		if (*conn) {
			delete Convert(*conn);
			*conn = nullptr;
		}
	});
}

// ---------------------------------------------------------------------------
// Query process management
// ---------------------------------------------------------------------------

DUCKDB_V2_ERROR duckdb_v2_connection_interrupt(duckdb_v2_connection_handle conn, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(conn);
	return WithErrorHandler(err, [&]() {
		// Record that the cancellation was user-initiated.
		auto &context = *Convert(conn)->context;
		GetBusySlot(context)->cancel_requests.fetch_add(1);

		// ClientContext::Interrupt is an atomic store; safe to call from any thread
		context.Interrupt();
	});
}

DUCKDB_V2_ERROR duckdb_v2_connection_progress_get(duckdb_v2_connection_handle conn, double *out_percentage,
                                                  uint64_t *out_rows_processed, uint64_t *out_total_rows_to_process,
                                                  duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(conn);
	DUCKDB_CHECK_ARG(out_percentage);
	DUCKDB_CHECK_ARG(out_rows_processed);
	DUCKDB_CHECK_ARG(out_total_rows_to_process);
	return WithErrorHandler(err, [&]() {
		auto progress = Convert(conn)->context->GetQueryProgress();
		*out_percentage = progress.GetPercentage();
		*out_rows_processed = progress.GetRowsProcessed();
		*out_total_rows_to_process = progress.GetTotalRowsToProcess();
	});
}
