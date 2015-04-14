/*
 * Copyright (C) 2015, International Business Machines
 * Corporation and others.  All Rights Reserved.
 *
 * file name: affixpatternparser.cpp
 */

#include "affixpatternparser.h"

#include "charstr.h"
#include "unicode/ucurr.h"
#include "unicode/plurrule.h"
#include "precision.h"
#include "unicode/dcfmtsym.h"
#include "uassert.h"
#include "unistrappender.h"

        static UChar gDefaultSymbols[] = {0xa4, 0xa4, 0xa4};

#define PACK_TOKEN_AND_LENGTH(t, l) ((UChar) (((t) << 8) | (l & 0xFF)))

#define UNPACK_TOKEN(c) ((AffixPattern::ETokenType) (((c) >> 8) & 0x7F))

#define UNPACK_LONG(c) (((c) >> 8) & 0x80)

#define UNPACK_LENGTH(c) ((c) & 0xFF)

U_NAMESPACE_BEGIN

static int32_t
nextToken(const UChar *buffer, int32_t idx, int32_t len, UChar *token) {
    if (buffer[idx] != 0x27 || idx + 1 == len) {
        *token = buffer[idx];
        return 1;
    }
    *token = buffer[idx + 1];
    if (buffer[idx + 1] == 0xA4) {
        int32_t i = 2;
        for (; idx + i < len && i < 4 && buffer[idx + i] == buffer[idx + 1]; ++i);
        return i;
    }
    return 2;
}

static int32_t
nextUserToken(const UChar *buffer, int32_t idx, int32_t len, UChar *token) {
    *token = buffer[idx];
    int32_t max;
    switch (buffer[idx]) {
    case 0x27:
        max = 2;
        break;
    case 0xA4:
        max = 3;
        break;
    default:
        max = 1;
        break;
    }
    int32_t i = 1;
    for (; idx + i < len && i < max && buffer[idx + i] == buffer[idx]; ++i);
    return i;
}

CurrencyAffixInfo::CurrencyAffixInfo() {
    UErrorCode status = U_ZERO_ERROR;
    set(NULL, NULL, NULL, status);
}

UBool CurrencyAffixInfo::isDefault() const {
    UnicodeString dSymbol(gDefaultSymbols, 1);
    UnicodeString dISO(gDefaultSymbols, 2);
    PluralAffix dLong;
    dLong.append(gDefaultSymbols, 3);
    return (fSymbol == dSymbol && fISO == dISO && fLong.equals(dLong));
}

void
CurrencyAffixInfo::set(
        const char *locale,
        const PluralRules *rules,
        const UChar *currency,
        UErrorCode &status) {
    if (U_FAILURE(status)) {
        return;
    }
    if (currency == NULL) {
        fSymbol.setTo(gDefaultSymbols, 1);
        fISO.setTo(gDefaultSymbols, 2);
        fLong.remove();
        fLong.append(gDefaultSymbols, 3);
        return;
    }
    int32_t len;
    UBool unusedIsChoice;
    const UChar *symbol = ucurr_getName(
            currency, locale, UCURR_SYMBOL_NAME, &unusedIsChoice,
            &len, &status);
    if (U_FAILURE(status)) {
        return;
    }
    fSymbol.setTo(symbol, len);
    fISO.setTo(currency, u_strlen(currency));
    fLong.remove();
    StringEnumeration* keywords = rules->getKeywords(status);
    if (U_FAILURE(status)) {
        return;
    }
    const UnicodeString* pluralCount;
    while ((pluralCount = keywords->snext(status)) != NULL) {
        CharString pCount;
        pCount.appendInvariantChars(*pluralCount, status);
        const UChar *pluralName = ucurr_getPluralName(
            currency, locale, &unusedIsChoice, pCount.data(),
            &len, &status);
        fLong.setVariant(pCount.data(), UnicodeString(pluralName, len), status);
    }
    delete keywords;
}

void
CurrencyAffixInfo::adjustPrecision(
        const UChar *currency, const UCurrencyUsage usage,
        FixedPrecision &precision, UErrorCode &status) {
    if (U_FAILURE(status)) {
        return;
    }

    int32_t digitCount = ucurr_getDefaultFractionDigitsForUsage(
            currency, usage, &status);
    precision.fMin.setFracDigitCount(digitCount);
    precision.fMax.setFracDigitCount(digitCount);
    double increment = ucurr_getRoundingIncrementForUsage(
            currency, usage, &status);
    if (increment == 0.0) {
        precision.fRoundingIncrement.clear();
    } else {
        precision.fRoundingIncrement.set(increment);
        // guard against round-off error
        precision.fRoundingIncrement.round(6);
    }
}

