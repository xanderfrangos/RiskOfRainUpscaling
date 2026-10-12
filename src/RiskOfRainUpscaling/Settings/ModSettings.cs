using System;
using BepInEx.Configuration;
using UnityEngine;

namespace RiskOfRainUpscaling
{
    internal enum DlssQualityLevel { Off, DLAA, Quality, Balanced, Performance, UltraPerformance }
    internal enum FrameGenerationMode { Off, TwoX, ThreeX, FourX, FiveX, SixX }
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
            Quality = config.Bind(UpscalingSection, "Quality", DlssQuality.DLAA, "Upscaling quality level for the selected upscaler. Below DLAA/Native AA, the Outlines setting chooses how interactable outlines are drawn.");
            Upscaler = config.Bind(UpscalingSection, "Upscaler", UpscalerBackend.Auto, "Upscaling runtime. FSR3DX11 (FSR 3 DX11 in Mod Options) runs AMD FSR 3.1 directly on the game's DirectX 11 device, the lighter FSR path. FSR (FSR3/4 DX12) runs AMD FSR 4 on supported AMD GPUs, otherwise FSR 3.1, on a separate DirectX 12 queue, with a copy round trip between the two APIs every frame. Auto uses NVIDIA DLSS on supported GPUs; on other GPUs, including AMD, Intel and NVIDIA GPUs without DLSS, it uses FSR3/4 DX12 when FSR 4 is available and FSR 3 DX11 otherwise.");
            Preset = config.Bind(UpscalingSection, "DLSS Preset", DlssPreset.Default, "DLSS model used for every quality mode; FSR ignores it. Default lets NVIDIA choose per mode (K for DLAA, Quality and Balanced, M for Performance, L for Ultra Performance). K, J, M and L are DLSS 4 transformer models: K is the quality reference, J is similar to K with slightly less ghosting and more flicker, M is faster than L with similar improvements, L is the sharpest and most expensive. E is the older CNN model: the cheapest and softest, and deprecated by NVIDIA. Changing it restarts the upscaler's history.");
            Outlines = config.Bind(UpscalingSection, "Outlines", OutlineMode.FullResolution, "How interactable outlines (chests, shrines, the teleporter) are drawn when upscaling below 100% (Quality to Ultra Performance). Full Resolution draws them at the display resolution after upscaling, with the outline lines anti-aliased (8 samples per pixel) and silhouettes smoothed with SMAA. Temporal reconstructs them with DLSS like the rest of the scene: steadier and softer, with render-resolution detail. Temporal needs DLSS alpha upscaling, so FSR always uses Full Resolution. DLAA/Native AA renders at the display resolution already and ignores this setting. Changing it restarts the upscaler's history.");
            Sharpness = config.Bind(UpscalingSection, "Sharpening", 0.7f, new ConfigDescription("RCAS sharpening after DLAA/DLSS or FSR, before the HUD. 0 disables it; 1 is strongest. Applies immediately.", new AcceptableValueRange<float>(0, 1)));
            MipBiasOffset = config.Bind(UpscalingSection, "MipBiasOffset", -1f, new ConfigDescription("Texture LOD bias added to log2(render/display) when the upscaler renders below 100%. NVIDIA recommends -1; raise toward 0 if fine textures shimmer.", new AcceptableValueRange<float>(-2f, 0f)));
            MainMenu = config.Bind(UpscalingSection, "MainMenu", true, "Enable upscaling and requested Frame Generation on the intro cutscene, title/main-menu and character select (lobby) scene cameras. Menu and subtitle UI renders afterward. Loading and spectator cameras remain excluded.");
            Generation = new FrameGenerationSetting(config);
            FrameGeneration = Generation.Requested;
            ReflexFrameLimiter = config.Bind(FrameGenerationSetting.Section, "ReflexFrameLimiter", true, GameFrameRateSetting.Description);
            Logging = new LoggingSettings(config);
            LoggingLevel = Logging.Level;
            ShowOverlay = config.Bind(LoggingSettings.Section, "ShowOverlay", false, "Show the input preview panel. The Toggle Debug Overlay shortcut toggles it.");
            CaptureWhileOff = config.Bind(LoggingSettings.Section, "CaptureWhileOff", false, "Keep capturing scene inputs for the debug overlay and Capture Diagnostics while upscaling is Off. Upscaling always captures the inputs it needs.");
            // Developer option: cost profiling to profile.log.
            Profiling = config.Bind(LoggingSettings.Section, "Profiling", false, "Developer option: every 2 seconds, write the GPU and CPU cost of each upscaling and Frame Generation step, and of each Unity camera stage, to profile.log in the diagnostics folder. Adds a few hundred GPU timestamp queries per frame. Applies immediately.");
            HotkeysEnabled = config.Bind("Hotkeys", "Enable Hotkeys", false, "Enable the mod's keyboard shortcuts. Applies immediately. Individual shortcuts can be disabled by setting them to None.");
            ToggleOverlay = BindHotkey(config, "Toggle Debug Overlay", KeyCode.F6, "Show or hide the input preview panel.");
            CycleQuality = BindHotkey(config, "Cycle Upscaler Quality", KeyCode.F7, "Cycle DLAA/DLSS or FSR quality while upscaling is enabled.");
            CaptureEvidence = BindHotkey(config, "Capture Diagnostics", KeyCode.F8, "Save diagnostic screenshots and capture data.");
            ToggleReconstruction = BindHotkey(config, "Toggle Upscaling", KeyCode.F10, "Toggle upscaling Off/on using the last quality. Also retries after a renderer failure.");
            ToggleFrameGeneration = BindHotkey(config, "Toggle Frame Generation", KeyCode.F11, "Toggle Frame Generation Off/on using the last multiplier. Applies live in D3D12; enabling from D3D11 requires a restart.");
        }

        private static ConfigEntry<KeyboardShortcut> BindHotkey(ConfigFile config, string name, KeyCode key, string description) =>
            config.Bind("Hotkeys", name, new KeyboardShortcut(key), description + " Applies immediately. Supports key combinations (for example F10 + LeftControl). None disables this shortcut.");

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
            get => FrameGeneration.Value ? (FrameGenerationMode)(Generation.Multiplier.Value - 1) : FrameGenerationMode.Off;
            set
            {
                if (!Enum.IsDefined(typeof(FrameGenerationMode), value)) throw new ArgumentOutOfRangeException(nameof(value));
                if (value != FrameGenerationMode.Off) Generation.Multiplier.Value = (int)value + 1;
                FrameGeneration.Value = value != FrameGenerationMode.Off;
            }
        }
    }
}
