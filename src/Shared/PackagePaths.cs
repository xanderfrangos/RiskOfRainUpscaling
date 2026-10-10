using System;
using System.IO;

namespace RiskOfRainUpscaling
{
    // Find the package by its assembly, since mod managers rename its directory.
    internal static class PackagePaths
    {
        internal static string FindFile(string root, string name)
        {
            var matches = Directory.GetFiles(root, name, SearchOption.AllDirectories);
            if (matches.Length != 1)
                throw new InvalidOperationException("Expected one " + name + " under " + root + "; found " + matches.Length + ". Check for missing or duplicate installations.");
            return matches[0];
        }

        internal static string RuntimeFolder(string pluginRoot)
        {
            return Path.Combine(Path.GetDirectoryName(FindFile(pluginRoot, "RiskOfRainUpscaling.dll")), "runtimes");
        }
    }
}
