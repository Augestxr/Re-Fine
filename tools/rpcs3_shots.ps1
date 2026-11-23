# rpcs3_shots.ps1 - 4i.0: capturas de la PV600 con la tecla de screenshot del propio RPCS3.
#
# Uso:  powershell -ExecutionPolicy Bypass -File tools\rpcs3_shots.ps1 -Tag r1 [-At "5,10,15,20,25,30,36"]
#
# Lanza RPCS3 con un config propio, sigue su log hasta la primera apertura de pv_600.ogg (t = 0, el
# mismo suceso que busca Re:Fine) y a los segundos -At envia F12 con PostMessage SOLO a la ventana de
# juego de ese proceso: sin foco, sin SendInput y sin capturar la pantalla. RPCS3 guarda el PNG en su
# carpeta; aqui se copian a work\rpcs3\<Tag>_shots\ con el segundo en el nombre.

param(
    [Parameter(Mandatory = $true)][string]$Tag,
    [string]$At = '5,10,15,20,25,30,36',
    [string]$Config = 'work\rpcs3\config_shot2.yml',
    [string]$Anchor = 'pv_600\.ogg',    # 4c.3: 'adv\.pam' para la pelicula
    [int]$Timeout = 400,
    [int]$Settle = 900    # ms de espera tras cada F12; con 0, los PNG se reparten al final por orden
)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$ins = Join-Path (Split-Path $root -Parent) 'openF-insumos'
$rp = Join-Path $ins 'rpcs3'
$rpcs3 = Join-Path $rp 'rpcs3.exe'
$eboot = Join-Path $ins 'Hatsune Miku - Project Diva F (USA)\PS3_GAME\USRDIR\EBOOT.BIN'
$log = Join-Path $rp 'log\RPCS3.log'
$config = Join-Path $root $Config
$out = Join-Path $root "work\rpcs3\${Tag}_shots"
if (Get-Process rpcs3 -ErrorAction SilentlyContinue) { throw 'Ya hay un rpcs3.exe abierto: cierralo antes' }
New-Item -ItemType Directory -Force $out | Out-Null

