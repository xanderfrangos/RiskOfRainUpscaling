#pragma once
#include <Windows.h>
#include <algorithm>
#include <stdexcept>

// Frame-start interval guard, independent of Reflex or graphics initialization.
class FramePacer {
    HANDLE timer{};
    LONGLONG lastTick{},frequency{};
    int previousLimit{};
public:
    FramePacer()=default;
    FramePacer(const FramePacer&)=delete;
    FramePacer& operator=(const FramePacer&)=delete;
    ~FramePacer(){if(timer)CloseHandle(timer);}
    void Reset(){lastTick=0;previousLimit=0;}
    bool Wait(int limit){
        if(limit<=0){Reset();return false;}
        if(previousLimit!=limit){lastTick=0;previousLimit=limit;}
        LARGE_INTEGER now{},rate{};QueryPerformanceCounter(&now);
        if(!frequency){QueryPerformanceFrequency(&rate);frequency=rate.QuadPart;}
        if(!lastTick){lastTick=now.QuadPart;return false;}
        LONGLONG deadline=lastTick+(frequency*limit+999999)/1000000;
        bool waited=now.QuadPart<deadline;
        if(waited){
            if(!timer){
                timer=CreateWaitableTimerExW(nullptr,nullptr,CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,TIMER_ALL_ACCESS);
                if(!timer)timer=CreateWaitableTimerW(nullptr,FALSE,nullptr);
                if(!timer)throw std::runtime_error("Frame cap timer creation failed");
            }
            do{
                LONGLONG remaining=deadline-now.QuadPart;
                LARGE_INTEGER due{};due.QuadPart=-std::max<LONGLONG>(1,(remaining*10000000+frequency-1)/frequency);
                DWORD timeout=static_cast<DWORD>((remaining*1000+frequency-1)/frequency+100);
                if(!SetWaitableTimer(timer,&due,0,nullptr,nullptr,FALSE) || WaitForSingleObject(timer,timeout)!=WAIT_OBJECT_0)
                    throw std::runtime_error("Frame cap timer wait failed");
                QueryPerformanceCounter(&now);
            }while(now.QuadPart<deadline);
        }
        // Late frames start a fresh interval, without catch-up bursts.
        lastTick=now.QuadPart;return waited;
    }
};

// Main Unity frame-start caller. Zero clears the guard; -1 indicates failure.
extern "C" __declspec(dllexport) int __cdecl RiskOfRainUpscaling_FramePacerWait(int microseconds);
