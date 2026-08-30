#include "core_functions/scalar/string_functions.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/common/vector_operations/ternary_executor.hpp"
#include "utf8proc.hpp"
#include "utf8proc_wrapper.hpp"

#include <string.h>
#include <ctype.h>
#include <unordered_map>
#include <unordered_set>

namespace duckdb {

static void BuildTranslateMap(const string_t &needle, const string_t &thread,
                              unordered_map<int32_t, int32_t> &to_replace, unordered_set<int32_t> &to_delete) {
	auto input_needle = needle.GetData();
	auto size_needle = needle.GetSize();
	auto input_thread = thread.GetData();
	auto size_thread = thread.GetSize();
	idx_t i = 0, j = 0;
	int sz = 0;

	// Character to be replaced
	while (i < size_needle && j < size_thread) {
		auto codepoint_needle = Utf8Proc::UTF8ToCodepoint(input_needle, sz, size_needle - i);
		input_needle += sz;
		i += UnsafeNumericCast<idx_t>(sz);
		auto codepoint_thread = Utf8Proc::UTF8ToCodepoint(input_thread, sz, size_thread - j);
		input_thread += sz;
		j += UnsafeNumericCast<idx_t>(sz);
		// Ignore unicode character that is existed in to_replace
		if (to_replace.count(codepoint_needle) == 0) {
			to_replace[codepoint_needle] = codepoint_thread;
		}
	}

	// Character to be deleted
	while (i < size_needle) {
		auto codepoint_needle = Utf8Proc::UTF8ToCodepoint(input_needle, sz, size_needle - i);
		input_needle += sz;
		i += UnsafeNumericCast<idx_t>(sz);
		// Add unicode character that will be deleted
		if (to_replace.count(codepoint_needle) == 0) {
			to_delete.insert(codepoint_needle);
		}
	}
}

static string_t TranslateScalarFunction(const string_t &haystack, const unordered_map<int32_t, int32_t> &to_replace,
                                        const unordered_set<int32_t> &to_delete, vector<char> &result) {
	auto input_haystack = haystack.GetData();
	auto size_haystack = haystack.GetSize();

	// Reuse the buffer
	result.clear();
	result.reserve(size_haystack);

	idx_t i = 0;
	int sz = 0, c_sz = 0;
	char c[5] = {'\0', '\0', '\0', '\0', '\0'};
	for (i = 0; i < size_haystack; i += UnsafeNumericCast<idx_t>(sz)) {
		auto codepoint_haystack = Utf8Proc::UTF8ToCodepoint(input_haystack, sz, size_haystack - i);
		auto replace_entry = to_replace.find(codepoint_haystack);
		if (replace_entry != to_replace.end()) {
			Utf8Proc::CodepointToUtf8(replace_entry->second, c_sz, c);
			result.insert(result.end(), c, c + c_sz);
		} else if (to_delete.count(codepoint_haystack) == 0) {
			result.insert(result.end(), input_haystack, input_haystack + sz);
		}
		input_haystack += sz;
	}

	return string_t(result.data(), UnsafeNumericCast<uint32_t>(result.size()));
}

static string_t TranslateScalarFunction(const string_t &haystack, const string_t &needle, const string_t &thread,
                                        vector<char> &result) {
	unordered_map<int32_t, int32_t> to_replace;
	unordered_set<int32_t> to_delete;
	BuildTranslateMap(needle, thread, to_replace, to_delete);
	return TranslateScalarFunction(haystack, to_replace, to_delete, result);
}

static void TranslateFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	const auto &haystack_vector = args.data[0];
	const auto &needle_vector = args.data[1];
	const auto &thread_vector = args.data[2];

	vector<char> buffer;
	auto &heap = StringVector::GetStringHeap(result);
	// a constant needle and thread describe the same mapping for every row, so build it once per chunk
	if (needle_vector.GetVectorType() == VectorType::CONSTANT_VECTOR && !ConstantVector::IsNull(needle_vector) &&
	    thread_vector.GetVectorType() == VectorType::CONSTANT_VECTOR && !ConstantVector::IsNull(thread_vector)) {
		unordered_map<int32_t, int32_t> to_replace;
		unordered_set<int32_t> to_delete;
		BuildTranslateMap(*ConstantVector::GetData<string_t>(needle_vector),
		                  *ConstantVector::GetData<string_t>(thread_vector), to_replace, to_delete);
		UnaryExecutor::Execute<string_t, string_t>(haystack_vector, result, args.size(), [&](string_t input_string) {
			return heap.AddString(TranslateScalarFunction(input_string, to_replace, to_delete, buffer));
		});
		return;
	}
	TernaryExecutor::Execute<string_t, string_t, string_t, string_t>(
	    haystack_vector, needle_vector, thread_vector, result,
	    [&](string_t input_string, string_t needle_string, string_t thread_string) {
		    return heap.AddString(TranslateScalarFunction(input_string, needle_string, thread_string, buffer));
	    });
}

ScalarFunction TranslateFun::GetFunction() {
	return ScalarFunction({LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR}, LogicalType::VARCHAR,
	                      TranslateFunction);
}

} // namespace duckdb
