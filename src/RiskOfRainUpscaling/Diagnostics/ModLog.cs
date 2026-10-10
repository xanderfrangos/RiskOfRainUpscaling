using System;
using System.IO;

namespace RiskOfRainUpscaling
{
    // Leveled mod logging: each message is checked against the live Logging Level,
    // sent to a sink (BepInEx) and appended to an optional log file kept open with
    // AutoFlush. Free of Unity types so tests can construct it with a lambda.
    internal sealed class ModLog : IDisposable
    {
        private readonly Func<ModLogLevel, bool> allows;
        private readonly Action<ModLogLevel, string> sink;
        private readonly object fileLock = new object();
        private StreamWriter file;

        internal ModLog(Func<ModLogLevel, bool> allows, Action<ModLogLevel, string> sink, string path = null)
        {
            this.allows = allows; this.sink = sink;
            if (path == null) return;
            try { file = new StreamWriter(new FileStream(path, FileMode.Create, FileAccess.Write, FileShare.ReadWrite)) { AutoFlush = true }; }
            catch (Exception exception) { sink?.Invoke(ModLogLevel.Warning, "Log file unavailable (" + path + "): " + exception.Message); }
        }
        // Everything, unfiltered, to one callback.
        internal static ModLog To(Action<string> sink) => new ModLog(_ => true, (_, message) => sink(message));

        internal bool IsEnabled(ModLogLevel level) => allows(level);
        internal void Write(ModLogLevel level, string message)
        {
            if (!allows(level)) return;
            sink?.Invoke(level, message);
            lock (fileLock)
            {
                if (file == null) return;
                try { file.WriteLine(DateTime.UtcNow.ToString("O") + " " + level.ToString().ToUpperInvariant() + " " + message); }
                catch (Exception) { file.Dispose(); file = null; }
            }
        }
        internal void Error(string message) => Write(ModLogLevel.Error, message);
        internal void Warning(string message) => Write(ModLogLevel.Warning, message);
        internal void Info(string message) => Write(ModLogLevel.Info, message);
        internal void Debug(string message) => Write(ModLogLevel.Debug, message);
        // Builds the message only when Debug is enabled.
        internal void Debug(Func<string> message) { if (allows(ModLogLevel.Debug)) Write(ModLogLevel.Debug, message()); }

        public void Dispose() { lock (fileLock) { file?.Dispose(); file = null; } }
    }
}
