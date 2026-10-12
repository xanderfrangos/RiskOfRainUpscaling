param([string]$GitPath)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'Build-Helpers.ps1')
if (-not $GitPath) { $GitPath = Find-BuildTool 'git.exe' @((Join-Path $env:ProgramFiles 'Git\cmd\git.exe'), (Join-Path $env:LOCALAPPDATA 'Programs\Git\cmd\git.exe')) 'Install Git for Windows.' }
$sdkRoot = Join-Path $projectRoot 'third_party\Streamline'
$sdkCommit = '2122257e0fce486f91b385aa63b9a09b0a34b363'
if (-not (Test-Path -LiteralPath (Join-Path $sdkRoot '.git'))) {
    & $GitPath -c http.sslBackend=openssl clone --depth 1 --branch v2.14.1 --filter=blob:none --sparse https://github.com/NVIDIA-RTX/Streamline.git $sdkRoot
    if ($LASTEXITCODE -ne 0) { throw 'Streamline SDK clone failed.' }
}
$actual = & $GitPath -C $sdkRoot rev-parse HEAD
if ($LASTEXITCODE -ne 0 -or "$actual".Trim() -ne $sdkCommit) { throw "SDK checkout must be pinned to $sdkCommit; found $actual" }
& $GitPath -c http.sslBackend=openssl -C $sdkRoot sparse-checkout set include docs
if ($LASTEXITCODE -ne 0) { throw 'SDK header checkout failed.' }
Write-Output "Streamline 2.14.1 headers ready at $sdkRoot ($sdkCommit)"

# AMD FidelityFX SDK: API headers, signed DX12 DLLs and license (FSR Frame Generation/Upscaling).
$ffxRoot = Join-Path $projectRoot 'third_party\FidelityFX-SDK'
$ffxCommit = '60f4ea81909200d8542eca14dccb2628b763a9a3'
if (-not (Test-Path -LiteralPath (Join-Path $ffxRoot '.git'))) {
    & $GitPath -c http.sslBackend=openssl clone --depth 1 --branch v2.3.0 --filter=blob:none --no-checkout https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK.git $ffxRoot
    if ($LASTEXITCODE -ne 0) { throw 'FidelityFX SDK clone failed.' }
}
$actual = & $GitPath -C $ffxRoot rev-parse HEAD
if ($LASTEXITCODE -ne 0 -or "$actual".Trim() -ne $ffxCommit) { throw "FidelityFX SDK must be pinned to $ffxCommit; found $actual" }
& $GitPath -c http.sslBackend=openssl -C $ffxRoot sparse-checkout set --no-cone '/Kits/FidelityFX/api/include/' '/Kits/FidelityFX/framegeneration/include/' '/Kits/FidelityFX/upscalers/include/' '/Kits/FidelityFX/signedbin/' '/Kits/FidelityFX/docs/' '/Kits/FidelityFX/readme.md'
if ($LASTEXITCODE -ne 0) { throw 'FidelityFX SDK checkout failed.' }
& $GitPath -c http.sslBackend=openssl -C $ffxRoot checkout -q
if ($LASTEXITCODE -ne 0) { throw 'FidelityFX SDK checkout failed.' }
Write-Output "FidelityFX SDK 2.3.0 headers and signed DLLs ready at $ffxRoot ($ffxCommit)"

# FSR 3.1 upscaler on D3D11 (OptiScaler's FidelityFX-SDK-DX11 port, MIT): sources built into the native DLL.
$ffxDx11Root = Join-Path $projectRoot 'third_party\FidelityFX-SDK-DX11'
$ffxDx11Commit = '9b04fa49abae5aa341c0007af13c8ba1b4ad9913'
if (-not (Test-Path -LiteralPath (Join-Path $ffxDx11Root '.git'))) {
    & $GitPath init -q $ffxDx11Root
    if ($LASTEXITCODE -ne 0) { throw 'FidelityFX DX11 checkout failed.' }
    & $GitPath -C $ffxDx11Root remote add origin https://github.com/optiscaler/FidelityFX-SDK-DX11.git
    if ($LASTEXITCODE -ne 0) { throw 'FidelityFX DX11 checkout failed.' }
}
& $GitPath -C $ffxDx11Root sparse-checkout set --no-cone '/LICENSE.txt' '/sdk/include/' '/sdk/src/shared/' '/sdk/src/components/fsr3upscaler/' '/sdk/src/backends/dx11/' '/sdk/src/backends/shared/' '/sdk/tools/binary_store/FidelityFX_SC.exe'
if ($LASTEXITCODE -ne 0) { throw 'FidelityFX DX11 checkout failed.' }
$actual = & $GitPath -C $ffxDx11Root rev-parse -q --verify HEAD 2>$null
if ("$actual".Trim() -ne $ffxDx11Commit) {
    # The pinned commit is fetched by hash, so later pushes to the fork's branch never change the build.
    & $GitPath -c http.sslBackend=openssl -C $ffxDx11Root fetch -q --depth 1 --filter=blob:none origin $ffxDx11Commit
    if ($LASTEXITCODE -ne 0) { throw 'FidelityFX DX11 fetch failed.' }
    & $GitPath -C $ffxDx11Root checkout -q --detach $ffxDx11Commit
    if ($LASTEXITCODE -ne 0) { throw 'FidelityFX DX11 checkout failed.' }
}
$actual = & $GitPath -C $ffxDx11Root rev-parse HEAD
if ($LASTEXITCODE -ne 0 -or "$actual".Trim() -ne $ffxDx11Commit) { throw "FidelityFX DX11 must be pinned to $ffxDx11Commit; found $actual" }
# Fixes on top of the pinned commit; Build.ps1 also applies any that are missing.
Install-BuildPatches $GitPath $projectRoot 'FidelityFX-SDK-DX11'
Write-Output "FidelityFX FSR 3.1.2 DX11 sources ready at $ffxDx11Root ($ffxDx11Commit)"
