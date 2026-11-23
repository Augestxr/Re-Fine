# rpcs3_song.ps1 - 5c.3: entra en una cancion en RPCS3, sin manos, y mide con rpcs3_watch y loopback.
#
# Uso:  powershell -ExecutionPolicy Bypass -File tools\rpcs3_song.ps1 -Tag r1
#
# Como tools\rpcs3_edit.ps1: sigue el log y manda teclas por PostMessage a la ventana de juego. Usa
# la partida que RPCS3 ya tiene. Cierra a los pocos segundos de cancion, antes de los resultados,
# para no autoguardar. Salidas en work\5c\<Tag>*.

param(
    [Parameter(Mandatory = $true)][string]$Tag,
    [string]$Config = 'work\rpcs3\config_cancion.yml',
    [int]$SongSeconds = 16,
    [string]$Shots = '',        # 6d.12: segundos de cancion con captura F12 (rpcs3_watch), "20,29.1,45.6"
    [int]$Timeout = 300,
    [switch]$Record,            # 6d.13: graba la zona de cliente de la ventana de juego con ffmpeg (gdigrab) en <Tag>.mkv,
                                # y <Tag>.sync (reloj de pared y QPC a la vez) para pasar a segundos de cancion
    [int]$Down = 0              # 6d.13: veces abajo en la lista de canciones antes de elegir (0 = la primera)
)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$ins = Join-Path (Split-Path $root -Parent) 'openF-insumos'
$rp = Join-Path $ins 'rpcs3'
$rpcs3 = Join-Path $rp 'rpcs3.exe'
$eboot = Join-Path $ins 'Hatsune Miku - Project Diva F (USA)\PS3_GAME\USRDIR\EBOOT.BIN'
$log = Join-Path $rp 'log\RPCS3.log'
$config = Join-Path $root $Config
$out = Join-Path $root "work\5c\$Tag"
if (Get-Process rpcs3 -ErrorAction SilentlyContinue) { throw 'Ya hay un rpcs3.exe abierto: cierralo antes' }
# 6d.13: un <Tag>.clock de otra corrida hacia creer que la cancion ya habia empezado (sin mas X)
Remove-Item "$out.clock", "$out.mkv", "$out.sync" -ErrorAction SilentlyContinue

