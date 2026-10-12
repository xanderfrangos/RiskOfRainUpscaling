using System;
using System.Globalization;

namespace RiskOfRainUpscaling
{
    // Separate ownership from Unity/Harmony so fallback and saved settings can be tested.
    internal sealed class ReflexLimiterController : IDisposable
    {
        private readonly Func<int> readTarget, readLimit;
        private readonly Action<int> writeTarget;
        private readonly Func<int, int> setLimit;
        private readonly Func<bool> reflex, dynamic;
        private readonly Action<string> log;
        private bool conflict, disposed;
        internal bool Owned { get; private set; }
        internal int Requested { get; private set; }
        internal string SavedValue => Requested.ToString(CultureInfo.InvariantCulture);
        internal string Status { get; private set; } = "waiting for Reflex";
        internal static int Microseconds(int fps) => fps <= 0 ? 0 : Math.Max(1, (int)Math.Round(1000000.0 / fps));
        // Dynamic Frame Generation's output target from fps_max: unlimited (0 or -1) follows the refresh rate.
        internal static int DynamicTarget(int fps) => fps <= 0 ? 0 : Math.Min(fps, 1000);

        internal ReflexLimiterController(Func<int> readTarget, Action<int> writeTarget,
            Func<int, int> setLimit, Func<int> readLimit, Action<string> log, Func<bool> reflex = null, Func<bool> dynamic = null)
        {
            this.readTarget=readTarget;this.writeTarget=writeTarget;this.setLimit=setLimit;this.readLimit=readLimit;this.log=log;this.reflex=reflex;this.dynamic=dynamic;
            Requested=readTarget();
        }
        internal void GameSetting(int value) { Requested=value;conflict=false; }
        internal void Tick(bool enabled, bool focused, bool running, int vsync)
        {
            if(disposed)return;
            if(!enabled){conflict=false;Release("disabled");return;}
            if(Owned && readTarget()!=-1){
                Requested=readTarget();conflict=true;Release("another mod changed Unity's limiter");return;
            }
            if(conflict){Status="yielded to another mod";return;}
            if(!focused || !running || vsync!=0){Release(vsync!=0?"VSync enabled":!focused?"unfocused":"paused");return;}
            if(!Owned)Requested=readTarget();
            int limit=Microseconds(Requested);
            // Only disable Unity after the driver accepted this exact cap.
            if(setLimit(limit)!=limit){Release("Reflex unavailable");return;}
            if(!Owned){Owned=true;writeTarget(-1);log("REFLEX_LIMITER_ACQUIRED requested="+Requested+" frameLimitUs="+limit+" Unity.targetFrameRate=-1");}
            // FSR Frame Generation has no Reflex; the native frame-start guard alone holds the cap.
            bool driver=reflex?.Invoke()??true;
            // Dynamic Frame Generation aims its output at the same fps_max; the base cap only bounds it.
            bool targeting=dynamic?.Invoke()??false;
            Status=Requested>0?(driver?"Reflex + guard ":"Frame-start guard ")+Requested+(targeting?" FPS Dynamic target":" base FPS"):
                (driver?"Reflex unlimited":"Unlimited (no Reflex)")+(targeting?"; Dynamic targets refresh rate":"");
        }
        internal void CheckHealth()
        {
            if(Owned && readLimit()!=Microseconds(Requested))Release("Reflex sleep/options failed");
        }
        private void Release(string reason)
        {
            // Clear the driver cap before reinstating the latest requested Unity cap.
            setLimit(0);
            if(Owned){Owned=false;writeTarget(Requested);log("REFLEX_LIMITER_RELEASED reason="+reason+" restored="+Requested);}
            Status="Unity limiter ("+reason+")";
        }
        public void Dispose(){if(disposed)return;Release("mod stopped");disposed=true;}
    }
}
