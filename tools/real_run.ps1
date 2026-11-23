# real_run.ps1 - 6b+6c p3: como song_run.ps1 (Cat Food, pv_600, perfil test), pero con teclas
# reales del teclado (SendInput, REAL_KEYS y REAL_BOT de src/main.cpp) en vez de PAD_KEYS/PAD_BOT:
# el mismo camino que usa the author (GetAsyncKeyState y los mensajes de la ventana).
# Trae la ventana al frente y mueve el estado real del teclado: no tocar el PC mientras corre.
#
# Uso:  powershell -ExecutionPolicy Bypass -File tools\real_run.ps1 -Tag k1 [-Close 150]
#           [-Fps 120] [-Extra '95:f:120;97:f:120'] [-Env "PAD_NO_SDL=1"] [-NoBot]
# Letras: las de PAD_KEYS (n Enter, s Espacio, u d l r flechas...), t Tab, f F1; raton: ^ v rueda, < > * clics.
# Resumen: lineas [menu], [real] REFUSED y los contadores [BOT] count al final del log.

param(
    [Parameter(Mandatory)][string]$Tag,
    [int]$Close = 150,
    [int]$Fps = 0,
    [string]$Extra = '',
    [string]$Env = '',
    [switch]$NoBot,
    [switch]$Mouse   # los Enter de los menus (43, 47, 50 s) con clic izquierdo y la rueda abajo/arriba a 45/46 s
)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$ev = New-Object Collections.ArrayList
# Las de song_run.ps1 (k2 de 5b.4 adelantadas 84 s) y START a los 30 s para saltar la pelicula.
$base = if ($Mouse) { '30:s', '38:s', '43:<', '45:v', '46:^', '47:<', '50:<', '64:n', '72:n', '77:n' } else { '30:s', '38:s', '43:n', '47:n', '50:n', '64:n', '72:n', '77:n' }
foreach ($x in $base) {
    $p = $x.Split(':'); [void]$ev.Add("$($p[0]):$($p[1]):250")
}
for ($t = 84; $t -le 114; $t += 3) { [void]$ev.Add("$($t):n:250") }
if ($Extra) { foreach ($x in $Extra.Split(';')) { [void]$ev.Add($x) } }
$sorted = $ev | Sort-Object { [double]($_.Split(':')[0]) }
$envs = "PS3_VERBOSE=0,REAL_KEYS=$($sorted -join ';')"
if (-not $NoBot) { $envs += ",REAL_BOT=0" }
if ($Fps) { $envs += ",REFINE_FPS_TARGET=$Fps" }
if ($Env) { $envs += ",$Env" }
& (Join-Path $PSScriptRoot 'run.ps1') -Timeout ($Close + 5) -CloseAt $Close -Tag $Tag -Env $envs
$log = Join-Path $root "work\run\$Tag.err.log"
$lines = Get-Content $log
$lines | Select-String -Pattern '^\[menu\] (bar|closed|input|panel)|REFUSED|keyboard active|SDL gamepad connected' | Select-Object -First 20 | ForEach-Object { $_.Line }
$counts = @{}
foreach ($l in $lines) { if ($l -match '^\[BOT\] count t=\S+ \+0x(\w+)=(\d+)') { $counts[$Matches[1]] = $Matches[2] } }
"BOT counts (+0C COOL, +10 FINE, +14 SAFE, +18 SAD, +1C WORST; toolsbot_stats.py): " + (($counts.Keys | Sort-Object | ForEach-Object { "$_=$($counts[$_])" }) -join ' ')
"BOT presses: " + ($lines | Select-String -Pattern '^\[BOT\] press' ).Count
