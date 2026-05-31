/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <folly/container/F14Map.h>
#include "velox/functions/lib/Re2Functions.h"
#include "velox/functions/lib/Utf8Utils.h"
#include "velox/functions/lib/string/StringImpl.h"

namespace facebook::velox::functions::sparksql {
namespace {

using ::re2::RE2;

void ensureRegexIsConstant(
    const char* functionName,
    const VectorPtr& patternVector) {
  if (!patternVector || !patternVector->isConstantEncoding()) {
    VELOX_USER_FAIL("{} requires a constant pattern.", functionName);
  }
}

// REGEXP_REPLACE(string, pattern, overwrite) → string
// REGEXP_REPLACE(string, pattern, overwrite, position) → string
//
// If a string has a substring that matches the given pattern, replace
// the match in the string wither overwrite and return the string. If
// optional paramter position is provided, only make replacements
// after that positon in the string (1 indexed).
//
// If position <= 0, throw error.
// If position > length string, return string.
//
// Patterns that RE2 cannot compile (e.g. Perl-only syntax such as lookaheads)
// are compiled with ICU, which matches Java's java.util.regex semantics.
template <typename T>
struct RegexpReplaceFunction {
  VELOX_DEFINE_FUNCTION_TYPES(T);

  static constexpr bool is_default_ascii_behavior = true;

  FOLLY_ALWAYS_INLINE void initialize(
      const std::vector<TypePtr>& inputTypes,
      const core::QueryConfig& config,
      const arg_type<Varchar>* stringInput,
      const arg_type<Varchar>* pattern,
      const arg_type<Varchar>* replacement) {
    initialize(inputTypes, config, stringInput, pattern, replacement, nullptr);
  }

  FOLLY_ALWAYS_INLINE void initialize(
      const std::vector<TypePtr>& /*inputTypes*/,
      const core::QueryConfig& config,
      const arg_type<Varchar>* /*stringInput*/,
      const arg_type<Varchar>* pattern,
      const arg_type<Varchar>* replacement,
      const arg_type<int32_t>* /*position*/) {
    if (pattern) {
      const auto processedPattern = prepareRegexpReplacePattern(*pattern);
      checkPatternUtf8(processedPattern);
      re_.emplace(processedPattern, RE2::Quiet);
      if (re_->ok()) {
        if (replacement) {
          constantReplacement_ =
              prepareRegexpReplaceReplacement(re_.value(), *replacement);
        }
      } else {
        // RE2 failed (e.g. Perl-only syntax like lookaheads). Try ICU, which
        // matches Java's java.util.regex semantics used by Spark.
        re_.reset();
        icuRe_ = detail::compileIcuPattern(processedPattern);
        if (replacement) {
          constantReplacement_ = prepareIcuReplacement(*replacement);
        }
      }
    }
    cache_.setMaxCompiledRegexes(config.exprMaxCompiledRegexes());
  }

  void call(
      out_type<Varchar>& result,
      const arg_type<Varchar>& stringInput,
      const arg_type<Varchar>& pattern,
      const arg_type<Varchar>& replacement) {
    call(result, stringInput, pattern, replacement, 1);
  }

  void call(
      out_type<Varchar>& result,
      const arg_type<Varchar>& stringInput,
      const arg_type<Varchar>& pattern,
      const arg_type<Varchar>& replacement,
      const arg_type<int32_t>& position) {
    if (performChecks(
            result, stringInput, pattern, replacement, position - 1)) {
      return;
    }
    size_t start = functions::stringImpl::cappedByteLength<false>(
        stringInput, position - 1);
    if (start > stringInput.size() + 1) {
      result = stringInput;
      return;
    }
    performReplace(result, stringInput, pattern, replacement, start);
  }

  void callAscii(
      out_type<Varchar>& result,
      const arg_type<Varchar>& stringInput,
      const arg_type<Varchar>& pattern,
      const arg_type<Varchar>& replacement) {
    callAscii(result, stringInput, pattern, replacement, 1);
  }

  void callAscii(
      out_type<Varchar>& result,
      const arg_type<Varchar>& stringInput,
      const arg_type<Varchar>& pattern,
      const arg_type<Varchar>& replacement,
      const arg_type<int32_t>& position) {
    if (performChecks(
            result, stringInput, pattern, replacement, position - 1)) {
      return;
    }
    performReplace(result, stringInput, pattern, replacement, position - 1);
  }

