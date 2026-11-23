# build_game.ps1 - lo que hace el paso "Building the game" del instalador (hito 8, parte 2).
# Corre en el PC del usuario: busca o instala las herramientas, hace el lift del EBOOT.elf del usuario y
# compila con clang-cl. Nada del juego sale de ese PC y nada de esto va a git generado.
#
#   -Check          solo mira que falta; escribe "need=llvm,python,vs" y las descargas en lineas "dl=..." (exit 0)
#   -Download       permite descargar lo que falte (el instalador lo pasa tras la pantalla de consentimiento)
#   -Src            carpeta con las fuentes (payload\src)       -Root   carpeta de trabajo (se crea y se borra)
#   -Elf            EBOOT.elf del usuario                       -Out    carpeta donde queda ReFine.exe y ui\
#   -Tools          carpeta de herramientas descargadas (se queda: la proxima vez no se vuelve a bajar)
# Salida: lineas "@@ <0..1> <texto>" que el instalador convierte en la barra; "ERR: ..." con exit != 0 si falla.
# Exit: 0 ok, 2 error, 3 faltan herramientas y no hay -Download.

param(
    [string]$Src, [string]$Root, [string]$Elf, [string]$Out, [string]$Tools,
    [switch]$Check, [switch]$Download, [int]$Jobs = 0, [switch]$KeepRoot
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

# Versiones y hashes fijados (SHA-256 del propio proyecto de origen; docs/instalador.md, seccion 6).
$PkgLlvm = @{ name = 'LLVM 23.1.5'; file = 'clang+llvm-23.1.5-x86_64-pc-windows-msvc.tar.zst'; mb = 264
    url = 'https://github.com/llvm/llvm-project/releases/download/llvmorg-23.1.5/clang+llvm-23.1.5-x86_64-pc-windows-msvc.tar.zst'
    sha = 'b2be33385ddc86e83511417a060de927ae8b09d1f3017cc3cdce5c73cd5029d7' }
$PkgPy = @{ name = 'Python 3.14.10 (embeddable)'; file = 'python-3.14.10-embed-amd64.zip'; mb = 13
    url = 'https://www.python.org/ftp/python/3.14.10/python-3.14.10-embed-amd64.zip'
    sha = '90905e64a840557d8c1bc4f5b74db3d7ee85e3ffb217611cac1c7813acea4159' }   # ASSUMPTION: sha del spdx.json de python.org; se confirma al bajarlo
$VSURL = 'https://aka.ms/vs/17/release/vs_buildtools.exe'

function P([double]$f, [string]$t) { Write-Host ("@@ {0:N3} {1}" -f $f, $t) }
function Fail([string]$m, [int]$code = 2) { Write-Host "ERR: $m"; exit $code }

# ---------------------------------------------------------------- deteccion
function Find-Python {
    $c = @()
    if ($Tools) { $c += (Join-Path $Tools 'python\python.exe') }
    $c += (Get-Command python -All -ErrorAction SilentlyContinue | ForEach-Object { $_.Source })
    foreach ($p in $c) {
        if (-not $p -or -not (Test-Path $p)) { continue }
        if ($p -match 'WindowsApps') { continue }                       # el atajo de la Microsoft Store no es Python
        & $p -c "import sys; sys.exit(0 if sys.version_info >= (3, 9) else 1)" 2>$null
        if ($LASTEXITCODE -eq 0) { return $p }
    }
    return $null
}
function Find-Llvm {
    $c = @("$env:ProgramFiles\LLVM\bin")
    if ($Tools) { $c += (Join-Path $Tools 'llvm\bin') }
    $c += (Get-Command clang-cl -All -ErrorAction SilentlyContinue | ForEach-Object { Split-Path $_.Source })
    foreach ($d in $c) { if ($d -and (Test-Path (Join-Path $d 'clang-cl.exe'))) { return $d } }
    return $null
}
function Find-Vs {
    $vw = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vw)) { return $null }
    $v = & $vw -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($v) { return $v } else { return $null }
}
$py = Find-Python; $llvm = Find-Llvm; $vs = Find-Vs
$need = @(); if (-not $llvm) { $need += 'llvm' }; if (-not $py) { $need += 'python' }; if (-not $vs) { $need += 'vs' }

if ($Check) {
    "need=$($need -join ',')"
    if ($need -contains 'llvm') { "dl=$($PkgLlvm.name)|$($PkgLlvm.mb)|github.com" }
    if ($need -contains 'python') { "dl=$($PkgPy.name)|$($PkgPy.mb)|python.org" }
    if ($need -contains 'vs') { "dl=Microsoft C++ Build Tools|3000|microsoft.com (needs administrator once)" }
    exit 0
}
if ($need.Count -and -not $Download) { Write-Host "need=$($need -join ',')"; exit 3 }
if (-not (Test-Path $Elf)) { Fail 'The game file (EBOOT.elf) is missing.' }
if (-not (Test-Path (Join-Path $Src 'CMakeLists.txt'))) { Fail 'The installer files are incomplete (sources missing).' }

