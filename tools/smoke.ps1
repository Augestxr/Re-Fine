# smoke.ps1 - prueba rapida de cada sesion (unos 2 min): titulo y un tramo corto de Cat Food
# (pv_600) con el perfil test, via tools\song_run.ps1. Imprime como mucho 10 lineas; el detalle
# queda en work\run\<Tag>.out.log / .err.log y work\run\<Tag>.smoke.txt.
#
# Uso:  powershell -ExecutionPolicy Bypass -File tools\smoke.ps1 [-Tag smoke] [-Close 120] [-Exe otro.exe]
#
# Que mira:
#   code       salida del proceso (run.ps1)
#   imports    lineas "unresolved NID" (con PS3_HLE_UNRESOLVED=fail paran el juego)
#   PSO FAIL   lineas "PSO FAIL" o "PSO creation FAIL" del backend
#   flips/s    [FRAMESTATS] de la ventana -Window (por defecto 95-115 s, ya en cancion)
#   cancion    si se abrio un pv_*.dsc (el juego cargo el guion de la cancion)
#
# ASSUMPTION: con las teclas de song_run (pelicula saltada a los 38 s) la cancion ya corre a los
# 95 s; sin correr todavia (sesion de solo docs, 2026-11-07). La primera corrida lo confirma.

param(
    [string]$Tag = 'smoke',
    [int]$Close = 120,
    [string]$Window = '95-115',
    [string]$Exe = ''
)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$a = @{ Tag = $Tag; Close = $Close; Env = "PS3_FRAMESTATS=$Window" }
if ($Exe) { $a.Exe = $Exe }

$runOut = & (Join-Path $PSScriptRoot 'song_run.ps1') @a 6>&1 2>&1 | Out-String
$detail = Join-Path $root "work\run\$Tag.smoke.txt"
$runOut | Set-Content -Encoding utf8 $detail

$logs = @("work\run\$Tag.out.log", "work\run\$Tag.err.log") | ForEach-Object { Join-Path $root $_ } |
    Where-Object { Test-Path $_ }
function Hits($pat) { @(Select-String -Path $logs -Pattern $pat -ErrorAction SilentlyContinue) }

$code = ($runOut -split "`n" | Where-Object { $_ -match '^== ' } | Select-Object -First 1)
if (-not $code) { $code = '(sin linea de run.ps1)' }
$unres = Hits 'unresolved NID'
$pso = Hits 'PSO (creation )?FAIL'
$flips = Hits '\[FRAMESTATS\] flips=' | Select-Object -First 1
$dsc = Hits "open '[^']*/pv_\d+[^']*\.dsc' -> fd" | Select-Object -First 1

Write-Host "SMOKE $Tag"
Write-Host "  code:     $($code.Trim())"
Write-Host "  imports sin handler: $($unres.Count)"
if ($unres.Count) { Write-Host "    1o: $($unres[0].Line.Trim())" }
Write-Host "  PSO FAIL: $($pso.Count)"
if ($flips) { Write-Host "  flips:    $(($flips.Line -replace '^.*flips=', '').Trim())" }
else { Write-Host "  flips:    sin [FRAMESTATS] (no llego a $Window s)" }
if ($dsc) { Write-Host "  cancion:  SI ($([regex]::Match($dsc.Line, 'pv_\d+[^/'']*\.dsc').Value))" }
else { Write-Host "  cancion:  NO" }
Write-Host "  detalle:  work\run\$Tag.*"
