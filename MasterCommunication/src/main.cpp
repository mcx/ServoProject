#include "MasterCommunication.h"
#include "DCServoCommunicator.h"

#include "DummyTrajectoryGenerator.h"
#include "PathAndMoveBuilder.h"

#include <numeric>
#include <cmath>
#define pi (M_PI)

#include <chrono>
#include <thread>
#include <algorithm>
#include <functional>
#include <mutex>

template <class T>
T interpolate(const T& a, const T& b, float t)
{
    return a * (1 - t) + b * t;
}

template <class T>
class SamplingHandler
{
public:
    SamplingHandler(std::function<T()> inInc, double inputDt) :
            inputIncrementer(std::move(inInc)),
            inputDt(inputDt)
    {
        n = inputIncrementer();
        np1 = inputIncrementer();
    }

    void increment(float dt)
    {
        interpolT += dt;

        while (interpolT > inputDt)
        {
            interpolT -= inputDt;

            n = np1;
            np1 = inputIncrementer();
        }
    }

    T getSample()
    {
        return interpolate(n, np1, interpolT);
    }

private:
    std::function<T()> inputIncrementer;
    double inputDt;
    double interpolT{0};
    T n{};
    T np1{};
};

class Robot
{
public:
    static const size_t dof = 6;

    Robot(Communication* communication) :
            dcServoArray{{{1, communication}, {2, communication},
                {3, communication}, {4, communication}, {5, communication}, {6, communication}}}
    {
        while (std::any_of(std::begin(dcServoArray), std::end(dcServoArray), [](auto& d)
                {
                    return !d.isInitComplete();
                }))
        {
            std::for_each(std::begin(dcServoArray), std::end(dcServoArray), [](auto& d)
                {
                    d.run();
                });
        }

        dcServoArray[0].setOffsetAndScaling(2 * pi / 4096.0, 302.75 / 4096.0 * 2 * pi, 0);
        dcServoArray[1].setOffsetAndScaling(2 * pi / 4096.0, (733.75 - 2048) / 4096.0 * 2 * pi, pi / 2);
        dcServoArray[2].setOffsetAndScaling(2 * pi / 4096.0, (656.25) / 4096.0 * 2 * pi, pi / 2);
        dcServoArray[3].setOffsetAndScaling(1.0 * pi / 2000, -(4.0 / 25.0), 0);
        dcServoArray[4].setOffsetAndScaling(-1.00 * pi / 2000, (2.0 / 25.0), 0);
        dcServoArray[5].setOffsetAndScaling(1.00 * pi / 2000, -(1.0 / 25.0), 0);

        std::transform(std::begin(dcServoArray), std::end(dcServoArray), std::begin(currentPosition), [](auto& d)
            {
                return d.getPosition();
            });

        t = std::thread{&Robot::run, this};
    }

    virtual ~Robot()
    {
        shutdown();
    }

    void run()
    {
        using namespace std::chrono;
        double cycleTime = 0.012;

        high_resolution_clock::time_point sleepUntilTimePoint = high_resolution_clock::now();
        high_resolution_clock::duration clockDurationCycleTime(
                duration_cast<high_resolution_clock::duration>(duration<double>(cycleTime)));

        while(!shuttingDown)
        {
            std::this_thread::sleep_until(sleepUntilTimePoint);
            sleepUntilTimePoint += clockDurationCycleTime;;

            std::function<void(double, Robot*)> tempSendHandlerFunction;
            std::function<void(double, Robot*)> tempReadHandlerFunction;
            {
                const std::lock_guard<std::mutex> lock(handlerFunctionMutex);
                tempSendHandlerFunction = sendCommandHandlerFunction;
                tempReadHandlerFunction = readResultHandlerFunction;
            }

            tempSendHandlerFunction(cycleTime, this);

            std::for_each(std::begin(dcServoArray), std::end(dcServoArray), [](auto& d)
                {
                    d.run();
                });

            std::transform(std::begin(dcServoArray), std::end(dcServoArray), std::begin(currentPosition), [](auto& d)
                {
                    return d.getPosition();
                });
 
            tempReadHandlerFunction(cycleTime, this);
        }
    }

