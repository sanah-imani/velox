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
#pragma once

#include <memory>
#include <vector>

#include <re2/re2.h>
#include "velox/expression/VectorFunction.h"
#include "velox/functions/Udf.h"
#include "velox/vector/BaseVector.h"

namespace facebook::velox::functions {

/// Representation of different kinds of patterns.
enum class PatternKind {
  /// Pattern containing wildcard character '_' only, such as _, __, ____.
  kExactlyN,
  /// Pattern containing wildcard characters ('_' or '%') only with at least one
  /// '%', such as ___%, _%__.
  kAtLeastN,
  /// Pattern with no wildcard characters, such as 'presto', 'foo'.
  kFixed,
  /// Pattern with single wildcard chars(_) & normal chars, such as
  /// '_pr_es_to_'.
  kRelaxedFixed,
  /// Fixed pattern followed by one or more '%', such as 'hello%', 'foo%%%%'.
  kPrefix,
  /// kRelaxedFixed pattern followed by one or more '%', such as '_pr_es_to_%',
  /// '_pr_es_to_%%%%'.
  kRelaxedPrefix,
  /// Fixed pattern preceded by one or more '%', such as '%foo', '%%%hello'.
  kSuffix,
  /// kRelaxedFixed preceded by one or more '%', such as '%_pr_es_to_',
  /// '%%%_pr_es_to_'.
  kRelaxedSuffix,
  /// Patterns matching '%{c0}%', such as '%foo%%', '%%%hello%'.
  kSubstring,
  /// Patterns matching '%{c0}%{c1}%', such as '%%foo%%bar%%', '%foo%bar%'.
  /// Note: Unlike kSubstring, kSubstrings applies only to constant patterns
  /// as pattern parsing is expensive.
  kSubstrings,
  /// Patterns which do not fit any of the above types, such as 'hello_world',
  /// '_presto%'.
  kGeneric,
};

// Kind of sub-pattern.
enum SubPatternKind {
  /// e.g. '___'.
  kSingleCharWildcard = 0,
  // e.g. '%%'.
  kAnyCharsWildcard = 1,
  // e.g. 'abc'.
  kLiteralString = 2
};

struct SubPatternMetadata {
  SubPatternKind kind;
  // The index of current pattern in terms of 'bytes'.
  size_t start;
  // Length in terms of bytes.
  size_t length;
};

class PatternMetadata {
 public:
  static PatternMetadata generic();

  static PatternMetadata atLeastN(size_t length);

  static PatternMetadata exactlyN(size_t length);

  static PatternMetadata fixed(const std::string& fixedPattern);

  static PatternMetadata relaxedFixed(
      std::string fixedPattern,
      std::vector<SubPatternMetadata> subPatterns);

  static PatternMetadata prefix(const std::string& fixedPattern);

  static PatternMetadata relaxedPrefix(
      std::string fixedPattern,
      std::vector<SubPatternMetadata> subPatterns);

  static PatternMetadata suffix(const std::string& fixedPattern);

  static PatternMetadata relaxedSuffix(
      std::string fixedPattern,
      std::vector<SubPatternMetadata> subPatterns);

  static PatternMetadata substring(const std::string& fixedPattern);

  static PatternMetadata substrings(std::vector<std::string> substrings);

  static std::vector<std::string> parseSubstrings(
      const std::string_view& pattern);

  PatternKind patternKind() const {
    return patternKind_;
  }

  size_t length() const {
    return length_;
  }

  const std::vector<SubPatternMetadata>& subPatterns() const {
    return subPatterns_;
  }

  const std::string& fixedPattern() const {
    return fixedPattern_;
  }

  const std::vector<std::string>& substrings() const {
    return substrings_;
  }

 private:
  PatternMetadata(
      PatternKind patternKind,
      size_t length,
      std::string fixedPattern,
      std::vector<SubPatternMetadata> subPatterns,
      std::vector<std::string> substrings);

  PatternKind patternKind_;

  /// Contains the length of the unescaped fixed pattern for patterns of kind
  /// k[Relaxed]Fixed, k[Relaxed]Prefix, k[Relaxed]Suffix and
  /// k[Relaxed]Substring. Contains the count of wildcard character '_' for
  /// patterns of kind kExactlyN and kAtLeastN. Contains 0 otherwise.
  size_t length_;

