#include "duckdb/optimizer/rule/or_equality_to_in_clause.hpp"

#include "duckdb/optimizer/matcher/expression_matcher.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"

namespace duckdb {

OrEqualityToInClauseRule::OrEqualityToInClauseRule(ExpressionRewriter &rewriter) : Rule(rewriter) {
	// we match on an OR expression
	root = make_uniq<ExpressionMatcher>();
	root->expr_type = make_uniq<SpecificExpressionTypeMatcher>(ExpressionType::CONJUNCTION_OR);
}

//! Records candidate as the expression every branch must compare against, and returns whether it
//! agrees with the branches seen so far
static bool MatchesTarget(const Expression &candidate, optional_ptr<const Expression> &target) {
	if (!target) {
		target = candidate;
		return true;
	}
	return target->Equals(candidate);
}

//! Collects the constants a single OR branch admits for target, covering both x = c and an
//! x IN (...) that an earlier rewrite already produced. Returns false if the branch is anything else.
static bool CollectBranchValues(const Expression &expr, optional_ptr<const Expression> &target, vector<Value> &values) {
	if (expr.GetExpressionType() == ExpressionType::COMPARE_IN) {
		auto &in_expr = expr.Cast<BoundOperatorExpression>();
		auto &children = in_expr.GetChildren();
		if (children.size() < 2 || !MatchesTarget(*children[0], target)) {
			return false;
		}
		for (idx_t i = 1; i < children.size(); i++) {
			if (children[i]->GetExpressionClass() != ExpressionClass::BOUND_CONSTANT) {
				return false;
			}
			values.push_back(children[i]->Cast<BoundConstantExpression>().GetValue());
		}
		return true;
	}
	if (expr.GetExpressionType() != ExpressionType::COMPARE_EQUAL || !BoundComparisonExpression::IsComparison(expr)) {
		return false;
	}
	auto &comp = expr.Cast<BoundFunctionExpression>();
	auto &left = BoundComparisonExpression::Left(comp);
	auto &right = BoundComparisonExpression::Right(comp);

	optional_ptr<const Expression> candidate;
	optional_ptr<const BoundConstantExpression> constant;
	if (right.GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
		candidate = left;
		constant = right.Cast<BoundConstantExpression>();
	} else if (left.GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
		candidate = right;
		constant = left.Cast<BoundConstantExpression>();
	} else {
		return false;
	}
	if (!MatchesTarget(*candidate, target)) {
		return false;
	}
	values.push_back(constant->GetValue());
	return true;
}

unique_ptr<Expression> OrEqualityToInClauseRule::Apply(LogicalOperator &op, vector<reference<Expression>> &bindings,
                                                       bool &changes_made, bool is_root) {
	auto &conj = bindings[0].get().Cast<BoundConjunctionExpression>();
	if (conj.GetChildren().size() < 2) {
		return nullptr;
	}

	optional_ptr<const Expression> target;
	vector<Value> values;
	for (auto &child : conj.GetChildren()) {
		if (!CollectBranchValues(*child, target, values)) {
			return nullptr;
		}
	}
	D_ASSERT(target);
	// the chain evaluates the target once per branch, the IN clause evaluates it once, so a target
	// that is volatile or inconsistent would change how many times it runs
	if (target->IsVolatile() || !target->IsConsistent()) {
		return nullptr;
	}
	for (auto &value : values) {
		// the types must match exactly, otherwise the IN clause would compare under different rules
		if (value.type() != target->GetReturnType()) {
			return nullptr;
		}
		// a nested constant holding an interior NULL compares differently through the IN clause's
		// mark join than through the chain, so leave those alone
		if (value.type().IsNested()) {
			return nullptr;
		}
	}

	auto in_expr = make_uniq<BoundOperatorExpression>(ExpressionType::COMPARE_IN, LogicalType::BOOLEAN);
	in_expr->GetChildrenMutable().push_back(target->Copy());
	for (auto &value : values) {
		in_expr->GetChildrenMutable().push_back(make_uniq<BoundConstantExpression>(std::move(value)));
	}
	changes_made = true;
	return std::move(in_expr);
}

} // namespace duckdb