Add-Type -Namespace ReFine -Name Keys -MemberDefinition @'
public delegate bool EnumProc(IntPtr h, IntPtr l);
[DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc f, IntPtr l);
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
[DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
[DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, System.Text.StringBuilder s, int n);
[DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
[DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
[StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
[StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
[DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
[DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
'@
$null = [ReFine.Keys]::SetProcessDPIAware()

function Find-GameWindow([int]$procId) {
    $script:found = [IntPtr]::Zero
    $cb = [ReFine.Keys+EnumProc] {
        param($h, $l)
        [uint32]$w = 0; $null = [ReFine.Keys]::GetWindowThreadProcessId($h, [ref]$w)
        if ($w -eq $procId -and [ReFine.Keys]::IsWindowVisible($h)) {
            $s = New-Object Text.StringBuilder 256
            $null = [ReFine.Keys]::GetWindowText($h, $s, 256)
            if ($s.ToString() -like 'FPS:*') { $script:found = $h; return $false }
        }
        return $true
    }
    $null = [ReFine.Keys]::EnumWindows($cb, [IntPtr]::Zero)
    return $script:found
}

$K = @{ X = 0x58, 0x2D; Enter = 0x0D, 0x1C; F12 = 0x7B, 0x58; Down = 0x28, 0x50 }
function Key([string]$name, [int]$holdMs = 200) {
    $h = Find-GameWindow $p.Id
    $vk = [IntPtr]$K[$name][0]; $scan = [int64]$K[$name][1]
    $null = [ReFine.Keys]::PostMessage($h, 0x100, $vk, [IntPtr](1 + ($scan -shl 16)))
    Start-Sleep -Milliseconds $holdMs
    $null = [ReFine.Keys]::PostMessage($h, 0x101, $vk, [IntPtr]([int64]0xC0000001 + ($scan -shl 16)))
}

$start = Get-Date
$p = Start-Process -FilePath $rpcs3 -ArgumentList @('--no-gui', '--config', "`"$config`"", "`"$eboot`"") `
    -WorkingDirectory $rp -PassThru
$null = $p.Handle
Write-Host "== $Tag : pid $($p.Id)"
$w = Start-Process -FilePath (Join-Path $root 'work\5c\rpcs3_watch.exe') -ArgumentList "$($p.Id) `"$out`" $SongSeconds $Shots" `
    -PassThru -RedirectStandardOutput "$out.watch.txt" -WindowStyle Hidden
$lb = Start-Process -FilePath (Join-Path $root 'work\5c\audio_loopback.exe') -ArgumentList "`"$out`" $Timeout" `
    -PassThru -RedirectStandardOutput "$out.names.txt" -WindowStyle Hidden
Start-Sleep -Seconds 3
$fs = New-Object IO.FileStream($log, 'Open', 'Read', 'ReadWrite,Delete')
$sr = New-Object IO.StreamReader($fs)

function WaitLog([string]$re, [scriptblock]$each = $null, [double]$everyS = 3) {
    $deadline = $start.AddSeconds($Timeout)
    $next = (Get-Date).AddSeconds($everyS)
    while ($true) {
        if ($p.HasExited) { throw 'RPCS3 termino antes de tiempo' }
        if ((Get-Date) -gt $deadline) { $null = $p.CloseMainWindow(); throw "No llego a $re" }
        $l = $sr.ReadLine()
        if ($null -eq $l) {
            if ($each -and (Get-Date) -gt $next) { & $each; $next = (Get-Date).AddSeconds($everyS) }
            Start-Sleep -Milliseconds 50; continue
        }
        if ($l -match $re) {
            Write-Host ("{0,6:0.0} s log: {1}" -f ((Get-Date) - $start).TotalSeconds, $Matches[0])
            return
        }
    }
}

WaitLog 'cellSaveDataListAutoLoad\(version'
Start-Sleep -Seconds 6
Key Enter
Start-Sleep -Seconds 3
Key X
WaitLog 'ShowSaveDataList\(save_entries=1'
Start-Sleep -Seconds 3
Key X
WaitLog 'open_msg_dialog'
Start-Sleep -Seconds 3
Key X
WaitLog 'SECURE\.BIN'
$ff = $null
if ($Record) {
    # 6d.13 (Q3): F12 no guardaba nada (6d.12); se graba la ventana de juego.
    $h = Find-GameWindow $p.Id
    $null = [ReFine.Keys]::SetForegroundWindow($h)
    $r = New-Object ReFine.Keys+RECT; $null = [ReFine.Keys]::GetClientRect($h, [ref]$r)
    $pt = New-Object ReFine.Keys+POINT; $null = [ReFine.Keys]::ClientToScreen($h, [ref]$pt)
    $wd = ($r.R - $r.L) -band -2; $ht = ($r.B - $r.T) -band -2
    # gdigrab (GDI) solo veia la pantalla de "Precompiling"; gfxcapture (por HWND) daba una imagen fija.
    # ddagrab (Desktop Duplication) ve la imagen real; da el monitor entero y se recorta a la ventana.
    $fargs = "-hide_banner -f lavfi -i ddagrab=output_idx=0:framerate=30:draw_mouse=0 " +
             "-vf hwdownload,format=bgra,crop=${wd}:${ht}:$($pt.X):$($pt.Y),scale=1280:720 -c:v libx264 -preset ultrafast -crf 16 -y `"$out.mkv`""
    $ff = Start-Process -FilePath 'ffmpeg' -ArgumentList $fargs -PassThru -WindowStyle Hidden -RedirectStandardError "$out.ffmpeg.log"
    $sw = [Diagnostics.Stopwatch]::GetTimestamp() / [double][Diagnostics.Stopwatch]::Frequency
    $wall = [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds() / 1000.0
    "wall $wall qpc $sw rect $($pt.X) $($pt.Y) $wd $ht" | Set-Content "$out.sync"
}
Start-Sleep -Seconds 14
# Menu principal, Play, Rhythm Game, seleccion: X hasta que abra el guion de una cancion.
if ($Down -gt 0) {
    # ASSUMPTION: la lista sale ~6 s despues del X que la abre; se baja tras el primer X que no abre guion
    for ($i = 0; $i -lt 3; $i++) { Key X; Start-Sleep -Seconds 3 }
    Start-Sleep -Seconds 4
    for ($i = 0; $i -lt $Down; $i++) { Key Down 120; Start-Sleep -Milliseconds 600 }
}
WaitLog 'pv_\d+_\w+\.dsc' { Key X } 3
# Ultimas confirmaciones hasta que el reloj arranque; rpcs3_watch termina solo tras $SongSeconds.
$i = 0
while (-not $w.HasExited -and ((Get-Date) -lt $start.AddSeconds($Timeout))) {
    $sz = 0; if (Test-Path "$out.clock") { $sz = (Get-Item "$out.clock").Length }
    if ($sz -eq 0) { Key X; if (-not $Record) { Key F12 80 } }   # 6d.13: con -Record sin F12 (ASSUMPTION: F12 congela RPCS3)
    Start-Sleep -Seconds 3
    $i++
}
$null = $p.CloseMainWindow()
if (-not $p.WaitForExit(15000)) { Stop-Process -Id $p.Id -Force }
Stop-Process -Id $lb.Id -Force -ErrorAction SilentlyContinue
if ($ff) { Stop-Process -Id $ff.Id -ErrorAction SilentlyContinue }
$sr.Close()
New-Item -ItemType Directory -Force "$out`_shots" | Out-Null
foreach ($d in 'screenshots', 'captures') { $dir = Join-Path $rp $d; if (Test-Path $dir) { Get-ChildItem $dir -Recurse -Filter *.png | Where-Object { $_.LastWriteTime -gt $start } | Copy-Item -Destination "$out`_shots" } }
Get-Content "$out.watch.txt"
Select-String $log -Pattern 'pv_\d+_\w+\.dsc' | Select-Object -Last 1 | ForEach-Object { $_.Line.Substring(0, [Math]::Min(150, $_.Line.Length)) }