void
AffixPattern::addLiteral(
        const UChar *literal, int32_t start, int32_t len) {
    char32Count += u_countChar32(literal + start, len);
    literals.append(literal, start, len);
    int32_t tlen = tokens.length();
    // Takes 4 UChars to encode maximum literal length.
    UChar *tokenChars = tokens.getBuffer(tlen + 4);

    // find start of literal size. May be tlen if there is no literal.
    // While finding start of literal size, compute literal length
    int32_t literalLength = 0;
    int32_t tLiteralStart = tlen;
    while (tLiteralStart > 0 && UNPACK_TOKEN(tokenChars[tLiteralStart - 1]) == kLiteral) {
        tLiteralStart--;
        literalLength <<= 8;
        literalLength |= UNPACK_LENGTH(tokenChars[tLiteralStart]);
    }
    // Add number of chars we just added to literal
    literalLength += len;

    // Now encode the new length starting at tLiteralStart
    tlen = tLiteralStart;
    tokenChars[tlen++] = PACK_TOKEN_AND_LENGTH(kLiteral, literalLength & 0xFF);
    literalLength >>= 8;
    while (literalLength) {
        tokenChars[tlen++] = PACK_TOKEN_AND_LENGTH(kLiteral | 0x80, literalLength & 0xFF);
        literalLength >>= 8;
    }
    tokens.releaseBuffer(tlen);
}

void
AffixPattern::add(ETokenType t) {
    add(t, 1);
}

void
AffixPattern::addCurrency(uint8_t count) {
    add(kCurrency, count);
}

void
AffixPattern::add(ETokenType t, uint8_t count) {
    U_ASSERT(t != kLiteral);
    char32Count += count;
    switch (t) {
    case kCurrency: 
        hasCurrencyToken = TRUE;
        break;
    case kPercent:
        hasPercentToken = TRUE;
        break;
    case kPerMill:
        hasPermillToken = TRUE;
        break;
    default:
        // Do nothing
        break;
    }
    tokens.append(PACK_TOKEN_AND_LENGTH(t, count));
}

void
AffixPattern::remove() {
    tokens.remove();
    literals.remove();
    hasCurrencyToken = FALSE;
    hasPercentToken = FALSE;
    hasPermillToken = FALSE;
    char32Count = 0;
}

static void escapeLiteral(
        const UnicodeString &literal, UnicodeStringAppender &appender) {
    int32_t len = literal.length();
    const UChar *buffer = literal.getBuffer();
    appender.append((UChar) 0x27);
    for (int32_t i = 0; i < len; ++i) {
        UChar ch = buffer[i];
        switch (ch) {
            case 0x27:
                appender.append((UChar) 0x27);
                appender.append((UChar) 0x27);
                break;
            default:
                appender.append(ch);
                break;
        }
    }
    appender.append((UChar) 0x27);
}

UnicodeString &
AffixPattern::toUserString(UnicodeString &appendTo) const {
    AffixPatternIterator iter;
    iterator(iter);
    UnicodeStringAppender appender(appendTo);
    UnicodeString literal;
    while (iter.nextToken()) {
        switch (iter.getTokenType()) {
        case kLiteral:
            escapeLiteral(iter.getLiteral(literal), appender);
            break;
        case kPercent:
            appender.append((UChar) 0x25);
            break;
        case kPerMill:
            appender.append((UChar) 0x2030);
            break;
        case kCurrency:
            {
                int32_t cl = iter.getTokenLength();
                for (int32_t i = 0; i < cl; ++i) {
                    appender.append((UChar) 0xA4);
                }
            }
            break;
        case kNegative:
            appender.append((UChar) 0x2D);
            break;
        default:
            U_ASSERT(FALSE);
            break;
        }
    }
    return appendTo;
}

class AffixPatternAppender : public UMemory {
public:
    AffixPatternAppender(AffixPattern &dest) : fDest(&dest), fIdx(0) { }

    inline void append(UChar x) {
        if (fIdx == UPRV_LENGTHOF(fBuffer)) {
            fDest->addLiteral(fBuffer, 0, fIdx);
            fIdx = 0;
        }
        fBuffer[fIdx++] = x;
    }

