# rpcs3_trace.ps1 - Hito 1.9: arranca F en el RPCS3 del usuario con log en Trace y guarda el log.
#
# Uso:  powershell -ExecutionPolicy Bypass -File tools\rpcs3_trace.ps1 -Tag primer_arranque [-Timeout 240]
#
# Lanza  rpcs3.exe --no-gui --config <work\rpcs3\config_trace.yml> "<disco>\PS3_GAME\USRDIR\EBOOT.BIN"
# (el config del usuario no se toca: config_trace.yml es su copia con los canales en Trace),
# espera -Timeout segundos, cierra RPCS3 (primero pidiendo cerrar la ventana, para que vuelque
# el log; si no responde en 15 s, lo mata), copia openF-insumos\rpcs3\log\RPCS3.log (y TTY.log)
# a work\rpcs3\<Tag>.log / <Tag>.tty.log e imprime SHA-256 y numero de lineas.
#
# RPCS3 escribe sus propios archivos en openF-insumos\rpcs3 (log, cache, dev_hdd0): es la unica
# escritura permitida ahi (CLAUDE.md). Los logs no se versionan.

#
# Con -StopFile <ruta> no hay tiempo fijo: se espera (sin limite) a que exista ese archivo, y solo
# entonces se cierra RPCS3. Es para las corridas jugadas a mano: asi no se corta un guardado.
#   powershell -ExecutionPolicy Bypass -File tools\rpcs3_trace.ps1 -Tag con_datos_1 -StopFile work\rpcs3\cierra.flag
# (para cerrar: crear work\rpcs3\cierra.flag). Se comprueba una vez por segundo; el archivo se
# borra al empezar y al terminar.

param(
    [Parameter(Mandatory = $true)][string]$Tag,
    [int]$Timeout = 240,
    [string]$StopFile = '',
    [string]$Config = ''    # otro config, p. ej. work\rpcs3\config_shot.yml (sin Trace)
)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$ins = Join-Path (Split-Path $root -Parent) 'openF-insumos'
$rpcs3 = Join-Path $ins 'rpcs3\rpcs3.exe'
$eboot = Join-Path $ins 'Hatsune Miku - Project Diva F (USA)\PS3_GAME\USRDIR\EBOOT.BIN'
$config = if ($Config) { (Resolve-Path $Config).Path } else { Join-Path $root 'work\rpcs3\config_trace.yml' }
$log = Join-Path $ins 'rpcs3\log\RPCS3.log'
$tty = Join-Path $ins 'rpcs3\log\TTY.log'
$outDir = Join-Path $root 'work\rpcs3'
foreach ($p in @($rpcs3, $eboot, $config)) { if (-not (Test-Path $p)) { throw "Falta $p" } }
if (Get-Process rpcs3 -ErrorAction SilentlyContinue) { throw 'Ya hay un rpcs3.exe abierto: cierralo antes' }

if ($StopFile) {
    if (-not [IO.Path]::IsPathRooted($StopFile)) { $StopFile = Join-Path $root $StopFile }
    if (Test-Path $StopFile) { Remove-Item $StopFile }
}

$start = Get-Date
$p = Start-Process -FilePath $rpcs3 -ArgumentList @('--no-gui', '--config', "`"$config`"", "`"$eboot`"") `
    -WorkingDirectory (Split-Path $rpcs3) -PassThru
$null = $p.Handle
if ($StopFile) {
    Write-Host "== $Tag : RPCS3 pid $($p.Id), inicio $($start.ToString('HH:mm:ss')), espera a $StopFile"
    $exited = $false
    while (-not (Test-Path $StopFile)) {
        if ($p.WaitForExit(1000)) { $exited = $true; break }
    }
    $why = "al aparecer $(Split-Path $StopFile -Leaf) ($((Get-Date).ToString('HH:mm:ss')))"
} else {
    Write-Host "== $Tag : RPCS3 pid $($p.Id), inicio $($start.ToString('HH:mm:ss')), $Timeout s"
    $exited = $p.WaitForExit($Timeout * 1000)
    $why = "tras $Timeout s"
}
if ($exited) {
    $status = "termino solo, exit code $($p.ExitCode)"
} else {
    $null = $p.CloseMainWindow()
    if ($p.WaitForExit(15000)) {
        $status = "cerrado $why (ventana cerrada)"
    } else {
        Stop-Process -Id $p.Id -Force
        $p.WaitForExit(5000) | Out-Null
        $status = "cerrado $why (proceso terminado a la fuerza)"
    }
}
if ($StopFile -and (Test-Path $StopFile)) { Remove-Item $StopFile }
$secs = [math]::Round(((Get-Date) - $start).TotalSeconds, 1)
Write-Host "== $Tag : $status, $secs s"

if ((Get-Item $log).LastWriteTime -lt $start) { throw "RPCS3.log no se escribio en esta corrida" }
New-Item -ItemType Directory -Force $outDir | Out-Null
$dst = Join-Path $outDir "$Tag.log"
Copy-Item $log $dst -Force
if (Test-Path $tty) { Copy-Item $tty (Join-Path $outDir "$Tag.tty.log") -Force }
$hash = (Get-FileHash $dst -Algorithm SHA256).Hash.ToLower()
$lines = 0; Get-Content $dst -ReadCount 10000 | ForEach-Object { $lines += $_.Count }
Write-Host "   $dst"
Write-Host "   sha256 $hash"
Write-Host "   lineas $lines"
