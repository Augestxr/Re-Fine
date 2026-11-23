# img_diff.ps1 - Diferencia media entre dos capturas (0 = iguales, 255 = opuestas), reducidas a 160x90.
# Para comparar la misma pantalla al mismo segundo real a dos fps distintos.
#
# Uso:  powershell -ExecutionPolicy Bypass -File tools\img_diff.ps1 -A a.png -B b.png

param(
    [Parameter(Mandatory)][string]$A,
    [Parameter(Mandatory)][string]$B
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
function Small([string]$p) {
    $src = [Drawing.Image]::FromFile((Resolve-Path $p))
    $bmp = New-Object Drawing.Bitmap 160, 90
    $g = [Drawing.Graphics]::FromImage($bmp)
    $g.InterpolationMode = 'HighQualityBilinear'
    $g.DrawImage($src, 0, 0, 160, 90)
    $g.Dispose(); $src.Dispose()
    return $bmp
}
$x = Small $A; $y = Small $B
$sum = 0.0
for ($j = 0; $j -lt 90; $j++) {
    for ($i = 0; $i -lt 160; $i++) {
        $p = $x.GetPixel($i, $j); $q = $y.GetPixel($i, $j)
        $sum += [Math]::Abs($p.R - $q.R) + [Math]::Abs($p.G - $q.G) + [Math]::Abs($p.B - $q.B)
    }
}
$x.Dispose(); $y.Dispose()
'{0:N2}' -f ($sum / (160 * 90 * 3))