    inline void append(UChar32 x) {
        if (fIdx >= UPRV_LENGTHOF(fBuffer) - 1) {
            fDest->addLiteral(fBuffer, 0, fIdx);
            fIdx = 0;
        }
        U16_APPEND_UNSAFE(fBuffer, fIdx, x);
    }

    inline void flush() {
        if (fIdx) {
            fDest->addLiteral(fBuffer, 0, fIdx);
        }
        fIdx = 0;
    }

    /**
     * flush the buffer when we go out of scope.
     */
    ~AffixPatternAppender() {
        flush();
    }
private:
    AffixPattern *fDest;
    int32_t fIdx;
    UChar fBuffer[32];
    AffixPatternAppender(const AffixPatternAppender &other);
    AffixPatternAppender &operator=(const AffixPatternAppender &other);
};


AffixPattern &
AffixPattern::parseUserAffixString(
        const UnicodeString &affixStr,
        AffixPattern &appendTo, 
        UErrorCode &status) {
    if (U_FAILURE(status)) {
        return appendTo;
    }
    int32_t len = affixStr.length();
    const UChar *buffer = affixStr.getBuffer();
    // 0 = not quoted; 1 = quoted.
    int32_t state = 0;
    AffixPatternAppender appender(appendTo);
    for (int32_t i = 0; i < len; ) {
        UChar token;
        int32_t tokenSize = nextUserToken(buffer, i, len, &token);
        i += tokenSize;
        if (token == 0x27 && tokenSize == 1) { // quote
            state = 1 - state;
            continue;
        }
        if (state == 0) {
            switch (token) {
            case 0x25:
                appender.flush();
                appendTo.add(kPercent, 1);
                break;
            case 0x27:  // double quote
                appender.append((UChar) 0x27);
                break;
            case 0x2030:
                appender.flush();
                appendTo.add(kPerMill, 1);
                break;
            case 0x2D:
                appender.flush();
                appendTo.add(kNegative, 1);
                break;
            case 0xA4:
                appender.flush();
                appendTo.add(kCurrency, tokenSize);
                break;
            default:
                appender.append(token);
                break;
            }
        } else {
            switch (token) {
            case 0x27:  // double quote
                appender.append((UChar) 0x27);
                break;
            case 0xA4: // included b/c tokenSize can be > 1
                for (int32_t j = 0; j < tokenSize; ++j) {
                    appender.append((UChar) 0xA4);
                }
                break;
            default:
                appender.append(token);
                break;
            }
        }
    }
    return appendTo;
}

AffixPattern &
AffixPattern::parseAffixString(
        const UnicodeString &affixStr,
        AffixPattern &appendTo, 
        UErrorCode &status) {
    if (U_FAILURE(status)) {
        return appendTo;
    }
    int32_t len = affixStr.length();
    const UChar *buffer = affixStr.getBuffer();
    for (int32_t i = 0; i < len; ) {
        UChar token;
        int32_t tokenSize = nextToken(buffer, i, len, &token);
        if (tokenSize == 1) {
            int32_t literalStart = i;
            ++i;
            while (i < len && (tokenSize = nextToken(buffer, i, len, &token)) == 1) {
                ++i;
            }
            appendTo.addLiteral(buffer, literalStart, i - literalStart);

            // If we reached end of string, we are done
            if (i == len) {
                return appendTo;
            }
        }
        i += tokenSize;
        switch (token) {
        case 0x25:
            appendTo.add(kPercent, 1);
            break;
        case 0x2030:
            appendTo.add(kPerMill, 1);
            break;
        case 0x2D:
            appendTo.add(kNegative, 1);
            break;
        case 0xA4:
            {
                if (tokenSize - 1 > 3) {
                    status = U_PARSE_ERROR;
                    return appendTo;
                }
                appendTo.add(kCurrency, tokenSize - 1);
            }
            break;
        default:
            appendTo.addLiteral(&token, 0, 1);
            break;
        }
    }
    return appendTo;
}

AffixPatternIterator &
AffixPattern::iterator(AffixPatternIterator &result) const {
    result.nextLiteralIndex = 0;
    result.lastLiteralLength = 0;
    result.nextTokenIndex = 0;
    result.tokens = &tokens;
    result.literals = &literals;
    return result;
}

