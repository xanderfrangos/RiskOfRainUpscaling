using BepInEx.Configuration;

namespace RiskOfRainUpscaling
{
    // Native order: 0 Auto, 1 DLSS, 2 FSR (FrameGenerationBackend in FrameGenerator.h).
    internal enum FrameGenerationBackend { Auto, DLSS, FSR }

    // Keep the presentation runtime for the session; toggle generation within it.
    internal sealed class FrameGenerationSetting
    {
        // Native startup reads Enabled, Multiplier, Dynamic and Backend from this section.
        // Native startup (FrameBridge.cpp) reads this section before graphics initialize.
        internal const string Section = "Frame Generation";
        internal const string Description = "Off, 2x-6x or Dynamic Frame Generation (DLSS Frame Generation on supported NVIDIA GPUs, FSR Frame Generation elsewhere; see Backend). FSR generates one frame per rendered frame, so 3x-6x and Dynamic run at 2x. Dynamic (DLSS only) lets DLSS choose how many frames to generate so the output reaches the Framerate Limit (or the monitor's refresh rate when unlimited); it needs an RTX 50-series GPU and driver 595.41 or newer, otherwise the highest supported multiplier is used. Frame generation requires the D3D12 presentation runtime to be active. This means enabling frame generation requires a restart. Turning Off keeps D3D12 for this session; restarting with Off saved starts D3D11. This feature requires upscaling to function. Unsupported multipliers use the highest supported mode.";
        internal readonly ConfigEntry<bool> Requested;
        internal readonly ConfigEntry<int> Multiplier;
        internal readonly ConfigEntry<bool> Dynamic;
        internal readonly ConfigEntry<FrameGenerationBackend> Backend;
        internal readonly bool EnabledAtStartup;
        internal readonly int MultiplierAtStartup;
        internal bool RuntimeAvailable { get; private set; }
        internal bool RestartRequired => Requested.Value && !RuntimeAvailable;
        internal bool LiveEnabled => RuntimeAvailable && Requested.Value;
        internal int LiveMultiplier => LiveEnabled ? Multiplier.Value : 0;
        internal bool LiveDynamic => LiveEnabled && Dynamic.Value;
        internal void SetPresentationState(int state)
        {
            // A pending bridge keeps its startup intent until graphics creation.
            if (state != 1) RuntimeAvailable = state == 2;
        }

        internal FrameGenerationSetting(ConfigFile config)
        {
            Requested = config.Bind(Section, "Enabled", false, Description);
            Multiplier = config.Bind(Section, "Multiplier", 2, new ConfigDescription("Total generated frames per rendered frame when Frame Generation is enabled (2–6). Off/On and multiplier changes apply live once the D3D12 presentation runtime is active. Enabling from Off requires a restart. Unsupported values fall back to the GPU/driver's maximum.", new AcceptableValueRange<int>(2, 6)));
            Dynamic = config.Bind(Section, "Dynamic", false, "Dynamic Multi Frame Generation (DLSS only): DLSS chooses how many frames to generate so the output reaches the Framerate Limit, or the monitor's refresh rate when the limit is unlimited. Overrides Multiplier while enabled; Multiplier is kept for fixed modes. Requires an RTX 50-series GPU and NVIDIA driver 595.41 or newer; otherwise the highest supported multiplier is used. FSR runs 2x. Applies live once the D3D12 presentation runtime is active.");
            Backend = config.Bind(Section, "Backend", FrameGenerationBackend.Auto, "Frame Generation runtime. Auto uses DLSS Frame Generation on supported NVIDIA GPUs and AMD FSR Frame Generation on other GPUs. FSR generates one frame per rendered frame (2x) regardless of multiplier selected. FSR has no Reflex, so the game's FPS cap is used. Requires a restart.");
            EnabledAtStartup = Requested.Value;
            MultiplierAtStartup = Multiplier.Value;
            RuntimeAvailable = EnabledAtStartup;
        }
    }
}