 private:
  bool performChecks(
      out_type<Varchar>& result,
      const arg_type<Varchar>& stringInput,
      const arg_type<Varchar>& pattern,
      const arg_type<Varchar>& replace,
      const arg_type<int32_t>& position) {
    VELOX_USER_CHECK_GE(
        position + 1, 1, "regexp_replace requires a position >= 1");
    if (position > stringInput.size()) {
      result = stringInput;
      return true;
    }

    if (stringInput.size() == 0 && pattern.size() == 0 && position == 1) {
      result = replace;
      return true;
    }
    return false;
  }

  void performReplace(
      out_type<Varchar>& result,
      const arg_type<Varchar>& stringInput,
      const arg_type<Varchar>& pattern,
      const arg_type<Varchar>& replace,
      const arg_type<int32_t>& position) {
    std::string prefix(stringInput.data(), position);
    std::string targetString(
        stringInput.data() + position, stringInput.size() - position);

    if (re_.has_value()) {
      // Constant RE2 pattern.
      const auto& rep = constantReplacement_.has_value()
          ? constantReplacement_.value()
          : prepareRegexpReplaceReplacement(re_.value(), replace);
      RE2::GlobalReplace(&targetString, re_.value(), rep);
    } else if (icuRe_) {
      // Constant ICU pattern (Perl-only syntax, e.g. lookaheads).
      const auto& rep = constantReplacement_.has_value()
          ? constantReplacement_.value()
          : prepareIcuReplacement(replace);
      targetString =
          detail::applyIcuReplace(StringView(targetString), icuRe_.get(), rep);
    } else {
      // Non-constant pattern: compile on demand.
      const auto processedPattern = prepareRegexpReplacePattern(pattern);
      checkPatternUtf8(processedPattern);
      auto compiled = cache_.findOrCompile(StringView(processedPattern));
      if (compiled.usesIcu()) {
        targetString = detail::applyIcuReplace(
            StringView(targetString), compiled.icu, prepareIcuReplacement(replace));
      } else {
        const auto rep =
            prepareRegexpReplaceReplacement(*compiled.re2, replace);
        RE2::GlobalReplace(&targetString, *compiled.re2, rep);
      }
    }
    result = prefix + targetString;
  }

  // Throws VeloxUserError if pattern is not valid UTF-8.
  static void checkPatternUtf8(const std::string& pattern) {
    const char* p = pattern.data();
    const char* end = p + pattern.size();
    while (p < end) {
      int32_t codePoint;
      int32_t len = tryGetUtf8CharLength(p, end - p, codePoint);
      VELOX_USER_CHECK_GT(len, 0, "invalid UTF-8 in regular expression");
      p += len;
    }
  }

  // Constant RE2-compiled pattern; set in initialize() when pattern is const
  // and RE2 can compile it.
  std::optional<RE2> re_{};

  // Constant ICU-compiled pattern; set in initialize() when pattern is const
  // but RE2 cannot compile it (Perl-only syntax).
  detail::IcuPatternPtr icuRe_{};

  // Preprocessed replacement, cached when both pattern and replacement are
  // constant. For RE2 patterns this is RE2-formatted (\N groups); for ICU
  // patterns this is the raw Java-style replacement ($N groups).
  std::optional<std::string> constantReplacement_{};

  // Cache for non-constant patterns.
  detail::ReCache cache_{0};
};

} // namespace

// These functions delegate to the RE2-based implementations in
// common/RegexFunctions.h, but check to ensure that syntax that has different
// semantics between Spark (which uses java.util.regex) and RE2 throws an
// error.
std::shared_ptr<exec::VectorFunction> makeRLike(
    const std::string& name,
    const std::vector<exec::VectorFunctionArg>& inputArgs,
    const core::QueryConfig& config) {
  // Return any errors from re2Search() first.
  auto result = makeRe2Search(name, inputArgs, config);
  ensureRegexIsConstant("RLIKE", inputArgs[1].constantValue);
  return result;
}

std::shared_ptr<exec::VectorFunction> makeRegexExtract(
    const std::string& name,
    const std::vector<exec::VectorFunctionArg>& inputArgs,
    const core::QueryConfig& config) {
  auto result = makeRe2Extract(name, inputArgs, config, /*emptyNoMatch=*/true);
  ensureRegexIsConstant("REGEXP_EXTRACT", inputArgs[1].constantValue);
  return result;
}

void registerRegexpReplace(const std::string& prefix) {
  registerFunction<RegexpReplaceFunction, Varchar, Varchar, Varchar, Varchar>(
      {prefix + "regexp_replace"});
  registerFunction<
      RegexpReplaceFunction,
      Varchar,
      Varchar,
      Varchar,
      Varchar,
      int32_t>({prefix + "regexp_replace"});
}

} // namespace facebook::velox::functions::sparksql
