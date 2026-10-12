# Shared discovery for the build and SDK fetch scripts. Compatible with PowerShell 5.1.
function Find-BuildTool {
    param([string]$Name, [string[]]$Fallbacks, [string]$Help)
    $command = Get-Command $Name -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($command) { return $command.Source }
    foreach ($candidate in $Fallbacks) {
        if ($candidate -and (Test-Path -LiteralPath $candidate -PathType Leaf)) { return $candidate }
    }
    throw "$Name was not found. $Help"
}

function Test-BuildGamePath {
    param([string]$Path)
    if (-not $Path) { return $false }
    foreach ($name in 'RoR2.dll', 'UnityEngine.CoreModule.dll', 'com.unity.multiplayer-hlapi.Runtime.dll') {
        if (-not (Test-Path -LiteralPath (Join-Path $Path "Risk of Rain 2_Data\Managed\$name") -PathType Leaf)) { return $false }
    }
    return $true
}

function Resolve-BuildGamePath {
    param([string]$Path)
    if ($Path) {
        if (-not (Test-BuildGamePath $Path)) { throw "Game references missing at '$Path'. Set GamePath to the Risk of Rain 2 installation folder." }
        return [IO.Path]::GetFullPath($Path)
    }
    $steamRoots = @()
    foreach ($key in 'HKCU:\Software\Valve\Steam', 'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam', 'HKLM:\SOFTWARE\Valve\Steam') {
        $steam = Get-ItemProperty -LiteralPath $key -ErrorAction SilentlyContinue
        if ($steam.SteamPath) { $steamRoots += $steam.SteamPath }
        if ($steam.InstallPath) { $steamRoots += $steam.InstallPath }
    }
    $steamRoots += Join-Path ${env:ProgramFiles(x86)} 'Steam'
    $libraries = @($steamRoots)
    foreach ($steamRoot in ($steamRoots | Select-Object -Unique)) {
        $vdf = Join-Path $steamRoot 'steamapps\libraryfolders.vdf'
        if (Test-Path -LiteralPath $vdf -PathType Leaf) {
            foreach ($match in [regex]::Matches((Get-Content -LiteralPath $vdf -Raw), '"path"\s+"([^"]+)"')) {
                $libraries += $match.Groups[1].Value.Replace('\\', '\')
            }
        }
    }
    foreach ($drive in (Get-PSDrive -PSProvider FileSystem)) {
        $libraries += Join-Path $drive.Root 'SteamLibrary'
        $libraries += Join-Path $drive.Root 'Steam'
    }
    foreach ($library in ($libraries | Select-Object -Unique)) {
        $candidate = Join-Path $library 'steamapps\common\Risk of Rain 2'
        if (Test-BuildGamePath $candidate) { return [IO.Path]::GetFullPath($candidate) }
    }
    throw 'Risk of Rain 2 was not found. Set GamePath in build/Build.local.psd1 or pass -GamePath.'
}

function Test-BuildProfile {
    param([string]$Path, [bool]$NeedsOptions)
    if (-not $Path) { return $false }
    foreach ($name in 'BepInEx.dll', 'Mono.Cecil.dll', '0Harmony.dll') {
        if (-not (Test-Path -LiteralPath (Join-Path $Path "BepInEx\core\$name") -PathType Leaf)) { return $false }
    }
    if ($NeedsOptions -and -not (Find-BuildOptions $Path)) { return $false }
    return $true
}

function Find-BuildOptions {
    param([string]$ProfilePath)
    $plugins = Join-Path $ProfilePath 'BepInEx\plugins'
    if (Test-Path -LiteralPath $plugins -PathType Container) {
        $options = Get-ChildItem -LiteralPath $plugins -Recurse -Filter 'RiskOfOptions.dll' -File | Sort-Object FullName | Select-Object -First 1
        if ($options) { return $options.FullName }
    }
}

function Resolve-BuildProfilePath {
    param([string]$Path, [string]$GamePath, [bool]$NeedsOptions)
    if ($Path) {
        if (-not (Test-BuildProfile $Path $NeedsOptions)) { throw "Build dependencies missing in '$Path'. Use a BepInEx 5 profile with Risk of Options, or pass -UseCachedReferences." }
        return [IO.Path]::GetFullPath($Path)
    }
    $profiles = @()
    foreach ($manager in 'r2modmanPlus-local', 'r2modman', 'Thunderstore Mod Manager\DataFolder') {
        $root = Join-Path $env:APPDATA "$manager\RiskOfRain2\profiles"
        if (Test-Path -LiteralPath $root -PathType Container) {
            $profiles += Get-ChildItem -LiteralPath $root -Directory | Where-Object { Test-BuildProfile $_.FullName $NeedsOptions }
        }
    }
    # Read build references from an existing mod install first, then the newest usable profile.
    $profile = $profiles | Sort-Object @{Expression={Test-Path -LiteralPath (Join-Path $_.FullName (Get-BuildProfileRelativePath 'BepInEx\plugins\RiskOfRainUpscaling\RiskOfRainUpscaling.dll'))};Descending=$true}, @{Expression='LastWriteTime';Descending=$true}, FullName | Select-Object -First 1
    if ($profile) { return $profile.FullName }
    if (Test-BuildProfile $GamePath $NeedsOptions) { return $GamePath }
}

# r2modman installs a package's BepInEx\plugins and BepInEx\patchers contents under an Author-Package folder,
# so package path BepInEx\plugins\RiskOfRainUpscaling\x is BepInEx\plugins\XanderChaos-RiskOfRainUpscaling\RiskOfRainUpscaling\x in a profile.
$BuildProfileModFolder = 'XanderChaos-RiskOfRainUpscaling'

function Get-BuildProfileRelativePath {
    param([string]$PackageRelativePath)
    if ($PackageRelativePath -notmatch '^BepInEx[\\/](plugins|patchers)[\\/](.+)$') { throw "Not a plugin or patcher path: $PackageRelativePath" }
    return "BepInEx\$($Matches[1])\$BuildProfileModFolder\$($Matches[2])"
}

function Get-BuildRelativePath {
    param([string]$Root, [string]$Path)
    $prefix = [IO.Path]::GetFullPath($Root).TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    $fullPath = [IO.Path]::GetFullPath($Path)
    if (-not $fullPath.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) { throw "Path escapes build output: $Path" }
    return $fullPath.Substring($prefix.Length)
}

# Applies third_party\patches\<name>\*.patch, in name order, to a pinned checkout; skips patches already applied.
function Install-BuildPatches {
    param([string]$GitPath, [string]$ProjectRoot, [string]$Name)
    $checkout = Join-Path $ProjectRoot "third_party\$Name"
    foreach ($patch in Get-ChildItem -LiteralPath (Join-Path $ProjectRoot "third_party\patches\$Name") -Filter '*.patch' -File | Sort-Object Name) {
        # git reports a patch that does not reverse-apply on stderr; that is the "not applied yet" answer.
        $previous = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
        & $GitPath -C $checkout apply --reverse --check $patch.FullName 2>$null | Out-Null
        $applied = $LASTEXITCODE -eq 0
        $ErrorActionPreference = $previous
        if ($applied) { continue }
        & $GitPath -C $checkout apply $patch.FullName
        if ($LASTEXITCODE -ne 0) { throw "$Name patch failed: $($patch.Name)" }
        Write-Output "Applied $Name patch $($patch.Name)"
    }
}
