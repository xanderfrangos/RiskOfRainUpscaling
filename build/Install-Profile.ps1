param(
    [string]$ProfilePath
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Build-Helpers.ps1')
if (Get-Process -Name 'Risk of Rain 2' -ErrorAction SilentlyContinue) { throw 'Close Risk of Rain 2 before installing.' }
# Like Build.ps1: parameters first, then build/Build.local.psd1, then discovery.
$settingsPath = Join-Path $PSScriptRoot 'Build.local.psd1'
if (Test-Path -LiteralPath $settingsPath) {
    $settings = Import-PowerShellDataFile -LiteralPath $settingsPath
    if (-not $ProfilePath) { $ProfilePath = $settings.ProfilePath }
}
# Discovery prefers a profile that already has this mod, then the newest BepInEx 5 profile.
if (-not $ProfilePath) { $ProfilePath = Resolve-BuildProfilePath $null $null $false }
if (-not $ProfilePath) { throw 'No BepInEx 5 mod profile was found. Pass -ProfilePath or set ProfilePath in build/Build.local.psd1.' }
$profileRoot = [IO.Path]::GetFullPath($ProfilePath)
Write-Output "Profile: $profileRoot"
if (-not (Test-Path -LiteralPath (Join-Path $profileRoot 'BepInEx\core\BepInEx.dll'))) { throw 'The destination is not a BepInEx 5 mod profile.' }
$configPath = Join-Path $profileRoot 'BepInEx\config\xanderfrangos.riskofrainupscaling.cfg'
$configBefore = if (Test-Path -LiteralPath $configPath) { (Get-FileHash -LiteralPath $configPath -Algorithm SHA256).Hash } else { $null }
$packageRoot = Join-Path $PSScriptRoot 'package'
$manifestPath = Join-Path $packageRoot 'BepInEx\plugins\RiskOfRainUpscaling\dependency-manifest.json'
if (-not (Test-Path -LiteralPath $manifestPath)) { throw 'Build the package first.' }
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
foreach ($file in $manifest.files) {
    $source = [IO.Path]::GetFullPath((Join-Path $packageRoot $file.path))
    if (-not $source.StartsWith([IO.Path]::GetFullPath($packageRoot) + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Manifest path escapes package.' }
    if ((Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash -ne $file.sha256) { throw "Package hash mismatch: $source" }
}
$backupRoot = Join-Path $PSScriptRoot ('profile-backups\' + [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss'))
foreach ($relative in 'BepInEx\plugins\RiskOfRainUpscaling','BepInEx\patchers\RiskOfRainUpscaling') {
    $sourceFolder = Join-Path $packageRoot $relative
    $profileRelative = Get-BuildProfileRelativePath $relative
    $destination = [IO.Path]::GetFullPath((Join-Path $profileRoot $profileRelative))
    if (-not $destination.StartsWith($profileRoot.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Destination escapes profile.' }
    if (Test-Path -LiteralPath $destination) {
        $backup = Join-Path $backupRoot $profileRelative
        New-Item -ItemType Directory -Path $backup -Force | Out-Null
        Copy-Item -LiteralPath $destination -Destination (Split-Path -Parent $backup) -Recurse -Force
    }
    New-Item -ItemType Directory -Path $destination -Force | Out-Null
    # Windows PowerShell 5.1 has no [IO.Path]::GetRelativePath.
    $sourcePrefix = [IO.Path]::GetFullPath($sourceFolder).TrimEnd('\') + '\'
    $shipped = @{}
    Get-ChildItem -LiteralPath $sourceFolder -File -Recurse | ForEach-Object {
        $relativeFile = $_.FullName.Substring($sourcePrefix.Length)
        $shipped[$relativeFile] = $true
        $target = Join-Path $destination $relativeFile
        New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force | Out-Null
        Copy-Item -LiteralPath $_.FullName -Destination $target -Force
        if ((Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash -ne (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash) { throw "Installed file mismatch: $target" }
    }
    # Files of an earlier version that this package no longer ships (backed up above).
    $destinationPrefix = $destination.TrimEnd('\') + '\'
    Get-ChildItem -LiteralPath $destination -File -Recurse | Where-Object { -not $shipped.ContainsKey($_.FullName.Substring($destinationPrefix.Length)) } | Remove-Item -Force
}
$verified = 0
foreach ($file in $manifest.files) {
    $installed = Join-Path $profileRoot (Get-BuildProfileRelativePath $file.path)
    if ((Get-FileHash -LiteralPath $installed -Algorithm SHA256).Hash -ne $file.sha256) { throw "Installed manifest mismatch: $installed" }
    ++$verified
}
if ($configBefore -and (Get-FileHash -LiteralPath $configPath -Algorithm SHA256).Hash -ne $configBefore) { throw 'Existing BepInEx configuration was changed.' }
$receipt = Join-Path $PSScriptRoot ("profile-installation-" + $manifest.version.Replace('.','') + '.json')
[ordered]@{version=$manifest.version;assembly=[Reflection.AssemblyName]::GetAssemblyName((Join-Path $profileRoot (Get-BuildProfileRelativePath 'BepInEx\plugins\RiskOfRainUpscaling\RiskOfRainUpscaling.dll'))).FullName;
    verifiedFiles=$verified;configSha256=$(if (Test-Path -LiteralPath $configPath) { (Get-FileHash -LiteralPath $configPath -Algorithm SHA256).Hash } else { $null });configBeforeSha256=$configBefore;
    backup=$backupRoot;profilePath=$profileRoot;installedUtc=[DateTime]::UtcNow.ToString('O')} |
    ConvertTo-Json | Set-Content -LiteralPath $receipt -Encoding utf8
Write-Output "Installed Risk of Rain Upscaling $($manifest.version) into $profileRoot. Launch using r2modman's Start modded."
Write-Output "Verified $verified files; receipt=$receipt; backup=$backupRoot"