    Eigen::Matrix<double, dof, 1> getPosition()
    {
        return currentPosition;
    }

    void setHandlerFunctions(const std::function<void(double, Robot*)>& newSendCommandHandlerFunction,
            const std::function<void(double, Robot*)>& newReadResultHandlerFunction)
    {
        const std::lock_guard<std::mutex> lock(handlerFunctionMutex);

        sendCommandHandlerFunction = newSendCommandHandlerFunction;
        readResultHandlerFunction = newReadResultHandlerFunction;
    }

    void removeHandlerFunctions()
    {
        setHandlerFunctions([](double cycleTime, Robot* robot){}, [](double cycleTime, Robot* robot){});
    }

    void shutdown()
    {
        if (!shuttingDown)
        {
            shuttingDown = true;
            t.join();
        }
    }
    std::array<DCServoCommunicator, dof> dcServoArray;

private:
    Eigen::Matrix<double, dof, 1> currentPosition{Eigen::Matrix<double, dof, 1>::Zero()};

    bool shuttingDown{false};

    std::thread t{};

    std::mutex handlerFunctionMutex{};
    std::function<void(double, Robot*)> sendCommandHandlerFunction{[](double cycleTime, Robot* robot){}};
    std::function<void(double, Robot*)> readResultHandlerFunction{[](double cycleTime, Robot* robot){}};
};

