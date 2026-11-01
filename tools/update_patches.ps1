# Regenerates patches/xboxrecomp-gpu-backend.patch from the toolkit's working tree, then proves that
# the pinned commit plus all three patches rebuilds that tree exactly.
#
#   powershell -File tools/update_patches.ps1          regenerate
#   powershell -File tools/update_patches.ps1 -Check   only say whether the patch is current; exit 1 if not
#
# The other two patches are fixed history. All toolkit work goes in this one, as a diff against the
# pinned commit with those two applied. Works in a temporary clone, so the toolkit is not touched.
param([switch]$Check)
$ErrorActionPreference = "Stop"
$PSNativeCommandUseErrorActionPreference = $false

$repo = Split-Path -Parent $PSScriptRoot
$toolkit = Join-Path $repo "third_party\xboxrecomp"
$base = @("xboxrecomp-decode-stream.patch", "xboxrecomp-extvma-buffers.patch") | ForEach-Object { Join-Path $repo "patches\$_" }
$patch = Join-Path $repo "patches\xboxrecomp-gpu-backend.patch"
$work = Join-Path ([IO.Path]::GetTempPath()) ("ucc2-patches-" + [Guid]::NewGuid().ToString("N"))

function Git-Do {
    git @args
    if ($LASTEXITCODE) { throw "git $args failed" }
}

try {
    $pin = (git -C $toolkit rev-parse HEAD).Trim()
    $changed = @(git -C $toolkit ls-files -m -o --exclude-standard)

    Git-Do clone -q $toolkit $work
    Push-Location $work
    Git-Do checkout -q $pin
    foreach ($p in $base) { Git-Do apply $p }
    Git-Do -c user.name=patches -c user.email=patches@localhost commit -q -am "base patches"
    $committed = (git rev-parse HEAD).Trim()

    foreach ($f in $changed) {
        $to = Join-Path $work $f
        New-Item -ItemType Directory -Force (Split-Path -Parent $to) | Out-Null
        Copy-Item (Join-Path $toolkit $f) $to -Force
    }
    Git-Do add -N .
    $new = Join-Path $work "new.patch"
    cmd /c "git diff --binary -- . > `"$new`""
    if ($LASTEXITCODE) { throw "git diff failed" }

    # Round trip: base patches plus the new one must give back every changed file, byte for byte.
    Remove-Item -Force (Join-Path $work ".git\index")
    Git-Do reset -q --hard $committed
    Git-Do clean -qfd -e new.patch
    Git-Do apply $new
    foreach ($f in $changed) {
        if ((Get-FileHash (Join-Path $work $f)).Hash -ne (Get-FileHash (Join-Path $toolkit $f)).Hash) {
            throw "round trip differs for $f"
        }
    }
    Pop-Location

    $current = (Test-Path $patch) -and ((Get-FileHash $new).Hash -eq (Get-FileHash $patch).Hash)
    if ($Check) {
        if (-not $current) { Write-Output "STALE: run tools/update_patches.ps1"; exit 1 }
        Write-Output "current: $($changed.Count) toolkit files match the patches"
    } elseif ($current) {
        Write-Output "already current: $($changed.Count) toolkit files match the patches"
    } else {
        Copy-Item $new $patch -Force
        Write-Output "updated $patch ($((Get-Item $patch).Length) bytes, $($changed.Count) toolkit files)"
    }
} finally {
    if ((Get-Location).Path -eq $work) { Pop-Location }
    if (Test-Path $work) { Remove-Item -Recurse -Force $work }
}
