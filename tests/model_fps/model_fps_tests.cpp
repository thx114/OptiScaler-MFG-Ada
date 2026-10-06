#include <cassert>
#include <iostream>
#include <cmath>
#include <cstdint>
struct NVSDK_NGX_Parameter { int allocatorTag=1; int value=0; };
#include "../../OptiScaler/inputs/NVNGX_DLSS.h"
#include "../../OptiScaler/framegen/FrameRateWindow.h"
struct Feature {};
void close(double actual,double expected) { if(std::abs(actual-expected)>1e-9) throw "rate mismatch"; }
int main() {
    ContextData<Feature> context;
    NVSDK_NGX_Parameter borrowed;
    int destroyed=0;
    context.createParams=&borrowed;context.ownsCreateParams=false;
    // Borrowed game/Bridge table carries OptiScaler allocator tag, but must
    // survive model re-init and the next evaluate (the actual failure case).
    ReleaseRebuildParameters(&context,[&](auto*){++destroyed;});
    assert(destroyed==0 && context.createParams==nullptr && !context.ownsCreateParams);
    ++borrowed.value;assert(borrowed.value==1);
    context.createParams=new NVSDK_NGX_Parameter;context.ownsCreateParams=true;
    ReleaseRebuildParameters(&context,[&](auto* p){++destroyed;delete p;});
    ReleaseRebuildParameters(&context,[&](auto*){++destroyed;});
    assert(destroyed==1);
    FrameRateWindow meter;
    assert(!meter.Observe(1000,100,9999).valid); // startup counter is discarded
    assert(!meter.Observe(1250,106,30).valid);
    auto r=meter.Observe(1500,112,30);
    assert(r.valid);close(r.source,24);close(r.present,120);
    // A stream reports only 24 presents/s despite a 6x request: no extrapolation.
    meter.Observe(1750,118,6);r=meter.Observe(2000,124,6);
    close(r.source,24);close(r.present,24);
    // Bursty callbacks / duplicate input IDs contribute no additional source frames.
    meter.Observe(2100,124,10);meter.Observe(2200,124,10);
    r=meter.Observe(2500,136,40);close(r.source,24);close(r.present,120);
    // Rebuild resets the epoch; never turn a wrap/reset into thousands of FPS.
    assert(!meter.Observe(2750,1,500).valid);
    meter.Observe(3000,7,30);r=meter.Observe(3250,13,30);
    close(r.source,24);close(r.present,120);
    assert(!meter.Observe(6000,200,500).valid); // stale interval
    assert(!meter.Observe(5999,201,6).valid); // clock regression
    FrameRateWindow zero;zero.Observe(100,1,6);r=zero.Observe(600,1,0);
    assert(r.valid);close(r.source,0);close(r.present,0);
    std::cout<<"PASS: borrowed tagged NGX parameters survive model rebuild; owned cleanup exactly once; measured source/present FPS, bursts, no multiplication, resets and stale windows\n";
}
