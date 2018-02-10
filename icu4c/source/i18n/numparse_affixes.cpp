// © 2018 and later: Unicode, Inc. and others.
// License & terms of use: http://www.unicode.org/copyright.html

#include "unicode/utypes.h"

#if !UCONFIG_NO_FORMATTING && !UPRV_INCOMPLETE_CPP11_SUPPORT

#include "numparse_types.h"
#include "numparse_affixes.h"
#include "numparse_utils.h"
#include "number_utils.h"

using namespace icu;
using namespace icu::numparse;
using namespace icu::numparse::impl;
using namespace icu::number;
using namespace icu::number::impl;


AffixPatternMatcherBuilder::AffixPatternMatcherBuilder(const UnicodeString& pattern,
                                                       AffixTokenMatcherWarehouse& warehouse,
                                                       IgnorablesMatcher* ignorables)
        : fMatchersLen(0),
          fLastTypeOrCp(0),
          fPattern(pattern),
          fWarehouse(warehouse),
          fIgnorables(ignorables) {}

void AffixPatternMatcherBuilder::consumeToken(AffixPatternType type, UChar32 cp, UErrorCode& status) {
    // This is called by AffixUtils.iterateWithConsumer() for each token.

    // Add an ignorables matcher between tokens except between two literals, and don't put two
    // ignorables matchers in a row.
    if (fIgnorables != nullptr && fMatchersLen > 0 &&
        (fLastTypeOrCp < 0 || !fIgnorables->getSet()->contains(fLastTypeOrCp))) {
        addMatcher(*fIgnorables);
    }

    if (type != TYPE_CODEPOINT) {
        // Case 1: the token is a symbol.
        switch (type) {
            case TYPE_MINUS_SIGN:
                addMatcher(fWarehouse.minusSign());
                break;
            case TYPE_PLUS_SIGN:
                addMatcher(fWarehouse.plusSign());
                break;
            case TYPE_PERCENT:
                addMatcher(fWarehouse.percent());
                break;
            case TYPE_PERMILLE:
                addMatcher(fWarehouse.permille());
                break;
            case TYPE_CURRENCY_SINGLE:
            case TYPE_CURRENCY_DOUBLE:
            case TYPE_CURRENCY_TRIPLE:
            case TYPE_CURRENCY_QUAD:
            case TYPE_CURRENCY_QUINT:
                // All currency symbols use the same matcher
                addMatcher(fWarehouse.currency(status));
                break;
            default:
                U_ASSERT(FALSE);
        }

    } else if (fIgnorables != nullptr && fIgnorables->getSet()->contains(cp)) {
        // Case 2: the token is an ignorable literal.
        // No action necessary: the ignorables matcher has already been added.

    } else {
        // Case 3: the token is a non-ignorable literal.
        addMatcher(fWarehouse.nextCodePointMatcher(cp));
    }
    fLastTypeOrCp = type != TYPE_CODEPOINT ? type : cp;
}

void AffixPatternMatcherBuilder::addMatcher(NumberParseMatcher& matcher) {
    if (fMatchersLen >= fMatchers.getCapacity()) {
        fMatchers.resize(fMatchersLen * 2, fMatchersLen);
    }
    fMatchers[fMatchersLen++] = &matcher;
}

AffixPatternMatcher AffixPatternMatcherBuilder::build() {
    return AffixPatternMatcher(fMatchers, fMatchersLen, fPattern);
}


AffixTokenMatcherWarehouse::AffixTokenMatcherWarehouse(const UChar* currencyCode,
                                                       const UnicodeString* currency1,
                                                       const UnicodeString* currency2,
                                                       const DecimalFormatSymbols* dfs,
                                                       IgnorablesMatcher* ignorables, const Locale* locale)
        : currency1(currency1),
          currency2(currency2),
          dfs(dfs),
          ignorables(ignorables),
          locale(locale),
          codePointCount(0),
          codePointNumBatches(0) {
    utils::copyCurrencyCode(this->currencyCode, currencyCode);
}

AffixTokenMatcherWarehouse::~AffixTokenMatcherWarehouse() {
    // Delete the variable number of batches of code point matchers
    for (int32_t i = 0; i < codePointNumBatches; i++) {
        delete[] codePointsOverflow[i];
    }
}

NumberParseMatcher& AffixTokenMatcherWarehouse::minusSign() {
    return fMinusSign = {*dfs, true};
}

