param(
    [string]$GamePath,
    [string]$ProfilePath,
    [string]$VisualStudioPath,
    [string]$RiskOfOptionsPath,
    [switch]$UseCachedReferences,
    [switch]$NoPause
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'Build-Helpers.ps1')
Start-Transcript -LiteralPath (Join-Path $PSScriptRoot 'Build.log') -Force | Out-Null
try {
# Load signature/hash commands before compilation so packaging prerequisites fail early.
Import-Module Microsoft.PowerShell.Security -ErrorAction Stop
$settingsPath = Join-Path $PSScriptRoot 'Build.local.psd1'
if (Test-Path -LiteralPath $settingsPath) {
    $settings = Import-PowerShellDataFile -LiteralPath $settingsPath
    foreach ($name in $settings.Keys) {
        if ($name -notin @('GamePath', 'ProfilePath', 'VisualStudioPath', 'RiskOfOptionsPath')) { throw "Unknown build setting: $name" }
        if (-not $PSBoundParameters.ContainsKey($name)) { Set-Variable -Name $name -Value $settings[$name] }
    }
}
$GamePath = Resolve-BuildGamePath $GamePath
$dotnet = Find-BuildTool 'dotnet.exe' @((Join-Path $env:ProgramFiles 'dotnet\dotnet.exe'), (Join-Path $env:LOCALAPPDATA 'Microsoft\dotnet\dotnet.exe')) 'Install the .NET SDK (the runtime alone is not enough).'
$sdks = & $dotnet --list-sdks
if ($LASTEXITCODE -ne 0 -or -not $sdks) { throw 'Install the .NET SDK (the runtime alone is not enough).' }
$git = Find-BuildTool 'git.exe' @((Join-Path $env:ProgramFiles 'Git\cmd\git.exe'), (Join-Path $env:LOCALAPPDATA 'Programs\Git\cmd\git.exe')) 'Install Git for Windows.'
if (-not $VisualStudioPath) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Install Visual Studio or Build Tools with Desktop development with C++, the Windows SDK and CMake tools; or specify -VisualStudioPath.' }
    $VisualStudioPath = (& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1)
}
if (-not $VisualStudioPath) { throw 'Install Visual Studio or Build Tools with Desktop development with C++ and the Windows SDK.' }
$devShell = Join-Path $VisualStudioPath 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll'
if (-not (Test-Path -LiteralPath $devShell)) { throw "Visual Studio developer tools missing: $devShell" }
Import-Module $devShell
Enter-VsDevShell -VsInstallPath $VisualStudioPath -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64'
$nmake = Join-Path $env:VCToolsInstallDir 'bin\Hostx64\x64\nmake.exe'
if (-not (Test-Path -LiteralPath $nmake)) { throw "x64 nmake not found: $nmake. Install the Visual Studio C++ tools." }
$cmake = Find-BuildTool 'cmake.exe' @((Join-Path $env:ProgramFiles 'CMake\bin\cmake.exe'), (Join-Path $VisualStudioPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe')) 'Install CMake 3.24 or newer, or the Visual Studio CMake tools.'
$fxc = Find-BuildTool 'fxc.exe' @((Join-Path $env:WindowsSdkDir "bin\$($env:WindowsSDKVersion)\x64\fxc.exe")) 'Install the Windows SDK through the Visual Studio installer.'
Write-Host "Game: $GamePath"
Write-Host "Compiler: $VisualStudioPath"
$referenceRoot = Join-Path $PSScriptRoot 'references\BepInEx'
New-Item -ItemType Directory -Path $referenceRoot -Force | Out-Null
$explicitOptions = -not [string]::IsNullOrWhiteSpace($RiskOfOptionsPath)
if (-not $RiskOfOptionsPath) { $RiskOfOptionsPath = Join-Path $PSScriptRoot 'references\RiskOfOptions.dll' }
if (-not $UseCachedReferences) {
    $needsOptions = -not $explicitOptions -and -not (Test-Path -LiteralPath $RiskOfOptionsPath -PathType Leaf)
    $ProfilePath = Resolve-BuildProfilePath $ProfilePath $GamePath $needsOptions
    if (-not $ProfilePath) {
        $cached = Test-Path -LiteralPath $RiskOfOptionsPath -PathType Leaf
        foreach ($name in 'BepInEx.dll', 'Mono.Cecil.dll', '0Harmony.dll') {
            $cached = $cached -and (Test-Path -LiteralPath (Join-Path $referenceRoot $name) -PathType Leaf)
        }
        if (-not $cached) { throw 'No usable BepInEx 5 profile was found. Install BepInEx and Rune580-Risk_Of_Options in an r2modman profile, or set ProfilePath in build/Build.local.psd1.' }
        $UseCachedReferences = $true
    }
}
if (-not $UseCachedReferences) {
    Write-Host "Build references: $ProfilePath"
    foreach ($name in 'BepInEx.dll', 'Mono.Cecil.dll', '0Harmony.dll') {
        $source = Join-Path $ProfilePath "BepInEx\core\$name"
        if (-not (Test-Path -LiteralPath $source)) { throw "Profile dependency missing: $source" }
        Copy-Item -LiteralPath $source -Destination (Join-Path $referenceRoot $name) -Force
    }
    if (-not $explicitOptions) {
        $installedOptions = Find-BuildOptions $ProfilePath
        if ($installedOptions) { Copy-Item -LiteralPath $installedOptions -Destination $RiskOfOptionsPath -Force }
    }
} else { Write-Host "Build references: cached in $referenceRoot" }
if (-not (Test-Path -LiteralPath $RiskOfOptionsPath)) { throw "Risk of Options build reference missing: $RiskOfOptionsPath" }
foreach ($name in 'BepInEx.dll', 'Mono.Cecil.dll', '0Harmony.dll') {
    if (-not (Test-Path -LiteralPath (Join-Path $referenceRoot $name))) { throw "Build reference missing: $name" }
}
$gameManaged = Join-Path $GamePath 'Risk of Rain 2_Data\Managed'
foreach ($name in 'RoR2.dll','UnityEngine.CoreModule.dll','com.unity.multiplayer-hlapi.Runtime.dll') {
    if (-not (Test-Path -LiteralPath (Join-Path $gameManaged $name))) { throw "Game dependency missing: $name" }
}
$sdkFiles = @('Streamline\include\sl.h', 'Streamline\license.txt', 'FidelityFX-SDK\Kits\FidelityFX\api\include\ffx_api.h', 'FidelityFX-SDK\Kits\FidelityFX\docs\license.md')
foreach ($name in 'amd_fidelityfx_loader_dx12.dll', 'amd_fidelityfx_framegeneration_dx12.dll', 'amd_fidelityfx_upscaler_dx12.dll') { $sdkFiles += "FidelityFX-SDK\Kits\FidelityFX\signedbin\$name" }
if (@($sdkFiles | Where-Object { -not (Test-Path -LiteralPath (Join-Path $projectRoot "third_party\$_") -PathType Leaf) }).Count -gt 0) {
    & (Join-Path $PSScriptRoot 'Fetch-SDK.ps1') -GitPath $git
}
$sdkCommit = & $git -C (Join-Path $projectRoot 'third_party\Streamline') rev-parse HEAD
if ($LASTEXITCODE -ne 0 -or "$sdkCommit".Trim() -ne '2122257e0fce486f91b385aa63b9a09b0a34b363') { throw 'Unexpected Streamline header version.' }
$ffxCommit = & $git -C (Join-Path $projectRoot 'third_party\FidelityFX-SDK') rev-parse HEAD
if ($LASTEXITCODE -ne 0 -or "$ffxCommit".Trim() -ne '60f4ea81909200d8542eca14dccb2628b763a9a3') { throw 'Unexpected FidelityFX SDK version.' }

Push-Location $projectRoot
try {
    foreach ($project in 'src/RiskOfRainUpscaling/RiskOfRainUpscaling.csproj','src/RiskOfRainUpscaling.Preloader/RiskOfRainUpscaling.Preloader.csproj') {
        & $dotnet build $project -c Release --nologo "-p:GamePath=$GamePath" "-p:ProfileCore=$referenceRoot" "-p:RiskOfOptionsPath=$RiskOfOptionsPath"
        if ($LASTEXITCODE -ne 0) { throw "Managed build failed: $project" }
    }
    # An absolute NMake path also works when the host supplies duplicate Path/PATH variables.
    & $cmake -S native -B build/native-nmake -G 'NMake Makefiles' -DCMAKE_BUILD_TYPE=Release "-DCMAKE_MAKE_PROGRAM=$nmake" "-DFXC_EXECUTABLE=$fxc"
    if ($LASTEXITCODE -ne 0) { throw 'Native configure failed.' }
    & $cmake --build build/native-nmake
    if ($LASTEXITCODE -ne 0) { throw 'Native build failed.' }

    $packageRoot = Join-Path $PSScriptRoot 'package'
    # Start from an empty tree: files an earlier build shipped must not reach the package or its manifest.
    $packageBepInEx = Join-Path $packageRoot 'BepInEx'
    if (Test-Path -LiteralPath $packageBepInEx) { Remove-Item -LiteralPath $packageBepInEx -Recurse -Force }
    $plugin = Join-Path $packageRoot 'BepInEx\plugins\RiskOfRainUpscaling'
    $patcher = Join-Path $packageRoot 'BepInEx\patchers\RiskOfRainUpscaling'
    New-Item -ItemType Directory -Path $plugin,$patcher -Force | Out-Null
    Copy-Item -LiteralPath 'src\RiskOfRainUpscaling\bin\Release\netstandard2.1\RiskOfRainUpscaling.dll' -Destination $plugin -Force
    Copy-Item -LiteralPath 'src\RiskOfRainUpscaling.Preloader\bin\Release\netstandard2.1\RiskOfRainUpscaling.Preloader.dll','build\native-nmake\RiskOfRainUpscaling.Native.dll' -Destination $patcher -Force
    $runtimeSets = @{
        D3D11 = @('sl.interposer.dll','sl.common.dll','sl.dlss.dll','nvngx_dlss.dll','sl.reflex.dll','sl.pcl.dll')
        D3D12 = @('sl.interposer.dll','sl.common.dll','sl.dlss_g.dll','nvngx_dlssg.dll','sl.reflex.dll','sl.pcl.dll')
    }
    foreach ($api in $runtimeSets.Keys) {
        $destination = Join-Path $plugin "runtimes\$api"
        New-Item -ItemType Directory -Path $destination -Force | Out-Null
        foreach ($name in $runtimeSets[$api]) {
            $source = Join-Path $projectRoot "third_party\NVIDIA-DLSS\$name"
            $signature = Get-AuthenticodeSignature -LiteralPath $source
            if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch 'NVIDIA') { throw "Invalid NVIDIA signature: $source" }
            Copy-Item -LiteralPath $source -Destination $destination -Force
        }
    }
    # AMD FidelityFX loader and effect DLLs, loaded by absolute path from runtimes\FSR.
    $ffxBin = Join-Path $projectRoot 'third_party\FidelityFX-SDK\Kits\FidelityFX\signedbin'
    $ffxDestination = Join-Path $plugin 'runtimes\FSR'
    New-Item -ItemType Directory -Path $ffxDestination -Force | Out-Null
    foreach ($name in 'amd_fidelityfx_loader_dx12.dll','amd_fidelityfx_framegeneration_dx12.dll','amd_fidelityfx_upscaler_dx12.dll') {
        $source = Join-Path $ffxBin $name
        $signature = Get-AuthenticodeSignature -LiteralPath $source
        if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch 'Advanced Micro Devices') { throw "Invalid AMD signature: $source" }
        Copy-Item -LiteralPath $source -Destination $ffxDestination -Force
    }
    Copy-Item -LiteralPath 'third_party\Streamline\license.txt' -Destination (Join-Path $plugin 'Streamline-LICENSE.txt') -Force
    Copy-Item -LiteralPath 'third_party\FidelityFX-SDK\Kits\FidelityFX\docs\license.md' -Destination (Join-Path $plugin 'FidelityFX-SDK-LICENSE.md') -Force
    Copy-Item -LiteralPath 'third_party\FidelityFX-FSR\LICENSE.txt' -Destination (Join-Path $plugin 'FidelityFX-FSR-LICENSE.txt') -Force
    Copy-Item -LiteralPath 'LICENSE' -Destination (Join-Path $plugin 'LICENSE.txt') -Force
    Copy-Item -LiteralPath 'THIRD_PARTY_NOTICES.md' -Destination $plugin -Force
    Get-ChildItem -LiteralPath $packageRoot -Filter '*.md' -File | Remove-Item -Force  # Docs from earlier builds.
    $packagedDocs = 'testing\UPSCALING_TESTING.md','testing\FRAME_GENERATION_TESTING.md','testing\FSR_TESTING.md','testing\UI_RECOMPOSITION_TESTING.md','testing\OPTIONS_TESTING.md','testing\STARTUP_TESTING.md','DIAGNOSTICS.md'
    foreach ($doc in $packagedDocs) { Copy-Item -LiteralPath (Join-Path 'docs' $doc) -Destination (Join-Path $packageRoot (Split-Path -Leaf $doc)) -Force }
    Copy-Item -LiteralPath 'README.md','CHANGELOG.md' -Destination $packageRoot -Force
    $files = Get-ChildItem -LiteralPath (Join-Path $packageRoot 'BepInEx') -Recurse -File | Where-Object { $_.Name -ne 'dependency-manifest.json' } | Sort-Object FullName | ForEach-Object {
        [ordered]@{path=(Get-BuildRelativePath $packageRoot $_.FullName);length=$_.Length;version=$_.VersionInfo.FileVersion;sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash}
    }
    $version = [Reflection.AssemblyName]::GetAssemblyName((Join-Path $plugin 'RiskOfRainUpscaling.dll')).Version.ToString(3)
    $manifest = [ordered]@{version=$version;builtUtc=[DateTime]::UtcNow.ToString('O');
        target='Unity 2021.3 Mono Windows x64';bepInEx=[Reflection.AssemblyName]::GetAssemblyName((Join-Path $referenceRoot 'BepInEx.dll')).FullName;
        streamLineSdk='2.14.1';sdkCommit=$sdkCommit.Trim();fidelityFxSdk='2.3.0';fidelityFxCommit=$ffxCommit.Trim();frameGenerationBackends='[Frame Generation] Backend: Auto (DLSS on supported NVIDIA GPUs, otherwise FSR), DLSS, FSR (2x); restart to change';upscalerBackends='[Upscaling] Upscaler: Auto (DLSS on supported NVIDIA GPUs, otherwise FSR 3.1/4 through a same-adapter D3D12 queue), DLSS, FSR; live';dlssRendering=$true;reconstructionDefault='DLAA';renderScale=1.0;dlssQualityModes='DLAA (default), Quality, Balanced, Performance, UltraPerformance; F7 cycles; lower modes compose the world into the HUD camera target';frameGeneration=$true;frameGenerationMultiplier=2;frameGenerationMultipliers=@(2,3,4,5,6);frameGenerationDynamic='DLSS only (bIsDynamicMFGSupported: RTX 50-series, driver 595.41+); output target = fps_max, unlimited = refresh rate; otherwise the highest supported multiplier; FSR 2x';frameGenerationFallback='highest supported GPU/driver multiplier';
        presentationBridge=$true;presentationDefault='D3D11 when Frame Generation Off at startup; D3D12 bridge when On at startup; live Off retains bridge';presentationConfiguration='BepInEx/config/xanderfrangos.riskofrainupscaling.cfg [Frame Generation]: Enabled and Multiplier apply live in D3D12; restart to enable from D3D11';
        reflexFrameLimiterDefault=$true;reflexCapGuard=$true;mainMenuDefault=$true;rcasDefault=0.7;dlaaPlacement='BeforePostProcessing (HDR; DLAA in place, DLSS upscaled before display-sized post-processing); End of Effects only for a camera without post-processing or, for the session, after an unsupported scene image effect';fovCutMinimumDegrees=5;fovCutRateDegreesPerSecond=180;
        riskOfOptions='optional: com.rune580.riskofoptions';liveOptions='Upscaler, Upscaler Quality, Outlines, RCAS Sharpening, Framerate Limit, Frame Generation Off/on, 2x–6x and Dynamic in D3D12, Logging Level, Enable Hotkeys and five key bindings';restartOptions='Enable DLSS Frame Generation from D3D11';loggingLevel='Off/Error/Warning/Info/Debug; live managed/native filtering and saved startup preference';hotkeys='Enable Hotkeys and five live rebindable shortcuts; default F6-F8, F10, F11; key combinations and None supported';
        debugOverlayDefault=$false;hudAlphaCapture='current-frame native HUD draw opacity, R8; UI recomposition guarded by HUD materials/image effects/overlays';uiRecomposition='always on';files=@($files)}
    $manifest | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $plugin 'dependency-manifest.json') -Encoding utf8
    Write-Output "Risk of Rain Upscaling $version package ready: $packageRoot"

    # Thunderstore package: manifest, README and icon are kept in Package; the BepInEx tree and CHANGELOG are replaced each build.
    $thunderstoreRoot = Join-Path $projectRoot 'Package'
    $thunderstoreManifest = Join-Path $thunderstoreRoot 'manifest.json'
    if (-not (Test-Path -LiteralPath $thunderstoreManifest -PathType Leaf)) { throw "Thunderstore manifest missing: $thunderstoreManifest" }
    Copy-Item -LiteralPath 'CHANGELOG.md' -Destination $thunderstoreRoot -Force
    $thunderstoreBepInEx = Join-Path $thunderstoreRoot 'BepInEx'
    if (Test-Path -LiteralPath $thunderstoreBepInEx) { Remove-Item -LiteralPath $thunderstoreBepInEx -Recurse -Force }
    Copy-Item -LiteralPath (Join-Path $packageRoot 'BepInEx') -Destination $thunderstoreRoot -Recurse -Force
    $manifestText = [IO.File]::ReadAllText($thunderstoreManifest)
    $versionPattern = '("version_number"\s*:\s*")[^"]*(")'
    if ($manifestText -notmatch $versionPattern) { throw "version_number missing from $thunderstoreManifest" }
    $updatedManifest = [regex]::Replace($manifestText, $versionPattern, "`${1}$version`${2}")
    if ($updatedManifest -ne $manifestText) { [IO.File]::WriteAllText($thunderstoreManifest, $updatedManifest) }
    Write-Output "Thunderstore package $version ready: $thunderstoreRoot"
}
finally { Pop-Location }
}
catch {
    Write-Host ($_ | Out-String) -ForegroundColor Red
    throw
}
finally { Stop-Transcript | Out-Null }

