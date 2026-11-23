# run.ps1 - Hito 3: ejecuta build\ReFine.exe con timeout y log a work\run\.
#
# Uso:  powershell -ExecutionPolicy Bypass -File tools\run.ps1 [-Timeout 60] [-Tag nombre]
#       (-Profile player: sin limite de tiempo salvo -Timeout explicito)
#           [-Profile test|player] [-KeepData] [-EmptyData] [-SeedSave carpeta] [-CloseAt s]
#           [-HleTrace N] [-SysTrace] [-FsLog] [-Env VAR=valor,VAR2=valor] [-Affinity 0x1]
#
# -Affinity: mascara de nucleos del proceso (solo diagnostico, p. ej. 0x1 = un nucleo).
# -Exe: otro ejecutable en vez de build\ReFine.exe (p. ej. un build anterior guardado
#       en work\ para medirlo en las mismas condiciones).
# -ThreadCpu desde-hasta (s, p. ej. 10-60): CPU de cada hilo del proceso en esa ventana
#       (% de un nucleo), con su nombre si el runtime se lo puso (SetThreadDescription).
#
# Perfiles (5a.0, docs/decisions.md). Lo que el juego escribe cae en work\profiles\<perfil>\:
#   -Profile test (por defecto): se borra y se copia de work\profiles\test_base al empezar.
#       -KeepData sigue con los datos de la corrida anterior (guardar y volver a cargar).
#       -EmptyData empieza sin datos: primer arranque.
#       -SeedSave <carpeta NPUB31241_SYSTEM00>: usa esa partida en lugar de la de la base.
#   -Profile player: la partida real del jugador. Solo cuando juega el, a mano: no admite
#       PAD_SCRIPT ni -CloseAt, y sus datos nunca se borran.
#
# -CloseAt s: cierra la ventana a los s segundos con un WM_CLOSE real (como la X).
# -CloseOnSave: la cierra al ver el primer archivo de partida escrito por esta corrida, con el guardado en marcha.
#
#   /dev_bdvd, /app_home, y cualquier ruta sin montaje propio -> work\vfs\bdvd
#       (COPIA del disco del usuario; el original en openF-insumos no se usa)
#   /dev_hdd0 -> <perfil>\hdd0      /dev_hdd1 -> <perfil>\hdd1
#   /dev_flash -> work\vfs\flash (copia del dev_flash de RPCS3)
#   directorio de trabajo -> <perfil>\run
#
# PS3_HLE_UNRESOLVED=fail siempre: un import sin handler devuelve error, no un exito
# falso (por defecto ps3recomp devuelve CELL_OK; ver docs/decisions.md).

param(
    [int]$Timeout = 60,
    [string]$Tag = '',
    [ValidateSet('test', 'player')][string]$Profile = 'test',
    [switch]$KeepData,
    [switch]$EmptyData,
    [string]$SeedSave = '',
    [double]$CloseAt = 0,
    [switch]$CloseOnSave,
    [int]$HleTrace = 0,
    [switch]$SysTrace,
    [switch]$FsLog,
    [string[]]$Env = @(),
    [string]$Affinity = '',
    [string]$Exe = '',
    [string]$ThreadCpu = ''
)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$exe = if ($Exe) { (Resolve-Path $Exe).Path } else { Join-Path $root 'build\ReFine.exe' }
$elf = Join-Path $root 'game\EBOOT.elf'
$vfs = Join-Path $root 'work\vfs'
$logDir = Join-Path $root 'work\run'
$profiles = Join-Path $root 'work\profiles'
$data = Join-Path $profiles $Profile
$base = Join-Path $profiles 'test_base'
$saveRel = 'hdd0\home\00000001\savedata'
foreach ($p in @($exe, $elf, "$vfs\bdvd\PS3_GAME")) { if (-not (Test-Path $p)) { throw "Falta $p" } }