NumberParseMatcher& AffixTokenMatcherWarehouse::plusSign() {
    return fPlusSign = {*dfs, true};
}

NumberParseMatcher& AffixTokenMatcherWarehouse::percent() {
    return fPercent = {*dfs};
}

NumberParseMatcher& AffixTokenMatcherWarehouse::permille() {
    return fPermille = {*dfs};
}

NumberParseMatcher& AffixTokenMatcherWarehouse::currency(UErrorCode& status) {
    return fCurrency = {{*locale, status}, {currencyCode, *currency1, *currency2}};
}

NumberParseMatcher& AffixTokenMatcherWarehouse::nextCodePointMatcher(UChar32 cp) {
    if (codePointCount < CODE_POINT_STACK_CAPACITY) {
        return codePoints[codePointCount++] = {cp};
    }
    int32_t totalCapacity = CODE_POINT_STACK_CAPACITY + codePointNumBatches * CODE_POINT_BATCH_SIZE;
    if (codePointCount >= totalCapacity) {
        // Need a new batch
        auto* nextBatch = new CodePointMatcher[CODE_POINT_BATCH_SIZE];
        if (codePointNumBatches >= codePointsOverflow.getCapacity()) {
            // Need more room for storing pointers to batches
            codePointsOverflow.resize(codePointNumBatches * 2, codePointNumBatches);
        }
        codePointsOverflow[codePointNumBatches++] = nextBatch;
    }
    return codePointsOverflow[codePointNumBatches - 1][(codePointCount++ - CODE_POINT_STACK_CAPACITY) %
                                                       CODE_POINT_BATCH_SIZE] = {cp};
}


CodePointMatcher::CodePointMatcher(UChar32 cp)
        : fCp(cp) {}

bool CodePointMatcher::match(StringSegment& segment, ParsedNumber& result, UErrorCode&) const {
    if (segment.matches(fCp)) {
        segment.adjustOffsetByCodePoint();
        result.setCharsConsumed(segment);
    }
    return false;
}

const UnicodeSet& CodePointMatcher::getLeadCodePoints() {
    if (fLocalLeadCodePoints.isNull()) {
        auto* leadCodePoints = new UnicodeSet();
        leadCodePoints->add(fCp);
        leadCodePoints->freeze();
        fLocalLeadCodePoints.adoptInstead(leadCodePoints);
    }
    return *fLocalLeadCodePoints;
}


AffixPatternMatcher AffixPatternMatcher::fromAffixPattern(const UnicodeString& affixPattern,
                                                          AffixTokenMatcherWarehouse& warehouse,
                                                          parse_flags_t parseFlags, bool* success,
                                                          UErrorCode& status) {
    if (affixPattern.isEmpty()) {
        *success = false;
        return {};
    }
    *success = true;

    IgnorablesMatcher* ignorables;
    if (0 != (parseFlags & PARSE_FLAG_EXACT_AFFIX)) {
        ignorables = nullptr;
    } else {
        ignorables = warehouse.ignorables;
    }

    AffixPatternMatcherBuilder builder(affixPattern, warehouse, ignorables);
    AffixUtils::iterateWithConsumer(UnicodeStringCharSequence(affixPattern), builder, status);
    return builder.build();
}

AffixPatternMatcher::AffixPatternMatcher(MatcherArray& matchers, int32_t matchersLen,
                                         const UnicodeString& pattern)
        : ArraySeriesMatcher(matchers, matchersLen), fPattern(pattern) {}

UnicodeString AffixPatternMatcher::getPattern() const {
    return fPattern.toAliasedUnicodeString();
}

bool AffixPatternMatcher::operator==(const AffixPatternMatcher& other) const {
    return fPattern == other.fPattern;
}


