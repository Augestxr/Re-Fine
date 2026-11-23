# fetch_ffmpeg.ps1 - 4c.3.1: descarga la build LGPL de FFmpeg que usan cellVdec y cellAdec.
#
# Uso:  powershell -ExecutionPolicy Bypass -File tools\fetch_ffmpeg.ps1
#
# Build fijada de BtbN/FFmpeg-Builds (enlazada desde ffmpeg.org para Windows). Comprueba el SHA-256
# del zip antes de abrirlo y deja en work\ffmpeg\ solo las DLL y las cabeceras: nada va a git.
# La licencia y el modo de cumplirla, en THIRD_PARTY_NOTICES.md y docs/decisions.md.

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$tag = 'autobuild-2026-10-31-12-59'
$zipName = 'ffmpeg-n9.0.2-39-g7adfbe40dc-win64-lgpl-shared-9.0.zip'
$sha256 = 'e907b01fa4783913151237e37df2a10fa97d99a1598a021bf59d4ae62ffed4c0'
$url = "https://github.com/BtbN/FFmpeg-Builds/releases/download/$tag/$zipName"

$out = Join-Path $root 'work\ffmpeg'
$stamp = Join-Path $out 'version.txt'
if ((Test-Path $stamp) -and ((Get-Content $stamp -Raw).Trim() -eq $sha256)) {
    Write-Output "FFmpeg ya esta en $out ($zipName)"
    exit 0
}

$dl = Join-Path $root 'work\ffmpeg_dl'
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
New-Item -ItemType Directory -Force (Join-Path $out 'bin') | Out-Null
Copy-Item (Join-Path $top 'bin\*.dll') (Join-Path $out 'bin')
Copy-Item -Recurse (Join-Path $top 'include') $out
Copy-Item (Join-Path $top 'LICENSE.txt') $out
Set-Content -Path $stamp -Value $sha256 -Encoding ascii
Remove-Item -Recurse -Force $dl

Get-ChildItem (Join-Path $out 'bin') | ForEach-Object {
    '{0}  {1,10}  {2}' -f (Get-FileHash -Algorithm SHA256 $_.FullName).Hash.ToLower(), $_.Length, $_.Name
}
Write-Output "FFmpeg listo en $out ($zipName)"
