# song_run.ps1 - 6.0: entra en Cat Food (pv_600) con el perfil test, saltando la pelicula con
# START, y la juega con PAD_BOT. Una corrida corta para medir en cancion, sin barridos.
#
# Uso:  powershell -ExecutionPolicy Bypass -File tools\song_run.ps1 -Tag s1 [-Close 170]
#           [-Env "FRAME_PROF=100-160,PS3_FRAMESTATS=100-160"] [-Shots '110,140'] [-Exe otro.exe]
#
# Las teclas son las de k2 (5b.4), adelantadas: la pelicula se salta a los 30 s en vez de
# esperar a los 122. El perfil es siempre test.

param(
    [Parameter(Mandatory)][string]$Tag,
    [int]$Close = 170,
    [string]$Env = '',
    [string]$Shots = '',
    [string]$Exe = '',
    [int]$Shift = 84,
    [string]$ExtraKeys = '',  # mas eventos de PAD_KEYS, p. ej. el OK de resultados: '334:n:250'
    [string]$SeedSave = ''    # carpeta NPUB31241_SYSTEM00 (una COPIA, p. ej. workackup\partida_<fecha>\...)
)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$ev = New-Object Collections.ArrayList
foreach ($x in '122:s', '127:n', '131:n', '134:n', '148:n', '156:n', '161:n') {
    $p = $x.Split(':'); [void]$ev.Add("$([int]$p[0] - $Shift):$($p[1]):250")
}
for ($t = 168; $t -le 198; $t += 3) { [void]$ev.Add("$($t - $Shift):n:250") }
$env:PAD_SCRIPT = '30:0x0008'
if ($ExtraKeys) { [void]$ev.Add($ExtraKeys) }
$env:PAD_KEYS = $ev -join ','
if ($Shots) {
    $job = Start-Job -ScriptBlock { param($r, $t, $a)
        Set-Location $r
        powershell -ExecutionPolicy Bypass -File tools\window_shot.ps1 -Name ReFine -At $a -Out "work\6_0\$t"
    } -ArgumentList $root, $Tag, $Shots
}
$envs = "PS3_VERBOSE=0,PAD_BOT=0"
if ($Env) { $envs += ",$Env" }
$a = @{ Timeout = $Close + 5; CloseAt = $Close; Tag = $Tag; Env = $envs }
if ($Exe) { $a.Exe = $Exe }
if ($SeedSave) { $a.SeedSave = $SeedSave }
& (Join-Path $PSScriptRoot 'run.ps1') @a
if ($Shots) { Receive-Job $job -Wait | Out-Null }
