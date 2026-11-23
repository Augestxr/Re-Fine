# song_start.ps1 - falla (exit 1) si la cancion no empieza: titulo -> PRESS START -> seleccion -> la cancion
# bajo el cursor (Cat Food con la partida de prueba) con el perfil test, via tools\song_run.ps1. Empezar = el juego abre un guion pv_NNN_*.dsc, carga el
# escenario de la cancion y no hay lecturas nulas del juego al elegirla (el fallo visto a 144 fps, 2026-11-12:
# carga el escenario de prueba STGTST y se queda sin cancion).
#
# Uso:  powershell -ExecutionPolicy Bypass -File tools\song_start.ps1 [-Fps 30,120] [-Renderer vulkan,d3d12]
#           [-SeedSave carpeta NPUB31241_SYSTEM00 (una copia)] [-Close 118]
param(
    [string]$Fps = '30,120',          # con powershell -File una lista llega como una cadena: se parte por comas
    [string]$Renderer = 'vulkan',
    [string]$SeedSave = '',
    [int]$Close = 118,
    [string]$Env = ''              # mas variables, p. ej. "REFINE_REALTIME=1"
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$bad = 0
foreach ($r in $Renderer.Split(',')) { foreach ($f in $Fps.Split(',')) {
    $tag = "start_${r}_$f"
    $e = "REFINE_FPS_TARGET=$f,REFINE_RENDERER=$r"
    if ($Env) { $e += ",$Env" }
    $a = @{ Tag = $tag; Close = $Close; Env = $e }
    if ($SeedSave) { $a.SeedSave = $SeedSave }
    & (Join-Path $PSScriptRoot 'song_run.ps1') @a 6>&1 2>&1 | Out-Null
    $log = Join-Path $root "work\run\$tag.err.log"
    $dsc = @(Select-String -Path $log -Pattern "open '[^']*/pv_\d+[^']*\.dsc' -> fd").Count
    $stage = @(Select-String -Path $log -Pattern 'objset/stgpv\d+').Count
    $null0 = @(Select-String -Path $log -Pattern '^\[null-read\]').Count
    $ok = $dsc -gt 0 -and $stage -gt 0 -and $null0 -eq 0
    if (-not $ok) { $bad++ }
    Write-Host ("{0} {1,-7} {2,3} fps: guion {3}, escenario {4}, lecturas nulas {5}" -f $(if ($ok) { 'OK  ' } else { 'FALLA' }), $r, $f, $dsc, $stage, $null0)
} }
if ($bad) { Write-Host "song_start: $bad corrida(s) sin cancion (logs en work\run\start_*)"; exit 1 }
Write-Host 'song_start: la cancion empieza en todas'
