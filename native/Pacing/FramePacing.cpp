#include "Pacing/FramePacer.h"
#include "Core/Native.h"

extern "C" int __cdecl RiskOfRainUpscaling_FramePacerWait(int limit){
    static FramePacer pacer;
    if(limit<0 || limit>1000000)return -1;
    try{pacer.Wait(limit);return limit;}
    catch(const std::exception& error){
        pacer.Reset();Log(ModLogLevel::Error, "VSYNC_FPS_GUARD_FAILED %s; Unity limiter retained",error.what());return -1;
    }
}
