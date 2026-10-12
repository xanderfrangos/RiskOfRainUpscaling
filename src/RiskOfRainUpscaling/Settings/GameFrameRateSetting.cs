using System;
using System.Globalization;

namespace RiskOfRainUpscaling
{
    // Read/write fps_max itself. Its getter preserves the requested game cap
    // while Reflex temporarily sets Unity's internal target to unlimited.
    internal sealed class GameFrameRateSetting
    {
        internal const string Description = "The game's framerate limit. 0 or -1 means unlimited. With 2x-6x frame generation, this limits your real frames before extra ones are added. With Dynamic, this is the framerate it aims for (your monitor's refresh rate when unlimited). Uses NVIDIA Reflex for lower input lag when DLSS is on and VSync is off.";
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
