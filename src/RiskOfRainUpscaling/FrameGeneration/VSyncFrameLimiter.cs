using System;

namespace RiskOfRainUpscaling
{
    // Unity ignores targetFrameRate with VSync on. Keep both game settings intact
    // and supply the missing cap only while Unity owns the limiter.
    internal sealed class VSyncFrameLimiter : IDisposable
    {
        private readonly Func<int> readTarget;
        private readonly Func<int, int> wait;
        private readonly Action<string> log;
        internal string Status { get; private set; }

        internal VSyncFrameLimiter(Func<int> readTarget, Func<int, int> wait, Action<string> log)
        { this.readTarget = readTarget; this.wait = wait; this.log = log; }

        internal void Tick(bool unityOwnsLimiter, bool focused, int vsync)
        {
            int fps = unityOwnsLimiter && focused && vsync > 0 ? readTarget() : 0;
            int limit = ReflexLimiterController.Microseconds(fps);
            int result = wait(limit);
            string next = limit == 0 ? null : result == limit ? "Unity + VSync guard " + fps + " base FPS" : "Unity limiter (VSync guard unavailable)";
            if (next != Status) log("VSYNC_FPS_GUARD requested=" + fps + " acceptedUs=" + result + "; Unity FPS and VSync settings preserved");
            Status = next;
        }
        public void Dispose() { wait(0); Status = null; }
    }
}
