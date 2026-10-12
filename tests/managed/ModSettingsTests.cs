using System;
using System.IO;
using System.Linq;
using BepInEx.Configuration;
using BepInEx.Logging;
using RiskOfRainUpscaling;
using UnityEngine;

internal static class ModSettingsTests
{
    internal static void Run()
    {
        string folder = Path.Combine(Path.GetTempPath(), "RiskOfRainUpscaling.OptionsTests." + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(folder);
        try
        {
            string path = Path.Combine(folder, "mod.cfg");
            var config = new ConfigFile(path, false);
            var settings = new ModSettings(config);
            void Check(bool condition, string message) { if (!condition) throw new Exception("Mod Options: " + message); }
            Check(settings.Enabled.Value && settings.QualityLevel == DlssQualityLevel.DLAA && settings.GenerationMode == FrameGenerationMode.Off, "fresh defaults: DLAA and Frame Generation Off");
            Check(settings.Sharpness.Value == 0.7f && settings.MipBiasOffset.Value == -1f && settings.LoggingLevel.Value == ModLogLevel.Off &&
                settings.MainMenu.Value && settings.ReflexFrameLimiter.Value, "feature defaults");
            Check(!settings.ShowOverlay.Value && !settings.CaptureWhileOff.Value, "diagnostics stay hidden and idle by default");
            // Auto: DLSS/DLSS-G on supported NVIDIA GPUs, FSR elsewhere.
            Check(settings.Upscaler.Value == UpscalerBackend.Auto && settings.Generation.Backend.Value == FrameGenerationBackend.Auto, "backend defaults");
            Check(settings.Preset.Value == DlssPreset.Default && !settings.Profiling.Value, "DLSS preset and profiling defaults");
            Check(settings.Outlines.Value == OutlineMode.FullResolution, "outlines drawn at full resolution when upscaling by default");
            config.Save();
            string text = File.ReadAllText(path);
            Check(new[] { "[General]", "[Upscaling]", "[Frame Generation]", "[Diagnostics]", "[Hotkeys]" }.All(text.Contains) && !text.Contains("[Phase"), "player-facing sections");
            var shortcuts = new[] { settings.ToggleOverlay, settings.CycleQuality, settings.CaptureEvidence,
                settings.ToggleReconstruction, settings.ToggleFrameGeneration };
            Check(!settings.HotkeysEnabled.Value && shortcuts.Select(entry => entry.Value.MainKey).SequenceEqual(
                new[] { KeyCode.F6, KeyCode.F7, KeyCode.F8, KeyCode.F10, KeyCode.F11 }), "hotkey defaults");
            using (var source = new ManualLogSource("LoggingTest"))
            {
                int messages = 0;
                LogLevel actualLevel = LogLevel.None;
                source.LogEvent += (_, args) => { ++messages; actualLevel = args.Level; };
                foreach (ModLogLevel configured in Enum.GetValues<ModLogLevel>())
                {
                    settings.LoggingLevel.Value = configured;
                    int before = messages;
                    foreach (ModLogLevel severity in new[] { ModLogLevel.Error, ModLogLevel.Warning, ModLogLevel.Info, ModLogLevel.Debug })
                        settings.Logging.Write(source, severity, severity.ToString());
                    Check(messages - before == (int)configured, "live logging threshold " + configured);
                    if (configured != ModLogLevel.Off)
                        Check(actualLevel == (configured == ModLogLevel.Error ? LogLevel.Error : configured == ModLogLevel.Warning ? LogLevel.Warning :
                            configured == ModLogLevel.Info ? LogLevel.Info : LogLevel.Debug), "BepInEx severity " + configured);
                }
                settings.LoggingLevel.Value = ModLogLevel.Off;
                int count = messages;
                settings.Logging.Write(source, ModLogLevel.Error, "suppressed after live change");
                Check(messages == count, "live Off suppresses errors");
            }
            foreach (DlssQualityLevel level in Enum.GetValues<DlssQualityLevel>())
            {
                settings.QualityLevel = level;
                Check(settings.QualityLevel == level, "select " + level);
                if (level != DlssQualityLevel.Off) Check(settings.Upscaling.Value && (int)settings.Quality.Value == (int)level - 1, "native mode mapping");
            }
            settings.QualityLevel = DlssQualityLevel.Off;
            Check(!settings.Upscaling.Value && settings.Quality.Value == DlssQuality.UltraPerformance, "Off preserves last active quality for F10");
            settings.Upscaling.Value = true;
            Check(settings.QualityLevel == DlssQualityLevel.UltraPerformance, "hotkey re-enable reflects in menu");
            settings.Quality.Value = DlssQuality.Quality;
            Check(settings.QualityLevel == DlssQualityLevel.Quality, "F7 reflects in menu");
            settings.Upscaling.Value = false;
            Check(settings.QualityLevel == DlssQualityLevel.Off, "failure fallback reflects in menu");
            foreach (FrameGenerationMode generation in Enum.GetValues<FrameGenerationMode>())
            {
                settings.GenerationMode = generation;
                Check(settings.GenerationMode == generation, "select " + generation);
                if (generation != FrameGenerationMode.Off)
                    Check(settings.FrameGeneration.Value && settings.Generation.Multiplier.Value == (int)generation + 1, "native multiplier mapping");
            }
            settings.FrameGeneration.Value = false;
            Check(settings.GenerationMode == FrameGenerationMode.Off, "F11 reflects in menu");
            settings.FrameGeneration.Value = true;
            Check(settings.GenerationMode == FrameGenerationMode.SixX, "F11 restores last multiplier");
            int sharpnessChanges = 0;
            settings.Sharpness.SettingChanged += (_, _) => ++sharpnessChanges;
            settings.Sharpness.Value = 0; settings.Sharpness.Value = 1;
            Check(sharpnessChanges == 2, "live setting notifications");
            settings.Sharpness.Value = 5; Check(settings.Sharpness.Value == 1, "sharpness clamping");
            settings.Sharpness.Value = -1; Check(settings.Sharpness.Value == 0, "sharpness Off");
            settings.LoggingLevel.Value = ModLogLevel.Debug;
            settings.HotkeysEnabled.Value = false;
            settings.ToggleReconstruction.Value = new KeyboardShortcut(KeyCode.K, KeyCode.LeftControl);
            settings.ToggleFrameGeneration.Value = KeyboardShortcut.Empty;
            settings.ShowOverlay.Value = true;
            settings.Upscaler.Value = UpscalerBackend.FSR; settings.Generation.Backend.Value = FrameGenerationBackend.FSR;
            config.Save();
            var reloaded = new ModSettings(new ConfigFile(path, false));
            Check(!reloaded.HotkeysEnabled.Value && reloaded.ToggleReconstruction.Value.Equals(new KeyboardShortcut(KeyCode.K, KeyCode.LeftControl)) &&
                reloaded.ToggleFrameGeneration.Value.Equals(KeyboardShortcut.Empty), "persist key combination, disabled binding and hotkey switch");
            config.Reload();
            Check(settings.LoggingLevel.Value == ModLogLevel.Debug && settings.ToggleReconstruction.Value.Equals(reloaded.ToggleReconstruction.Value), "reload shared preferences");
            Check(reloaded.Sharpness.Value == 0 && reloaded.LoggingLevel.Value == ModLogLevel.Debug && reloaded.ShowOverlay.Value &&
                reloaded.Upscaler.Value == UpscalerBackend.FSR && reloaded.Generation.Backend.Value == FrameGenerationBackend.FSR &&
                reloaded.QualityLevel == DlssQualityLevel.Off && reloaded.Quality.Value == DlssQuality.Quality && reloaded.GenerationMode == FrameGenerationMode.SixX,
                "persist settings");
            // "FSR" stays the FSR3/4 DX12 path for existing configs; the D3D11 path has its own name.
            reloaded.Upscaler.Value = UpscalerBackend.FSR3DX11; reloaded.Upscaler.ConfigFile.Save();
            Check(new ModSettings(new ConfigFile(path, false)).Upscaler.Value == UpscalerBackend.FSR3DX11, "persist FSR 3 DX11 upscaler");
            Console.WriteLine("PASS: fresh defaults and sections, backend defaults, quality/FG choices, logging thresholds, hotkey defaults/combinations/disable, notifications, bounds and persistence.");
        }
        finally { Directory.Delete(folder, true); }
    }
}
