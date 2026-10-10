$ErrorActionPreference = 'Stop'
$fixtureRoot = Join-Path $PSScriptRoot ('test-results\profile-install-' + [guid]::NewGuid())
$assembly = Join-Path $PSScriptRoot 'package\BepInEx\plugins\RiskOfRainUpscaling\RiskOfRainUpscaling.dll'
if (-not (Test-Path -LiteralPath $assembly)) { throw 'Build the package before testing profile installation.' }
foreach ($case in 'fresh','upgrade') {
    $root = Join-Path $fixtureRoot $case
    $profile = Join-Path $root 'profile'
    $package = Join-Path $root 'package'
    $plugin = Join-Path $package 'BepInEx\plugins\RiskOfRainUpscaling'
    $patcher = Join-Path $package 'BepInEx\patchers\RiskOfRainUpscaling'
    $core = Join-Path $profile 'BepInEx\core'
    New-Item -ItemType Directory -Path $plugin,$patcher,$core -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'Install-Profile.ps1'),(Join-Path $PSScriptRoot 'Build-Helpers.ps1') -Destination $root
    Copy-Item -LiteralPath $assembly -Destination $plugin
    'native fixture' | Set-Content -LiteralPath (Join-Path $patcher 'RiskOfRainUpscaling.Native.dll')
    'core fixture' | Set-Content -LiteralPath (Join-Path $core 'BepInEx.dll')
    $configFolder = Join-Path $profile 'BepInEx\config'
    New-Item -ItemType Directory -Path $configFolder -Force | Out-Null
    $configPath = Join-Path $configFolder 'xanderfrangos.riskofrainupscaling.cfg'
    "[Frame Generation]`r`nEnabled = false`r`n[Upscaling]`r`nSharpening = 0.35`r`n" | Set-Content -LiteralPath $configPath
    $configBefore = (Get-FileHash -LiteralPath $configPath).Hash
    # Another mod's plugin must be left alone.
    $otherPlugin = Join-Path $profile 'BepInEx\plugins\OtherAuthor-OtherMod\Other.dll'
    New-Item -ItemType Directory -Path (Split-Path -Parent $otherPlugin) -Force | Out-Null
    'other mod fixture' | Set-Content -LiteralPath $otherPlugin
    $previousFile = Join-Path $profile 'BepInEx\plugins\XanderChaos-RiskOfRainUpscaling\RiskOfRainUpscaling\RiskOfRainUpscaling.dll'
    if ($case -eq 'upgrade') {
        New-Item -ItemType Directory -Path (Split-Path -Parent $previousFile) -Force | Out-Null
        'previous version fixture' | Set-Content -LiteralPath $previousFile
    }
    $files = foreach ($source in (Join-Path $plugin 'RiskOfRainUpscaling.dll'),(Join-Path $patcher 'RiskOfRainUpscaling.Native.dll')) {
        @{path=[IO.Path]::GetRelativePath($package,$source);sha256=(Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash}
    }
    @{version='1.0.0';files=@($files)} | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $plugin 'dependency-manifest.json')
    & (Join-Path $root 'Install-Profile.ps1') -ProfilePath $profile | Out-Null
    foreach ($file in $files) {
        $installed = Join-Path $profile ($file.path -replace '^BepInEx\\(plugins|patchers)\\', 'BepInEx\$1\XanderChaos-RiskOfRainUpscaling\')
        if ((Get-FileHash -LiteralPath $installed -Algorithm SHA256).Hash -ne $file.sha256) { throw "Installed file mismatch: $($file.path) ($case)" }
    }
    $backups = @(Get-ChildItem -LiteralPath (Join-Path $root 'profile-backups') -Filter 'RiskOfRainUpscaling.dll' -Recurse -File -ErrorAction SilentlyContinue)
    if ($case -eq 'upgrade' -and ($backups.Count -ne 1 -or (Get-Content -LiteralPath $backups[0].FullName) -ne 'previous version fixture')) { throw 'Previous install was not backed up.' }
    if ($case -eq 'fresh' -and $backups.Count -ne 0) { throw 'Fresh install created a plugin backup.' }
    if (-not (Test-Path -LiteralPath $otherPlugin)) { throw "Another mod's plugin was removed: $case" }
    if (Test-Path -LiteralPath (Join-Path $profile 'BepInEx\plugins\RiskOfRainUpscaling')) { throw "Installed beside the mod-manager folder: $case" }
    if ((Get-FileHash -LiteralPath $configPath).Hash -ne $configBefore) { throw "BepInEx config changed during install: $case" }
}
Write-Output 'PASS: profile install verifies installed hashes, backs up a previous install, and preserves the config and other mods.'
