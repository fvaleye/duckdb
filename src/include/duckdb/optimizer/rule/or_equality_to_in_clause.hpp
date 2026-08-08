//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/rule/or_equality_to_in_clause.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/optimizer/rule.hpp"

namespace duckdb {

//! Rewrites x = c1 OR x = c2 OR ... into x IN (c1, c2, ...), so that the disjunction benefits from
//! the filter pushdown and the mark join rewrite that an IN list already gets
class OrEqualityToInClauseRule : public Rule {
public:
	explicit OrEqualityToInClauseRule(ExpressionRewriter &rewriter);

	unique_ptr<Expression> Apply(LogicalOperator &op, vector<reference<Expression>> &bindings, bool &changes_made,
	                             bool is_root) override;
};

} // namespace duckdb
