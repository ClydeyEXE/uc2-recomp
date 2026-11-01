# Minimal XDVDFS (Xbox ISO) extractor. Usage: extract_xiso.ps1 -Iso <file> -Out <dir> [-ListOnly]
param(
    [Parameter(Mandatory)][string]$Iso,
    [Parameter(Mandatory)][string]$Out,
    [switch]$ListOnly
)
$ErrorActionPreference = 'Stop'
$SECSZ = 2048
$fs = [IO.File]::OpenRead($Iso)
$magic = [Text.Encoding]::ASCII.GetBytes('MICROSOFT*XBOX*MEDIA')

function Read-At([long]$pos, [int]$len) {
    $buf = New-Object byte[] $len
    $fs.Seek($pos, 'Begin') | Out-Null
    $n = 0
    while ($n -lt $len) { $r = $fs.Read($buf, $n, $len - $n); if ($r -le 0) { break }; $n += $r }
    $buf
}

# Find the volume descriptor (sector 32) under the known layout offsets
$base = $null
foreach ($off in 0L, 0xFDA0000L, 0x2080000L, 0x18300000L) {
    $b = Read-At ($off + 32 * $SECSZ) 20
    if ([Linq.Enumerable]::SequenceEqual([byte[]]$b, [byte[]]$magic)) { $base = $off; break }
}
if ($null -eq $base) { throw 'XDVDFS signature not found' }
Write-Host ("XDVDFS found, partition offset 0x{0:X}" -f $base)

$vd = Read-At ($base + 32 * $SECSZ) $SECSZ
$rootSector = [BitConverter]::ToUInt32($vd, 0x14)
$rootSize = [BitConverter]::ToUInt32($vd, 0x18)

$script:count = 0
function Walk-Dir([uint32]$dirSec, [uint32]$size, [string]$rel) {
    if ($size -eq 0) { return }
    $dir = Read-At ($base + [long]$dirSec * $SECSZ) $size
    $stack = New-Object Collections.Generic.Stack[int]
    $seen = New-Object Collections.Generic.HashSet[int]
    $stack.Push(0)
    while ($stack.Count) {
        $o = $stack.Pop()
        if (-not $seen.Add($o)) { continue }
        if ($o + 14 -gt $dir.Length) { continue }
        $l = [BitConverter]::ToUInt16($dir, $o)
        if ($l -eq 0xFFFF) { continue }
        $r = [BitConverter]::ToUInt16($dir, $o + 2)
        $st = [BitConverter]::ToUInt32($dir, $o + 4)
        $sz = [BitConverter]::ToUInt32($dir, $o + 8)
        $attr = $dir[$o + 12]
        $nl = $dir[$o + 13]
        $name = [Text.Encoding]::ASCII.GetString($dir, $o + 14, $nl)
        if ($l) { $stack.Push($l * 4) }
        if ($r) { $stack.Push($r * 4) }
        $path = if ($rel) { "$rel\$name" } else { $name }
        if ($attr -band 0x10) {
            if (-not $ListOnly) { New-Item -ItemType Directory -Force (Join-Path $Out $path) | Out-Null }
            Walk-Dir $st $sz $path
        } else {
            $script:count++
            Write-Host ("{0,12}  {1}" -f $sz, $path)
            if (-not $ListOnly) {
                $dest = Join-Path $Out $path
                New-Item -ItemType Directory -Force (Split-Path $dest) | Out-Null
                $o2 = [IO.File]::Create($dest)
                $fs.Seek($base + [long]$st * $SECSZ, 'Begin') | Out-Null
                $left = [long]$sz
                $buf = New-Object byte[] (4MB)
                while ($left -gt 0) {
                    $n = $fs.Read($buf, 0, [int][Math]::Min($left, $buf.Length))
                    if ($n -le 0) { break }
                    $o2.Write($buf, 0, $n); $left -= $n
                }
                $o2.Dispose()
            }
        }
    }
}
Walk-Dir $rootSector $rootSize ''
$fs.Dispose()
Write-Host "$script:count files"
