using System;
using System.Globalization;

namespace RiskOfRainUpscaling
{
    // Read/write fps_max itself. Its getter preserves the requested game cap
    // while Reflex temporarily sets Unity's internal target to unlimited.
    internal sealed class GameFrameRateSetting
    {
        internal const string Description = "Replace the game's limiter with NVIDIA Reflex while DLSS is enabled and VSync is off. With frame generation enabled, this is your base FPS.";
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
