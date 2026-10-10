param([string]$TestOutput, [string]$CecilPath)
$ErrorActionPreference = 'Stop'
Add-Type -Path $CecilPath
# The profile's pure managed BepInEx DLL has Required32Bit set. Unity's Mono
# accepts it, but the x64 .NET test host does not. Clear only that flag in the
# test output copy; build references and shipped assemblies are untouched.
$path = Join-Path $TestOutput 'BepInEx.dll'
$temporary = $null
$module = [Mono.Cecil.ModuleDefinition]::ReadModule($path)
try {
    $required32 = [int][Mono.Cecil.ModuleAttributes]::Required32Bit
    if (([int]$module.Attributes -band $required32) -ne 0) {
        $module.Attributes = [Mono.Cecil.ModuleAttributes]([int]$module.Attributes -band (-bnot $required32))
        $temporary = "$path.test-copy"
        $module.Write($temporary)
    }
}
finally { $module.Dispose() }
if ($temporary) { Move-Item -LiteralPath $temporary -Destination $path -Force }
