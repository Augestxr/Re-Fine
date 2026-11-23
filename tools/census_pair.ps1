# census_pair.ps1 - 6d: hoja con un paso del censo, fila de arriba a 30 y de abajo al otro fps,
# en los mismos instantes. Uso:
#   powershell -ExecutionPolicy Bypass -File tools\census_pair.ps1 -Tag m0 -Step carga [-Fps 120] [-K 0,3,6,9,12,15,20,27]
param(
    [Parameter(Mandatory)][string]$Tag,
    [Parameter(Mandatory)][string[]]$Step,
    [int]$Fps = 120,
    [string]$K = '0,3,6,9,12,15,20,27',
    [double]$Scale = 0.3,
    [string]$Crop = ''
)
$root = Split-Path $PSScriptRoot -Parent
$ks = $K -split ',' | ForEach-Object { [int]$_ }
$Step = @($Step | ForEach-Object { $_ -split ',' })
$files = foreach ($s in $Step) { foreach ($f in 30, $Fps) { foreach ($i in $ks) {
    Join-Path $root ("work\census\{0}_{1}\{2}_{3:00}.bmp" -f $Tag, $f, $s, $i) } } }
$out = Join-Path $root "work\census\${Tag}_$($Step -join '+')_$Fps.png"
& (Join-Path $PSScriptRoot 'shots_sheet.ps1') -Files ($files -join ',') -Out $out -Cols $ks.Count -Scale $Scale -Crop $Crop
