using System;
using System.IO;
using BepInEx.Configuration;
using RiskOfRainUpscaling;

internal static class FrameGenerationSettingTests
{
    internal static void Run()
    {
        string folder = Path.Combine(Path.GetTempPath(), "RiskOfRainUpscaling.StartupSettingTests." + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(folder);
        try
        {
            string path = Path.Combine(folder, "mod.cfg");
            void Check(bool condition, string message) { if (!condition) throw new Exception("Frame Generation startup: " + message); }
            var fresh = new FrameGenerationSetting(new ConfigFile(Path.Combine(folder, "fresh.cfg"), false));
            Check(!fresh.EnabledAtStartup && !fresh.LiveEnabled && !fresh.RestartRequired && fresh.MultiplierAtStartup == 2 &&
                fresh.Backend.Value == FrameGenerationBackend.Auto, "fresh config starts Off with saved 2x Auto");
            File.WriteAllText(path, "[" + FrameGenerationSetting.Section + "]\nEnabled = false\n");
            var config = new ConfigFile(path, false);
            var setting = new FrameGenerationSetting(config);
            setting.SetPresentationState(0);
            Check(!setting.EnabledAtStartup && !setting.RestartRequired && !setting.LiveEnabled, "D3D11 Off startup");
            setting.Requested.Value = true;
            setting.Multiplier.Value = 4;
            Check(!setting.EnabledAtStartup && setting.RestartRequired && setting.LiveMultiplier == 0, "Off startup cannot enable a multiplier live");
            setting.Multiplier.Value = 2;
            config.Save();
            var restarted = new FrameGenerationSetting(new ConfigFile(path, false));
            restarted.SetPresentationState(2);
            Check(restarted.EnabledAtStartup && restarted.MultiplierAtStartup == 2 && !restarted.RestartRequired, "saved On starts 2x");
            foreach (int multiplier in new[] { 3, 4, 5, 6 })
            {
                restarted.Multiplier.Value = multiplier;
                Check(!restarted.RestartRequired && restarted.LiveMultiplier == multiplier && restarted.MultiplierAtStartup == multiplier - 1, "enabled multiplier applies live " + multiplier);
                restarted.Requested.ConfigFile.Save();
                restarted = new FrameGenerationSetting(new ConfigFile(path, false));
                Check(restarted.EnabledAtStartup && restarted.MultiplierAtStartup == multiplier && !restarted.RestartRequired, "restart applies " + multiplier);
            }
            restarted.Multiplier.Value = 3;
            Check(!restarted.RestartRequired && restarted.LiveMultiplier == 3, "enabled-to-enabled change needs no restart");
            restarted.Multiplier.Value = 6;
            Check(!restarted.RestartRequired, "reverting multiplier cancels restart");
            restarted.Requested.Value = false;
            Check(restarted.RuntimeAvailable && !restarted.RestartRequired && !restarted.LiveEnabled && restarted.LiveMultiplier == 0, "D3D12 Off applies live without releasing runtime");
            restarted.Multiplier.Value = 4;
            Check(!restarted.RestartRequired && restarted.LiveMultiplier == 0, "multiplier stays saved while Off");
            restarted.Requested.Value = true;
            Check(restarted.RuntimeAvailable && !restarted.RestartRequired && restarted.LiveEnabled && restarted.LiveMultiplier == 4, "D3D12 re-enables at saved multiplier live");
            Check(!fresh.Dynamic.Value && !restarted.LiveDynamic, "Dynamic defaults Off");
            restarted.Dynamic.Value = true;
            Check(!restarted.RestartRequired && restarted.LiveDynamic && restarted.LiveMultiplier == 4, "Dynamic applies live and keeps the fixed multiplier");
            restarted.Requested.Value = false;
            Check(!restarted.LiveDynamic && restarted.Dynamic.Value, "Dynamic stays saved while Off");
            restarted.Requested.Value = true; restarted.Requested.ConfigFile.Save();
            var dynamicRestart = new FrameGenerationSetting(new ConfigFile(path, false));
            dynamicRestart.SetPresentationState(2);
            Check(dynamicRestart.LiveDynamic && dynamicRestart.MultiplierAtStartup == 4, "Dynamic survives restart");
            dynamicRestart.SetPresentationState(-1);
            Check(!dynamicRestart.LiveDynamic && dynamicRestart.RestartRequired, "native D3D11 fallback cannot run Dynamic");
            restarted.Dynamic.Value = false;
            foreach (int multiplier in new[] { 2, 4, 6 })
            {
                restarted.Multiplier.Value = multiplier;
                restarted.Requested.Value = false;
                restarted.SetPresentationState(2);
                Check(!restarted.RestartRequired && !restarted.LiveEnabled && restarted.RuntimeAvailable, "D3D12 keeps runtime while Off");
                restarted.Requested.Value = true;
                Check(!restarted.RestartRequired && restarted.LiveMultiplier == multiplier, "repeat D3D12 Off/on at " + multiplier);
            }
            restarted.SetPresentationState(-1);
            Check(restarted.RestartRequired && !restarted.LiveEnabled, "native D3D11 fallback requires restart to enable");
            restarted.Requested.Value = false;
            Check(!restarted.RestartRequired, "D3D11 Off never needs restart");
            restarted.SetPresentationState(2);
            restarted.Multiplier.Value = 6;
            restarted.Requested.Value = false; restarted.Requested.ConfigFile.Save();
            var off = new FrameGenerationSetting(new ConfigFile(path, false));
            Check(!off.EnabledAtStartup && off.MultiplierAtStartup == 6 && !off.RestartRequired, "Off preserves multiplier");
            off.Multiplier.Value = -1; Check(off.Multiplier.Value == 2 && !off.RestartRequired, "clamp below range while Off");
            off.Multiplier.Value = 99; Check(off.Multiplier.Value == 6, "clamp above range");
            off.Requested.Value = true;
            Check(off.RestartRequired && !off.LiveEnabled, "restart with Off saved returns to D3D11");
            off.Requested.Value = false;
            Check(!off.RestartRequired && !off.LiveEnabled, "reverting pending enable cancels restart");
            File.WriteAllText(path, "[" + FrameGenerationSetting.Section + "]\nEnabled = true\nMultiplier = invalid\n");
            var invalid = new FrameGenerationSetting(new ConfigFile(path, false));
            Check(invalid.MultiplierAtStartup == 2, "invalid multiplier defaults to 2x");
            Console.WriteLine("PASS: fresh Off default with saved 2x, live D3D12 Off/on, multipliers and Dynamic, D3D11 enable restart/revert, native fallback, saved Off startup, bounds and persistence.");
        }
        finally { Directory.Delete(folder, true); }
    }
}