# ---------------------------------------------------------------- descargas
function Get-Verified($pkg, [string]$dir) {
    New-Item -ItemType Directory -Force $dir | Out-Null
    $f = Join-Path $dir $pkg.file
    if (-not (Test-Path $f)) {
        Invoke-WebRequest -Uri $pkg.url -OutFile $f -UseBasicParsing
    }
    $h = (Get-FileHash $f -Algorithm SHA256).Hash.ToLower()
    if ($h -ne $pkg.sha) { Remove-Item $f -Force; Fail "$($pkg.name): the download does not match the expected checksum." }
    return $f
}
$dl = Join-Path $Tools '_download'
if ($need -contains 'python') {
    P 0.03 "Downloading $($PkgPy.name)"
    $z = Get-Verified $PkgPy $dl
    Expand-Archive $z (Join-Path $Tools 'python') -Force
    # The embeddable build ignores the script's folder and PYTHONPATH while python*._pth exists (the ps3recomp tools
    # import their neighbours: "No module named spu_elf"). Without it Python uses its normal path; python314.zip stays.
    Get-ChildItem (Join-Path $Tools 'python') -Filter '*._pth' | Remove-Item -Force
    $py = Find-Python
    if (-not $py) { Fail 'Python was downloaded but does not run.' }
}
if ($need -contains 'llvm') {
    P 0.06 "Downloading $($PkgLlvm.name) ($($PkgLlvm.mb) MB)"
    $z = Get-Verified $PkgLlvm $dl
    P 0.14 'Unpacking LLVM'
    $dest = Join-Path $Tools 'llvm'
    if (Test-Path $dest) { Remove-Item $dest -Recurse -Force }
    $tmp = Join-Path $Tools 'llvm_tmp'
    if (Test-Path $tmp) { Remove-Item $tmp -Recurse -Force }
    New-Item -ItemType Directory -Force $tmp | Out-Null
    # tar.exe de Windows no abre .zst; Python 3.14+ si (compression.zstd). Una sola pasada, sin pip.
    $pyx = Join-Path $Tools '_untar.py'
    "import sys, tarfile`nwith tarfile.open(sys.argv[1], 'r:zst') as t: t.extractall(sys.argv[2], filter='data')" | Set-Content $pyx -Encoding ASCII
    & $py -I $pyx $z $tmp
    if ($LASTEXITCODE -ne 0) { Fail 'Could not unpack LLVM.' }
    $inner = Get-ChildItem $tmp -Directory | Select-Object -First 1
    Move-Item $inner.FullName $dest
    Remove-Item $tmp, $pyx -Recurse -Force -ErrorAction SilentlyContinue
    $llvm = Find-Llvm
    if (-not $llvm) { Fail 'LLVM was unpacked but clang-cl was not found.' }
}
if ($need -contains 'vs') {
    P 0.18 'Installing the Microsoft C++ Build Tools (administrator needed once)'
    New-Item -ItemType Directory -Force $dl | Out-Null
    $b = Join-Path $dl 'vs_buildtools.exe'
    Invoke-WebRequest -Uri $VSURL -OutFile $b -UseBasicParsing
    $sig = Get-AuthenticodeSignature $b
    if ($sig.Status -ne 'Valid' -or $sig.SignerCertificate.Subject -notmatch 'Microsoft Corporation') { Remove-Item $b -Force; Fail 'The Microsoft installer is not signed by Microsoft.' }
    $p = Start-Process $b -Verb RunAs -Wait -PassThru -ArgumentList '--quiet --wait --norestart --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended'
    if ($p.ExitCode -notin 0, 3010) { Fail "The Microsoft C++ Build Tools did not install (code $($p.ExitCode))." }
    $vs = Find-Vs
    if (-not $vs) { Fail 'The C++ Build Tools were installed but not found.' }
}
Remove-Item $dl -Recurse -Force -ErrorAction SilentlyContinue