AffixMatcherWarehouse::AffixMatcherWarehouse(const AffixPatternProvider& patternInfo,
                                             NumberParserImpl& output,
                                             AffixTokenMatcherWarehouse& warehouse,
                                             const IgnorablesMatcher& ignorables, parse_flags_t parseFlags,
                                             UErrorCode& status)
        : fAffixTokenMatcherWarehouse(std::move(warehouse)) {
    if (!isInteresting(patternInfo, ignorables, parseFlags, status)) {
        return;
    }

    // The affixes have interesting characters, or we are in strict mode.
    // Use initial capacity of 6, the highest possible number of AffixMatchers.
    UnicodeString sb;
    bool includeUnpaired = 0 != (parseFlags & PARSE_FLAG_INCLUDE_UNPAIRED_AFFIXES);
    UNumberSignDisplay signDisplay = (0 != (parseFlags & PARSE_FLAG_PLUS_SIGN_ALLOWED)) ? UNUM_SIGN_ALWAYS
                                                                                        : UNUM_SIGN_NEVER;

    int32_t numAffixMatchers = 0;
    int32_t numAffixPatternMatchers = 0;

    AffixPatternMatcher* posPrefix = nullptr;
    AffixPatternMatcher* posSuffix = nullptr;

    // Pre-process the affix strings to resolve LDML rules like sign display.
    for (int8_t signum = 1; signum >= -1; signum--) {
        // Generate Prefix
        bool hasPrefix = false;
        PatternStringUtils::patternInfoToStringBuilder(
                patternInfo, true, signum, signDisplay, StandardPlural::OTHER, false, sb);
        fAffixPatternMatchers[numAffixPatternMatchers] = AffixPatternMatcher::fromAffixPattern(
                sb, warehouse, parseFlags, &hasPrefix, status);
        AffixPatternMatcher* prefix = hasPrefix ? &fAffixPatternMatchers[numAffixPatternMatchers++]
                                                : nullptr;

        // Generate Suffix
        bool hasSuffix = false;
        PatternStringUtils::patternInfoToStringBuilder(
                patternInfo, false, signum, signDisplay, StandardPlural::OTHER, false, sb);
        fAffixPatternMatchers[numAffixPatternMatchers] = AffixPatternMatcher::fromAffixPattern(
                sb, warehouse, parseFlags, &hasSuffix, status);
        AffixPatternMatcher* suffix = hasSuffix ? &fAffixPatternMatchers[numAffixPatternMatchers++]
                                                : nullptr;

        if (signum == 1) {
            posPrefix = prefix;
            posSuffix = suffix;
        } else if (equals(prefix, posPrefix) && equals(suffix, posSuffix)) {
            // Skip adding these matchers (we already have equivalents)
            continue;
        }

        // Flags for setting in the ParsedNumber
        int flags = (signum == -1) ? FLAG_NEGATIVE : 0;

        // Note: it is indeed possible for posPrefix and posSuffix to both be null.
        // We still need to add that matcher for strict mode to work.
        fAffixMatchers[numAffixMatchers++] = {prefix, suffix, flags};
        if (includeUnpaired && prefix != nullptr && suffix != nullptr) {
            // The following if statements are designed to prevent adding two identical matchers.
            if (signum == 1 || equals(prefix, posPrefix)) {
                fAffixMatchers[numAffixMatchers++] = {prefix, nullptr, flags};
            }
            if (signum == 1 || equals(suffix, posSuffix)) {
                fAffixMatchers[numAffixMatchers++] = {nullptr, suffix, flags};
            }
        }
    }

    // Put the AffixMatchers in order, and then add them to the output.
    // TODO
//    Collections.sort(matchers, COMPARATOR);
//    output.addMatchers(matchers);
}

bool AffixMatcherWarehouse::isInteresting(const AffixPatternProvider& patternInfo,
                                          const IgnorablesMatcher& ignorables, parse_flags_t parseFlags,
                                          UErrorCode& status) {
    UnicodeStringCharSequence posPrefixString(patternInfo.getString(AffixPatternProvider::AFFIX_POS_PREFIX));
    UnicodeStringCharSequence posSuffixString(patternInfo.getString(AffixPatternProvider::AFFIX_POS_SUFFIX));
    UnicodeStringCharSequence negPrefixString(UnicodeString(u""));
    UnicodeStringCharSequence negSuffixString(UnicodeString(u""));
    if (patternInfo.hasNegativeSubpattern()) {
        negPrefixString = UnicodeStringCharSequence(patternInfo.getString(AffixPatternProvider::AFFIX_NEG_PREFIX));
        negSuffixString = UnicodeStringCharSequence(patternInfo.getString(AffixPatternProvider::AFFIX_NEG_SUFFIX));
    }

    if (0 == (parseFlags & PARSE_FLAG_USE_FULL_AFFIXES) &&
        AffixUtils::containsOnlySymbolsAndIgnorables(posPrefixString, *ignorables.getSet(), status) &&
        AffixUtils::containsOnlySymbolsAndIgnorables(posSuffixString, *ignorables.getSet(), status) &&
        AffixUtils::containsOnlySymbolsAndIgnorables(negPrefixString, *ignorables.getSet(), status) &&
        AffixUtils::containsOnlySymbolsAndIgnorables(negSuffixString, *ignorables.getSet(), status)
        // HACK: Plus and minus sign are a special case: we accept them trailing only if they are
        // trailing in the pattern string.
        && !AffixUtils::containsType(posSuffixString, TYPE_PLUS_SIGN, status) &&
        !AffixUtils::containsType(posSuffixString, TYPE_MINUS_SIGN, status) &&
        !AffixUtils::containsType(negSuffixString, TYPE_PLUS_SIGN, status) &&
        !AffixUtils::containsType(negSuffixString, TYPE_MINUS_SIGN, status)) {
        // The affixes contain only symbols and ignorables.
        // No need to generate affix matchers.
        return false;
    }
    return true;
}

