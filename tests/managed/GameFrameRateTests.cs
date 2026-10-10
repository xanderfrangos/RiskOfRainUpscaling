using System;
using System.Globalization;
using System.Collections.Generic;
using System.IO;
using BepInEx.Configuration;
using RiskOfRainUpscaling;

internal static class GameFrameRateTests
{
    internal static void Run()
    {
        int target = 120, native = 0;
        using var limiter = new ReflexLimiterController(() => target, value => target = value,
            value => native = value, () => native, _ => { });
        // Model the existing fps_max hooks, using the actual limiter controller.
        string Read() => limiter.Owned ? limiter.SavedValue : target.ToString(CultureInfo.InvariantCulture);
        void Write(string text)
        {
            int value = int.Parse(text, CultureInfo.InvariantCulture);
            limiter.GameSetting(value);
            if (!limiter.Owned) target = value;
        }
        var setting = new GameFrameRateSetting(Read, Write);
        void Check(bool condition, string message) { if (!condition) throw new Exception("Game FPS sync: " + message); }
        void Tick(bool running = true, int vsync = 0) => limiter.Tick(true, true, running, vsync);
        Check(setting.Value == 120, "initial game setting, without a separate override");
        Write("60"); Check(setting.Value == 60, "game menu change appears in Mod Options");
        setting.Value = 90; Check(Read() == "90" && target == 90, "Mod Options changes game menu and Unity cap");
        Tick(); Check(target == -1 && setting.Value == 90 && native == 11111, "show requested cap while Reflex owns pacing");
        Write("144"); Tick(); Check(setting.Value == 144 && native == 6944, "game menu change while Reflex owns pacing");
        setting.Value = 30; Tick(); Check(Read() == "30" && native == 33333, "Mod Options changes shared Reflex cap");
        Tick(running: false); Check(setting.Value == 30 && target == 30 && native == 0, "pause preserves shared value");
        setting.Value = 60; Tick(); Tick(vsync: 1);
        Check(setting.Value == 60 && target == 60 && native == 0, "VSync fallback preserves changed value");
        setting.Value = -1; Tick(); Check(Read() == "-1" && native == 0, "game's -1 unlimited convention");
        setting.Value = 120; Tick(); native = -1; limiter.CheckHealth();
        Check(setting.Value == 120 && target == 120, "failed Reflex preserves cap in both menus");
        setting.Value = 2000; Check(Read() == "2000", "existing high game caps are not truncated");
        limiter.Dispose(); Check(target == 2000 && setting.Value == 2000, "shutdown keeps latest shared game setting");
        ReconstructionToggle();
        Console.WriteLine("PASS: shared game/Mod Options FPS setting, both change directions, Reflex ownership, pause/VSync/failure fallback, unlimited and shutdown persistence.");
    }

