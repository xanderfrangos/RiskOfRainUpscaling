#include "Pacing/FramePacer.h"
#include <cstdio>

static double Seconds(){
    LARGE_INTEGER ticks{},frequency{};QueryPerformanceCounter(&ticks);QueryPerformanceFrequency(&frequency);
    return static_cast<double>(ticks.QuadPart)/frequency.QuadPart;
}
int main(){
    // No graphics device or SDK initialization: this is the Unity/VSync fallback.
    for(int limit : {33333,25000,16667}){
        if(RiskOfRainUpscaling_FramePacerWait(0)!=0 || RiskOfRainUpscaling_FramePacerWait(limit)!=limit)return 1;
        double start=Seconds();
        for(int frame=0;frame<12;++frame)if(RiskOfRainUpscaling_FramePacerWait(limit)!=limit)return 2;
        double elapsed=Seconds()-start,expected=12*limit/1000000.0;
        if(elapsed<expected*.995 || elapsed>expected*2.5)return 3;
        printf("PASS: Unity/VSync guard limitUs=%d measured=%.2f FPS without Reflex initialization\n",limit,12/elapsed);
    }
    // Changing a one-second cap must discard its deadline immediately.
    if(RiskOfRainUpscaling_FramePacerWait(1000000)!=1000000)return 4;
    double start=Seconds();
    if(RiskOfRainUpscaling_FramePacerWait(16667)!=16667 || Seconds()-start>.25)return 5;
    if(RiskOfRainUpscaling_FramePacerWait(0)!=0)return 6;
    start=Seconds();
    for(int frame=0;frame<1000;++frame)if(RiskOfRainUpscaling_FramePacerWait(0)!=0)return 7;
    if(Seconds()-start>.25)return 8;
    if(RiskOfRainUpscaling_FramePacerWait(-1)!=-1 || RiskOfRainUpscaling_FramePacerWait(1000001)!=-1)return 9;
    // A slow frame must not create a backlog of catch-up waits.
    if(RiskOfRainUpscaling_FramePacerWait(16667)!=16667)return 10;
    Sleep(50);start=Seconds();
    if(RiskOfRainUpscaling_FramePacerWait(16667)!=16667 || Seconds()-start>.015)return 11;
    RiskOfRainUpscaling_FramePacerWait(0);
    puts("PASS: cap changes, unlimited, bounds, and slow-frame recovery.");
    return 0;
}
