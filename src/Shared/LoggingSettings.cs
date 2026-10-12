using BepInEx.Configuration;
using BepInEx.Logging;

namespace RiskOfRainUpscaling
{
    // Values also match the native logging API.
    internal enum ModLogLevel { Off, Error, Warning, Info, Debug }

    internal sealed class LoggingSettings
    {
        internal const string Section = "Diagnostics";
        internal readonly ConfigEntry<ModLogLevel> Level;

        internal LoggingSettings(ConfigFile config)
        {
            Level = config.Bind(Section, "Logging Level", ModLogLevel.Off,
                "How much detail the mod writes to its log files. Leave it Off unless you're reporting a problem; Debug records the most.");
        }

        internal bool Allows(ModLogLevel severity) => Allows(Level.Value, severity);
        internal static bool Allows(ModLogLevel level, ModLogLevel severity) =>
            severity >= ModLogLevel.Error && severity <= ModLogLevel.Debug && level >= severity;

        internal void Write(ManualLogSource source, ModLogLevel severity, string message)
        {
            if (!Allows(severity)) return;
            source.Log(severity == ModLogLevel.Error ? LogLevel.Error :
                severity == ModLogLevel.Warning ? LogLevel.Warning :
                severity == ModLogLevel.Debug ? LogLevel.Debug : LogLevel.Info, message);
        }
    }
}
