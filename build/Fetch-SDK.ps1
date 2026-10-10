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
