using System;
using System.Collections.Generic;
using System.Diagnostics;
using UnityEngine;
using UnityEngine.LowLevel;
using UnityEngine.Rendering;

namespace RiskOfRainUpscaling
{
    // Insert boundaries into Unity's existing loop; preserve every other mod's entries.
    internal sealed class FrameGenerationTiming : IDisposable
    {
        private sealed class SimulationStart { }
        private sealed class SimulationEnd { }
        private sealed class RenderStart { }
        private readonly Func<bool> eligible;
        private readonly ModLog log;
        private bool installed, disposed;
        private readonly ReflexFrameLimiter limiter;
        internal string LimiterStatus => limiter?.Status??"Unity limiter";
        // Whether this frame requested Frame Generation; set at the frame's start, before rendering.
        internal bool Requested { get; private set; }
        private string timingStatus;
        private double lastTime, windowStart, worstMs;
        private int intervals, firstSdkCount;
        private readonly CommandBuffer renderFrame=new CommandBuffer{name="RiskOfRainUpscaling.FrameGeneration.RenderFrame"};
        internal FrameGenerationTiming(Func<bool> eligible, Func<bool> replaceLimiter, ModLog log)
        {
            this.eligible=eligible;this.log=log;
            var loop=PlayerLoop.GetCurrentPlayerLoop();
            bool a=Insert(ref loop,"EarlyUpdate",typeof(SimulationStart),Begin,true);
            bool b=Insert(ref loop,"PreLateUpdate",typeof(SimulationEnd),End,false);
            bool c=Insert(ref loop,"PostLateUpdate",typeof(RenderStart),Render,true);
            if(a && b && c){PlayerLoop.SetPlayerLoop(loop);installed=true;limiter=new ReflexFrameLimiter(replaceLimiter,log);log.Info("FRAME_GEN_TIMING Unity loop: EarlyUpdate start / PreLateUpdate end / PostLateUpdate render start; native render end/Present use the same Unity frame ID.");}
            else {NativeApi.FrameGenerationSetEnabled(0);log.Warning("FRAME_GEN_TIMING_FAILED Unity loop boundaries unavailable; FG suspended.");}
        }
        private void Begin()
        {
            if(disposed)return;
            limiter?.Begin();
            Requested=installed && eligible();
            NativeApi.FrameGenerationSetEnabled(Requested?1:0);
            NativeApi.FrameGenerationMarker(0,Time.frameCount);
            limiter?.AfterSleep();
            RecordPacing();
        }
        private void RecordPacing()
        {
            double now=(double)Stopwatch.GetTimestamp()/Stopwatch.Frequency;
            string status=LimiterStatus;
            if(timingStatus!=status || lastTime==0 || now-lastTime>.5){
                timingStatus=status;windowStart=lastTime=now;intervals=0;worstMs=0;firstSdkCount=NativeApi.FrameGenerationPresentedFrames();return;
            }
            worstMs=Math.Max(worstMs,(now-lastTime)*1000);lastTime=now;++intervals;
            if(now-windowStart<5)return;
            int sdkPresents=NativeApi.FrameGenerationPresentedFrames()-firstSdkCount;
            log.Debug("REFLEX_PACING limiter="+status+" baseFPS="+(intervals/(now-windowStart)).ToString("F2",System.Globalization.CultureInfo.InvariantCulture)+
                " worstFrameMs="+worstMs.ToString("F2",System.Globalization.CultureInfo.InvariantCulture)+" samples="+intervals+
                // Measured presents per rendered frame: the ratio Dynamic Frame Generation chose.
                " sdkPresents="+sdkPresents+" effectiveMultiplier="+((double)sdkPresents/intervals).ToString("F2",System.Globalization.CultureInfo.InvariantCulture)+
                " dynamic="+NativeApi.FrameGenerationDynamic()+" guardWaitsTotal="+NativeApi.ReflexGuardWaits()+" FGstate="+NativeApi.FrameGenerationState()+"; frame-start timing, not physical display timing");
            windowStart=now;intervals=0;worstMs=0;firstSdkCount=NativeApi.FrameGenerationPresentedFrames();
        }
        private void End(){if(!disposed)NativeApi.FrameGenerationMarker(1,Time.frameCount);}
        private void Render(){if(!disposed){
            NativeApi.FrameGenerationMarker(2,Time.frameCount);
            renderFrame.Clear();renderFrame.IssuePluginEventAndData(NativeApi.FrameGenerationGetBeforeHudEvent(),-Time.frameCount,IntPtr.Zero);Graphics.ExecuteCommandBuffer(renderFrame);
        }}
        private static bool Insert(ref PlayerLoopSystem system,string parent,Type type,PlayerLoopSystem.UpdateFunction update,bool first)
        {
            if(system.type?.Name==parent){
                var list=new List<PlayerLoopSystem>(system.subSystemList??Array.Empty<PlayerLoopSystem>());
                list.RemoveAll(s=>s.type==type);
                var entry=new PlayerLoopSystem{type=type,updateDelegate=update};
                if(first)list.Insert(0,entry);else list.Add(entry);
                system.subSystemList=list.ToArray();return true;
            }
            if(system.subSystemList==null)return false;
            for(int i=0;i<system.subSystemList.Length;++i)if(Insert(ref system.subSystemList[i],parent,type,update,first))return true;
            return false;
        }
        private static void Remove(ref PlayerLoopSystem system)
        {
            if(system.subSystemList==null)return;
            var list=new List<PlayerLoopSystem>(system.subSystemList);
            list.RemoveAll(s=>s.type==typeof(SimulationStart)||s.type==typeof(SimulationEnd)||s.type==typeof(RenderStart));
            system.subSystemList=list.ToArray();
            for(int i=0;i<system.subSystemList.Length;++i)Remove(ref system.subSystemList[i]);
        }
        public void Dispose()
        {
            if(disposed)return;disposed=true;Requested=false;limiter?.Dispose();NativeApi.FrameGenerationSetEnabled(0);
            renderFrame.Release();
            if(!installed)return;
            var loop=PlayerLoop.GetCurrentPlayerLoop();Remove(ref loop);PlayerLoop.SetPlayerLoop(loop);installed=false;
            log.Info("FRAME_GEN_TIMING_RETIRED Unity loop entries removed.");
        }
    }
}