# Con powershell -File, '-Env A=1,B=1' llega como UNA cadena: se parte tambien por comas.
$envPairs = @($Env | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
# 6b+6c: el perfil test lleva su propio refine.ini (vacio al empezar), nunca el del jugador en %LOCALAPPDATA%.
if ($Profile -eq 'test' -and -not ($envPairs | Where-Object { $_ -match '^REFINE_INI=' })) { $envPairs += "REFINE_INI=$(Join-Path $data 'refine.ini')" }
# 6b+6c p3: las corridas automaticas no esperan en la pantalla de inicio (REFINE_LAUNCHER=1 para probarla).
if ($Profile -eq 'test' -and -not ($envPairs | Where-Object { $_ -match '^REFINE_LAUNCHER=' })) { $envPairs += 'REFINE_LAUNCHER=0' }
if ($Profile -eq 'player') {
    $scripted = $CloseAt -gt 0 -or $CloseOnSave -or $env:PAD_SCRIPT -or $env:PAD_AUTOPRESS -or $env:PAD_KEYS -or $env:PAD_KEYCYCLE -or $env:PAD_VPAD -or
        ($envPairs | Where-Object { $_ -match '^PAD_(SCRIPT|AUTOPRESS|VPAD)=' })
    if ($scripted) { throw 'El perfil player es solo para jugar a mano: nada de PAD_SCRIPT, PAD_AUTOPRESS ni -CloseAt' }
    if ($KeepData -or $EmptyData -or $SeedSave) { throw '-KeepData, -EmptyData y -SeedSave son del perfil test' }
} elseif (-not $KeepData) {
    if ($SeedSave -and -not (Test-Path "$SeedSave\PARAM.SFO")) { throw "$SeedSave no es una partida" }
    if (-not $EmptyData -and -not (Test-Path $base)) { throw "Falta $base (crearla con -EmptyData, ver decisions.md)" }
    if (Test-Path $data) { Remove-Item $data -Recurse -Force }
    if ($EmptyData) { New-Item -ItemType Directory $data | Out-Null }
    else { Copy-Item $base $data -Recurse }
    if ($SeedSave) {
        $dst = Join-Path $data "$saveRel\$(Split-Path $SeedSave -Leaf)"
        if (Test-Path $dst) { Remove-Item $dst -Recurse -Force }
        New-Item -ItemType Directory -Force (Split-Path $dst -Parent) | Out-Null
        Copy-Item $SeedSave $dst -Recurse
    }
}
foreach ($d in @('hdd0', 'hdd1', 'run')) { New-Item -ItemType Directory -Force "$data\$d" | Out-Null }
New-Item -ItemType Directory -Force $logDir | Out-Null

if (-not $Tag) { $Tag = Get-Date -Format 'yyyyMMdd_HHmmss' }
$out = Join-Path $logDir "$Tag.out.log"
$err = Join-Path $logDir "$Tag.err.log"

$vars = @{
    PS3_VFS_ROOT       = "$vfs\bdvd"
    PS3_HDD0_ROOT      = "$data\hdd0"
    PS3_HDD1_ROOT      = "$data\hdd1"
    PS3_DEV_FLASH      = "$vfs\flash"
    PS3_HLE_UNRESOLVED = 'fail'
    PS3_TITLE          = 'Re:Fine'
}
if ($HleTrace -gt 0) { $vars.PS3_HLE_TRACE = "$HleTrace" }
if ($SysTrace) { $vars.PS3_SCTRACE = '1' }
if ($FsLog) { $vars.PS3_FSLOG = '1' }
foreach ($kv in $envPairs) {
    $k, $v = $kv -split '=', 2; $vars[$k] = $v
}
foreach ($k in $vars.Keys) { Set-Item "env:$k" $vars[$k] }

$start = Get-Date
$p = Start-Process -FilePath $exe -ArgumentList "`"$elf`"" -WorkingDirectory "$data\run" `
    -RedirectStandardOutput $out -RedirectStandardError $err -PassThru -WindowStyle Normal
$null = $p.Handle   # sin leer el handle, ExitCode queda vacio al terminar
if ($Affinity) { $p.ProcessorAffinity = [IntPtr][Convert]::ToInt64($Affinity, 16) }
$cpuReport = @()
if ($ThreadCpu) {
    Add-Type -Namespace ReFine -Name Thr -MemberDefinition @'
[DllImport("kernel32.dll")] public static extern IntPtr OpenThread(uint access, bool inherit, uint id);
[DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr h);
[DllImport("kernel32.dll")] public static extern int GetThreadDescription(IntPtr h, out IntPtr desc);
[DllImport("kernel32.dll")] public static extern IntPtr LocalFree(IntPtr p);
public static string Name(int id) {
    IntPtr h = OpenThread(0x0040, false, (uint)id);   // THREAD_QUERY_INFORMATION
    if (h == IntPtr.Zero) return "";
    IntPtr d; string s = "";
    if (GetThreadDescription(h, out d) >= 0 && d != IntPtr.Zero) { s = Marshal.PtrToStringUni(d); LocalFree(d); }
    CloseHandle(h); return s;
}
'@
    $from, $to = $ThreadCpu -split '-' | ForEach-Object { [double]$_ }
    function Snap { $p.Refresh(); $h = @{}; foreach ($t in $p.Threads) { try { $h[$t.Id] = $t.TotalProcessorTime.TotalSeconds } catch {} }; $h }
    function WaitUntil([double]$s) { $ms = [int](($start.AddSeconds($s) - (Get-Date)).TotalMilliseconds); if ($ms -gt 0) { $null = $p.WaitForExit($ms) } }
    WaitUntil $from; $t0 = Get-Date; $c0 = Snap
    WaitUntil $to; $t1 = Get-Date; $c1 = Snap
    if (-not $p.HasExited) {
        $win = ($t1 - $t0).TotalSeconds
        $mainTid = ($p.Threads | Sort-Object { $_.StartTime } | Select-Object -First 1).Id
        $cpuReport = foreach ($id in $c1.Keys) {
            $d = $c1[$id] - $(if ($c0.ContainsKey($id)) { $c0[$id] } else { 0 })
            [pscustomobject]@{ tid = $id; pct = [math]::Round(100 * $d / $win, 1)
                name = $(if ($id -eq $mainTid) { 'main (hilo principal del proceso) ' } else { '' }) + [ReFine.Thr]::Name($id) }
        }
        $cpuReport = @("[THREADCPU] ventana $from-$to s ($([math]::Round($win,2)) s medidos), % de un nucleo:") +
            ($cpuReport | Sort-Object pct -Descending | Select-Object -First 16 |
             ForEach-Object { "[THREADCPU] {0,6} {1,6:N1} %  {2}" -f $_.tid, $_.pct, $_.name })
    }
}
# 6d.10: WM_CLOSE a la ventana del juego, nunca a la consola. CloseMainWindow() elige la
# "ventana principal" del proceso, y Windows le atribuye tambien la de su consola: si elige esa,
# el WM_CLOSE llega como CTRL_CLOSE_EVENT y el proceso muere con 0xC000013A sin pasar por
# REQUEST_EXITGAME (m1_30 de 6d.9).
Add-Type -Namespace ReFine -Name Win -MemberDefinition @'
public delegate bool EnumProc(IntPtr h, IntPtr l);
[DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc f, IntPtr l);
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
[DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
[DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassName(IntPtr h, System.Text.StringBuilder s, int n);
[DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
public static int CloseGame(uint id) {
    int n = 0;
    EnumWindows((h, l) => {
        uint p; GetWindowThreadProcessId(h, out p);
        if (p != id || !IsWindowVisible(h)) return true;
        var c = new System.Text.StringBuilder(64); GetClassName(h, c, 64);
        if (c.ToString() == "ConsoleWindowClass" || c.ToString() == "PseudoConsoleWindow") return true;
        if (PostMessage(h, 0x0010, IntPtr.Zero, IntPtr.Zero)) n++;
        return true;
    }, IntPtr.Zero);
    return n;
}
'@
function Close-Game { $p.Refresh(); [ReFine.Win]::CloseGame([uint32]$p.Id) -gt 0 }
$closedAt = $null
if ($CloseOnSave) {
    $saves = Join-Path $data $saveRel
    $limit = $start.AddSeconds($Timeout)
    while (-not $p.HasExited -and -not $closedAt -and (Get-Date) -lt $limit) {
        if (Get-ChildItem $saves -Recurse -File -ErrorAction SilentlyContinue | Where-Object { $_.LastWriteTime -gt $start }) {
            $null = Close-Game; $closedAt = Get-Date
        } else { Start-Sleep -Milliseconds 20 }
    }
}
if ($CloseAt -gt 0) {
    $ms = [int](($start.AddSeconds($CloseAt) - (Get-Date)).TotalMilliseconds)
    if ($ms -lt 0 -or -not $p.WaitForExit($ms)) {
        if (-not (Close-Game)) { throw 'sin ventana que cerrar' }
        $closedAt = Get-Date
    }
}
# 2026-11-11: the player profile is a real game: never killed by a time limit (the 60 s default ended
# three of the player's games, taken for crashes). An explicit -Timeout still applies.
if ($Profile -eq 'player' -and -not $PSBoundParameters.ContainsKey('Timeout')) { $p.WaitForExit(); $finished = $true }
else { $finished = $p.WaitForExit([math]::Max(0, [int](($start.AddSeconds($Timeout) - (Get-Date)).TotalMilliseconds))) }
if (-not $finished) {
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    $p.WaitForExit(5000) | Out-Null
    $status = "TIMEOUT tras $Timeout s (proceso terminado)"
} else {
    $status = "termino solo, exit code $($p.ExitCode) (0x{0:X8})" -f $p.ExitCode
}
$secs = [math]::Round(((Get-Date) - $start).TotalSeconds, 1)

Write-Host "== $Tag : $status, $secs s, perfil $Profile"
if ($closedAt) { Write-Host ("   ventana cerrada a {0:N1} s; salida {1:N1} s despues" -f ($closedAt - $start).TotalSeconds, ((Get-Date) - $closedAt).TotalSeconds) }
# Resumen corto (10 lineas como mucho); el detalle queda en los logs.
$lg = @($out, $err) | Where-Object { Test-Path $_ }
$fs = Select-String -Path $lg -Pattern '\[FRAMESTATS\] flips=' -ErrorAction SilentlyContinue | Select-Object -Last 1
$fpsMed = if ($fs) { [regex]::Match($fs.Line, '\(([\d.]+)/s\)').Groups[1].Value } else { 'n/d' }
$nUnres = @(Select-String -Path $lg -Pattern 'unresolved NID' -ErrorAction SilentlyContinue).Count
$nPso = @(Select-String -Path $lg -Pattern 'PSO (creation )?FAIL' -ErrorAction SilentlyContinue).Count
$errs = @(Select-String -Path $lg -Pattern '(error|fatal|exception)' -ErrorAction SilentlyContinue)
Write-Host "   fps medio: $fpsMed | imports sin handler: $nUnres | PSO FAIL: $nPso | errores: $($errs.Count)"
if ($errs.Count) { Write-Host "   1er error: $($errs[0].Line.Trim().Substring(0, [math]::Min(150, $errs[0].Line.Trim().Length)))" }
if ($cpuReport) { $cpuReport | Select-Object -First 3 | ForEach-Object { Write-Host $_ } }
Select-String -Path $lg -Pattern '^\[quit\]' -ErrorAction SilentlyContinue | Select-Object -First 2 | ForEach-Object { Write-Host "   $($_.Line)" }
Write-Host "   logs: $out (+ .err.log)"
