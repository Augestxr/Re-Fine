# shots_sheet.ps1 - 6d: hoja de contacto PNG con las capturas BMP de SHOTS (src/main.cpp).
#
# Uso:  powershell -ExecutionPolicy Bypass -File tools\shots_sheet.ps1 -Files a.bmp,b.bmp,... -Out hoja.png
#           [-Cols 4] [-Scale 0.5]
#
# Pone cada imagen con su nombre debajo, en filas de -Cols. Solo lee las capturas de la ventana.

param(
    [Parameter(Mandatory)][string[]]$Files,
    [Parameter(Mandatory)][string]$Out,
    [int]$Cols = 4,
    [double]$Scale = 0.5,
    [string]$Crop = ''     # x,y,ancho,alto en pixeles de la captura: solo esa zona
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$Files = @($Files | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
$imgs = $Files | ForEach-Object { [Drawing.Image]::FromFile((Resolve-Path $_).Path) }
$src = if ($Crop) { $c = $Crop -split ',' | ForEach-Object { [int]$_ }; New-Object Drawing.Rectangle $c[0], $c[1], $c[2], $c[3] }
       else { New-Object Drawing.Rectangle 0, 0, $imgs[0].Width, $imgs[0].Height }
$w = [int]($src.Width * $Scale); $h = [int]($src.Height * $Scale); $lab = 14
$rows = [math]::Ceiling($imgs.Count / $Cols)
$sheet = New-Object Drawing.Bitmap ($w * $Cols), (($h + $lab) * $rows)
$g = [Drawing.Graphics]::FromImage($sheet)
$g.Clear([Drawing.Color]::Black)
$font = New-Object Drawing.Font 'Arial', 8
for ($i = 0; $i -lt $imgs.Count; $i++) {
    $x = ($i % $Cols) * $w; $y = [math]::Floor($i / $Cols) * ($h + $lab)
    $g.DrawImage($imgs[$i], (New-Object Drawing.Rectangle $x, $y, $w, $h), $src, [Drawing.GraphicsUnit]::Pixel)
    $g.DrawString([IO.Path]::GetFileNameWithoutExtension($Files[$i]), $font, [Drawing.Brushes]::White, $x + 2, $y + $h)
}
$g.Dispose()
$sheet.Save($Out, [Drawing.Imaging.ImageFormat]::Png)
$sheet.Dispose(); $imgs | ForEach-Object { $_.Dispose() }
Write-Host "hoja $Out ($($Files.Count) imagenes)"