    private static void ReconstructionToggle()
    {
        string folder = Path.Combine(Path.GetTempPath(), "RiskOfRainUpscaling.LimiterToggleTests." + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(folder);
        try
        {
            var settings = new ModSettings(new ConfigFile(Path.Combine(folder, "mod.cfg"), false));
            settings.QualityLevel = DlssQualityLevel.Balanced;
            int target = 120, native = 0, vsyncGuard = 0;
            var events = new List<string>();
            using var limiter = new ReflexLimiterController(() => target,
                value => { target = value; events.Add("Unity:" + value); },
                value => { native = value; events.Add("native:" + value); return value; },
                () => native, _ => { });
            string Read() => limiter.Owned ? limiter.SavedValue : target.ToString(CultureInfo.InvariantCulture);
            void Write(string text)
            {
                int value = int.Parse(text, CultureInfo.InvariantCulture);
                limiter.GameSetting(value);
                if (!limiter.Owned) target = value;
            }
            var fps = new GameFrameRateSetting(Read, Write);
            using var vsyncLimiter = new VSyncFrameLimiter(() => target, value => vsyncGuard = value, _ => { });
            void Tick(int vsync = 0, bool focused = true, bool running = true)
            {
                limiter.Tick(settings.ReconstructionEnabled, focused, running, vsync);
                vsyncLimiter.Tick(!limiter.Owned, focused, vsync);
            }
            void Check(bool condition, string message) { if (!condition) throw new Exception("DLSS limiter toggle: " + message); }

            Tick(); fps.Value = 90; Tick();
            events.Clear();
            settings.Upscaling.Value = false; // F10 uses this same entry.
            Tick();
            Check(!limiter.Owned && native == 0 && target == 90 && fps.Value == 90, "F10 Off restores the latest requested cap");
            Check(events.Count == 2 && events[0] == "native:0" && events[1] == "Unity:90", "clear native pacing before restoring Unity pacing");
            Check(limiter.Status.StartsWith("Unity limiter"), "overlay reports Unity ownership");
            Write("144"); Tick();
            Check(target == 144 && native == 0 && fps.Value == 144, "game menu works while DLSS is Off");
            fps.Value = 60; Tick(); Tick();
            Check(target == 60 && native == 0 && !limiter.Owned, "Mod Options cap remains active while DLSS is Off");
            settings.Upscaling.Value = true; Tick();
            Check(limiter.Owned && target == -1 && native == 16667 && fps.Value == 60 && settings.QualityLevel == DlssQualityLevel.Balanced,
                "F10 On resumes Reflex with the current cap and saved quality");
            settings.FrameGeneration.Value = false; Tick();
            Check(limiter.Owned && native == 16667, "F11 Off keeps Reflex while reconstruction is enabled");
            settings.QualityLevel = DlssQualityLevel.Off; Tick();
            Check(!limiter.Owned && target == 60 && native == 0, "quality Off in Mod Options also restores Unity");
            fps.Value = -1;
            settings.QualityLevel = DlssQualityLevel.Performance; Tick();
            settings.Upscaling.Value = false; Tick();
            Check(!limiter.Owned && target == -1 && native == 0 && fps.Value == -1, "preserve explicitly selected unlimited across toggles");
            fps.Value = 40; settings.Upscaling.Value = true; Tick(vsync: 1);
            Check(!limiter.Owned && target == 40 && native == 0 && vsyncGuard == 25000 && fps.Value == 40,
                "VSync no longer bypasses the saved 40 FPS cap while DLSS is enabled");
            Check(vsyncLimiter.Status == "Unity + VSync guard 40 base FPS", "overlay identifies VSync cap enforcement");
            settings.Upscaling.Value = false; Tick(vsync: 1);
            Check(target == 40 && native == 0 && vsyncGuard == 25000, "F10 Off keeps the cap with VSync without enabling Reflex pacing");
            Write("30"); Tick(vsync: 2);
            Check(target == 30 && vsyncGuard == 33333 && fps.Value == 30, "game menu changes apply with nonzero VSync counts");
            fps.Value = 60; Tick(vsync: 1, running: false);
            Check(target == 60 && vsyncGuard == 16667, "Mod Options cap also applies while paused with VSync");
            Tick(vsync: 1, focused: false);
            Check(target == 60 && vsyncGuard == 0 && vsyncLimiter.Status == null, "focus loss clears only the fallback guard");
            Tick(vsync: 0);
            Check(target == 60 && vsyncGuard == 0, "VSync Off returns to Unity's normal cap with DLSS Off");
            settings.Upscaling.Value = true; Tick(vsync: 1); Tick();
            Check(limiter.Owned && target == -1 && native == 16667 && vsyncGuard == 0, "Reflex reacquisition clears the VSync guard to avoid double limiting");
            fps.Value = -1; Tick(vsync: 1);
            Check(!limiter.Owned && target == -1 && vsyncGuard == 0 && fps.Value == -1, "VSync preserves unlimited without adding a cap");
            fps.Value = 40;
            using var failedGuard = new VSyncFrameLimiter(() => target, value => value == 0 ? 0 : -1, _ => { });
            failedGuard.Tick(true, true, 1);
            Check(target == 40 && fps.Value == 40 && failedGuard.Status == "Unity limiter (VSync guard unavailable)",
                "timer failure preserves game settings and reports that the guard is unavailable");
            Console.WriteLine("PASS: F10/quality Off restores latest game FPS cap, release order, live edits while Off, resume with current cap, F11 independence and unlimited.");
            Console.WriteLine("PASS: VSync cap enforcement with DLSS On/Off, live shared cap changes, pause/focus, VSync counts, unlimited and no double limiting.");
        }
        finally { Directory.Delete(folder, true); }
    }
}