bool AffixMatcherWarehouse::equals(const AffixPatternMatcher* lhs, const AffixPatternMatcher* rhs) {
    if (lhs == nullptr && rhs == nullptr) {
        return true;
    }
    if (lhs == nullptr || rhs == nullptr) {
        return false;
    }
    return *lhs == *rhs;
}


AffixMatcher::AffixMatcher(AffixPatternMatcher* prefix, AffixPatternMatcher* suffix, result_flags_t flags)
        : fPrefix(prefix), fSuffix(suffix), fFlags(flags) {}

bool AffixMatcher::match(StringSegment& segment, ParsedNumber& result, UErrorCode& status) const {
    if (!result.seenNumber()) {
        // Prefix
        // Do not match if:
        // 1. We have already seen a prefix (result.prefix != null)
        // 2. The prefix in this AffixMatcher is empty (prefix == null)
        if (!result.prefix.isBogus() || fPrefix == nullptr) {
            return false;
        }

        // Attempt to match the prefix.
        int initialOffset = segment.getOffset();
        bool maybeMore = fPrefix->match(segment, result, status);
        if (initialOffset != segment.getOffset()) {
            result.prefix = fPrefix->getPattern();
        }
        return maybeMore;

    } else {
        // Suffix
        // Do not match if:
        // 1. We have already seen a suffix (result.suffix != null)
        // 2. The suffix in this AffixMatcher is empty (suffix == null)
        // 3. The matched prefix does not equal this AffixMatcher's prefix
        if (!result.suffix.isBogus() || fSuffix == nullptr || !matched(fPrefix, result.prefix)) {
            return false;
        }

        // Attempt to match the suffix.
        int initialOffset = segment.getOffset();
        bool maybeMore = fSuffix->match(segment, result, status);
        if (initialOffset != segment.getOffset()) {
            result.suffix = fSuffix->getPattern();
        }
        return maybeMore;
    }
}

const UnicodeSet& AffixMatcher::getLeadCodePoints() {
    if (fLocalLeadCodePoints.isNull()) {
        auto* leadCodePoints = new UnicodeSet();
        if (fPrefix != nullptr) {
            leadCodePoints->addAll(fPrefix->getLeadCodePoints());
        }
        if (fSuffix != nullptr) {
            leadCodePoints->addAll(fSuffix->getLeadCodePoints());
        }
        leadCodePoints->freeze();
        fLocalLeadCodePoints.adoptInstead(leadCodePoints);
    }
    return *fLocalLeadCodePoints;
}

void AffixMatcher::postProcess(ParsedNumber& result) const {
    // Check to see if our affix is the one that was matched. If so, set the flags in the result.
    if (matched(fPrefix, result.prefix) && matched(fSuffix, result.suffix)) {
        // Fill in the result prefix and suffix with non-null values (empty string).
        // Used by strict mode to determine whether an entire affix pair was matched.
        if (result.prefix.isBogus()) {
            result.prefix = UnicodeString();
        }
        if (result.suffix.isBogus()) {
            result.suffix = UnicodeString();
        }
        result.flags |= fFlags;
    }
}

bool AffixMatcher::matched(const AffixPatternMatcher* affix, const UnicodeString& patternString) {
    return (affix == nullptr && patternString.isBogus()) ||
           (affix != nullptr && affix->getPattern() == patternString);
}


#endif /* #if !UCONFIG_NO_FORMATTING */

