UBool
AffixPatternIterator::nextToken() {
    int32_t tlen = tokens->length();
    if (nextTokenIndex == tlen) {
        return FALSE;
    }
    ++nextTokenIndex;
    const UChar *tokenBuffer = tokens->getBuffer();
    if (UNPACK_TOKEN(tokenBuffer[nextTokenIndex - 1]) ==
            AffixPattern::kLiteral) {
        while (nextTokenIndex < tlen &&
                UNPACK_LONG(tokenBuffer[nextTokenIndex])) {
            ++nextTokenIndex;
        }
        lastLiteralLength = 0;
        int32_t i = nextTokenIndex - 1;
        for (; UNPACK_LONG(tokenBuffer[i]); --i) {
            lastLiteralLength <<= 8;
            lastLiteralLength |= UNPACK_LENGTH(tokenBuffer[i]);
        }
        lastLiteralLength <<= 8;
        lastLiteralLength |= UNPACK_LENGTH(tokenBuffer[i]);
        nextLiteralIndex += lastLiteralLength;
    }
    return TRUE;
}

AffixPattern::ETokenType
AffixPatternIterator::getTokenType() const {
    return UNPACK_TOKEN(tokens->charAt(nextTokenIndex - 1));
}

UnicodeString &
AffixPatternIterator::getLiteral(UnicodeString &result) const {
    const UChar *buffer = literals->getBuffer();
    result.setTo(buffer + (nextLiteralIndex - lastLiteralLength), lastLiteralLength);
    return result;
}

int32_t
AffixPatternIterator::getTokenLength() const {
    const UChar *tokenBuffer = tokens->getBuffer();
    AffixPattern::ETokenType type = UNPACK_TOKEN(tokenBuffer[nextTokenIndex - 1]);
    return type == AffixPattern::kLiteral ? lastLiteralLength : UNPACK_LENGTH(tokenBuffer[nextTokenIndex - 1]);
}

AffixPatternParser::AffixPatternParser()
        : fPercent("%"), fPermill("\u2030"), fNegative("-") {
    fPermill = fPermill.unescape();
}

AffixPatternParser::AffixPatternParser(
        const DecimalFormatSymbols &symbols) {
    setDecimalFormatSymbols(symbols);
}

void
AffixPatternParser::setDecimalFormatSymbols(
        const DecimalFormatSymbols &symbols) {
    fPercent = symbols.getConstSymbol(DecimalFormatSymbols::kPercentSymbol);
    fPermill = symbols.getConstSymbol(DecimalFormatSymbols::kPerMillSymbol);
    fNegative = symbols.getConstSymbol(DecimalFormatSymbols::kMinusSignSymbol);
}

PluralAffix &
AffixPatternParser::parse(
        const AffixPattern &affixPattern,
        const CurrencyAffixInfo &currencyAffixInfo,
        PluralAffix &appendTo, 
        UErrorCode &status) const {
    if (U_FAILURE(status)) {
        return appendTo;
    }
    AffixPatternIterator iter;
    affixPattern.iterator(iter);
    UnicodeString literal;
    while (iter.nextToken()) {
        switch (iter.getTokenType()) {
        case AffixPattern::kPercent:
            appendTo.append(fPercent, UNUM_PERCENT_FIELD);
            break;
        case AffixPattern::kPerMill:
            appendTo.append(fPermill, UNUM_PERMILL_FIELD);
            break;
        case AffixPattern::kNegative:
            appendTo.append(fNegative, UNUM_SIGN_FIELD);
            break;
        case AffixPattern::kCurrency:
            switch (iter.getTokenLength()) {
                case 1:
                    appendTo.append(
                            currencyAffixInfo.fSymbol, UNUM_CURRENCY_FIELD);
                    break;
                case 2:
                    appendTo.append(
                            currencyAffixInfo.fISO, UNUM_CURRENCY_FIELD);
                    break;
                case 3:
                    appendTo.append(
                            currencyAffixInfo.fLong, UNUM_CURRENCY_FIELD, status);
                    break;
                default:
                    U_ASSERT(FALSE);
                    break;
            }
            break;
        case AffixPattern::kLiteral:
            appendTo.append(iter.getLiteral(literal));
            break;
        default:
            U_ASSERT(FALSE);
            break;
        }
    }
    return appendTo;
}


U_NAMESPACE_END

