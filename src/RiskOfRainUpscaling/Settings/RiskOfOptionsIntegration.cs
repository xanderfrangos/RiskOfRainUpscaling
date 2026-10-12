using System;
using System.IO;
using System.Reflection;
using System.Resources;
using System.Runtime.CompilerServices;
using RiskOfOptions;
using RiskOfOptions.OptionConfigs;
using RiskOfOptions.Options;
using UnityEngine;

namespace RiskOfRainUpscaling
{
    // Keep all RoO types behind this non-inlined call so RoO remains optional.
    internal static class RiskOfOptionsIntegration
    {
        internal const string Guid = "com.rune580.riskofoptions";

        [MethodImpl(MethodImplOptions.NoInlining)]
        internal static void Register(ModSettings settings, GameFrameRateSetting frameRate, Func<bool> dlss, Action<string> warning)
        {
            ModSettingsManager.SetModDescription("Adds upscaling and frame generation to Risk of Rain 2. This is offered in two flavors: DLSS or AMD FSR. Also supports frame generation (2x-6x or Dynamic on NVIDIA, 2x on FSR). Enabling frame generation requires a restart for the D3D12 bridge.", Plugin.Guid, "Risk of Rain Upscaling");
            try { ModSettingsManager.SetModIcon(LoadIcon(), Plugin.Guid, "Risk of Rain Upscaling"); }
            catch (Exception e) { warning("Risk of Options icon unavailable: " + e.Message); }
            Add(new ChoiceOption(settings.Upscaler, new ChoiceConfig { name = "Upscaler", category = "Graphics" }));
            var quality = new LiveChoice<DlssQualityLevel>("Upscaler Quality", settings.Quality.Description.Description,
                () => settings.QualityLevel, value => settings.QualityLevel = value);
            Add(quality);
            // Only DLSS reads the preset; greyed out while upscaling is Off or FSR runs.
            var preset = new ChoiceOption(settings.Preset, new ChoiceConfig { name = "DLSS Preset", category = "Graphics", checkIfDisabled = () => !dlss() });
            Add(preset);
            // Only lower-resolution qualities read it; greyed out at DLAA/Native AA and while upscaling is Off.
            var outlines = new ChoiceOption(settings.Outlines, new ChoiceConfig { name = "Outlines", category = "Graphics", checkIfDisabled = () => !settings.Upscaling.Value || settings.Quality.Value == DlssQuality.DLAA });
            Add(outlines);
            var generation = new LiveChoice<FrameGenerationMode>("Frame Generation", settings.FrameGeneration.Description.Description,
                () => settings.GenerationMode, value => settings.GenerationMode = value);
            Add(generation);
            // The preloader reads the backend before graphics startup.
            Add(new ChoiceOption(settings.Generation.Backend, new ChoiceConfig { name = "Frame Generation Backend", category = "Graphics", restartRequired = true }));
            Add(new StepSliderOption(settings.Sharpness, new StepSliderConfig { name = "RCAS Sharpening", category = "Graphics", min = 0, max = 1, increment = 0.01f, FormatString = "{0:0.00}" }));
            Add(new GameFrameRateOption(frameRate));
            Add(new ChoiceOption(settings.LoggingLevel));
            // Cost profiling to profile.log; applied live by Plugin.ApplyLiveSettings.
            Add(new CheckBoxOption(settings.Profiling, new CheckBoxConfig { name = "Cost Profiling", category = LoggingSettings.Section, description = settings.Profiling.Description.Description }));
            Add(new CheckBoxOption(settings.HotkeysEnabled));
            Add(new KeyBindOption(settings.ToggleOverlay));
            Add(new KeyBindOption(settings.CycleQuality));
            Add(new KeyBindOption(settings.CaptureEvidence));
            Add(new KeyBindOption(settings.ToggleReconstruction));
            Add(new KeyBindOption(settings.ToggleFrameGeneration));
            // RoO exposes no public API for enum display labels. Its language table
            // is the only compatibility shim; failure keeps the usable enum labels.
            try
            {
                var language = typeof(ModSettingsManager).Assembly.GetType("RiskOfOptions.Lib.LanguageApi");
                var add = language?.GetMethod("Add", BindingFlags.Static | BindingFlags.NonPublic, null, new[] { typeof(string), typeof(string) }, null);
                if (add == null) throw new MissingMethodException("RiskOfOptions language labels");
                Label(add, quality, nameof(DlssQualityLevel.DLAA), "DLAA/Native AA");
                Label(add, quality, nameof(DlssQualityLevel.UltraPerformance), "Ultra Performance");
                Label(add, preset, nameof(DlssPreset.Default), "Default (NVIDIA per mode)");
                Label(add, preset, nameof(DlssPreset.K), "K (transformer)");
                Label(add, preset, nameof(DlssPreset.M), "M (transformer, faster than L)");
                Label(add, preset, nameof(DlssPreset.L), "L (transformer, sharpest, slowest)");
                Label(add, preset, nameof(DlssPreset.J), "J (transformer)");
                Label(add, preset, nameof(DlssPreset.E), "E (CNN, cheapest)");
                Label(add, outlines, nameof(OutlineMode.FullResolution), "Full Resolution");
                foreach (FrameGenerationMode mode in Enum.GetValues(typeof(FrameGenerationMode)))
                    if (mode != FrameGenerationMode.Off && mode != FrameGenerationMode.Dynamic) Label(add, generation, mode.ToString(), ((int)mode + 1) + "x");
                Label(add, generation, nameof(FrameGenerationMode.Dynamic), "Dynamic (DLSS only)");
            }
            catch (Exception e) { warning("Risk of Options choice labels use enum names: " + e.Message); }
        }

