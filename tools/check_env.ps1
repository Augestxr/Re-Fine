# check_env.ps1 - Hito 0: comprueba las herramientas que necesita Re:Fine.
#
# Uso:  powershell -ExecutionPolicy Bypass -File tools\check_env.ps1
#
# Busca cada herramienta en el PATH y, si no esta, en los sitios donde suele
# instalarse (Visual Studio trae su propio CMake y Ninja; LLVM va en Program
# Files). Imprime la version encontrada y OK / FALTA / VERSION. Sale con codigo
# 1 si algo falta o no cumple la version minima.

$ErrorActionPreference = 'Continue'
$script:fallos = 0

function Write-Row([string]$name, [string]$estado, [string]$detalle) {
    $color = switch ($estado) { 'OK' { 'Green' } 'AVISO' { 'Yellow' } default { 'Red' } }
    Write-Host ('{0,-10} ' -f $name) -NoNewline
    Write-Host ('{0,-8}' -f $estado) -ForegroundColor $color -NoNewline
    Write-Host " $detalle"
    if ($estado -ne 'OK' -and $estado -ne 'AVISO') { $script:fallos++ }
}

# Devuelve la ruta del ejecutable: primero el PATH, luego las rutas candidatas.
function Find-Exe([string]$cmd, [string[]]$candidatos) {
    $g = Get-Command $cmd -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($g -and $g.Source) { return $g.Source }
    foreach ($c in $candidatos) {
        $hit = Get-ChildItem -Path $c -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($hit) { return $hit.FullName }
    }
    return $null
}

# Extrae la primera version x.y[.z] de un texto.
function Get-Ver([string]$texto) {
    if ($texto -match '(\d+)\.(\d+)(\.(\d+))?') {
        $patch = 0; if ($Matches[4]) { $patch = [int]$Matches[4] }
        return [version]::new([int]$Matches[1], [int]$Matches[2], $patch)
    }
    return $null
}

function Get-Output([string]$exe, [string[]]$argv) {
    # java -version y otros escriben en stderr: se junta todo.
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $exe
    $psi.Arguments = ($argv -join ' ')
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.UseShellExecute = $false
    $p = [System.Diagnostics.Process]::Start($psi)
    $out = $p.StandardOutput.ReadToEnd() + $p.StandardError.ReadToEnd()
    $p.WaitForExit()
    return $out.Trim()
}

# --- Visual Studio (vswhere) ------------------------------------------------
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsInstalls = @()
if (Test-Path $vswhere) {
    $vsInstalls = & $vswhere -all -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -format json | ConvertFrom-Json
}
$vsPaths = @($vsInstalls | ForEach-Object { $_.installationPath })
$vsCmake = $vsPaths | ForEach-Object { "$_\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" }
$vsNinja = $vsPaths | ForEach-Object { "$_\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe" }
$vsClang = $vsPaths | ForEach-Object { "$_\VC\Tools\Llvm\x64\bin\clang-cl.exe" }

Write-Host "Re:Fine - comprobacion de entorno (hito 0)"
Write-Host ("-" * 60)

# --- Python 3.10+ -----------------------------------------------------------
$py = Find-Exe 'python' @()
if ($py) {
    $v = Get-Ver (Get-Output $py @('--version'))
    if ($v -ge [version]'3.10.0') { Write-Row 'Python' 'OK' "$v  ($py)" }
    else { Write-Row 'Python' 'VERSION' "$v < 3.10  ($py)" }
} else { Write-Row 'Python' 'FALTA' 'winget install Python.Python.3.12' }

# --- CMake 3.20+ ------------------------------------------------------------
$cm = Find-Exe 'cmake' (@("$env:ProgramFiles\CMake\bin\cmake.exe") + $vsCmake)
if ($cm) {
    $v = Get-Ver (Get-Output $cm @('--version'))
    if ($v -ge [version]'3.20.0') { Write-Row 'CMake' 'OK' "$v  ($cm)" }
    else { Write-Row 'CMake' 'VERSION' "$v < 3.20  ($cm)" }
} else { Write-Row 'CMake' 'FALTA' 'winget install Kitware.CMake' }

# --- Ninja --------------------------------------------------------------------
$nj = Find-Exe 'ninja' $vsNinja
if ($nj) { Write-Row 'Ninja' 'OK' "$(Get-Output $nj @('--version'))  ($nj)" }
else { Write-Row 'Ninja' 'FALTA' 'winget install Ninja-build.Ninja' }

# --- MSVC (VS 2022 o posterior) -----------------------------------------------
# Vale VS 2022 (17.x) o posterior: VS Build Tools 2026 (18.x) se acepto el
# 2026-10-24 (docs/decisions.md). clang-cl solo usa sus cabeceras y librerias.
if ($vsInstalls.Count -gt 0) {
    $vs = $vsInstalls | Where-Object { (Get-Ver $_.installationVersion).Major -ge 17 } |
        Sort-Object { [version]$_.installationVersion } -Descending | Select-Object -First 1
    if ($vs) {
        Write-Row 'MSVC' 'OK' "$($vs.displayName) $($vs.installationVersion)"
    } else {
        $otro = $vsInstalls | Select-Object -First 1
        Write-Row 'MSVC' 'VERSION' "$($otro.displayName) $($otro.installationVersion) (se pide VS 2022 o posterior)"
    }
} else { Write-Row 'MSVC' 'FALTA' 'winget install Microsoft.VisualStudio.2022.BuildTools (carga de trabajo C++)' }