  /// Contains the fixed pattern in patterns of kind k[Relaxed]Fixed,
  /// k[Relaxed]Prefix, k[Relaxed]Suffix and k[Relaxed]Substring.
  std::string fixedPattern_;

  /// Contains the literal/single char wildcard sub patterns, it is only
  /// used for kRelaxedXxx patterns. e.g. If the pattern is: _pr_sto%, we will
  /// have four sub-patterns here: _, pr, _ and sto.
  std::vector<SubPatternMetadata> subPatterns_;

  std::vector<std::string> substrings_;
};

/// The functions in this file use RE2 as the primary regex engine. RE2 is
/// fast, but supports only a subset of PCRE syntax and in particular does not
/// support backtracking and associated features (e.g. lookaheads, backreferences).
/// When a pattern cannot be compiled by RE2, these functions fall back to ICU
/// regex, which supports full PCRE/Perl syntax including lookaheads.
/// See https://github.com/google/re2/wiki/Syntax for RE2 details.

/// re2Match(string, pattern) → bool
///
/// Returns whether str matches the regex pattern.  pattern will be parsed using
/// RE2 pattern syntax, a subset of PCRE. If the pattern is invalid, throws an
/// exception.
std::shared_ptr<exec::VectorFunction> makeRe2Match(
    const std::string& name,
    const std::vector<exec::VectorFunctionArg>& inputArgs,
    const core::QueryConfig& config);

std::vector<std::shared_ptr<exec::FunctionSignature>> re2MatchSignatures();

/// re2Search(string, pattern) → bool
///
/// Returns whether str has a substr that matches the regex pattern.  pattern
/// will be parsed using RE2 pattern syntax, a subset of PCRE. If the pattern is
/// invalid, throws an exception.
std::shared_ptr<exec::VectorFunction> makeRe2Search(
    const std::string& name,
    const std::vector<exec::VectorFunctionArg>& inputArgs,
    const core::QueryConfig& config);

std::vector<std::shared_ptr<exec::FunctionSignature>> re2SearchSignatures();

/// re2Extract(string, pattern, group_id) → string
/// re2Extract(string, pattern) → string
///
/// If string has a substring that matches the given pattern, returns the
/// substring matching the given group in the pattern. pattern will be parsed
/// using the RE2 pattern syntax, a subset of PCRE. Groups are 1-indexed.
/// Providing zero as the group_id extracts and returns the entire match; this
/// is more efficient than extracting a subgroup. Extracting the first subgroup
/// is more efficient than extracting larger indexes; use non-capturing
/// subgroups (?:...) if the pattern includes groups that don't need to be
/// captured.
///
/// If the pattern is invalid or the group id is out of range, throws an
/// exception. If the pattern does not match, returns null.
///
/// If group_id parameter is not specified, extracts and returns the entire
/// match.
std::shared_ptr<exec::VectorFunction> makeRe2Extract(
    const std::string& name,
    const std::vector<exec::VectorFunctionArg>& inputArgs,
    const core::QueryConfig& config,
    const bool emptyNoMatch);

std::vector<std::shared_ptr<exec::FunctionSignature>> re2ExtractSignatures();

/// Return the pair {pattern kind, length of the fixed pattern} for fixed,
/// prefix, and suffix patterns. Return the pair {pattern kind, number of '_'
/// characters} for patterns with wildcard characters only. Return
/// {kGenericPattern, 0} for generic patterns).
PatternMetadata determinePatternKind(
    std::string_view pattern,
    std::optional<char> escapeChar);

std::shared_ptr<exec::VectorFunction> makeLike(
    const std::string& name,
    const std::vector<exec::VectorFunctionArg>& inputArgs,
    const core::QueryConfig& config);

std::vector<std::shared_ptr<exec::FunctionSignature>> likeSignatures();

/// re2ExtractAll(string, pattern, group_id) → array<string>
/// re2ExtractAll(string, pattern) → array<string>
///
/// If string has a substring that matches the given pattern, returns ALL of the
/// substrings matching the given group in the pattern. pattern will be parsed
/// using the RE2 pattern syntax, a subset of PCRE. Groups are 1-indexed.
/// Providing zero as the group_id extracts and returns the entire match; this
/// is more efficient than extracting a subgroup. Extracting the first subgroup
/// is more efficient than extracting larger indexes; use non-capturing
/// subgroups (?:...) if the pattern includes groups that don't need to be
/// captured.
///
/// If the pattern is invalid or the group id is out of range, throws an
/// exception. If the pattern does not match, returns null.
///
/// If group_id parameter is not specified, extracts and returns the entire
/// match.
std::shared_ptr<exec::VectorFunction> makeRe2ExtractAll(
    const std::string& name,
    const std::vector<exec::VectorFunctionArg>& inputArgs,
    const core::QueryConfig& config);

std::vector<std::shared_ptr<exec::FunctionSignature>> re2ExtractAllSignatures();

namespace detail {

/// Deleter for ICU regex patterns stored as void* to avoid ICU headers here.
struct IcuPatternDeleter {
  void operator()(void* p) const noexcept;
};

/// Owning pointer to a compiled icu::RegexPattern, type-erased to void*.
using IcuPatternPtr = std::unique_ptr<void, IcuPatternDeleter>;

/// Compiles pattern with ICU regex. Throws VeloxUserError on invalid pattern.
IcuPatternPtr compileIcuPattern(const std::string& pattern);

/// Applies ICU-based global replacement. pattern must be a valid IcuPatternPtr.
std::string applyIcuReplace(
    StringView str,
    void* pattern,
    const std::string& replacement);

/// Non-owning view of a compiled regex, backed by either RE2 or ICU.
/// Exactly one of re2/icu is non-null.
struct CompiledRegex {
  RE2* re2{nullptr};
  // Actually icu::RegexPattern*, stored as void* to avoid ICU headers here.
  void* icu{nullptr};