        private static Sprite LoadIcon()
        {
            using (var stream = typeof(RiskOfOptionsIntegration).Assembly.GetManifestResourceStream("RiskOfRainUpscaling.icon-45.png"))
            using (var bytes = new MemoryStream())
            {
                if (stream == null) throw new MissingManifestResourceException("icon-45.png");
                stream.CopyTo(bytes);
                var texture = new Texture2D(2, 2);
                if (!texture.LoadImage(bytes.ToArray())) throw new InvalidDataException("icon-45.png");
                return Sprite.Create(texture, new Rect(0, 0, texture.width, texture.height), new Vector2(0.5f, 0.5f));
            }
        }

        private static void Add(BaseOption option) =>ModSettingsManager.AddOption(option, Plugin.Guid, "Risk of Rain Upscaling");

        [MethodImpl(MethodImplOptions.NoInlining)]
        internal static bool IsBindingHotkey() => UnityEngine.Object.FindObjectOfType<RiskOfOptions.Utils.KeyBindUtil>();
        private static void Label(MethodInfo add, BaseOption option, string value, string label) => add.Invoke(null, new object[] { Token(option, value), label });
        private static string Token(BaseOption option, string value) =>
            ("RISK_OF_OPTIONS." + option.ModGuid + "." + option.Category + "." + option.Name + ".choice.item." + value).Replace(" ", "_").ToUpper();

        private sealed class GameFrameRateOption : IntFieldOption
        {
            private readonly GameFrameRateSetting setting;
            internal GameFrameRateOption(GameFrameRateSetting setting)
                : base(new IntFieldConfig { name = "Framerate Limit", category = "Graphics",
                    description = GameFrameRateSetting.Description, Min = -1, Max = int.MaxValue,
                    checkIfDisabled = () => !RoR2.RoR2Application.loadFinished }, setting.Value)
            { this.setting = setting; }
            protected override void SetProperties()
            {
                SetName(config.name, config); SetCategoryName(config.category, config); SetDescription(config.description, config);
            }
            public override int Value
            {
                get => setting.Value;
                set => setting.Value = value;
            }
        }

        // A combined enum choice writes the canonical settings instead of keeping
        // a second copy that could drift after hotkeys or a renderer failure.
        private sealed class LiveChoice<T> : ChoiceOption where T : struct, Enum
        {
            private readonly Func<T> read;
            private readonly Action<T> write;
            internal LiveChoice(string name, string description, Func<T> read, Action<T> write, bool restartRequired = false)
                : base(new ChoiceConfig { name = name, description = description, category = "Graphics", restartRequired = restartRequired }, read())
            { this.read = read; this.write = write; }
            protected override void SetProperties()
            {
                SetName(config.name, config); SetCategoryName(config.category, config); SetDescription(config.description, config);
            }
            public override object Value
            {
                get => read();
                set => write((T)Enum.Parse(typeof(T), value.ToString()));
            }
        }
    }
}
