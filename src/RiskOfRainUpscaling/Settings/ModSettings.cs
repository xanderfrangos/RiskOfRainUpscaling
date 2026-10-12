using System;
using BepInEx.Configuration;
using UnityEngine;

namespace RiskOfRainUpscaling
{
    internal enum DlssQualityLevel { Off, DLAA, Quality, Balanced, Performance, UltraPerformance }
    // Fixed values map to multiplier (value + 1); Dynamic stays last so they keep their numbers.
    internal enum FrameGenerationMode { Off, TwoX, ThreeX, FourX, FiveX, SixX, Dynamic }
    internal enum OutlineMode { FullResolution, Temporal }

    // The menu and hotkeys share the existing entries, including renderer fallbacks.
    internal sealed class ModSettings
    {
        internal const string UpscalingSection = "Upscaling";
        internal readonly ConfigEntry<bool> Enabled;
        internal readonly ConfigEntry<bool> Upscaling;
        internal readonly ConfigEntry<DlssQuality> Quality;
        internal readonly ConfigEntry<UpscalerBackend> Upscaler;
        internal readonly ConfigEntry<DlssPreset> Preset;
        internal readonly ConfigEntry<OutlineMode> Outlines;
        internal readonly ConfigEntry<bool> FrameGeneration;
        internal readonly FrameGenerationSetting Generation;
        internal readonly ConfigEntry<float> Sharpness, MipBiasOffset;
        internal readonly ConfigEntry<bool> MainMenu, ReflexFrameLimiter;
        internal readonly ConfigEntry<bool> ShowOverlay, CaptureWhileOff, Profiling;
        internal readonly ConfigEntry<ModLogLevel> LoggingLevel;
        internal readonly LoggingSettings Logging;
        internal readonly ConfigEntry<bool> HotkeysEnabled;
        internal readonly ConfigEntry<KeyboardShortcut> ToggleOverlay, CycleQuality, CaptureEvidence,
            ToggleReconstruction, ToggleFrameGeneration;