  bool usesIcu() const noexcept {
    return icu != nullptr;
  }
};

// A cache of compiled regular expressions (RE2 or ICU instances). Allows up
// to 'expression.max_compiled_regexes' different expressions.
//
// Compiling regular expressions is expensive. It can take up to 200 times
// more CPU time to compile a regex vs. evaluate it.
//
// RE2 is tried first. If RE2 cannot compile the pattern (e.g. because it
// contains Perl-only syntax like lookaheads), ICU regex is used instead.
class ReCache {
 public:
  explicit ReCache(uint64_t maxCompiledRegexes)
      : maxCompiledRegexes_(maxCompiledRegexes) {}

  void setMaxCompiledRegexes(uint64_t maxCompiledRegexes) {
    maxCompiledRegexes_ = maxCompiledRegexes;
  }

  CompiledRegex findOrCompile(const StringView& pattern);

  Expected<CompiledRegex> tryFindOrCompile(const StringView& pattern);

 private:
  folly::F14FastMap<std::string, std::unique_ptr<RE2>> re2Cache_;
  folly::F14FastMap<std::string, IcuPatternPtr> icuCache_;
  uint64_t maxCompiledRegexes_;
};

} // namespace detail

/// Prepares a replacement string for use with ICU regex replaceAll.
/// Handles backslash-escape sequences: \$ → $ and \X → X for other chars.
/// $N and ${name} group references are passed through unchanged.
FOLLY_ALWAYS_INLINE std::string prepareIcuReplacement(
    const StringView& replacement) {
  if (replacement.size() == 0) {
    return std::string{};
  }
  std::string result;
  result.reserve(replacement.size());
  for (size_t i = 0; i < replacement.size(); ++i) {
    const char c = replacement.data()[i];
    if (c == '\\' && i + 1 < replacement.size()) {
      const char next = replacement.data()[i + 1];
      if (next == '$' || next == '\\') {
        // Keep as-is: ICU interprets \$ as literal $ and \\ as literal \.
        result.push_back(c);
        result.push_back(next);
        ++i;
      } else {
        // Unescape: \X → X for all other characters.
        result.push_back(next);
        ++i;
      }
    } else {
      result.push_back(c);
    }
  }
  return result;
}

/// regexp_replace(string, pattern, replacement) -> string
/// regexp_replace(string, pattern) -> string
///
/// If string has substrings that match the given pattern, return a new string
/// that has all the matched substrings replaced with the given replacement
/// sequence or removed if no replacement sequence is provided. Patterns that
/// RE2 cannot compile (e.g. those using Perl-only lookahead syntax) are
/// handled by ICU regex instead. replacement is a string that may contain
/// references to the named or numbered capturing groups in the pattern.
template <
    typename T,
    std::string (*prepareRegexpPattern)(const StringView&),
    std::string (*prepareRegexpReplacement)(const RE2&, const StringView&)>
struct Re2RegexpReplace {
  Re2RegexpReplace() : cache_(0) {}

