// © 2017 and later: Unicode, Inc. and others.
// License & terms of use: http://www.unicode.org/copyright.html

#ifndef NUMBERFORMAT_NUMFMTTER_SCIENTIFIC_H
#define NUMBERFORMAT_NUMFMTTER_SCIENTIFIC_H

#include "number_types.h"

U_NAMESPACE_BEGIN namespace number {
namespace impl {

// Forward-declare
class ScientificHandler;

class ScientificModifier : public UMemory, public Modifier {
  public:
    ScientificModifier();

    void set(int32_t exponent, const ScientificHandler *handler);

    int32_t apply(NumberStringBuilder &output, int32_t leftIndex, int32_t rightIndex,
                  UErrorCode &status) const override;

    int32_t getPrefixLength(UErrorCode &status) const override;

    int32_t getCodePointCount(UErrorCode &status) const override;

    bool isStrong() const override;

  private:
    int32_t fExponent;
    const ScientificHandler *fHandler;
};

class ScientificHandler : public UMemory, public MicroPropsGenerator, public MultiplierProducer {
  public:
    ScientificHandler(const Notation *notation, const DecimalFormatSymbols *symbols,
                      const MicroPropsGenerator *parent);

    void
    processQuantity(DecimalQuantity &quantity, MicroProps &micros, UErrorCode &status) const override;

    int32_t getMultiplier(int32_t magnitude) const override;

  private:
    const Notation::ScientificSettings& fSettings;
    const DecimalFormatSymbols *fSymbols;
    const MicroPropsGenerator *fParent;

    friend class ScientificModifier;
};

} // namespace impl
} // namespace number
U_NAMESPACE_END

#endif //NUMBERFORMAT_NUMFMTTER_SCIENTIFIC_H