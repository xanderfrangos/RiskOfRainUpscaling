param([switch]$FrameGeneration,[switch]$ReflexLimiter)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
Push-Location $projectRoot
try {
    & dotnet run --project tests/managed -c Release
    if ($LASTEXITCODE -ne 0) { throw 'Managed camera tests failed.' }
    New-Item -ItemType Directory -Path 'build\test-results' -Force | Out-Null
    & 'build\native-nmake\RiskOfRainUpscaling.LoggingTests.exe'
    if ($LASTEXITCODE -ne 0) { throw 'Native logging threshold regression failed.' }
    foreach($adapter in @('--warp','--hardware')){
        & 'build\native-nmake\RiskOfRainUpscaling.HudOpacityTests.exe' $adapter
        if($LASTEXITCODE -ne 0){throw "HUD opacity capture test failed ($adapter)."}
    }
    & 'build\native-nmake\RiskOfRainUpscaling.FramePacingTests.exe'
    if ($LASTEXITCODE -ne 0) { throw 'Unity/VSync FPS guard regression failed.' }
    & 'build\native-nmake\RiskOfRainUpscaling.RcasTests.exe'
    if ($LASTEXITCODE -ne 0) { throw 'RCAS shader/settings regression failed.' }
    foreach ($adapter in 'warp','hardware') {
        & 'build\native-nmake\RiskOfRainUpscaling.GraphicsHooksTests.exe' "--$adapter"
        if ($LASTEXITCODE -ne 0) { throw "Native startup hook test failed ($adapter); see build/test-results/native.log." }
        Copy-Item -LiteralPath 'build\test-results\native.log' -Destination "build\test-results\$adapter.log" -Force
    }
    & 'build\native-nmake\RiskOfRainUpscaling.SceneInputsTests.exe'
    if ($LASTEXITCODE -ne 0) { throw "Scene input test failed, exit=$LASTEXITCODE" }
    Copy-Item -LiteralPath 'build\test-results\native.log' -Destination 'build\test-results\scene-inputs.log' -Force
    & 'build\native-nmake\RiskOfRainUpscaling.DiagnosticsTests.exe'
    if ($LASTEXITCODE -ne 0) { throw "Diagnostics preview/capture test failed, exit=$LASTEXITCODE" }
    Copy-Item -LiteralPath 'build\test-results\native.log' -Destination 'build\test-results\diagnostics.log' -Force
    & 'build\native-nmake\RiskOfRainUpscaling.PipelineTests.exe' --warp
    if ($LASTEXITCODE -ne 0) { throw "Upscaler fallback/state test failed, exit=$LASTEXITCODE" }
    Copy-Item -LiteralPath 'build\test-results\native.log' -Destination 'build\test-results\pipeline-warp.log' -Force
    & 'build\native-nmake\RiskOfRainUpscaling.PipelineTests.exe' --hardware
    if ($LASTEXITCODE -ne 0) { throw "Synthetic hardware upscaler evaluation failed, exit=$LASTEXITCODE" }
    Copy-Item -LiteralPath 'build\test-results\native.log' -Destination 'build\test-results\pipeline-hardware.log' -Force
    if(Select-String -LiteralPath 'build\test-results\pipeline-hardware.log' -SimpleMatch 'presentCommon() was not observed' -Quiet) {
        throw 'DLAA SDK presentation bookkeeping is still missing.'
    }
    & (Join-Path $PSScriptRoot 'Test-Startup.ps1')
    & (Join-Path $PSScriptRoot 'Test-ProfileInstall.ps1')
    foreach ($api in 'warp','hardware') {
        & 'build\native-nmake\RiskOfRainUpscaling.FrameBridgeTests.exe' "--$api"
        if ($LASTEXITCODE -ne 0) { throw "Frame bridge test failed ($api), exit=$LASTEXITCODE" }
        Copy-Item -LiteralPath 'build\test-results\native.log' -Destination "build\test-results\frame-bridge-$api.log" -Force
    }
    & 'build\native-nmake\RiskOfRainUpscaling.FrameBridgeTests.exe' --warp --fg-fallback
    if($LASTEXITCODE -ne 0){throw "FG unsupported-adapter fallback failed, exit=$LASTEXITCODE"}
    Copy-Item -LiteralPath 'build\test-results\native.log' -Destination 'build\test-results\frame-generation-warp-fallback.log' -Force
    & 'build\native-nmake\RiskOfRainUpscaling.PipelineTests.exe' --hardware --bridge
    if ($LASTEXITCODE -ne 0) { throw "Upscaling with the frame bridge failed, exit=$LASTEXITCODE" }
    Copy-Item -LiteralPath 'build\test-results\native.log' -Destination 'build\test-results\pipeline-bridge-hardware.log' -Force
    if(Select-String -LiteralPath 'build\test-results\pipeline-bridge-hardware.log' -SimpleMatch 'presentCommon() was not observed' -Quiet) {
        throw 'DLAA SDK presentation bookkeeping is missing with the frame bridge.'
    }
    if($FrameGeneration -or $ReflexLimiter){
        & 'build\native-nmake\RiskOfRainUpscaling.PipelineTests.exe' --fg --visible --ui-recomposition
        if($LASTEXITCODE -ne 0){throw "Hardware Frame Generation UI recomposition test failed, exit=$LASTEXITCODE"}
        Copy-Item -LiteralPath 'build\test-results\native.log' -Destination 'build\test-results\frame-generation-ui.log' -Force
        Copy-Item -LiteralPath 'build\test-results\FrameGeneration\sl.log' -Destination 'build\test-results\frame-generation-ui-sl.log' -Force
        # The game's HUD camera draws into a RenderTexture (bottom-up rows on D3D); every HUD-less consumer must see top-down rows.
        foreach($arguments in @(@('--fg','--visible','--hud-render-texture'),@('--fg','--visible','--ui-recomposition','--hud-render-texture'),@('--fg','--visible','--fsr','--hud-render-texture'),@('--fg','--visible','--fsr','--ui-recomposition','--hud-render-texture'),@('--fg','--visible','--hud-hdr-target'),@('--fg','--visible','--fsr','--hud-hdr-target'))){
            & 'build\native-nmake\RiskOfRainUpscaling.PipelineTests.exe' @arguments
            if($LASTEXITCODE -ne 0){throw "Frame Generation RenderTexture HUD target test failed ($($arguments -join ' ')), exit=$LASTEXITCODE"}
        }
        # Run on an interactive desktop: NVIDIA suppresses FG without foreground focus.
        foreach($multiplier in 2..6){
            & 'build\native-nmake\RiskOfRainUpscaling.PipelineTests.exe' --fg --visible --fg-multiplier $multiplier
            if($LASTEXITCODE -ne 0){throw "Hardware Frame Generation ${multiplier}x test failed, exit=$LASTEXITCODE"}
            $suffix=if($multiplier -eq 2){'hardware'}else{"${multiplier}x"}
            Copy-Item -LiteralPath 'build\test-results\native.log' -Destination "build\test-results\frame-generation-$suffix.log" -Force
            Copy-Item -LiteralPath 'build\test-results\FrameGeneration\sl.log' -Destination "build\test-results\frame-generation-$suffix-sl.log" -Force
        }
        & 'build\native-nmake\RiskOfRainUpscaling.PipelineTests.exe' --fg --visible --fg-live
        if($LASTEXITCODE -ne 0){throw "Live Frame Generation multiplier test failed, exit=$LASTEXITCODE"}
        Copy-Item -LiteralPath 'build\test-results\native.log' -Destination 'build\test-results\frame-generation-live.log' -Force
        Copy-Item -LiteralPath 'build\test-results\FrameGeneration\sl.log' -Destination 'build\test-results\frame-generation-live-sl.log' -Force
        & 'build\native-nmake\RiskOfRainUpscaling.PipelineTests.exe' --fg --visible --app-waitable
        if($LASTEXITCODE -ne 0){throw "Frame Generation application wait-handle test failed, exit=$LASTEXITCODE"}
        Copy-Item -LiteralPath 'build\test-results\native.log' -Destination 'build\test-results\frame-generation-app-waitable.log' -Force
        Copy-Item -LiteralPath 'build\test-results\FrameGeneration\sl.log' -Destination 'build\test-results\frame-generation-app-waitable-sl.log' -Force
        if($ReflexLimiter){
            foreach($appWaitable in $false,$true){
                $arguments=@('--fg','--visible','--reflex-limit')
                $suffix=if($appWaitable){'app-waitable'}else{'hardware'}
                if($appWaitable){$arguments+='--app-waitable'}
                & 'build\native-nmake\RiskOfRainUpscaling.PipelineTests.exe' @arguments
                if($LASTEXITCODE -ne 0){throw "Reflex limiter/FG test failed ($suffix), exit=$LASTEXITCODE"}
                Copy-Item -LiteralPath 'build\test-results\native.log' -Destination "build\test-results\reflex-limiter-$suffix.log" -Force
                Copy-Item -LiteralPath 'build\test-results\FrameGeneration\sl.log' -Destination "build\test-results\reflex-limiter-$suffix-sl.log" -Force
            }
        }
    }
}
finally { Pop-Location }
