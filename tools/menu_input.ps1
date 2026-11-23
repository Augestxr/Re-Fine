# menu_input.ps1 - con el menu del port (F1) abierto, el juego no recibe nada (0196, docs/revision-2026-11-21.md).
#
# Recorre tools\menu_route.txt: abre el menu en la lista de canciones, en la pausa de una cancion (con "reanudar"
# elegido) y en plena cancion, y pulsa en el. Entre "in_" y "end_" el juego no debe leer ningun boton (salvo el
# START con el que el menu pausa la cancion) y la cancion debe seguir parada (velocidad 0) al cerrarlo.
#
# Uso:  powershell -ExecutionPolicy Bypass -File tools\menu_input.ps1 [-Fps 30,60,120] [-Exe otro.exe]
param([string]$Fps = '30,60,120', [string]$Exe = '', [string]$Env = '')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$route = Join-Path $PSScriptRoot 'menu_route.txt'
$bad = 0
foreach ($f in $Fps.Split(',')) {
    $tag = "menuin_$f"
    $e = "PS3_VERBOSE=0,PAD_BOT=0,REFINE_PAD_LOG=1,REFINE_ROUTE=$route,REFINE_FPS_TARGET=$f"
    if ($Env) { $e += ",$Env" }
    $a = @{ Timeout = 240; Tag = $tag; Env = $e }
    if ($Exe) { $a.Exe = $Exe }
    & (Join-Path $PSScriptRoot 'run.ps1') @a 6>&1 2>&1 | Out-Null
    $sec = ''; $seen = @{}; $speed = @{}; $was = 0
    foreach ($l in Get-Content (Join-Path $root "work\run\$tag.err.log")) {
        if ($l -match '^\[ev\] [\d.]+ mark in_(\w+)') { $sec = $Matches[1]; $seen[$sec] = @(); $speed[$sec] = @() }
        elseif ($l -match '^\[ev\] [\d.]+ mark end_') { $sec = '' }
        elseif ($sec -and $l -match '^\[ev\] [\d.]+ gamespeed ([\d.]+)') { $speed[$sec] += $Matches[1] }
        elseif ($l -match '^\[pad-game\] \d+ ms buttons ([0-9A-F]+)') {
            $b = [Convert]::ToInt32($Matches[1], 16); $new = $b -band (-bnot $was); $was = $b
            if ($sec -eq 'song') { $new = $new -band (-bnot 8) }
            if ($sec -and $new) { $seen[$sec] += ('{0:X4}' -f $new) }
        }
    }
    foreach ($s in 'list', 'pause', 'song') {
        $ok = $seen.ContainsKey($s) -and $seen[$s].Count -eq 0 -and $speed[$s].Count -eq 2
        if ($s -ne 'list') { $ok = $ok -and -not ($speed[$s] | Where-Object { [double]$_ -ne 0 }) }
        if (-not $ok) { $bad++ }
        Write-Host ("{0} {1,3} fps, {2,-5}: botones que vio el juego [{3}]; velocidad al cerrar y 2,5 s despues [{4}]" -f
            $(if ($ok) { 'OK   ' } else { 'FALLA' }), $f, $s, ($seen[$s] -join ' '), ($speed[$s] -join ' '))
    }
}
if ($bad) { exit 1 }
