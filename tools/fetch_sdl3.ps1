# fetch_sdl3.ps1 - 5d.1: descarga la version de SDL3 que usa cellPad para los mandos.
#
# Uso:  powershell -ExecutionPolicy Bypass -File tools\fetch_sdl3.ps1
#
# Paquete oficial de desarrollo de libsdl-org/SDL para Visual C++ (cabeceras, SDL3.lib y SDL3.dll,
# x64). Comprueba el SHA-256 del zip antes de abrirlo y deja en work\sdl3\ solo lo que hace falta:
# nada va a git. Licencia en THIRD_PARTY_NOTICES.md; por que SDL3, en docs/decisions.md.

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$version = '3.4.18'
$zipName = "SDL3-devel-$version-VC.zip"
$sha256 = '78d84602ae616cfe26b33a73b7d3b9b6b56e8a6650e53d92c9c6ca938f747acd'
$url = "https://github.com/libsdl-org/SDL/releases/download/release-$version/$zipName"

$out = Join-Path $root 'work\sdl3'
$stamp = Join-Path $out 'version.txt'
if ((Test-Path $stamp) -and ((Get-Content $stamp -Raw).Trim() -eq $sha256)) {
    Write-Output "SDL3 ya esta en $out ($zipName)"
    exit 0
}

$dl = Join-Path $root 'work\sdl3_dl'
if (Test-Path $dl) { Remove-Item -Recurse -Force $dl }
New-Item -ItemType Directory -Force $dl | Out-Null
$zip = Join-Path $dl $zipName
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$ProgressPreference = 'SilentlyContinue'
Invoke-WebRequest -Uri $url -OutFile $zip

$got = (Get-FileHash -Algorithm SHA256 $zip).Hash.ToLower()
if ($got -ne $sha256) { throw "SHA-256 distinto: $got, se esperaba $sha256" }

$x = Join-Path $dl 'x'
Expand-Archive -Path $zip -DestinationPath $x
$top = (Get-ChildItem $x -Directory | Select-Object -First 1).FullName

if (Test-Path $out) { Remove-Item -Recurse -Force $out }
New-Item -ItemType Directory -Force (Join-Path $out 'lib') | Out-Null
Copy-Item -Recurse (Join-Path $top 'include') $out
Copy-Item (Join-Path $top 'lib\x64\SDL3.lib') (Join-Path $out 'lib')
Copy-Item (Join-Path $top 'lib\x64\SDL3.dll') (Join-Path $out 'lib')
Copy-Item (Join-Path $top 'LICENSE.txt') $out
Set-Content -Path $stamp -Value $sha256 -Encoding ascii
Remove-Item -Recurse -Force $dl

Get-ChildItem (Join-Path $out 'lib') | ForEach-Object {
    '{0}  {1,10}  {2}' -f (Get-FileHash -Algorithm SHA256 $_.FullName).Hash.ToLower(), $_.Length, $_.Name
}
Write-Output "SDL3 listo en $out ($zipName)"
