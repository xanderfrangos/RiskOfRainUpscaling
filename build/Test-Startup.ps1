$ErrorActionPreference = 'Stop'
$fixtureRoot = Join-Path $PSScriptRoot 'native-nmake\startup-test'
$profileRoot = Join-Path $fixtureRoot 'profile with spaces'
# Mod managers use Author-Package directories; avoid assuming RiskOfRainUpscaling.
$patcher = Join-Path $profileRoot 'BepInEx\patchers\TestAuthor-TestPackage'
$plugin = Join-Path $profileRoot 'BepInEx\plugins\TestAuthor-TestPackage'
$logs = Join-Path $profileRoot 'BepInEx\RiskOfRainUpscaling-Diagnostics'
$configFolder = Join-Path $profileRoot 'BepInEx\config'
$configPath = Join-Path $configFolder 'xanderfrangos.riskofrainupscaling.cfg'
$results = Join-Path $PSScriptRoot 'test-results'
New-Item -ItemType Directory -Path $patcher,$plugin,$logs,$results,$configFolder -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'native-nmake\RiskOfRainUpscaling.Native.dll') -Destination $patcher -Force
$testExe = Join-Path $fixtureRoot 'RiskOfRainUpscaling.StartupTests.exe'
foreach ($mode in 'native','config-off','config-on','config-3x','config-4x','config-5x','config-6x','config-low','config-high','config-invalid','late','late-off','device-only') {
    $multiplierCase = $mode -like 'config-*' -and $mode -notin 'config-off','config-on'
    if (Test-Path -LiteralPath $configPath) { Remove-Item -LiteralPath $configPath }
    if ($mode -in 'config-off','config-on','late','late-off','device-only' -or $multiplierCase) {
        $preference = if ($mode -in 'config-off','late-off') { 'false' } else { 'true' }
        $value = switch($mode) { 'config-low' { '-1' } 'config-high' { '99' } 'config-invalid' { 'invalid' } default { if($multiplierCase){$mode.Substring(7,1)}else{'2'} } }
        "[Frame Generation]`r`nEnabled = $preference`r`nMultiplier = $value`r`n" | Set-Content -LiteralPath $configPath -Encoding utf8
    }
    $arguments = @($patcher,$logs,(Join-Path $plugin 'runtimes'))
    if ($mode -in 'late','device-only') { $arguments += "--$mode" }
    if ($mode -eq 'late-off') { $arguments += '--late','--fg-off' }
    if ($mode -eq 'config-on' -or $multiplierCase) { $arguments += '--fg-on' }
    if($multiplierCase){$expected=if($mode -eq 'config-high'){6}elseif($mode -in 'config-low','config-invalid'){2}else{[int]$value};$arguments += "$expected"}
    & $testExe @arguments | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "Startup fixture failed ($mode), exit=$LASTEXITCODE" }
    $nativeLog = Join-Path $logs 'native.log'
    $native = Get-Content -LiteralPath $nativeLog -Raw
    if ($mode -in 'late','device-only') {
        if ([regex]::Matches($native,'WARNING EARLY_HOOKS_MISSED').Count -ne 1 -or -not $native.Contains('interpolation latched OFF')) {
            throw "Missed early hooks did not warn once and keep FG off ($mode)."
        }
    } elseif ($mode -eq 'late-off') {
        # Frame Generation was never requested: warn about the hooks, but report it Off, not failed.
        if ([regex]::Matches($native,'WARNING EARLY_HOOKS_MISSED').Count -ne 1 -or $native.Contains('FRAME_GEN_FAILED')) {
            throw "Missed early hooks with Frame Generation saved Off must warn once without latching FG failed."
        }
    } else {
        foreach ($event in 'Initialize stage=preloader','IMPORT_RESOLVER_CHAIN UnityPlayer.dll!GetProcAddress','DYNAMIC_GRAPHICS_LOOKUP D3D11CreateDevice',
            'BEFORE_D3D11_CREATE_DEVICE','SWAPCHAIN_CREATED','GET_BUFFER_INTERFACE','RESIZE_BUFFERS','FIRST_PRESENT1','Initialize stage=plugin-awake') {
            if (-not $native.Contains($event)) { throw "Missing startup fixture evidence ($mode): $event" }
        }
        if ($native.Contains('EARLY_HOOKS_MISSED')) { throw "Early hooks incorrectly reported missing ($mode)." }
    }
    if ($mode -eq 'config-on' -or $multiplierCase) {
        foreach ($event in 'FRAME_BRIDGE_STARTUP mode=Bridge','FRAME_BRIDGE_READY','FRAME_BRIDGE_RESIZED','FRAME_BRIDGE_PRESENT','FRAME_BRIDGE_RETIRED') {
            if (-not $native.Contains($event)) { throw "Missing early bridge evidence: $event" }
        }
        if ($native.Contains('FRAME_BRIDGE_FALLBACK') -or $native.Contains('FRAME_BRIDGE_CREATE_FAILED')) { throw 'Startup fixture fell back instead of creating the bridge.' }
    }
    if ($mode -in 'native','config-off') {
        if (-not $native.Contains('FRAME_BRIDGE_STARTUP mode=Native') -or $native.Contains('FRAME_BRIDGE_READY') -or $native.Contains('FRAME_GEN_FAILED')) {
            throw "FG Off must select D3D11 without attempting the D3D12 bridge/FG runtime ($mode)."
        }
    }
    Copy-Item -LiteralPath $nativeLog -Destination (Join-Path $results "startup-$mode.log") -Force
}
Write-Output 'Startup: FG Off selects D3D11; 2x–6x and bounds select the saved startup multiplier; session mode is frozen; preloader hooks and missed-hook lockout passed.'
