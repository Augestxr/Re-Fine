# key_taps.ps1 - 0185: ninguna pulsacion se pierde. En la lista de canciones (copia de una partida con muchas
# canciones) pulsa ABAJO -Count veces durante -TapMs ms cada una y entra en la cancion bajo el cursor. Con todas las
# pulsaciones contadas, la cancion que se abre es siempre la misma; -Latch 0 (REFINE_PAD_LATCH=0) es el
# comportamiento anterior, para comparar. Imprime el guion pv_NNN que el juego abre.
# Uso:  powershell -ExecutionPolicy Bypass -File tools\key_taps.ps1 -SeedSave work\backup\partida_<fecha>\NPUB31241_SYSTEM00 [-Runs 2] [-Latch 1] [-TapMs 10] [-Fps 120]
param(
    [Parameter(Mandatory)][string]$SeedSave,
    [int]$Runs = 2,
    [int]$Latch = 1,
    [int]$TapMs = 10,
    [int]$Fps = 120,
    [int]$Count = 3,
    [double]$From = 66    # segundo de la corrida en que la lista de canciones ya esta en pantalla (linea "[fps] tasks: ... song select=1 (N s)")
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$taps = (0..($Count - 1) | ForEach-Object { '{0:0.00}:d:{1}' -f ($From + 0.25 * $_), $TapMs }) -join ','
for ($i = 1; $i -le $Runs; $i++) {
    $tag = "taps_${Latch}_$i"
    & (Join-Path $PSScriptRoot 'song_run.ps1') -Tag $tag -Close 112 -SeedSave $SeedSave -ExtraKeys $taps `
        -Env "REFINE_FPS_TARGET=$Fps,REFINE_PAD_LATCH=$Latch,REFINE_PAD_LOG=1" 6>&1 2>&1 | Out-Null
    # las veces que el juego leyo ABAJO (0x0040) recien pulsado, de las -Count del guion
    $downs = 0; $was = $false
    Select-String -Path (Join-Path $root "work\run\$tag.err.log") -Pattern '^\[pad-game\] \d+ ms buttons ([0-9A-F]{4})' | ForEach-Object {
        $on = ([Convert]::ToInt32($_.Matches[0].Groups[1].Value, 16) -band 0x40) -ne 0
        if ($on -and -not $was) { $downs++ }
        $was = $on
    }
    $m = Select-String -Path (Join-Path $root "work\run\$tag.out.log"), (Join-Path $root "work\run\$tag.err.log") -Pattern 'pv_(\d+)_\w+\.dsc' | Select-Object -First 1
    $pv = if ($m) { $m.Matches[0].Value } else { 'ninguna' }
    $sel = Select-String -Path (Join-Path $root "work\run\$tag.err.log") -Pattern 'song select=1 \(([\d.]+) s\)' | Select-Object -First 1
    $at = if ($sel) { $sel.Matches[0].Groups[1].Value } else { '?' }
    Write-Host ("latch {0}, corrida {1}: el juego leyo {4} de {5} pulsaciones; abre {2} (lista de canciones desde {3} s)" -f $Latch, $i, $pv, $at, $downs, $Count)
}
