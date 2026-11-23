# test_spu.ps1 - Tests de semantica SPU de ps3recomp (los que corre su CI y dos mas).
#
# Uso:  powershell -ExecutionPolicy Bypass -File tools\test_spu.ps1
#
# Compila con clang (LLVM) sobre el entorno de MSVC y ejecuta, en work\spu_tests\:
#   test_spu_helpers            semantica de cada instruccion (el que corre la CI)
#   test_spu_numeric_semantics  casos numericos limite
#   test_spu_shufb              shufb SIMD contra la referencia escalar (-msse4.1)
# La suite de integracion runtime/spu/tests/run_tests.sh no se usa: no enlaza con el
# runtime del commit fijado y su generador de shufb usa la tabla de opcodes antigua
# (ver docs/spu.md).

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$p = Join-Path $root 'external\ps3recomp'
$out = Join-Path $root 'work\spu_tests'
New-Item -ItemType Directory -Force $out | Out-Null

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
cmd /c "`"$vs\VC\Auxiliary\Build\vcvars64.bat`" >nul && set" | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') { Set-Item "env:$($Matches[1])" $Matches[2] }
}
$env:Path = "$env:ProgramFiles\LLVM\bin;" + $env:Path

$tests = @(
    @{ n = 'test_spu_helpers'; f = @() },
    @{ n = 'test_spu_numeric_semantics'; f = @() },
    @{ n = 'test_spu_shufb'; f = @('-msse4.1') }
)
$fail = 0
foreach ($t in $tests) {
    $exe = Join-Path $out "$($t.n).exe"
    $args = @('-std=c11', '-O2', '-w') + $t.f + @('-I', "$p\runtime", '-I', "$p\runtime\spu",
        '-I', "$p\include", "$p\runtime\spu\tests\$($t.n).c", '-o', $exe)
    & clang @args
    if ($LASTEXITCODE -ne 0) { Write-Host "BUILD FAILED $($t.n)"; $fail++; continue }
    $o = & $exe 2>&1
    $rc = $LASTEXITCODE
    Write-Host ("{0,-28} exit={1}  {2}" -f $t.n, $rc, (($o | Select-Object -Last 1) -join ''))
    if ($rc -ne 0) { $fail++ }
}
if ($fail) { Write-Host "$fail test(s) fallaron"; exit 1 }
Write-Host 'Todos los tests SPU pasan.'
