# key_burst.ps1 - ninguna pulsacion se pierde a ningun fps (0191, docs/tiempo_real.md).
#
# Recorre el juego hasta la lista de canciones (tools\key_route.txt) y manda -Count pulsaciones de ABAJO, una
# cada -Period ms, encoladas donde se encola la de una tecla real. Al final compara: encoladas = dadas al
# juego + leidas mientras seguian pulsadas, 0 descartadas, y los flancos que el juego leyo ([pad-game]).
#
# Uso:  powershell -ExecutionPolicy Bypass -File tools\key_burst.ps1 [-Fps 30,120,240] [-Count 1000] [-Period 80]
param(
    [string]$Fps = '30,120,240',
    [int]$Count = 1000,
    [int]$Period = 150,
    [string]$Env = 'REFINE_REALTIME=1'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$route = Join-Path $root 'work\rt\key_route.txt'
New-Item -ItemType Directory -Force (Split-Path $route) | Out-Null
(Get-Content (Join-Path $PSScriptRoot 'key_route.txt')) -replace 'taps d \d+ \d+', "taps d $Count $Period" | Set-Content $route
$bad = 0
foreach ($f in $Fps.Split(',')) {
    $tag = "taps_$f"
    $e = "PS3_VERBOSE=0,PAD_BOT=0,REFINE_PAD_LOG=1,REFINE_ROUTE=$route,REFINE_FPS_TARGET=$f"
    if ($Env) { $e += ",$Env" }
    & (Join-Path $PSScriptRoot 'run.ps1') -Timeout ([int](60 + $Count * $Period / 1000 + 90)) -Tag $tag -Env $e 6>&1 2>&1 | Out-Null
    $log = Join-Path $root "work\run\$tag.err.log"
    $m = Select-String -Path $log -Pattern '^\[ev\] [\d.]+ taps (.*)$' | Select-Object -Last 1
    # flancos de ABAJO (0x0040) que el juego leyo, solo desde la marca "taps"
    $from = (Select-String -Path $log -Pattern '^\[ev\] ([\d.]+) mark taps' | Select-Object -Last 1)
    $t0 = if ($from) { [double]$from.Matches[0].Groups[1].Value } else { 0 }
    $edges = 0; $was = $false; $first = $null
    Select-String -Path $log -Pattern '^\[pad-game\] (\d+) ms buttons ([0-9A-F]{4})' | ForEach-Object {
        $ms = [double]$_.Matches[0].Groups[1].Value
        if (-not $first) { $first = $ms }
        $on = ([Convert]::ToInt32($_.Matches[0].Groups[2].Value, 16) -band 0x40) -ne 0
        if ($ms - $first -ge $t0 - 1000) { if ($on -and -not $was) { $edges++ } }
        $was = $on
    }
    $txt = if ($m) { $m.Matches[0].Groups[1].Value } else { 'SIN LINEA taps (el guion no llego)' }
    $ok = $m -and $txt -match 'dropped 0' -and $edges -ge $Count
    if (-not $ok) { $bad++ }
    Write-Host ("{0} {1,3} fps: {2}; flancos leidos por el juego {3} de {4}" -f $(if ($ok) { 'OK   ' } else { 'FALLA' }), $f, $txt, $edges, $Count)
}
if ($bad) { Write-Host "key_burst: $bad corrida(s) con pulsaciones perdidas (logs en work\run\taps_*)"; exit 1 }
Write-Host 'key_burst: ninguna pulsacion perdida'