# --- clang-cl -----------------------------------------------------------------
$cc = Find-Exe 'clang-cl' (@("$env:ProgramFiles\LLVM\bin\clang-cl.exe") + $vsClang)
if ($cc) { Write-Row 'clang-cl' 'OK' "$(Get-Ver (Get-Output $cc @('--version')))  ($cc)" }
else { Write-Row 'clang-cl' 'FALTA' 'winget install LLVM.LLVM' }

# --- Git ----------------------------------------------------------------------
$git = Find-Exe 'git' @("$env:ProgramFiles\Git\cmd\git.exe")
if ($git) { Write-Row 'Git' 'OK' "$(Get-Ver (Get-Output $git @('--version')))  ($git)" }
else { Write-Row 'Git' 'FALTA' 'winget install Git.Git' }

# --- Node 22/24 o 26.12.1+ ------------------------------------------------------
# REA acepta 22, 24 o 26.12.1 en adelante; el usuario usa 26.12.1 (docs/decisions.md).
$node = Find-Exe 'node' @("$env:ProgramFiles\nodejs\node.exe")
if ($node) {
    $v = Get-Ver (Get-Output $node @('--version'))
    if ($v.Major -eq 22 -or $v.Major -eq 24 -or $v -ge [version]'26.12.1') { Write-Row 'Node' 'OK' "$v  ($node)" }
    else { Write-Row 'Node' 'VERSION' "$v (se pide 22, 24 o 26.12.1+)  winget upgrade OpenJS.NodeJS" }
} else { Write-Row 'Node' 'FALTA' 'winget install OpenJS.NodeJS' }

# --- JDK 21 -------------------------------------------------------------------
$javaCands = @()
if ($env:JAVA_HOME) { $javaCands += "$env:JAVA_HOME\bin\java.exe" }
$javaCands += "$env:ProgramFiles\Eclipse Adoptium\jdk-21*\bin\java.exe"
$javaCands += "$env:ProgramFiles\Microsoft\jdk-21*\bin\java.exe"
$javaCands += "$env:ProgramFiles\Java\jdk-21*\bin\java.exe"
# Se busca un JDK 21 aunque el java del PATH sea otro.
$jdk21 = $javaCands | ForEach-Object { Get-ChildItem $_ -ErrorAction SilentlyContinue } |
    Where-Object { (Get-Ver (Get-Output $_.FullName @('-version'))).Major -eq 21 } | Select-Object -First 1
$java = Find-Exe 'java' @()
if ($jdk21) { Write-Row 'JDK' 'OK' "21  ($($jdk21.FullName))" }
elseif ($java) {
    $v = Get-Ver (Get-Output $java @('-version'))
    Write-Row 'JDK' 'VERSION' "$v (se pide 21)  winget install EclipseAdoptium.Temurin.21.JDK"
} else { Write-Row 'JDK' 'FALTA' 'winget install EclipseAdoptium.Temurin.21.JDK' }

# --- Ghidra -------------------------------------------------------------------
# Se busca support\analyzeHeadless.bat en GHIDRA_INSTALL_DIR o en carpetas tipicas.
$ghCands = @()
if ($env:GHIDRA_INSTALL_DIR) { $ghCands += "$env:GHIDRA_INSTALL_DIR\support\analyzeHeadless.bat" }
foreach ($base in @('C:\', 'C:\tools', "$env:ProgramFiles", "$env:USERPROFILE", "$env:USERPROFILE\Desktop", "$env:USERPROFILE\Downloads")) {
    $ghCands += "$base\ghidra*\support\analyzeHeadless.bat"
}
$gh = $ghCands | ForEach-Object { Get-ChildItem $_ -ErrorAction SilentlyContinue } | Select-Object -First 1
if ($gh) {
    $root = Split-Path (Split-Path $gh.FullName)
    $props = Join-Path $root 'Ghidra\application.properties'
    $ver = '?'
    if (Test-Path $props) {
        $line = Select-String -Path $props -Pattern '^application.version=' | Select-Object -First 1
        if ($line) { $ver = $line.Line.Split('=')[1] }
    }
    Write-Row 'Ghidra' 'OK' "$ver  ($root)"
} else { Write-Row 'Ghidra' 'FALTA' 'descargar de github.com/NationalSecurityAgency/ghidra/releases y fijar GHIDRA_INSTALL_DIR' }

Write-Host ("-" * 60)
if ($script:fallos -eq 0) { Write-Host 'Entorno completo.' -ForegroundColor Green; exit 0 }
Write-Host "$script:fallos herramienta(s) faltan o no cumplen la version." -ForegroundColor Red
exit 1
