using System;
using System.Globalization;

namespace RiskOfRainUpscaling
{
    // Read/write fps_max itself. Its getter preserves the requested game cap
    // while Reflex temporarily sets Unity's internal target to unlimited.
    internal sealed class GameFrameRateSetting
    {
        internal const string Description = "Replace the game's limiter with NVIDIA Reflex while DLSS is enabled and VSync is off. With 2x-6x frame generation, this is your base FPS. With Dynamic frame generation, this is the output FPS that DLSS aims for; unlimited (0 or -1) aims for the monitor's refresh rate. Keep this limiter on with Dynamic: NVIDIA designs Dynamic around the Reflex limiter.";
        private readonly Func<string> read;
        private readonly Action<string> write;

        internal GameFrameRateSetting(Func<string> read, Action<string> write)
        { this.read = read; this.write = write; }

        internal int Value
        {
            get => int.Parse(read(), CultureInfo.InvariantCulture);
            set => write(Math.Max(-1, value).ToString(CultureInfo.InvariantCulture));
        }
    }
}
