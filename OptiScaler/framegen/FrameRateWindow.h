#pragma once
#include <cstdint>

// Count over elapsed wall time, not callback spacing or requested FG factor.
// Provider presents can include repeated/generated pictures; never call this
// a measurement of unique images or reconstruct source FPS by division.
class FrameRateWindow
{
public:
    struct Rates { double source=0, present=0; bool valid=false; uint64_t updatedMs=0; };
private:
    uint64_t beginMs=0,beginSource=0,lastMs=0,lastSource=0,presents=0;
    bool started=false;
    Rates latest{};
public:
    Rates Observe(uint64_t nowMs,uint64_t sourceCounter,uint32_t reportedPresents)
    {
        if (!started || nowMs<lastMs || sourceCounter<lastSource || nowMs-lastMs>2000) {
            started=true;beginMs=lastMs=nowMs;beginSource=lastSource=sourceCounter;
            presents=0;latest={};return latest;
        }
        presents+=reportedPresents;
        lastMs=nowMs;lastSource=sourceCounter;
        const uint64_t elapsed=nowMs-beginMs;
        if (elapsed>=500) {
            latest={1000.0*double(sourceCounter-beginSource)/double(elapsed),
                    1000.0*double(presents)/double(elapsed),true,nowMs};
            beginMs=nowMs;beginSource=sourceCounter;presents=0;
        }
        return latest;
    }
};
