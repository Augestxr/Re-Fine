# lift.ps1 - Hito 2: EBOOT.elf -> C++ (work/recompiled). Nada de esto va a git.
#
# Uso:  powershell -ExecutionPolicy Bypass -File tools\lift.ps1
#
# Pasos (flags explicados en docs/decisions.md):
#   1. ppu_loader.py   -> work/loader_out (funciones, imports, TOC, segmentos)
#   2. ppu_lifter.py   -> work/recompiled (ppu_recomp*.cpp + ppu_recomp.h)
#   3. gen_hle_nids.py -> work/recompiled/ppu_hle_nids.cpp (tabla NID -> handler)
#
# El codigo generado no se edita a mano: si algo falla, se corrige ps3recomp con un
# .patch en patches/ y se vuelve a correr este script.

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$tools = Join-Path $root 'external\ps3recomp\tools'
$elf = Join-Path $root 'game\EBOOT.elf'
$loaderOut = Join-Path $root 'work\loader_out'
$recomp = Join-Path $root 'work\recompiled'

# Fin (exclusivo) de la ultima seccion ejecutable del EBOOT (BLUS31319, APP_VER 01.00).
# .init 0x10200, .text 0x10240-0x9c91b4, .fini, y los stubs de import 0x9c91e8-0x9cb488
# (tools/elf_info.py, docs/analysis.md). Lo que va detras es .rodata.
$codeEnd = '0x9cb488'

if (-not (Test-Path $elf)) { throw "Falta $elf" }
$env:PYTHONUTF8 = '1'

function Run([string]$what, [string[]]$argv) {
    Write-Host "== $what"
    & python @argv
    if ($LASTEXITCODE -ne 0) { throw "$what fallo (exit $LASTEXITCODE)" }
}

# config/insn_patches.json no lleva bytes del EBOOT ("check" = SHA-256, hito 8): se
# comprueba con el EBOOT del usuario y se escribe con "orig" en work/ (fuera de git).
$insnSpec = Join-Path $root 'work\lift\insn_patches.json'
New-Item -ItemType Directory -Force (Split-Path $insnSpec) | Out-Null
Run 'insn_patches' @("$root\tools\insn_patches_resolve.py", $elf, "$root\config\insn_patches.json", $insnSpec)
Run 'loader' @("$tools\ppu_loader.py", $elf, '-o', $loaderOut)
if (Test-Path $recomp) { Remove-Item -Recurse -Force $recomp }
Run 'lifter' @("$tools\ppu_lifter.py", $elf,
    '--functions', "$loaderOut\EBOOT.functions.json",
    '--hle-stubs', "$loaderOut\EBOOT.imports.json",
    '--code-end', $codeEnd,
    '--insn-patches', $insnSpec,
    '-o', $recomp)
Run 'func_hooks' @("$root\tools\func_hooks.py", $recomp, "$root\config\func_hooks.json")
Run 'gen_hle_nids' @("$tools\gen_hle_nids.py", '--all', '--out', "$recomp\ppu_hle_nids.cpp")
Write-Host "Listo: $recomp"
