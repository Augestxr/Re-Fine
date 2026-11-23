# compare_shots.ps1 - 4i.2: una imagen por segundo, RPCS3 a la izquierda y Re:Fine a la derecha.
#
# Uso:  powershell -ExecutionPolicy Bypass -File tools\compare_shots.ps1 -Rpcs3 r1 -ReFine k1 [-Out work\shots\4i_comparacion]

param(
    [Parameter(Mandatory = $true)][string]$Rpcs3,
    [Parameter(Mandatory = $true)][string]$ReFine,
    [string]$Out = 'work\shots\4i_comparacion'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
Add-Type -AssemblyName System.Drawing
$dst = Join-Path $root $Out
New-Item -ItemType Directory -Force $dst | Out-Null
$w = 1280; $h = 720
$sw = 640; $sh = 360
$files = Get-ChildItem (Join-Path $root "work\rpcs3\${Rpcs3}_shots") -Filter 't*.png'
$sheet = New-Object Drawing.Bitmap ($sw * 2), ($sh * $files.Count)
$gs = [Drawing.Graphics]::FromImage($sheet)
$row = 0
foreach ($f in $files) {
    $s = $f.BaseName
    $o = Join-Path $root "work\shots\${ReFine}_$s.png"
    if (-not (Test-Path $o)) { Write-Host "falta $o"; continue }
    $l = [Drawing.Image]::FromFile($f.FullName); $r = [Drawing.Image]::FromFile($o)
    $bmp = New-Object Drawing.Bitmap ($w * 2), $h
    $g = [Drawing.Graphics]::FromImage($bmp)
    $g.DrawImage($l, 0, 0, $w, $h); $g.DrawImage($r, $w, 0, $w, $h); $g.Dispose()
    $bmp.Save((Join-Path $dst "$s.png"), [Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
    $gs.DrawImage($l, 0, $row * $sh, $sw, $sh); $gs.DrawImage($r, $sw, $row * $sh, $sw, $sh)
    $l.Dispose(); $r.Dispose(); $row++
}
$gs.Dispose()
$sheet.Save((Join-Path $dst 'hoja.png'), [Drawing.Imaging.ImageFormat]::Png); $sheet.Dispose()
Get-ChildItem $dst | Select-Object Name, Length | Format-Table -AutoSize