  VELOX_DEFINE_FUNCTION_TYPES(T);

  FOLLY_ALWAYS_INLINE void initialize(
      const std::vector<TypePtr>& /*inputTypes*/,
      const core::QueryConfig& config,
      const arg_type<Varchar>* /*string*/,
      const arg_type<Varchar>* pattern,
      const arg_type<Varchar>* replacement) {
    if (pattern != nullptr) {
      const auto processedPattern = prepareRegexpPattern(*pattern);
      re_.emplace(processedPattern, RE2::Quiet);
      if (!re_->ok()) {
        // RE2 failed — try ICU as fallback for Perl-only syntax.
        re_.reset();
        icuRe_ = detail::compileIcuPattern(processedPattern);
      }
    }
    cache_.setMaxCompiledRegexes(config.exprMaxCompiledRegexes());

    if (replacement != nullptr && pattern != nullptr) {
      if (icuRe_) {
        processedReplacement_ = prepareIcuReplacement(*replacement);
      } else {
        processedReplacement_ =
            prepareRegexpReplacement(re_.value(), *replacement);
      }
      constantReplacement_ = true;
    }
  }

  FOLLY_ALWAYS_INLINE void initialize(
      const std::vector<TypePtr>& inputTypes,
      const core::QueryConfig& config,
      const arg_type<Varchar>* string,
      const arg_type<Varchar>* pattern) {
    initialize(inputTypes, config, string, pattern, nullptr);
  }

  FOLLY_ALWAYS_INLINE void call(
      out_type<Varchar>& out,
      const arg_type<Varchar>& string,
      const arg_type<Varchar>& pattern,
      const arg_type<Varchar>& replacement = StringView{}) {
    if (icuRe_) {
      // Constant ICU pattern.
      if (!constantReplacement_) {
        processedReplacement_ = prepareIcuReplacement(replacement);
      }
      result_ = detail::applyIcuReplace(string, icuRe_.get(), processedReplacement_);
    } else if (re_.has_value()) {
      // Constant RE2 pattern.
      if (!constantReplacement_) {
        processedReplacement_ =
            prepareRegexpReplacement(re_.value(), replacement);
      }
      result_.assign(string.data(), string.size());
      RE2::GlobalReplace(&result_, re_.value(), processedReplacement_);
    } else {
      // Non-constant pattern: look up in cache.
      const auto processedPattern = prepareRegexpPattern(pattern);
      auto compiled = cache_.findOrCompile(StringView(processedPattern));
      if (compiled.usesIcu()) {
        const auto repl = prepareIcuReplacement(replacement);
        result_ = detail::applyIcuReplace(string, compiled.icu, repl);
      } else {
        const auto repl = prepareRegexpReplacement(*compiled.re2, replacement);
        result_.assign(string.data(), string.size());
        RE2::GlobalReplace(&result_, *compiled.re2, repl);
      }
    }
    UDFOutputString::assign(out, result_);
  }

 private:
  // Used when pattern is constant and RE2 compiled successfully.
  std::optional<RE2> re_;

  // Used when pattern is constant and RE2 failed (ICU fallback).
  detail::IcuPatternPtr icuRe_;

  // True if replacement is constant.
  bool constantReplacement_{false};

  // Processed replacement string (cached when replacement is constant).
  std::string processedReplacement_;

  // Used when pattern is not constant.
  detail::ReCache cache_;

