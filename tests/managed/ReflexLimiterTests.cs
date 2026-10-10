using System;
using System.Collections.Generic;
using RiskOfRainUpscaling;

internal static class ReflexLimiterTests
{
    internal static void Run()
    {
        int target=30, native=-1;
        bool failSet=false;
        var events=new List<string>();
        var c=new ReflexLimiterController(()=>target,v=>{events.Add("unity:"+v);target=v;},
            v=>{events.Add("native:"+v);if(native<0 || failSet)return -1;return native=v;},()=>native,_=>{});
        void Check(bool condition,string message){if(!condition)throw new Exception("Reflex limiter: "+message);}
        void Tick(bool enabled=true,bool focus=true,bool running=true,int vsync=0)=>c.Tick(enabled,focus,running,vsync);
        Check(ReflexLimiterController.Microseconds(30)==33333 && ReflexLimiterController.Microseconds(60)==16667 &&
            ReflexLimiterController.Microseconds(144)==6944 && ReflexLimiterController.Microseconds(-1)==0 &&
            ReflexLimiterController.Microseconds(0)==0 && ReflexLimiterController.Microseconds(1)==1000000 &&
            ReflexLimiterController.Microseconds(int.MaxValue)==1,"cap conversion/bounds");
        Tick();Check(!c.Owned && target==30,"startup fallback");
        native=0;events.Clear();Tick();
        Check(c.Owned && target==-1 && native==33333 && c.SavedValue=="30","acquire/settings preservation");
        Check(c.Status=="Reflex + guard 30 base FPS","Reflex status");
        Check(events[0]=="native:33333" && events[1]=="unity:-1","disable Unity only after native acceptance");
        c.GameSetting(60);Tick();c.CheckHealth();
        Check(target==-1 && native==16667 && c.SavedValue=="60","live cap change");
        c.GameSetting(-1);Tick();c.CheckHealth();Check(c.Owned && native==0 && c.SavedValue=="-1","unlimited");
        c.GameSetting(0);Tick();Check(c.Owned && native==0 && c.SavedValue=="0","preserve Unity default cap");
        c.GameSetting(60);Tick();events.Clear();Tick(vsync:1);
        Check(!c.Owned && target==60 && native==0,"VSync release");
        Check(events[0]=="native:0" && events[1]=="unity:60","clear native before Unity restore");
        Tick();Tick(focus:false);Check(!c.Owned && target==60 && native==0,"focus release");
        target=90;Tick();Check(c.Owned && c.Requested==90 && native==11111,"adopt settings changed while inactive");
        Tick(running:false);Check(!c.Owned && target==90 && native==0,"pause release");
        Tick();target=45;Tick();Check(!c.Owned && target==45 && native==0,"foreign mod ownership");
        Tick();Check(!c.Owned && target==45,"do not fight another mod");
        c.GameSetting(45);Tick();Check(c.Owned && native==22222,"explicit setting resumes replacement");
        c.GameSetting(75);failSet=true;Tick();Check(!c.Owned && target==75,"options failure restores latest request");
        failSet=false;native=0;Tick();native=-1;c.CheckHealth();Check(!c.Owned && target==75,"sleep/runtime failure restores same frame");
        native=0;Tick();c.GameSetting(120);Tick(enabled:false);
        Check(!c.Owned && native==0 && target==120,"config disable restores latest request");
        Tick();c.GameSetting(144);c.Dispose();Check(!c.Owned && native==0 && target==144,"dispose restores saved request");
        Tick();Check(target==144 && native==0,"disposed controller cannot reacquire");
        c.Dispose();
        // FSR Frame Generation accepts the cap through the frame-start guard alone.
        bool reflex=false;target=60;native=0;
        var f=new ReflexLimiterController(()=>target,v=>target=v,v=>native=v,()=>native,_=>{},()=>reflex);
        f.Tick(true,true,true,0);Check(f.Owned && native==16667 && f.Status=="Frame-start guard 60 base FPS","FSR guard status");
        f.GameSetting(-1);f.Tick(true,true,true,0);Check(f.Status=="Unlimited (no Reflex)","FSR unlimited status");
        reflex=true;f.Tick(true,true,true,0);Check(f.Status=="Reflex unlimited","Reflex unlimited status");
        f.Dispose();
        Console.WriteLine("PASS: Reflex/FSR guard status, cap conversion, native-first acquisition, saved/live settings, unlimited, VSync/focus/pause, foreign-mod ownership, options/sleep failure, disable and disposal.");
    }
}