# ---------------------------------------------------------------- arbol de trabajo
P 0.22 'Preparing the sources'
if (Test-Path $Root) { Remove-Item $Root -Recurse -Force }
New-Item -ItemType Directory -Force $Root | Out-Null
& robocopy $Src $Root /E /NFL /NDL /NJH /NJS /NP | Out-Null
if ($LASTEXITCODE -ge 8) { Fail 'Could not copy the sources.' }
# Las DLL de FFmpeg viajan una sola vez, en la raiz del payload; CMake las espera en work\ffmpeg\bin.
$ffbin = Join-Path $Root 'work\ffmpeg\bin'
New-Item -ItemType Directory -Force $ffbin | Out-Null
foreach ($d in 'avcodec-63', 'avutil-61', 'swresample-7', 'swscale-10') { Copy-Item (Join-Path (Split-Path $Src) "$d.dll") $ffbin -Force }
New-Item -ItemType Directory -Force (Join-Path $Root 'game') | Out-Null
Copy-Item $Elf (Join-Path $Root 'game\EBOOT.elf') -Force

$env:Path = "$(Split-Path $py);$llvm;$env:Path"
$env:REFINE_LLVM_BIN = $llvm
$env:PYTHONUTF8 = '1'
$ps = "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe"
function Step([string]$what, [string[]]$a) {
    $log = Join-Path $Root "work\$what.log"
    New-Item -ItemType Directory -Force (Split-Path $log) | Out-Null
    $p = Start-Process $ps -ArgumentList (@('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File') + $a) -NoNewWindow -PassThru `
        -RedirectStandardOutput $log -RedirectStandardError "$log.err" -WorkingDirectory $Root
    $null = $p.Handle   # sin esto ExitCode sale vacio con Start-Process -PassThru y redireccion
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $peak = 0.0
    while (-not $p.WaitForExit(5000)) {   # muestra la RAM usada cada 5 s (para docs/perf.md); espera bloqueante
        $os = Get-CimInstance Win32_OperatingSystem
        $used = ($os.TotalVisibleMemorySize - $os.FreePhysicalMemory) / 1MB
        if ($used -gt $peak) { $peak = $used }
    }
    $p.WaitForExit()
    Write-Host ("TIME {0} {1:N1} min, RAM used peak {2:N1} GB" -f $what, $sw.Elapsed.TotalMinutes, $peak)
    if ($p.ExitCode -ne 0) {
        $tail = @(Get-Content $log -Tail 8; Get-Content "$log.err" -Tail 8 -ErrorAction SilentlyContinue) -join ' | '
        Fail "$what failed (code $($p.ExitCode)). $tail"
    }
}

# Vulkan: cabeceras de Khronos y DXC (dxcapi.h), con SHA-256 fijado en fetch_vulkan.ps1. Sin ellas CMake se detiene
# (no compila una version sin Vulkan en silencio): el error de la descarga sale tal cual.
P 0.23 'Fetching the Vulkan headers'
Step 'vulkan' @("$Root\tools\fetch_vulkan.ps1")
P 0.25 'Opening the game code (lift)'
Step 'lift' @("$Root\tools\lift.ps1")
P 0.40 'Opening the audio programs (lift SPU)'
Step 'lift_spu' @("$Root\tools\lift_spu.ps1")

# Hilos y RAM: -Jobs = min(hilos, RAM libre / 3 GB), al menos 1.
if ($Jobs -le 0) {
    $os = Get-CimInstance Win32_OperatingSystem
    $byRam = [math]::Floor(($os.FreePhysicalMemory / 1MB) / 3)
    $Jobs = [int][math]::Max(1, [math]::Min([Environment]::ProcessorCount, $byRam))
}
P 0.45 "Compiling the game ($Jobs at a time)"
# -InlineMem (hito 7, 2026-11-11): the guest loads and stores compiled inline. The game thread goes from
# 6.65 to 2.85 ms a frame in a song (docs/perf.md): the build a player gets is the fast one.
Step 'build' @("$Root\tools\build.ps1", '-Jobs', "$Jobs", '-InlineMem')

$exe = Join-Path $Root 'build\ReFine.exe'
if (-not (Test-Path $exe)) { Fail 'The build finished without ReFine.exe.' }
P 0.95 'Copying the game'
New-Item -ItemType Directory -Force $Out | Out-Null
Copy-Item $exe $Out -Force
New-Item -ItemType Directory -Force (Join-Path $Out 'ui') | Out-Null
Copy-Item (Join-Path $Root 'build\ui\*') (Join-Path $Out 'ui') -Recurse -Force
# Vulkan carga dxcompiler.dll junto al exe (HLSL a SPIR-V); sin ella el juego cae a Direct3D 12.
$dxc = Join-Path (Split-Path $Src) 'dxcompiler.dll'
if (-not (Test-Path $dxc)) { $dxc = Join-Path $Root 'build\dxcompiler.dll' }
if (Test-Path $dxc) { Copy-Item $dxc $Out -Force }
if (-not $KeepRoot) { Remove-Item $Root -Recurse -Force -ErrorAction SilentlyContinue }
P 1.0 'Done'
exit 0
