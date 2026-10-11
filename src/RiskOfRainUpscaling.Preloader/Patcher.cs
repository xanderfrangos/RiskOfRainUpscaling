using System;
using System.Collections.Generic;
using System.IO;
using System.Reflection;
using BepInEx;
using BepInEx.Configuration;
using BepInEx.Logging;
using Mono.Cecil;

namespace RiskOfRainUpscaling.Preloader
{
    public static class Patcher
    {
        public static IEnumerable<string> TargetDLLs { get { return new string[0]; } }
        public static void Patch(AssemblyDefinition assembly) { }

        public static void Initialize()
        {
            var log = Logger.CreateLogSource("RiskOfRainUpscaling.Startup");
            LoggingSettings logging = null;
            try
            {
                // The plugin GUID names this file; native startup reads it before the plugin loads.
                var config = new ConfigFile(Path.Combine(Paths.ConfigPath, "xanderfrangos.riskofrainupscaling.cfg"), true);
                logging = new LoggingSettings(config);
                if (!config.Bind(LoggingSettings.Section, "TraceGraphicsStartup", true, "Install the early graphics hooks that DLSS, the D3D12 presentation bridge and Frame Generation require (their startup is also logged to native.log). Disabling this keeps Frame Generation off and DLSS unavailable (Auto upscaling uses FSR). Requires restart.").Value)
                {
                    logging.Write(log, ModLogLevel.Warning, "Graphics startup tracing disabled; Frame Generation stays OFF and DLSS is unavailable (Auto upscaling uses FSR). Enable TraceGraphicsStartup and restart to install early hooks.");
                    return;
                }
                var folder = Path.GetDirectoryName(Assembly.GetExecutingAssembly().Location);
                var runtimes = PackagePaths.RuntimeFolder(Paths.PluginPath);
                var generation = new FrameGenerationSetting(config);
                logging.Write(log, ModLogLevel.Info, "Frame Generation startup=" + (generation.EnabledAtStartup ? generation.MultiplierAtStartup + "x; backend=" + generation.Backend.Value + "; D3D12 presentation" : "Off; D3D11 presentation") + "; Off/on and multipliers apply live in D3D12; enabling from D3D11 requires restart.");
                NativeApi.Load(Path.Combine(folder, "RiskOfRainUpscaling.Native.dll"));
                NativeApi.SetLoggingLevel((int)logging.Level.Value);
                var result = NativeApi.Initialize(Path.Combine(Paths.BepInExRootPath, "RiskOfRainUpscaling-Diagnostics"),
                    runtimes, "preloader");
                if (result <= 0)
                    logging.Write(log, ModLogLevel.Warning, "No new early graphics hooks installed; Frame Generation requires observed device and swap-chain creation. See RiskOfRainUpscaling-Diagnostics/native.log.");
                else
                    logging.Write(log, ModLogLevel.Info, "Startup observer installed; patched import slots=" + result + ". See RiskOfRainUpscaling-Diagnostics/native.log.");
            }
            catch (Exception ex)
            {
                string message = "Startup observer unavailable; Frame Generation stays OFF and the original renderer continues. " + ex;
                if (logging == null) log.LogError(message);
                else logging.Write(log, ModLogLevel.Error, message);
            }
        }
    }
}
