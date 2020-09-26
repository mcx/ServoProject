#ifndef OPTICAL_ENCODER_HANDLER_H
#define OPTICAL_ENCODER_HANDLER_H

#include "AdcHandler.h"
#include "EncoderHandler.h"
#include <array>

class OpticalEncoderHandler : public EncoderHandlerInterface
{
  public:
    static constexpr int vecSize = 512;
    OpticalEncoderHandler(const std::array<uint16_t, vecSize>& aVec, const std::array<uint16_t, vecSize>& bVec,
            int16_t sensor1Pin, int16_t sensor2Pin, float unitsPerRev);

    ~OpticalEncoderHandler();

    virtual void init() override;

    virtual void triggerSample() override;

    virtual float getValue() override;

    virtual DiagnosticData getDiagnosticData();

  private:
    void updatePosition();
    uint32_t calcCost(int& i, uint16_t a, uint16_t b);

    DiagnosticData diagnosticData;

    uint16_t lastMinCostIndex{0};
    uint16_t predictNextPos{0};

    std::array<uint16_t, vecSize> aVec;
    std::array<uint16_t, vecSize> bVec;

    AnalogSampler sensor1;
    AnalogSampler sensor2;
    uint16_t sensor1Value{0};
    uint16_t sensor2Value{0};
    float value{0.0};
    float wrapAroundCorretion{0.0};
    bool newData{false};

    const float scaling;
};

#endif
