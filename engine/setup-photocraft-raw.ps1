# Fetch PhotoCraft's crates/raw at the pinned commit, verify it, and stage it
# with the tracked overlay where engine/raw-photocraft builds it from. The
# Windows counterpart of setup-photocraft-raw.sh; see that script for the
# layout and the reasons. Needs git and tar (both ship with Windows 10+ /
# Git for Windows). No Python.
[CmdletBinding()]
param([string]$DependencyDirectory = (Join-Path $PSScriptRoot '..\build\deps'))
$ErrorActionPreference = 'Stop'
$DependencyDirectory = [IO.Path]::GetFullPath($DependencyDirectory)
New-Item -ItemType Directory -Path $DependencyDirectory -Force | Out-Null

$third = Join-Path $PSScriptRoot 'third_party\photocraft-raw'
$overlay = Join-Path $third 'files'
$pin = @{}
foreach ($line in Get-Content -LiteralPath (Join-Path $third 'PIN')) {
    $name, $value = $line.Trim() -split '=', 2
    if ($value) { $pin[$name] = $value }
}
$commit = $pin['commit']
$rawTree = $pin['raw_tree']
if ($commit -notmatch '^[0-9a-f]{40}$' -or $rawTree -notmatch '^[0-9a-f]{40}$') {
    throw 'engine/third_party/photocraft-raw/PIN does not hold a commit and a tree id'
}
foreach ($required in 'Cargo.toml', 'src\develop.rs', 'src\lib.rs') {
    if (-not (Test-Path -LiteralPath (Join-Path $overlay $required) -PathType Leaf)) {
        throw "Missing tracked PhotoCraft raw overlay: $overlay"
    }
}

$url = 'https://github.com/storytold/photocraft.git'
$checkout = Join-Path $DependencyDirectory "photocraft-$commit"
$stage = Join-Path $DependencyDirectory 'photocraft-raw'

function Invoke-Git {
    # Returns stdout; throws on a non-zero exit. $LASTEXITCODE, not $?, is the
    # exit status of a native command in both Windows PowerShell and pwsh.
    $output = & git @args 2>$null
    if ($LASTEXITCODE) { throw "git $($args -join ' ') exited $LASTEXITCODE" }
    return $output
}

function Test-Checkout([string]$Path) {
    $head = & git -C $Path rev-parse 'pinned^{commit}' 2>$null
    if ($LASTEXITCODE -or "$head".Trim() -ne $commit) { return $false }
    $tree = & git -C $Path rev-parse 'pinned:crates/raw' 2>$null
    return (-not $LASTEXITCODE) -and ("$tree".Trim() -eq $rawTree)
}

if (Test-Path -LiteralPath $checkout) {
    if (-not (Test-Checkout $checkout)) { throw "$checkout does not hold PhotoCraft $commit; not touched." }
} else {
    $partial = "$checkout.partial"
    if (Test-Path -LiteralPath $partial) { Remove-Item -LiteralPath $partial -Recurse -Force }
    Invoke-Git init -q --bare $partial | Out-Null
    # One commit, no history. git names every object by its hash, which is
    # the verification of what the server sent.
    Invoke-Git -C $partial fetch -q --depth 1 $url $commit | Out-Null
    Invoke-Git -C $partial update-ref refs/tags/pinned FETCH_HEAD | Out-Null
    if (-not (Test-Checkout $partial)) {
        Remove-Item -LiteralPath $partial -Recurse -Force
        throw "$url $commit does not match its pin."
    }
    Rename-Item -LiteralPath $partial -NewName (Split-Path -Leaf $checkout)
}

function Get-TextSha256([string]$Path) {
    # Hash with CR removed, so a CRLF checkout stamps the same as an LF one.
    $bytes = [IO.File]::ReadAllBytes($Path) | Where-Object { $_ -ne 13 }
    $hasher = [Security.Cryptography.SHA256]::Create()
    try {
        return [BitConverter]::ToString($hasher.ComputeHash([byte[]]@($bytes))).Replace('-', '').ToLowerInvariant()
    } finally {
        $hasher.Dispose()
    }
}

$overlayFiles = [string[]]@(Get-ChildItem -LiteralPath $overlay -File -Recurse |
    ForEach-Object { $_.FullName.Substring($overlay.Length + 1).Replace('\', '/') })
# Ordinal order, as LC_ALL=C sort gives in the shell script.
[Array]::Sort($overlayFiles, [StringComparer]::Ordinal)
$stampLines = @("commit=$commit", "raw_tree=$rawTree")
foreach ($relative in $overlayFiles) {
    $stampLines += "overlay $relative $(Get-TextSha256 (Join-Path $overlay $relative))"
}
$stamp = ($stampLines -join "`n")
$stampFile = Join-Path $stage '.spk-pin'
if ((Test-Path -LiteralPath $stampFile) -and
    (([IO.File]::ReadAllText($stampFile)).Replace("`r", '').TrimEnd("`n") -eq $stamp)) {
    Write-Host "PhotoCraft raw $commit staged in $stage"
    return
}

$partial = "$stage.partial"
if (Test-Path -LiteralPath $partial) { Remove-Item -LiteralPath $partial -Recurse -Force }
New-Item -ItemType Directory -Path $partial -Force | Out-Null
$tarball = Join-Path $partial 'raw.tar'
# Out of the object database, not a working tree. -o keeps the binary archive
# away from PowerShell's text pipeline.
Invoke-Git -C $checkout archive --format=tar -o $tarball pinned crates/raw/src crates/raw/README.md | Out-Null
& tar -xf $tarball -C $partial --strip-components=2
if ($LASTEXITCODE) { throw "tar exited $LASTEXITCODE" }
Remove-Item -LiteralPath $tarball -Force
# The licence texts and PhotoCraft's lock file, byte for byte, the same way.
Invoke-Git -C $checkout archive --format=tar -o $tarball pinned LICENSE-MIT LICENSE-APACHE NOTICE Cargo.lock | Out-Null
& tar -xf $tarball -C $partial
if ($LASTEXITCODE) { throw "tar exited $LASTEXITCODE" }
Remove-Item -LiteralPath $tarball -Force
Rename-Item -LiteralPath (Join-Path $partial 'Cargo.lock') -NewName 'photocraft-Cargo.lock'
$utf8 = New-Object Text.UTF8Encoding($false)
foreach ($relative in $overlayFiles) {
    $destination = Join-Path $partial $relative.Replace('/', '\')
    # The overlay replaces whole files; every one it replaces must exist
    # upstream, so a pin that renamed them fails here.
    if ($relative -ne 'Cargo.toml' -and -not (Test-Path -LiteralPath $destination -PathType Leaf)) {
        throw "The overlay replaces $relative, which PhotoCraft $commit does not have."
    }
    New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
    $text = [IO.File]::ReadAllText((Join-Path $overlay $relative.Replace('/', '\'))).Replace("`r", '')
    [IO.File]::WriteAllText($destination, $text, $utf8)
}
[IO.File]::WriteAllText((Join-Path $partial '.spk-pin'), $stamp + "`n", $utf8)
if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
Rename-Item -LiteralPath $partial -NewName (Split-Path -Leaf $stage)
Write-Host "PhotoCraft raw $commit staged in $stage"
