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

# Brings a pinned checkout to its commit plus third_party\patches\<name>\*.patch, in name
# order. The tree must be the commit plus the first few patches (none, some or all); the
# rest are applied. Any other change, staged, unstaged or untracked, stops the build, so
# only reviewed code is compiled. A tree that already matches is left untouched.
function Install-BuildPatches {
    param([string]$GitPath, [string]$ProjectRoot, [string]$Name)
    $checkout = Join-Path $ProjectRoot "third_party\$Name"
    $patches = @(Get-ChildItem -LiteralPath (Join-Path $ProjectRoot "third_party\patches\$Name") -Filter '*.patch' -File | Sort-Object Name)
    $restore = "Restore it with: git -C `"$checkout`" checkout -- . ; git -C `"$checkout`" clean -fd (or delete the folder to fetch it again)."
    $untracked = & $GitPath -C $checkout ls-files --others --exclude-standard
    if ($LASTEXITCODE -ne 0) { throw "$Name status failed." }
    # safecrlf off: comparing a core.autocrlf working tree would otherwise warn on every build.
    & $GitPath -c core.safecrlf=false -C $checkout diff --cached --quiet
    if ($LASTEXITCODE -ne 0 -or $untracked) { throw "third_party\$Name has staged or untracked changes beyond its patches. $restore" }
    # A scratch copy of the checkout's index (it keeps the sparse-checkout flags) takes
    # the patches one at a time; the working tree is compared with it after each.
    $gitDir = "$(& $GitPath -C $checkout rev-parse --absolute-git-dir)".Trim()
    $scratch = Join-Path $gitDir 'build-patches.index'
    Copy-Item -LiteralPath (Join-Path $gitDir 'index') -Destination $scratch -Force
    $previousIndex = $env:GIT_INDEX_FILE
    $applied = -1
    try {
        $env:GIT_INDEX_FILE = $scratch
        for ($count = 0; $count -le $patches.Count; ++$count) {
            if ($count -gt 0) {
                & $GitPath -C $checkout apply --cached $patches[$count - 1].FullName
                if ($LASTEXITCODE -ne 0) { throw "$Name patch $($patches[$count - 1].Name) does not apply to the pinned commit." }
            }
            & $GitPath -c core.safecrlf=false -C $checkout diff --quiet
            if ($LASTEXITCODE -eq 0) { $applied = $count; break }
        }
    }
    finally {
        $env:GIT_INDEX_FILE = $previousIndex
        Remove-Item -LiteralPath $scratch -Force -ErrorAction SilentlyContinue
    }
    if ($applied -lt 0) { throw "third_party\$Name has local changes beyond its patches. $restore" }
    for ($count = $applied; $count -lt $patches.Count; ++$count) {
        & $GitPath -C $checkout apply $patches[$count].FullName
        if ($LASTEXITCODE -ne 0) { throw "$Name patch failed: $($patches[$count].Name)" }
        Write-Output "Applied $Name patch $($patches[$count].Name)"
    }
}
