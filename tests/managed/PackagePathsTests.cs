using System;
using System.IO;
using RiskOfRainUpscaling;

internal static class PackagePathsTests
{
    internal static void Run()
    {
        var root = Path.Combine(Path.GetTempPath(), "RiskOfRainUpscaling-path-tests-" + Guid.NewGuid());
        try
        {
            var plugin = Path.Combine(root, "plugins", "TestAuthor-TestPackage", "nested");
            var patcher = Path.Combine(root, "patchers", "TestAuthor-TestPackage");
            Directory.CreateDirectory(plugin);
            Directory.CreateDirectory(patcher);
            File.WriteAllText(Path.Combine(plugin, "RiskOfRainUpscaling.dll"), "fixture");
            File.WriteAllText(Path.Combine(patcher, "RiskOfRainUpscaling.Native.dll"), "fixture");
            if (PackagePaths.RuntimeFolder(Path.Combine(root, "plugins")) != Path.Combine(plugin, "runtimes"))
                throw new Exception("Runtime discovery failed in renamed/nested package folder");
            if (PackagePaths.FindFile(Path.Combine(root, "patchers"), "RiskOfRainUpscaling.Native.dll") != Path.Combine(patcher, "RiskOfRainUpscaling.Native.dll"))
                throw new Exception("Native companion discovery failed in renamed package folder");
            bool rejected = false;
            try { PackagePaths.FindFile(root, "Missing.dll"); }
            catch (InvalidOperationException) { rejected = true; }
            if (!rejected) throw new Exception("Missing package was accepted");
            File.WriteAllText(Path.Combine(root, "RiskOfRainUpscaling.dll"), "duplicate");
            rejected = false;
            try { PackagePaths.FindFile(root, "RiskOfRainUpscaling.dll"); }
            catch (InvalidOperationException) { rejected = true; }
            if (!rejected) throw new Exception("Duplicate package was accepted");
            Console.WriteLine("PASS: renamed/nested package discovery; missing and duplicate installations rejected.");
        }
        finally { if (Directory.Exists(root)) Directory.Delete(root, true); }
    }
}
