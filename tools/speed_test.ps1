# speed_test.ps1 - la prueba de velocidad (docs/tiempo_real.md): el mismo recorrido (tools\speed_route.txt,
# pulsaciones atadas a eventos del juego) a varios fps, y la comparacion de la hora real de cada evento contra
# la corrida a 30 (tools\speed_cmp.py). Lo que a mas fps llega antes o despues cuenta frames, no tiempo.
#
# Uso:  powershell -ExecutionPolicy Bypass -File tools\speed_test.ps1 [-Fps 30,120] [-Route tools\speed_route.txt]
#           [-Env "REFINE_REALTIME=0"] [-Noise] [-Tag sp] [-Skip30]
# -Noise: una segunda corrida a 30 para apartar lo que ya varia entre dos corridas iguales (cargas).
# -Skip30: reutiliza la corrida a 30 anterior del mismo -Tag.
# Nadie debe tocar la ventana del juego durante las corridas (el raton mete pulsaciones).
param(
    [string]$Fps = '30,120',
    [string]$Route = '',
    [string]$Env = '',
    [switch]$Noise,
    [string]$Tag = 'sp',
    [switch]$Skip30,
    [int]$Timeout = 400
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
if (-not $Route) { $Route = Join-Path $PSScriptRoot 'speed_route.txt' }
$Route = (Resolve-Path $Route).Path
$list = @($Fps.Split(',') | ForEach-Object { [int]$_ })
function Run([string]$tag, [int]$f) {
    $e = "PS3_VERBOSE=0,PAD_BOT=0,REFINE_FPS_TARGET=$f,REFINE_ROUTE=$Route"
    if ($Env) { $e += ",$Env" }
    & (Join-Path $PSScriptRoot 'run.ps1') -Timeout $Timeout -Tag $tag -Env $e 6>&1 2>&1 | Out-Null
    $log = Join-Path $root "work\run\$tag.err.log"
    $end = Select-String -Path $log -Pattern '^\[ev\] [\d.]+ route (.*)$' | Select-Object -Last 1
    Write-Host ("{0}: {1} fps, fin del guion: {2}" -f $tag, $f, $(if ($end) { $end.Matches[0].Groups[1].Value } else { 'NO LLEGO' }))
    return $log
}
$base = Join-Path $root "work\run\${Tag}_30.err.log"
if (-not $Skip30) { $base = Run "${Tag}_30" 30 }
$noiseLog = Join-Path $root "work\run\${Tag}_30b.err.log"
if ($Noise -and -not $Skip30) { $noiseLog = Run "${Tag}_30b" 30 }
$bad = 0
foreach ($f in $list) {
    if ($f -eq 30) { continue }
    $log = Run "${Tag}_$f" $f
    Write-Host "== $f fps contra 30 =="
    $a = @((Join-Path $PSScriptRoot 'speed_cmp.py'), $base, $log)
    if ($Noise -and (Test-Path $noiseLog)) { $a += @('--noise', $noiseLog) }
    python @a
    if ($LASTEXITCODE -ne 0) { $bad++ }
}
if ($bad) { exit 1 }
