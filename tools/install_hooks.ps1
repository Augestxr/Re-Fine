# install_hooks.ps1 - Activa los hooks versionados de tools\hooks (firewall pre-commit).
#
# Uso:  powershell -ExecutionPolicy Bypass -File tools\install_hooks.ps1
#
# Pone core.hooksPath = tools/hooks en el config local del repo: git ejecuta los hooks
# directamente desde el arbol, asi que un cambio en tools\hooks\pre-commit vale al
# momento sin volver a instalar. Git para Windows los corre con su sh.

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$hook = Join-Path $root 'tools\hooks\pre-commit'
if (-not (Test-Path $hook)) { throw "Falta $hook" }

git -C $root config core.hooksPath tools/hooks
if ($LASTEXITCODE -ne 0) { throw "git config fallo: exit $LASTEXITCODE" }
Write-Host "core.hooksPath = $(git -C $root config core.hooksPath)"
