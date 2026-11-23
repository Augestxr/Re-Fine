# build_installer.ps1 - arma dist\payload con el build actual y compila el setup con Inno Setup.
# No recompila nada: usa build\ReFine.exe tal como esta.
#
# Uso:  powershell -ExecutionPolicy Bypass -File tools\installer\build_installer.ps1

$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$here = $PSScriptRoot
$payload = Join-Path $root 'dist\payload'
$iscc = Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 6\ISCC.exe'
if (-not (Test-Path $iscc)) { throw "Falta Inno Setup: $iscc" }

if (Test-Path $payload) { Remove-Item $payload -Recurse -Force }
New-Item -ItemType Directory -Force "$payload\licenses" | Out-Null

foreach ($f in 'ReFine.exe', 'avcodec-63.dll', 'avutil-61.dll', 'swresample-7.dll', 'swscale-10.dll', 'SDL3.dll') {
    Copy-Item "$root\build\$f" $payload
}
# Despliegue local del runtime de VC++, sin vc_redist: ese instalador pide administrador
$vs = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\18\BuildTools\VC\Redist\MSVC"
$crt = Get-ChildItem "$vs\*\x64\Microsoft.VC*.CRT" -Directory | Sort-Object FullName | Select-Object -Last 1
foreach ($f in 'msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll') { Copy-Item "$($crt.FullName)\$f" $payload }

Copy-Item "$here\refine.cmd", "$here\refine.ini", "$here\elegir_carpeta.ps1", "$here\LEEME.txt" $payload
Copy-Item "$root\work\ffmpeg\LICENSE.txt" "$payload\licenses\FFmpeg-LGPL-3.0.txt"
Copy-Item "$root\work\sdl3\LICENSE.txt" "$payload\licenses\SDL3-zlib.txt"
Copy-Item "$root\external\ps3recomp\LICENSE" "$payload\licenses\ps3recomp-LICENSE.txt"
Copy-Item "$root\THIRD_PARTY_NOTICES.md" "$payload\licenses\THIRD_PARTY_NOTICES.md"
Copy-Item "$here\FFmpeg-fuente.txt" "$payload\licenses\FFmpeg-fuente.txt"

# Cortafuegos: solo pasan los tipos esperados y nada con cabecera ELF, SCE o PSF
$ok = '.exe', '.dll', '.cmd', '.ini', '.ps1', '.txt', '.md'
foreach ($f in Get-ChildItem $payload -Recurse -File) {
    if ($ok -notcontains $f.Extension) { throw "Tipo no permitido en el instalador: $($f.Name)" }
    $b = [IO.File]::ReadAllBytes($f.FullName)[0..3]
    $m = [Text.Encoding]::ASCII.GetString($b)
    if ($m -eq "$([char]0x7F)ELF" -or $m -eq "SCE$([char]0)" -or $m -eq "$([char]0)PSF") { throw "Archivo del juego en el instalador: $($f.Name)" }
}

& $iscc /Q "$here\ReFine.iss"
if ($LASTEXITCODE -ne 0) { throw "ISCC salio con $LASTEXITCODE" }
Get-ChildItem "$root\dist\*.exe" | ForEach-Object { '{0}  {1:N1} MB' -f $_.FullName, ($_.Length / 1MB) }