        internal ModSettings(ConfigFile config)
        {
            Enabled = config.Bind("General", "Enabled", true, "Master switch for upscaling, Frame Generation and diagnostics. Off restores the game's own rendering; a D3D12 presentation runtime selected at startup stays until restart.");
            Upscaling = config.Bind(UpscalingSection, "Enabled", true, "Reconstruct the world with the selected Upscaler (DLSS or FSR) at the selected Quality. Upscaler Quality in Mod Options turns it off or selects the quality. The Toggle Upscaling shortcut toggles it (default F10). Off keeps the saved Quality.");
            Quality = config.Bind(UpscalingSection, "Quality", DlssQuality.DLAA, "How much to upscale. DLAA/Native AA renders at full resolution for the best image. Quality, Balanced, Performance and Ultra Performance render at lower resolutions for more FPS. Off turns upscaling off.");
            Upscaler = config.Bind(UpscalingSection, "Upscaler", UpscalerBackend.Auto, "Which upscaler to use. Auto picks the best one for your graphics card: DLSS on NVIDIA RTX cards, otherwise FSR. FSR 3 DX11 is the lighter FSR option. FSR3/4 DX12 uses the better-looking FSR 4 on supported AMD cards, but costs a little more.");
            Preset = config.Bind(UpscalingSection, "DLSS Preset", DlssPreset.Default, "Which DLSS model to use (DLSS only). Default lets NVIDIA pick and is recommended. K, J, M and L are the newer, better-looking models: L is the sharpest but slowest, and M is a faster alternative to L. E is the older model: the fastest but softest.");
            Outlines = config.Bind(UpscalingSection, "Outlines", OutlineMode.FullResolution, "How outlines on chests, shrines and the teleporter are drawn at Quality and lower. Full Resolution keeps them crisp. Temporal is steadier but softer (DLSS only).");
            Sharpness = config.Bind(UpscalingSection, "Sharpening", 0.7f, new ConfigDescription("How much to sharpen the image after upscaling. 0 is off; 1 is strongest.", new AcceptableValueRange<float>(0, 1)));
            MipBiasOffset = config.Bind(UpscalingSection, "MipBiasOffset", -1f, new ConfigDescription("Texture LOD bias added to log2(render/display) when the upscaler renders below 100%. NVIDIA recommends -1; raise toward 0 if fine textures shimmer.", new AcceptableValueRange<float>(-2f, 0f)));
            MainMenu = config.Bind(UpscalingSection, "MainMenu", true, "Enable upscaling and requested Frame Generation on the intro cutscene, title/main-menu and character select (lobby) scene cameras. Menu and subtitle UI renders afterward. Loading and spectator cameras remain excluded.");
            Generation = new FrameGenerationSetting(config);
            FrameGeneration = Generation.Requested;
            ReflexFrameLimiter = config.Bind(FrameGenerationSetting.Section, "ReflexFrameLimiter", true, "Replace the game's limiter with NVIDIA Reflex while DLSS is enabled and VSync is off. With 2x-6x frame generation, the game's framerate limit is your base FPS. With Dynamic frame generation, it is the output FPS that DLSS aims for; unlimited (0 or -1) aims for the monitor's refresh rate. Keep this limiter on with Dynamic: NVIDIA designs Dynamic around the Reflex limiter.");
            Logging = new LoggingSettings(config);
            LoggingLevel = Logging.Level;
            ShowOverlay = config.Bind(LoggingSettings.Section, "ShowOverlay", false, "Show the input preview panel. The Toggle Debug Overlay shortcut toggles it.");
            CaptureWhileOff = config.Bind(LoggingSettings.Section, "CaptureWhileOff", false, "Keep capturing scene inputs for the debug overlay and Capture Diagnostics while upscaling is Off. Upscaling always captures the inputs it needs.");
            // Developer option: cost profiling to profile.log.
            Profiling = config.Bind(LoggingSettings.Section, "Profiling", false, "For developers: regularly records how long each step of the mod takes to profile.log in the diagnostics folder. Slightly lowers performance, so leave it off normally.");
            HotkeysEnabled = config.Bind("Hotkeys", "Enable Hotkeys", false, "Turns the mod's keyboard shortcuts on or off. Set a shortcut to None to turn off just that one.");
            ToggleOverlay = BindHotkey(config, "Toggle Debug Overlay", KeyCode.F6, "Shows or hides the debug overlay.");
            CycleQuality = BindHotkey(config, "Cycle Upscaler Quality", KeyCode.F7, "Switches to the next upscaler quality level.");
            CaptureEvidence = BindHotkey(config, "Capture Diagnostics", KeyCode.F8, "Saves screenshots and data that help when reporting a problem.");
            ToggleReconstruction = BindHotkey(config, "Toggle Upscaling", KeyCode.F10, "Turns upscaling on or off. Also tries again if upscaling stopped working.");
            ToggleFrameGeneration = BindHotkey(config, "Toggle Frame Generation", KeyCode.F11, "Turns frame generation on or off. Turning it on needs a restart if it was off when the game started.");
        }

        private static ConfigEntry<KeyboardShortcut> BindHotkey(ConfigFile config, string name, KeyCode key, string description) =>
            config.Bind("Hotkeys", name, new KeyboardShortcut(key), description + " Key combinations work too (for example F10 + LeftControl).");

        internal bool ReconstructionEnabled => Upscaling.Value;

        internal DlssQualityLevel QualityLevel
        {
            get => Upscaling.Value ? (DlssQualityLevel)((int)Quality.Value + 1) : DlssQualityLevel.Off;
            set
            {
                if (!Enum.IsDefined(typeof(DlssQualityLevel), value)) throw new ArgumentOutOfRangeException(nameof(value));
                if (value != DlssQualityLevel.Off) Quality.Value = (DlssQuality)((int)value - 1);
                Upscaling.Value = value != DlssQualityLevel.Off;
            }
        }
        internal FrameGenerationMode GenerationMode
        {
            get => !FrameGeneration.Value ? FrameGenerationMode.Off : Generation.Dynamic.Value ? FrameGenerationMode.Dynamic : (FrameGenerationMode)(Generation.Multiplier.Value - 1);
            set
            {
                if (!Enum.IsDefined(typeof(FrameGenerationMode), value)) throw new ArgumentOutOfRangeException(nameof(value));
                // Off keeps the saved mode so the toggle shortcut restores it.
                if (value != FrameGenerationMode.Off) Generation.Dynamic.Value = value == FrameGenerationMode.Dynamic;
                if (value != FrameGenerationMode.Off && value != FrameGenerationMode.Dynamic) Generation.Multiplier.Value = (int)value + 1;
                FrameGeneration.Value = value != FrameGenerationMode.Off;
            }
        }
    }
}
