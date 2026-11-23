# mem_watch.ps1 - 6d.12 (P5): memoria real de ReFine.exe cada segundo, desde fuera y solo leyendo.
#
# Uso:  powershell -ExecutionPolicy Bypass -File tools\mem_watch.ps1 -Out work\x\mem.csv [-Seconds 200]
#
# Espera a que arranque ReFine.exe y apunta, por segundo desde que aparece: working set y bytes
# privados (Get-Process) y VRAM dedicada y compartida del proceso (contadores "GPU Process Memory"
# de Windows 10+). Acaba cuando el proceso sale o pasan -Seconds.
param([Parameter(Mandatory)][string]$Out, [int]$Seconds = 400)
[Globalization.CultureInfo]::CurrentCulture = [Globalization.CultureInfo]::InvariantCulture
$p = $null
for ($i = 0; $i -lt 120 -and -not $p; $i++) { $p = Get-Process ReFine -ErrorAction SilentlyContinue | Select-Object -First 1; if (-not $p) { Start-Sleep -Milliseconds 500 } }
if (-not $p) { throw 'ReFine.exe no arranco' }
$t0 = $p.StartTime
't,ws_mb,private_mb,gpu_dedicated_mb,gpu_shared_mb' | Set-Content $Out
$paths = @("\GPU Process Memory(pid_$($p.Id)_*)\Dedicated Usage", "\GPU Process Memory(pid_$($p.Id)_*)\Shared Usage")
while (-not $p.HasExited -and ((Get-Date) - $t0).TotalSeconds -lt $Seconds) {
    $p.Refresh()
    $ded = 0; $sh = 0
    try {
        $c = Get-Counter -Counter $paths -ErrorAction Stop
        foreach ($s in $c.CounterSamples) { if ($s.Path -like '*dedicated usage') { $ded += $s.CookedValue } else { $sh += $s.CookedValue } }
    } catch {}
    $t = ((Get-Date) - $t0).TotalSeconds
    '{0:0.0},{1:0.0},{2:0.0},{3:0.0},{4:0.0}' -f $t, ($p.WorkingSet64 / 1MB), ($p.PrivateMemorySize64 / 1MB), ($ded / 1MB), ($sh / 1MB) | Add-Content $Out
    Start-Sleep -Milliseconds 1000
}
