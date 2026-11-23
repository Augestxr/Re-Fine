# rpcs3_edit.ps1 - 5b.2: Edit > Load Data en RPCS3, sin manos, para comparar la pantalla con Re:Fine.
#
# Uso:  powershell -ExecutionPolicy Bypass -File tools\rpcs3_edit.ps1 -Tag ed1
#
# Sigue el log y manda teclas con PostMessage solo a la ventana de juego: sin foco, sin SendInput y
# sin mirar la pantalla. Teclas del teclado por defecto de RPCS3: X Equis, C Circulo, Enter START,
# flechas cruceta. Cada paso espera a su linea del log. Las capturas con F12 traen los overlays;
# PrintWindow da un cuadro viejo con la ventana de Vulkan, por eso se quito.
# Guarda en work\rpcs3\<Tag>_shots\. Usa la partida que RPCS3 ya tiene (NPUB31241_SYSTEM00).

param(
    [Parameter(Mandatory = $true)][string]$Tag,
    [string]$Config = 'work\rpcs3\config_cancion.yml',
    [int]$Timeout = 420
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

Add-Type -Namespace ReFine -Name Keys -MemberDefinition @'
public delegate bool EnumProc(IntPtr h, IntPtr l);
[DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc f, IntPtr l);
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
[DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
[DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, System.Text.StringBuilder s, int n);
[DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
'@

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

# vk, scan
$K = @{ X = 0x58, 0x2D; C = 0x43, 0x2E; Enter = 0x0D, 0x1C; Down = 0x28, 0x50; F12 = 0x7B, 0x58 }

function Key([string]$name, [int]$holdMs = 200) {
    $h = Find-GameWindow $p.Id
    $vk = [IntPtr]$K[$name][0]; $scan = [int64]$K[$name][1]
    if ($name -eq 'Down') { $scan += 0x100 }   # bit 24 de lParam: tecla extendida; sin el, Qt ve el teclado numerico
    $null = [ReFine.Keys]::PostMessage($h, 0x100, $vk, [IntPtr](1 + ($scan -shl 16)))
    Start-Sleep -Milliseconds $holdMs
    $null = [ReFine.Keys]::PostMessage($h, 0x101, $vk, [IntPtr]([int64]0xC0000001 + ($scan -shl 16)))
    Write-Host ("{0,6:0.0} s tecla {1}" -f ((Get-Date) - $start).TotalSeconds, $name)
}

function Shot([string]$name) {
    Key F12 80
    Start-Sleep -Milliseconds 700
    foreach ($d in 'screenshots', 'captures') {
        $dir = Join-Path $rp $d
        if (Test-Path $dir) {
            Get-ChildItem $dir -Recurse -Filter *.png | Where-Object { $_.LastWriteTime -gt $start } |
                Sort-Object LastWriteTime | Select-Object -Last 1 |
                ForEach-Object { Copy-Item $_.FullName (Join-Path $out "$name.png") }
        }
    }
}

$start = Get-Date
$p = Start-Process -FilePath $rpcs3 -ArgumentList @('--no-gui', '--config', "`"$config`"", "`"$eboot`"") `
    -WorkingDirectory $rp -PassThru
$null = $p.Handle
Write-Host "== $Tag : pid $($p.Id)"
Start-Sleep -Seconds 3
$fs = New-Object IO.FileStream($log, 'Open', 'Read', 'ReadWrite,Delete')
$sr = New-Object IO.StreamReader($fs)

function WaitLog([string]$re) {
    $deadline = $start.AddSeconds($Timeout)
    while ($true) {
        if ($p.HasExited) { throw 'RPCS3 termino antes de tiempo' }
        if ((Get-Date) -gt $deadline) {
            Shot 'timeout'
            $null = $p.CloseMainWindow()
            throw "No llego a $re"
        }
        $l = $sr.ReadLine()
        if ($null -eq $l) { Start-Sleep -Milliseconds 50; continue }
        if ($l -match $re) {
            Write-Host ("{0,6:0.0} s log: {1}" -f ((Get-Date) - $start).TotalSeconds, $re)
            return
        }
    }
}

WaitLog 'cellSaveDataListAutoLoad\(version'
Start-Sleep -Seconds 6
Key Enter
Start-Sleep -Seconds 3
Shot 'a_menu_titulo'
Key X
WaitLog 'ShowSaveDataList\(save_entries=1'
Start-Sleep -Seconds 3
Shot 'b_lista_con_partida'
Key X
WaitLog 'open_msg_dialog'
Start-Sleep -Seconds 3
Shot 'c_load_this_data'
Key X
WaitLog 'SECURE\.BIN'
Start-Sleep -Seconds 14
Shot 'd_tras_cargar'
foreach ($i in 1..3) { Key Down; Start-Sleep -Seconds 1 }
Shot 'e_home'
Key X
Start-Sleep -Seconds 9
Shot 'f_edit'
Key Down
Start-Sleep -Seconds 1
Key X
WaitLog 'ShowSaveDataList\(save_entries=0'
Start-Sleep -Seconds 3
Shot 'g_lista_vacia'
Key C
WaitLog 'Native UI dialog returned with selection -2'
Start-Sleep -Seconds 3
Shot 'h_tras_volver'
if (-not $p.CloseMainWindow()) { Write-Host 'CloseMainWindow sin efecto' }
if (-not $p.WaitForExit(15000)) { Stop-Process -Id $p.Id -Force }
$sr.Close()
Get-ChildItem $out | Select-Object Name, Length | Format-Table -AutoSize