void playPath(Robot& robot, 
        PathAndMoveBuilder& pathBuilder
        const double playbackSpeed = 1.0,
        const std::array<bool, 6> activeMove = {true, true, true, true, true, true})
{
    if (playbackSpeed > 1.0)
    {
        throw -1;
    }

    double dt{0.001};

    RobotParameters::DynamicRobotDynamics dynamics{dt};

    DummyTrajectoryGenerator trajGen{&dynamics, 0};

    auto startPos = robot.getPosition();
    trajGen.setStart(startPos, 0.1);
    pathBuilder.renderTo(trajGen, startPos);

    trajGen.calculateTrajectory();

    bool doneRunning = false;
    bool reachedEndOfTrajectory = false;

    auto iter = std::cbegin(trajGen);
    auto endIter = std::cend(trajGen);
    auto outK = *iter;
    ++iter;
    auto outKp1 = *iter;
    auto pwm = outK.u;
    double playbackSpeedT = 0;
    SamplingHandler<TrajectoryItem<6, double> > sampler([&]()
            {
                auto outJ = interpolate(outK, outKp1, playbackSpeedT);
                playbackSpeedT += playbackSpeed;
                if (playbackSpeedT >= 1.0)
                {
                    playbackSpeedT -= 1.0;
                    outK = outKp1;
                    if (iter == endIter)
                    {
                        reachedEndOfTrajectory = true;
                    }
                    ++iter;
                    outKp1 = *iter;
                }
                auto outJp1 = interpolate(outK, outKp1, playbackSpeedT);
                outJ.v *= playbackSpeed;
                outJp1.v *= playbackSpeed;
                dynamics.recalculateFreedForward(outJ, outJp1);
                pwm = dynamics.recalcPwm(outJ.u, outJ.v);
                return outJ;
            }, dt);

    auto sendCommandHandlerFunction = [&sampler, &activeMove](double dt, Robot* robot)
        {
            auto& servos = robot->dcServoArray;

            auto trajItem = sampler.getSample();
            sampler.increment(dt);

            for (size_t i = 0; i != servos.size(); ++i)
            {
                if (activeMove[i])
                {
                    servos[i].setReference(trajItem.p[i], trajItem.v[i], trajItem.u[i]);
                }
            }
        };

    double t = 0;
    EigenVectord6 lastTempC;
    auto readResultHandlerFunction = [&t, &lastTempC, &doneRunning, &reachedEndOfTrajectory, &pwm](double dt, Robot* robot)
        {
            auto& servos = robot->dcServoArray;

            bool communicationError = std::any_of(std::begin(servos), std::end(servos), [](auto& d)
                    {
                        return !d.isCommunicationOk();
                    });

            EigenVectord6 posJ;
            std::transform(std::cbegin(servos), std::cend(servos), std::begin(posJ),
                    [](const auto& c){return c.getPosition();});

            EigenVectord6 velJ;
            std::transform(std::cbegin(servos), std::cend(servos), std::begin(velJ),
                    [](const auto& c){return c.getVelocity();});

            EigenVectord6 erroJ;
            std::transform(std::cbegin(servos), std::cend(servos), std::begin(erroJ),
                    [](const auto& c){return c.getControlError();});

            JointSpaceCoordinate tempJ{posJ};
            CartesianCoordinate tempC{tempJ};

            if (t == 0)
            {
                lastTempC = tempC.c;
            }
            EigenVectord6 tempCVel = (tempC.c - lastTempC) / dt;
            lastTempC = tempC.c;

            const EigenVectord6& viewPos = tempC.c;
            const EigenVectord6& viewVel = tempCVel;

            std::string printVecName;
            auto printVecFunc = [&printVecName](int i, const auto& v)
                    {
                        std::cout << " " << printVecName << i << ":" << v;
                        return ++i;
                    };

            std::cout << "t:" << t;
            printVecName = "p";
            std::accumulate(std::cbegin(viewPos), std::cend(viewPos), 0, printVecFunc);
            printVecName = "pj";
            std::accumulate(std::cbegin(posJ), std::cend(posJ), 0, printVecFunc);
            printVecName = "v";
            std::accumulate(std::cbegin(viewVel), std::cend(viewVel), 0, printVecFunc);
            printVecName = "vj";
            std::accumulate(std::cbegin(velJ), std::cend(velJ), 0, printVecFunc);
            printVecName = "e";
            std::accumulate(std::cbegin(erroJ), std::cend(erroJ), 0, printVecFunc);
            printVecName = "u";
            std::accumulate(std::cbegin(pwm), std::cend(pwm), 0, printVecFunc);
            std::cout << "\n";

            t += dt;

            if (reachedEndOfTrajectory || communicationError)
            {
                robot->removeHandlerFunctions();
                doneRunning = true;
            }
        };

    robot.setHandlerFunctions(sendCommandHandlerFunction, readResultHandlerFunction);

    while(!doneRunning)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

void recordeOpticalEncoderData(Robot& robot, size_t i, float pwm, float time)
{
    bool doneRunning = false;

    auto sendCommandHandlerFunction = [&i, &pwm](double dt, Robot* robot)
        {
            auto& servos = robot->dcServoArray;

            if (pwm != 0.0)
            {
                servos[i].setOpenLoopControlSignal(pwm, true);
            }
        };

    double t = 0;
    auto readResultHandlerFunction = [&t, &doneRunning, &i, time](double dt, Robot* robot)
        {
            t += dt;
            auto& servos = robot->dcServoArray;
            auto opticalEncoderData = servos[i].getOpticalEncoderChannelData();
            std::cout << opticalEncoderData.a << ", " 
                      << opticalEncoderData.b << ", "
                      << opticalEncoderData.minCostIndex << ", "
                      << opticalEncoderData.minCost << "\n";

            if (t >= time)
            {
                robot->removeHandlerFunctions();
                doneRunning = true;
            }
        };

    robot.setHandlerFunctions(sendCommandHandlerFunction, readResultHandlerFunction);

    while(!doneRunning)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

void recordeMomentOfInertia(Robot& robot, size_t i, float amp, float freq)
{
    std::for_each(std::begin(robot.dcServoArray), std::end(robot.dcServoArray), [](auto& d)
        {
            d.disableBacklashControl();
        });

    bool doneRunning = false;

    double t = 0;
    float pos = 0;
    float vel = 0;
    float acc = 0;

    auto startPos = robot.getPosition();

    auto sendCommandHandlerFunction = [&](double dt, Robot* robotPointer)
        {
            t += dt;

            const double freqScaling = freq * (2.0 * pi);
            pos = startPos[i] + amp * (1 - cos(t * freqScaling));
            vel = amp * sin(t * freqScaling) * freqScaling;
            acc = amp * cos(t * freqScaling) * freqScaling * freqScaling;

            auto& servos = robotPointer->dcServoArray;

            for (size_t j = 0; j != servos.size(); ++j)
            {
                if (i == j)
                {
                    servos[j].setReference(pos, vel, 0);
                }
                else
                {
                    servos[j].setReference(startPos[j], 0, 0);
                }
            }
        };

    const double runTime = std::ceil(15 * freq) / freq;

    auto readResultHandlerFunction = [&](double dt, Robot* robotPointer)
        {
            auto& servo = robotPointer->dcServoArray[i];

            std::cout << "t: " << t << ", "
                        << "p: " << servo.getPosition() << ", "
                        << "v: " << servo.getVelocity() << ", "
                        << "u: " << servo.getControlSignal() << ", "
                        << "acc: " << acc << "\n";

            if (t >= runTime)
            {
                robotPointer->removeHandlerFunctions();
                doneRunning = true;
            }
        };

    robot.setHandlerFunctions(sendCommandHandlerFunction, readResultHandlerFunction);

    while(!doneRunning)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

void recordeCurrentAndPwmBehaviour(Robot& robot, size_t i, double pwm)
{
    std::for_each(std::begin(robot.dcServoArray), std::end(robot.dcServoArray), [](auto& d)
        {
            d.disableBacklashControl();
        });

    bool doneRunning = false;

    auto pwmTestVec = std::vector<double>{pwm/4.0, 0, -pwm/4.0, 0,
                                        2.0 * pwm/4.0, 0, -2.0 * pwm/4.0, 0,
                                        3.0 * pwm/4.0, 0, -3.0 * pwm/4.0, 0,
                                        4.0 * pwm/4.0, 0, -4.0 * pwm/4.0, 0};

    double t = 0;
    auto sendCommandHandlerFunction = [&](double dt, Robot* robotPointer)
        {
            auto& servo = robotPointer->dcServoArray[i];

            double p = 0;
            if (t < pwmTestVec.size())
            {
                p = pwmTestVec.at(static_cast<size_t>(t));
            }
            servo.setOpenLoopControlSignal(p, true);
        };

    double runTime = pwmTestVec.size();
    auto readResultHandlerFunction = [&](double dt, Robot* robotPointer)
        {
            t += dt;
            auto& servo = robotPointer->dcServoArray[i];

            std::cout << "t: " << t << ", "
                        << "p: " << servo.getPosition() << ", "
                        << "v: " << servo.getVelocity() << ", "
                        << "c: " << servo.getCurrent() << "\n";

            if (t >= runTime)
            {
                robotPointer->removeHandlerFunctions();
                doneRunning = true;
            }
        };

    robot.setHandlerFunctions(sendCommandHandlerFunction, readResultHandlerFunction);

    while(!doneRunning)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

PathAndMoveBuilder createPath()
{
    PathAndMoveBuilder pathBuilder;

    VelocityLimiter jointVelocityLimiter(3.0);
    VelocityLimiter cartesianVelocityLimiter(0.1, EigenVectord6{1, 1, 1, 0, 0, 0});
    cartesianVelocityLimiter.add(0.4, EigenVectord6{0,0,0,1,1,1});
    std::shared_ptr<JointSpaceDeviationLimiter> noDeviationLimiterJoint = 
            std::make_shared<JointSpaceDeviationLimiter>(std::numeric_limits<double>::max());
    std::shared_ptr<JointSpaceDeviationLimiter> deviationLimiterJoint =
            std::make_shared<JointSpaceDeviationLimiter>(0.0001);
    std::shared_ptr<CartesianSpaceDeviationLimiter> deviationLimiterCartesian =
            std::make_shared<CartesianSpaceDeviationLimiter>(0.0001);
    deviationLimiterCartesian->add(0.01, EigenVectord6{0.0, 0.0, 0.0, 1.0, 1.0, 1.0});

    EigenVectord6 xV{0.1, 0.0, 0.0, 0, 0, 0};
    EigenVectord6 yV{0.0, 0.1, 0.0, 0, 0, 0};
    EigenVectord6 zV{0.0, 0.0, 0.1, 0, 0, 0};
    EigenVectord6 raV{0, 0, 0, 0.9, 0.0, 0.0};
    EigenVectord6 rbV{0, 0, 0, 0.0, 1.0, 0.0};

    JointSpaceCoordinate jointSpaceHome{{0.0, pi / 2, pi / 2, 0, 0, 0}};
    CartesianCoordinate tempCartCoord(jointSpaceHome);

    EigenVectord6 crossCenter{0.0, 0.1, -0.1, 0, 0, 0};
    crossCenter += tempCartCoord.c;

    CartesianCoordinate tempCartCoord2(crossCenter);
    JointSpaceCoordinate crossCenterJointSpace{tempCartCoord2};

    pathBuilder.append(JointSpaceLinearPath::create(crossCenterJointSpace,
        jointVelocityLimiter, jointVelocityLimiter, deviationLimiterJoint));

    if (true)
    {
        pathBuilder.append(CartesianSpaceLinearPath::create(
                CartesianCoordinate{crossCenter + xV},
                cartesianVelocityLimiter, cartesianVelocityLimiter, deviationLimiterCartesian));
        pathBuilder.append(CartesianSpaceLinearPath::create(
                CartesianCoordinate{crossCenter + xV + 0.05 * zV},
                cartesianVelocityLimiter, cartesianVelocityLimiter, deviationLimiterCartesian));
        pathBuilder.append(CartesianSpaceLinearPath::create(
                CartesianCoordinate{crossCenter - xV + 0.05 * zV},
                cartesianVelocityLimiter, cartesianVelocityLimiter, deviationLimiterCartesian));
        pathBuilder.append(CartesianSpaceLinearPath::create(
                CartesianCoordinate{crossCenter - xV},
                cartesianVelocityLimiter, cartesianVelocityLimiter, deviationLimiterCartesian));
        pathBuilder.append(CartesianSpaceLinearPath::create(
                CartesianCoordinate{crossCenter},
                cartesianVelocityLimiter, cartesianVelocityLimiter, deviationLimiterCartesian));

        pathBuilder.append(CartesianSpaceLinearPath::create(
                CartesianCoordinate{crossCenter + yV},
                cartesianVelocityLimiter, cartesianVelocityLimiter, deviationLimiterCartesian));
        pathBuilder.append(CartesianSpaceLinearPath::create(
                CartesianCoordinate{crossCenter + yV + 0.05 * zV},
                cartesianVelocityLimiter, cartesianVelocityLimiter, deviationLimiterCartesian));
        pathBuilder.append(CartesianSpaceLinearPath::create(
                CartesianCoordinate{crossCenter - yV + 0.05 * zV},
                cartesianVelocityLimiter, cartesianVelocityLimiter, deviationLimiterCartesian));
        pathBuilder.append(CartesianSpaceLinearPath::create(
                CartesianCoordinate{crossCenter - yV},
                cartesianVelocityLimiter, cartesianVelocityLimiter, deviationLimiterCartesian));
        pathBuilder.append(CartesianSpaceLinearPath::create(
                CartesianCoordinate{crossCenter},
                cartesianVelocityLimiter, cartesianVelocityLimiter, deviationLimiterCartesian));

        pathBuilder.append(CartesianSpaceLinearPath::create(
                CartesianCoordinate{crossCenter + zV},
                cartesianVelocityLimiter, cartesianVelocityLimiter, deviationLimiterCartesian));
        pathBuilder.append(CartesianSpaceLinearPath::create(
                CartesianCoordinate{crossCenter + zV + 0.05 * yV},
                cartesianVelocityLimiter, cartesianVelocityLimiter, deviationLimiterCartesian));
        pathBuilder.append(CartesianSpaceLinearPath::create(
                CartesianCoordinate{crossCenter - zV + 0.05 * yV},
                cartesianVelocityLimiter, cartesianVelocityLimiter, deviationLimiterCartesian));
        pathBuilder.append(CartesianSpaceLinearPath::create(
                CartesianCoordinate{crossCenter - zV},
                cartesianVelocityLimiter, cartesianVelocityLimiter, deviationLimiterCartesian));
        pathBuilder.append(CartesianSpaceLinearPath::create(
                CartesianCoordinate{crossCenter},
                cartesianVelocityLimiter, cartesianVelocityLimiter, deviationLimiterCartesian));

        pathBuilder.append(CartesianSpaceLinearPath::create(
                CartesianCoordinate{crossCenter + raV},
                cartesianVelocityLimiter, cartesianVelocityLimiter, deviationLimiterCartesian));
        pathBuilder.append(CartesianSpaceLinearPath::create(
                CartesianCoordinate{crossCenter - raV},
                cartesianVelocityLimiter, cartesianVelocityLimiter, deviationLimiterCartesian));
        pathBuilder.append(CartesianSpaceLinearPath::create(
                CartesianCoordinate{crossCenter},
                cartesianVelocityLimiter, cartesianVelocityLimiter, deviationLimiterCartesian));

        pathBuilder.append(CartesianSpaceLinearPath::create(
                CartesianCoordinate{crossCenter + rbV},
                cartesianVelocityLimiter, cartesianVelocityLimiter, deviationLimiterCartesian));
        pathBuilder.append(CartesianSpaceLinearPath::create(
                CartesianCoordinate{crossCenter - rbV},
                cartesianVelocityLimiter, cartesianVelocityLimiter, deviationLimiterCartesian));
        pathBuilder.append(CartesianSpaceLinearPath::create(
                CartesianCoordinate{crossCenter},
                cartesianVelocityLimiter, cartesianVelocityLimiter, deviationLimiterCartesian));

        pathBuilder.append(JointSpaceLinearPath::create(jointSpaceHome,
            jointVelocityLimiter, jointVelocityLimiter, deviationLimiterJoint));
    }

    pathBuilder.append(JointSpaceLinearPath::create({-0.5, 0.9, 0.9, 1.4, 0, 0},
            jointVelocityLimiter, VelocityLimiter{0.1}, deviationLimiterJoint));

    pathBuilder.append(JointSpaceLinearPath::create({0.5, pi / 2 + 0, pi / 2 + 0, 0, 1.4, 0},
            jointVelocityLimiter, jointVelocityLimiter, deviationLimiterJoint));

    pathBuilder.append(JointSpaceLinearPath::create({-0.5, 0.9, 0.9, 0, 0, 1.4},
            jointVelocityLimiter, jointVelocityLimiter, deviationLimiterJoint));

    pathBuilder.append(JointSpaceLinearPath::create({0, pi / 2 + 0, pi / 2 + 0, 0, 0, 0},
            jointVelocityLimiter, VelocityLimiter{0.1}, deviationLimiterJoint));

    return pathBuilder;
}

int main()
{
    //SerialCommunication communication{"/dev/ttyACM0"};
    SimulateCommunication communication;

    Robot robot(&communication);
    
    if (false)
    {
        recordeOpticalEncoderData(robot, 2, 13.0, 100);
    }
    else if (false)
    {
        recordeMomentOfInertia(robot, 2, 0.05, 4);
    }
    else if (false)
    {
        recordeCurrentAndPwmBehaviour(robot, 2, 200);
    }
    else if (true)
    {
        try
        {
            PathAndMoveBuilder pathBuilder{createPath()};
            playPath(robot, pathBuilder, 1.0, {true, true, true, true, true, true});
        }
        catch (std::exception& e)
        {
            std::cout << e.what(); 
        }
    }

    robot.shutdown();

    return 0;
}