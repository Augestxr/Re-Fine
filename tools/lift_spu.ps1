# lift_spu.ps1 - Hito 4: programas SPU embebidos en el EBOOT -> C (work/spu). Nada va a git.
#
# Uso:  powershell -ExecutionPolicy Bypass -File tools\lift_spu.ps1
#
#   1. extract_spu_images.py   -> work/spu_images/*.elf (ver docs/spu.md)
#   2. build_spu_workloads.py  -> traduce TODAS las imagenes a work/spu/<imagen>/
#      (spu_lifter --auto-functions, un prefijo de simbolos por imagen)
#   3. build_spu_workloads.py --skip-lift sobre las imagenes ACTIVAS
#      -> work/spu/spu_workloads.c, el registro por huella que compila CMake.
#      Una imagen traducida pero no activa se compila y no se registra: el runtime
#      nunca la despacha (synth2, el mezclador, se activa en el hito 4b).
#
# Sin --code-end: en la imagen 0 las cadenas de Edge (0xB2A4-0xB8A4) quedan en medio
# de .text con codigo detras, y cada .word se traduce como spu_unsupported(), una
# trampa que avisa si se ejecuta. Ver docs/decisions.md.

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$tools = Join-Path $root 'external\ps3recomp\tools'
$elf = Join-Path $root 'game\EBOOT.elf'
$images = Join-Path $root 'work\spu_images'
$active = Join-Path $root 'work\spu_images_active'
$spu = Join-Path $root 'work\spu'

# Imagenes que el runtime puede despachar (por nombre de extract_spu_images.py).
# F tiene una sola imagen, spu_0000_at_00B82180 = synth2 (mismo hash que en F2nd; docs/spu.md).
# Activa desde el hito 4b: el mezclador de audio la arranca como grupo lv2 y sin ella el
# arranque se interbloquea (docs/hle-log.md).
$activeNames = @('spu_0000_at_00B82180')   # synth2 (mezclador de audio)

$env:PYTHONUTF8 = '1'
function Run([string]$what, [string[]]$argv) {
    Write-Host "== $what"
    & python @argv
    if ($LASTEXITCODE -ne 0) { throw "$what fallo (exit $LASTEXITCODE)" }
}

foreach ($d in @($images, $active, $spu)) { if (Test-Path $d) { Remove-Item -Recurse -Force $d } }
Run 'extract' @("$tools\extract_spu_images.py", $elf, '-o', $images)

Run 'lift (todas)' @("$tools\build_spu_workloads.py", '--images', $images, '--lifted', $spu,
    '--out', "$spu\spu_workloads_all.c.txt", '--title', 'ReFine')

if ($activeNames.Count -eq 0) {
    # Sin imagenes activas no hay registro: CMake compila los spu_recomp.c y no genera
    # spu_workloads.c, asi que el runtime no despacha ninguna (igual que synth2 en F2nd).
    Write-Host "== registro: ninguna imagen activa (sin spu_workloads.c)"
    Write-Host "Listo: $spu"
    return
}
New-Item -ItemType Directory -Force $active | Out-Null
foreach ($n in $activeNames) { Copy-Item "$images\$n.elf" $active }
Run 'registro (activas)' @("$tools\build_spu_workloads.py", '--images', $active, '--lifted', $spu,
    '--out', "$spu\spu_workloads.c", '--title', 'ReFine', '--skip-lift', '--constructor')
Write-Host "Listo: $spu"
