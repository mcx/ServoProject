#include <Arduino.h>
#undef max
#undef min
#include "ArduinoC++BugFixes.h"
#include "ThreadHandler.h"

#include <Eigen30.h>
#include "CurrentControlLoop.h"
#include "EncoderHandler.h"
#include "OpticalEncoderHandler.h"
#include "KalmanFilter.h"

#include "config/config.h"

#ifndef DC_SERVO_H
#define DC_SERVO_H

class ReferenceInterpolator
{
 public:
    ReferenceInterpolator();

    void loadNew(float position, float velocity, float feedForward);

    void updateTiming();

    void resetTiming();

    void getNext(float& position, float& velocity, float& feedForward);

    void setGetTimeInterval(const uint16_t& interval);

    void setLoadTimeInterval(const uint16_t& interval);

    int16_t midPointTimeOffset{0};
 private:
    float pos[3]{0};
    float vel[3]{0};
    float feed[3]{0};

    uint16_t lastUpdateTimingTimestamp{0};
    uint16_t lastGetTimestamp{0};

    bool timingInvalid{true};
    uint16_t loadTimeInterval{12000};
    float invertedLoadInterval{1.0 / 12000};
    uint16_t getTimeInterval{1200};
};

class DCServo
{
 public:
    static DCServo* getInstance();

    bool isEnabled();

    void enable(bool b = true);

    void openLoopMode(bool enable, bool pwmMode = false);

    void onlyUseMainEncoder(bool b = true);

    void setControlSpeed(uint8_t controlSpeed);

    void setBacklashControlSpeed(uint8_t backlashControlSpeed);

    void loadNewReference(float pos, int16_t vel, int16_t feedForwardU = 0);

    void triggerReferenceTiming();

    float getPosition();

    int16_t getVelocity();
    
    int16_t getControlSignal();

    int16_t getCurrent();

    int16_t getPwmControlSignal();

    uint16_t getLoopNumber();

    float getBacklashCompensation();

    float getMainEncoderPosition();

    template <class T>
    T getMainEncoderDiagnosticData();

 private:
    DCServo();

    void controlLoop();

    void identTestLoop();

    bool controlEnabled;
    bool onlyUseMainEncoderControl;
    bool openLoopControlMode;
    bool pwmOpenLoopMode;

    uint8_t controlSpeed{50};
    uint8_t backlashControlSpeed{10};

    //L[0]: Proportional gain of position control loop
    //L[1]: Proportional gain of velocity control loop
    //L[2]: Integral action gain of velocity control loop
    //L[3]: Integral anti windup gain of velocity control loop
    //L[4]: Backlash compensation integral action gain
    Eigen::Matrix<float, 5, 1> L;

    uint16_t loopNumber;
    float rawMainPos;
    float rawOutputPos;
    float outputPosOffset;
    float initialOutputPosOffset;

    //x[0]: Estimated position
    //x[1]: Estimated velocity
    //x[2]: Estimated load disturbance
    Eigen::Vector3f x;

#ifdef SIMULATE
    Eigen::Vector3f xSim;
#endif

    int16_t current;
    int16_t pwmControlSIgnal;
    float controlSignal;
    float uLimitDiff;

    ReferenceInterpolator refInterpolator;

    float Ivel;

    std::unique_ptr<CurrentController> currentController;
    decltype(ConfigHolder::createMainEncoderHandler()) mainEncoderHandler;
    std::unique_ptr<EncoderHandlerInterface> outputEncoderHandler;
    std::unique_ptr<KalmanFilter> kalmanFilter;

    std::vector<Thread*> threads;
};

template <class T>
T DCServo::getMainEncoderDiagnosticData()
{
    T out = {0};
    return out;
}

template <>
OpticalEncoderHandler::DiagnosticData DCServo::getMainEncoderDiagnosticData();

#endif