Add-Type -Namespace ReFine -Name Win -MemberDefinition @'
public delegate bool EnumProc(IntPtr h, IntPtr l);
[DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc f, IntPtr l);
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
[DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
[DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, System.Text.StringBuilder s, int n);
[DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
'@

function Find-GameWindow([int]$procId) {
    $script:found = [IntPtr]::Zero
    $cb = [ReFine.Win+EnumProc] {
        param($h, $l)
        [uint32]$w = 0; $null = [ReFine.Win]::GetWindowThreadProcessId($h, [ref]$w)
        if ($w -eq $procId -and [ReFine.Win]::IsWindowVisible($h)) {
            $s = New-Object Text.StringBuilder 256
            $null = [ReFine.Win]::GetWindowText($h, $s, 256)
            if ($s.ToString() -like 'FPS:*') { $script:found = $h; return $false }
        }
        return $true
    }
    $null = [ReFine.Win]::EnumWindows($cb, [IntPtr]::Zero)
    return $script:found
}

function Get-NewPngs([string[]]$dirs, [datetime]$since) {
    foreach ($d in $dirs) {
        if (Test-Path $d) { Get-ChildItem $d -Recurse -Filter *.png | Where-Object { $_.LastWriteTime -gt $since } }
    }
}

$start = Get-Date
$p = Start-Process -FilePath $rpcs3 -ArgumentList @('--no-gui', '--config', "`"$config`"", "`"$eboot`"") `
    -WorkingDirectory $rp -PassThru
$null = $p.Handle
Write-Host "== $Tag : pid $($p.Id), inicio $($start.ToString('HH:mm:ss'))"

# Espera al ancla leyendo el log por el final, sin bloquear a RPCS3.
Start-Sleep -Seconds 3
$fs = New-Object IO.FileStream($log, 'Open', 'Read', 'ReadWrite,Delete')
$sr = New-Object IO.StreamReader($fs)
$t0 = $null; $line0 = ''
$deadline = $start.AddSeconds($Timeout)
while (-not $t0) {
    if ($p.HasExited) { throw 'RPCS3 termino antes de la PV' }
    if ((Get-Date) -gt $deadline) { break }
    $l = $sr.ReadLine()
    if ($null -eq $l) { Start-Sleep -Milliseconds 50; continue }
    if ($l -match "sys_fs_open\(path=.*$Anchor") { $t0 = Get-Date; $line0 = $l.Substring(0, 20) }
}
if (-not $t0) { $p.CloseMainWindow() | Out-Null; throw "No llego a $Anchor dentro del tiempo" }
Write-Host "ancla ${Anchor}: $line0 (a $([int]($t0 - $start).TotalSeconds) s de reloj)"
# 4j.1: si Dump to file esta activo, tamano del volcado de audio en el ancla, para situarla en el WAV.
Get-ChildItem $rp -Recurse -Depth 2 -Filter *.wav -ErrorAction SilentlyContinue |
    Where-Object { $_.LastWriteTime -gt $start } | ForEach-Object {
        $wf = New-Object IO.FileStream($_.FullName, 'Open', 'Read', 'ReadWrite,Delete')
        Write-Host "wav en el ancla: $($_.FullName) $($wf.Length) bytes"; $wf.Close()
    }

$h = Find-GameWindow $p.Id
Write-Host "ventana de juego: 0x$('{0:X}' -f $h.ToInt64())"
if ($h -eq [IntPtr]::Zero) { $p.CloseMainWindow() | Out-Null; throw 'Sin ventana de juego' }

$VK_F12 = [IntPtr]0x7B
$lpDown = [IntPtr]0x00580001   # scan 0x58
$lpUp = [IntPtr]([int64]0xC0580001)
$dirs = @((Join-Path $rp 'screenshots'), (Join-Path $rp 'captures'))
foreach ($s in ($At -split ',' | ForEach-Object { [double]$_ })) {
    $ms = [int](($t0.AddSeconds($s) - (Get-Date)).TotalMilliseconds)
    if ($ms -gt 0) { Start-Sleep -Milliseconds $ms }
    $sent = Get-Date
    $null = [ReFine.Win]::PostMessage($h, 0x100, $VK_F12, $lpDown)
    Start-Sleep -Milliseconds 60
    $null = [ReFine.Win]::PostMessage($h, 0x101, $VK_F12, $lpUp)
    if (-not $Settle) { continue }
    Start-Sleep -Milliseconds $Settle
    $new = @(Get-NewPngs $dirs $sent.AddSeconds(-1))
    Write-Host ("+{0,4:0.0} s enviada; PNG nuevos: {1}" -f (($sent - $t0).TotalSeconds), $new.Count)
    $new | Sort-Object LastWriteTime | Select-Object -Last 1 | ForEach-Object {
        Copy-Item $_.FullName (Join-Path $out ("t{0:00}.png" -f [int]$s))
    }
}
Start-Sleep -Seconds 2
if (-not $Settle) {
    $times = @($At -split ',' | ForEach-Object { [double]$_ })
    $pngs = @(Get-NewPngs $dirs $t0 | Sort-Object Name)
    Write-Host "F12 enviadas: $($times.Count); PNG: $($pngs.Count)"
    for ($i = 0; $i -lt $pngs.Count; $i++) {
        Copy-Item $pngs[$i].FullName (Join-Path $out ('{0:000}_{1}' -f $i, $pngs[$i].Name))
    }
}
if (-not $p.CloseMainWindow()) { Write-Host 'CloseMainWindow sin efecto' }
if (-not $p.WaitForExit(15000)) { Stop-Process -Id $p.Id -Force }
$sr.Close()
Get-ChildItem $out | Select-Object Name, Length | Format-Table -AutoSize
