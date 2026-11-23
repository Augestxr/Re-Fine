# apply_patches.ps1 - Deja external\ps3recomp en su commit fijado + patches\*.patch.
#
# Uso:  powershell -ExecutionPolicy Bypass -File tools\apply_patches.ps1
#
# Descarta cualquier cambio local del submodulo (que no se edita a mano: los cambios
# viven en patches\) y aplica los parches en orden de nombre. Para crear un parche
# nuevo: editar el submodulo, `git -C external\ps3recomp diff > patches\NNNN-que.patch`
# y volver a correr este script para comprobar que aplica desde cero.

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$sub = Join-Path $root 'external\ps3recomp'

# Commit fijado: el del submodulo en HEAD. Antes del primer commit del repo (hito 0)
# no hay HEAD y se toma el del indice (git submodule add ya lo dejo ahi).
$null = git -C $root rev-parse --verify -q HEAD
if ($LASTEXITCODE -eq 0) { $pinned = (git -C $root ls-tree HEAD external/ps3recomp).Split()[2] }
else { $pinned = (git -C $root ls-files --stage external/ps3recomp).Split()[1]; Write-Host "(sin HEAD: commit fijado tomado del indice)" }
git -C $sub checkout --quiet --force $pinned
if ($LASTEXITCODE -ne 0) { throw "no se pudo volver al commit fijado $pinned" }
git -C $sub clean --quiet -fd
Write-Host "ps3recomp en $pinned (limpio)"

$patches = Get-ChildItem (Join-Path $root 'patches') -Filter '*.patch' | Sort-Object Name
foreach ($p in $patches) {
    git -C $sub apply --check $p.FullName
    if ($LASTEXITCODE -ne 0) { throw "$($p.Name) no aplica sobre $pinned" }
    git -C $sub apply $p.FullName
    if ($LASTEXITCODE -ne 0) { throw "$($p.Name) fallo al aplicar" }
    Write-Host "aplicado $($p.Name)"
}
Write-Host "$($patches.Count) parche(s) aplicados"