  // Scratch memory for the replacement result.
  std::string result_;
};

std::shared_ptr<exec::VectorFunction> makeRegexpReplaceWithLambda(
    const std::string& name,
    const std::vector<exec::VectorFunctionArg>& inputArgs,
    const core::QueryConfig& config);

std::vector<std::shared_ptr<exec::FunctionSignature>>
regexpReplaceWithLambdaSignatures();

/// This function preprocesses an input pattern string to follow RE2 syntax.
/// Java Pattern supports named capturing groups in the format
/// (?<name>regex), but in RE2, this is written as (?P<name>regex), so we need
/// to convert the former format to the latter.
/// Presto https://prestodb.io/docs/current/functions/regexp.html
/// Spark
/// https://archive.apache.org/dist/spark/docs/3.5.2/api/sql/index.html#regexp_replace
FOLLY_ALWAYS_INLINE std::string prepareRegexpReplacePattern(
    const StringView& pattern) {
  static const RE2 kRegex("[(][?]<([^>]*)>");

  std::string newPattern = pattern.getString();
  RE2::GlobalReplace(&newPattern, kRegex, R"((?P<\1>)");

  return newPattern;
}

/// This function preprocesses an input replacement string to follow RE2 syntax
/// for java.util.regex used by Presto and Spark. These are the replacements
/// that are required.
/// 1. RE2 replacement only supports group index capture, so we need to convert
/// group name captures to group index captures.
/// 2. Group index capture in java.util.regex replacement is '$N', while in RE2
/// replacement it is '\N'. We need to convert it.
/// 3. Replacement in RE2 only supports '\' followed by a digit or another '\',
/// while java.util.regex will ignore '\' in replacements, so we need to
/// unescape it.
FOLLY_ALWAYS_INLINE std::string prepareRegexpReplaceReplacement(
    const RE2& re,
    const StringView& replacement) {
  if (replacement.size() == 0) {
    return std::string{};
  }

  auto newReplacement = replacement.getString();

  static const RE2 kExtractRegex(R"(\${([^}]*)})");
  VELOX_DCHECK(
      kExtractRegex.ok(),
      "Invalid regular expression {}: {}.",
      R"(\${([^}]*)})",
      kExtractRegex.error());

  // If newReplacement contains a reference to a
  // named capturing group ${name}, replace the name with its index.
  re2::StringPiece groupName[2];
  while (kExtractRegex.Match(
      newReplacement,
      0,
      newReplacement.size(),
      RE2::UNANCHORED,
      groupName,
      2)) {
    auto groupIter = re.NamedCapturingGroups().find(std::string(groupName[1]));
    if (groupIter == re.NamedCapturingGroups().end()) {
      VELOX_USER_FAIL(
          "Invalid replacement sequence: unknown group {{ {} }}.",
          std::string(groupName[1]));
    }

    RE2::GlobalReplace(
        &newReplacement,
        fmt::format(R"(\${{{}}})", std::string(groupName[1])),
        fmt::format("${}", groupIter->second));
  }

  // Convert references to numbered capturing groups from $g to \g.
  static const RE2 kConvertRegex(R"(\$(\d+))");
  VELOX_DCHECK(
      kConvertRegex.ok(),
      "Invalid regular expression {}: {}.",
      R"(\$(\d+))",
      kConvertRegex.error());
  RE2::GlobalReplace(&newReplacement, kConvertRegex, R"(\\\1)");

  // Un-escape character except digit or '\\'
  static const RE2 kUnescapeRegex(R"(\\([^0-9\\]))");
  VELOX_DCHECK(
      kUnescapeRegex.ok(),
      "Invalid regular expression {}: {}.",
      R"(\\([^0-9\\]))",
      kUnescapeRegex.error());
  RE2::GlobalReplace(&newReplacement, kUnescapeRegex, R"(\1)");

  return newReplacement;
}

} // namespace facebook::velox::functions

template <>
struct fmt::formatter<facebook::velox::functions::PatternKind>
    : formatter<int> {
  auto format(facebook::velox::functions::PatternKind s, format_context& ctx)
      const {
    return formatter<int>::format(static_cast<int>(s), ctx);
  }
};
