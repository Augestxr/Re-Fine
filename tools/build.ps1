# build.ps1 - Configura y compila ReFine con clang-cl sobre las cabeceras de MSVC.
#
# Uso:  powershell -ExecutionPolicy Bypass -File tools\build.ps1 [-Config Release] [-Jobs N] [-InlineMem] [-BuildDir build_fast]
#
# Necesita work/recompiled (tools\lift.ps1). Salida en build/ (ignorado por git).

param(
    [string]$Config = 'Release',
    [int]$Jobs = 0,
    [switch]$Full,       # salida completa (antes era lo normal); tambien con -Verbose
    [switch]$InlineMem,  # REFINE_INLINE_MEM=ON: la build final y sus medidas (docs/decisions.md, 6.0.3)
    [string]$BuildDir = 'build'   # otra carpeta (p. ej. build_fast con -InlineMem) para no rehacer la de desarrollo al cambiar de modo
)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$build = Join-Path $root $BuildDir

# Entorno de MSVC (cabeceras, librerias, SDK) desde la instalacion de VS mas reciente.
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'No hay Visual Studio con las herramientas de C++' }
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
cmd /c "`"$vcvars`" >nul && set" | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') { Set-Item "env:$($Matches[1])" $Matches[2] }
}

# clang-cl de LLVM y CMake/Ninja (los de VS si no hay otros en el PATH).
$llvmBin = if ($env:REFINE_LLVM_BIN) { $env:REFINE_LLVM_BIN } else { "$env:ProgramFiles\LLVM\bin" }   # el instalador fija REFINE_LLVM_BIN
$extra = @($llvmBin,
    "$vs\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin",
    "$vs\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja")
$env:Path = ($extra -join ';') + ';' + $env:Path

cmake -S $root -B $build -G Ninja "-DCMAKE_BUILD_TYPE=$Config" `
    -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl `
    "-DRECOMP_DIR=$root\work\recompiled" "-DSPU_DIR=$root\work\spu" `
    "-DREFINE_INLINE_MEM=$(if ($InlineMem) { 'ON' } else { 'OFF' })" "-DREFINE_SPU_ARCH=-msse4.1"
if ($LASTEXITCODE -ne 0) { throw "cmake (configurar) fallo: exit $LASTEXITCODE" }

$jobArgs = @()
if ($Jobs -gt 0) { $jobArgs = @('--', "-j$Jobs") }
# La compilacion va por cmd: asi el stderr de clang no se convierte en ErrorRecord de PowerShell 5.1
# y el codigo de salida es el de ninja. Ademas se mira la salida: "Listo" solo si no hubo fallos.
$exe = Join-Path $build 'ReFine.exe'
$log = Join-Path $build 'build_last.log'
$sw = [Diagnostics.Stopwatch]::StartNew()
$verb = $Full -or ($VerbosePreference -eq 'Continue')
if ($verb) { cmd /c "cmake --build `"$build`" $($jobArgs -join ' ') 2>&1" | Tee-Object -FilePath $log }
else { cmd /c "cmake --build `"$build`" $($jobArgs -join ' ') > `"$log`" 2>&1" }
$code = $LASTEXITCODE
$failed = Select-String -Path $log -Pattern '^FAILED:', 'build stopped', ': error' -Quiet
if ($code -ne 0 -or $failed -or -not (Test-Path $exe)) {
    Write-Host "ERROR: la compilacion fallo (exit $code). Log completo: $log" -ForegroundColor Red
    if (-not $verb) {
        # Solo errores, nunca avisos de third_party; como mucho 30 lineas.
        Select-String -Path $log -Pattern ': error|^FAILED:|undefined symbol|LNK\d+' |
            Where-Object { $_.Line -notmatch 'third_party' } | Select-Object -First 30 | ForEach-Object { Write-Host $_.Line }
    }
    exit 1
}
Write-Host ("OK en {0:N1} min ({1}). Log: {2}" -f $sw.Elapsed.TotalMinutes, $Config, $log)
