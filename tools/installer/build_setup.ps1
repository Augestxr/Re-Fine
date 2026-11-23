# build_setup.ps1 - arma el instalador nuevo (ReFineSetup.exe) en dist\ReFineSetup-<version>\.
# Sustituye a build_installer.ps1 (Inno, 0.1.1).
#
# El payload se arma SIEMPRE desde build\ y tools\installer (nunca de un dist\payload guardado):
# lo que se empaqueta es lo que se acaba de compilar.
#
# Publico (por defecto): ReFineSetup.exe, ui\ y payload\ sin partidas ni arte de terceros. NO lleva
#   ReFine.exe ni codigo generado: el instalador los hace en el PC del usuario (build_game.ps1) y
#   por eso lleva payload\src\ (fuentes del runtime, parches ya aplicados, lifter, RmlUi, SDL3, FFmpeg).
# Privado: -WithSave <carpeta|latest>  anade payload\partida\NPUB31241_* (latest: la mas reciente de
#   work\backup\partida_*) y el arte de tools\installer\art_privado como fondo. Lleva el
#   ReFine.exe de build\ y su ui\ (menu F1, pantalla de inicio): no descarga ni compila nada.
#   Falla si el exe empaquetado no tiene el mismo SHA-256 que build\ReFine.exe. Solo a mano; nunca a GitHub.
#
# Uso:  powershell -ExecutionPolicy Bypass -File tools\installer\build_setup.ps1 [-WithSave <carpeta|latest>]

param(
    [string]$WithSave = $null,
    [string]$Version = '1.0.0-rc1'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$setup = Join-Path $root 'build\setup'
if (-not (Test-Path "$setup\ReFineSetup.exe")) { throw "Falta $setup\ReFineSetup.exe: cmake --build build --target ReFineSetup" }
$private = $PSBoundParameters.ContainsKey('WithSave')

# --- payload en un directorio temporal, desde build\ ---
$Payload = Join-Path $root 'dist\_stage'
if (Test-Path $Payload) { Remove-Item $Payload -Recurse -Force }
New-Item -ItemType Directory -Force "$Payload\licenses" | Out-Null
foreach ($f in 'avcodec-63.dll', 'avutil-61.dll', 'swresample-7.dll', 'swscale-10.dll', 'SDL3.dll') {
    if (-not (Test-Path "$root\build\$f")) { throw "Falta build\$f" }
    Copy-Item "$root\build\$f" $Payload
}
$vs = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\18\BuildTools\VC\Redist\MSVC"
$crt = Get-ChildItem "$vs\*\x64\Microsoft.VC*.CRT" -Directory | Sort-Object FullName | Select-Object -Last 1
if (-not $crt) { throw "No encuentro el runtime de VC++ en $vs" }
foreach ($f in 'msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll') { Copy-Item "$($crt.FullName)\$f" $Payload }
Copy-Item "$PSScriptRoot\refine.cmd", "$PSScriptRoot\refine.ini", "$PSScriptRoot\elegir_carpeta.ps1", "$PSScriptRoot\LEEME.txt" $Payload
# LEEME: titulo y version segun la edicion (la privada lleva el aviso de no compartir).
$title = "Re:Fine $Version" + $(if ($private) { ' - PRUEBA PRIVADA, no lo compartas ni lo publiques.' } else { '' })
$leeme = [IO.File]::ReadAllText("$PSScriptRoot\LEEME.txt").Replace('{{TITLE}}', $title).Replace('{{VER}}', $Version)
[IO.File]::WriteAllText("$Payload\LEEME.txt", $leeme, (New-Object Text.UTF8Encoding($false)))
Copy-Item "$root\work\ffmpeg\LICENSE.txt" "$Payload\licenses\FFmpeg-LGPL-3.0.txt"
Copy-Item "$root\work\sdl3\LICENSE.txt" "$Payload\licenses\SDL3-zlib.txt"
Copy-Item "$root\external\ps3recomp\LICENSE" "$Payload\licenses\ps3recomp-LICENSE.txt"
Copy-Item "$root\THIRD_PARTY_NOTICES.md" "$Payload\licenses\THIRD_PARTY_NOTICES.md"
Copy-Item "$PSScriptRoot\FFmpeg-fuente.txt" "$Payload\licenses\FFmpeg-fuente.txt"
# Licencias de lo propio de la interfaz (RmlUi MIT, FreeType FTL, fuentes OFL, stb): la OFL pide que acompanen a las fuentes.
foreach ($d in Get-ChildItem "$root\third_party" -Directory) {
    foreach ($l in Get-ChildItem $d.FullName -File | Where-Object { $_.Name -match '^(LICEN[CS]E|FTL|OFL)' }) {
        Copy-Item $l.FullName "$Payload\licenses\$($d.Name)-$($l.Name)"
    }
}
$nlic = @(Get-ChildItem "$Payload\licenses" -Filter '*OFL*').Count
if ($nlic -lt 4) { throw "Faltan licencias OFL en payload\licenses ($nlic)" }
# DXC (backend Vulkan, en ejecucion): solo dxcompiler.dll, con sus licencias.
foreach ($f in 'LICENSE-LLVM.txt', 'LICENSE-MS.txt', 'LICENCE-MIT.txt') {
    if (-not (Test-Path "$root\work\dxc\$f")) { throw "Falta work\dxc\$f" }
    Copy-Item "$root\work\dxc\$f" "$Payload\licenses\DXC-$f"
}
# dxcompiler.dll (de Microsoft, firmada, redistribuible con las licencias de arriba): la usa el backend Vulkan en ejecucion.
# Va en las dos versiones; la publica la copia junto al juego al terminar el build (build_game.ps1).
if (-not (Test-Path "$root\work\dxc\dxcompiler.dll")) { throw 'Falta work\dxc\dxcompiler.dll: corre tools\fetch_vulkan.ps1' }
$sig = Get-AuthenticodeSignature "$root\work\dxc\dxcompiler.dll"
if ($sig.Status -ne 'Valid' -or $sig.SignerCertificate.Subject -notmatch 'O=Microsoft Corporation') { throw "dxcompiler.dll sin firma valida de Microsoft ($($sig.Status))" }
Copy-Item "$root\work\dxc\dxcompiler.dll" $Payload
if ($private) {
    if (-not (Test-Path "$root\build\ReFine.exe")) { throw 'Falta build\ReFine.exe' }
    # Hito 7: the private one carries the fast build (guest memory inline, about half the game thread's
    # time; docs/perf.md 2026-11-11). Build it with tools\build.ps1 -InlineMem first.
    if (-not (Select-String -Path "$root\build\CMakeCache.txt" -Pattern '^REFINE_INLINE_MEM:BOOL=ON' -Quiet)) {
        throw 'build\ReFine.exe es la build de desarrollo (sin -InlineMem): compila con tools\build.ps1 -InlineMem antes de empaquetar'
    }
    if (-not (Test-Path "$root\build\ui\menu.rml")) { throw 'Falta build\ui\menu.rml' }
    Copy-Item "$root\build\ReFine.exe" $Payload
    Copy-Item "$root\build\ui" "$Payload\ui" -Recurse
}

$out = Join-Path $root ("dist\ReFineSetup-$Version" + $(if ($private) { '-PRIVADO' } else { '' }))
if (Test-Path $out) { Remove-Item $out -Recurse -Force }
New-Item -ItemType Directory -Force $out | Out-Null
Copy-Item "$setup\ReFineSetup.exe" $out
Copy-Item "$setup\ui" "$out\ui" -Recurse
Copy-Item $Payload "$out\payload" -Recurse
Remove-Item $Payload -Recurse -Force

if (-not $private) {
    Copy-Item "$PSScriptRoot\build_game.ps1" "$out\payload"
    # Fuentes que lift.ps1 y build.ps1 necesitan en el PC del usuario (ps3recomp ya parcheado: MIT).
    $src = "$out\payload\src"
    New-Item -ItemType Directory -Force "$src\tools", "$src\work\ffmpeg", "$src\work\sdl3" | Out-Null
    foreach ($f in 'CMakeLists.txt', 'THIRD_PARTY_NOTICES.md') { Copy-Item "$root\$f" $src }
    foreach ($d in 'src', 'config', 'patches', 'third_party') { Copy-Item "$root\$d" "$src\$d" -Recurse }
    foreach ($f in 'lift.ps1', 'lift_spu.ps1', 'build.ps1', 'insn_patches_resolve.py', 'fetch_vulkan.ps1') { Copy-Item "$root\tools\$f" "$src\tools" }
    & robocopy "$root\external\ps3recomp" "$src\external\ps3recomp" /E /XD .git tests docs scratch __pycache__ /XF .git /NFL /NDL /NJH /NJS /NP | Out-Null
    if ($LASTEXITCODE -ge 8) { throw 'robocopy de ps3recomp fallo' }
    Copy-Item "$root\work\ffmpeg\include" "$src\work\ffmpeg\include" -Recurse
    Copy-Item "$root\work\sdl3\*" "$src\work\sdl3" -Recurse
    Copy-Item "$root\work\rmlui" "$src\work\rmlui" -Recurse
    # Cortafuegos de la version publica: ni partidas ni arte, ni cabeceras del juego.
    $bad = Get-ChildItem $out -Recurse -File | Where-Object { $_.FullName -match 'NPUB31241|partida|PARAM\.(SFO|PFD)|SECURE\.BIN|\\art\\' }
    if ($bad) { throw "En la version publica: $($bad[0].FullName)" }
} else {
    if ($WithSave -eq 'latest') {
        $WithSave = (Get-ChildItem "$root\work\backup" -Directory -Filter 'partida_*' | Sort-Object Name | Select-Object -Last 1).FullName
    }
    $save = Get-ChildItem $WithSave -Directory -Filter 'NPUB31241_*' | Select-Object -First 1
    if (-not $save) { throw "No hay NPUB31241_* en $WithSave" }
    New-Item -ItemType Directory -Force "$out\payload\partida" | Out-Null
    Copy-Item $save.FullName "$out\payload\partida\$($save.Name)" -Recurse
    $art = Join-Path $PSScriptRoot 'art_privado\hatsune_miku_project_diva_f_by_evoxiii-d52lcj2.png'
    if (Test-Path $art) { New-Item -ItemType Directory -Force "$out\ui\art" | Out-Null; Copy-Item $art "$out\ui\art\bg.png" }
    Write-Host "PRIVADO: con la partida $($save.FullName). No se publica." -ForegroundColor Yellow
    # Comprobacion: el exe empaquetado es el de build\ y existe ui\ del juego.
    $h1 = (Get-FileHash "$out\payload\ReFine.exe" -Algorithm SHA256).Hash
    $h2 = (Get-FileHash "$root\build\ReFine.exe" -Algorithm SHA256).Hash
    if ($h1 -ne $h2) { throw "ReFine.exe empaquetado ($h1) distinto de build\ReFine.exe ($h2)" }
    foreach ($f in 'menu.rml', 'theme.rcss', 'menu.rcss') { if (-not (Test-Path "$out\payload\ui\$f")) { throw "Falta payload\ui\$f" } }
    Write-Host "OK: ReFine.exe = build\ReFine.exe (SHA-256 $h1), ui\ presente"
}
# Cortafuegos comun: ningun ELF/SELF/SFO del juego ni codigo generado; ReFine.exe solo en la privada.
foreach ($f in Get-ChildItem $out -Recurse -File | Where-Object { $_.FullName -notlike "$out\payload\partida\*" }) {   # la partida del amigo (privada) lleva su PARAM.SFO de guardado
    $fs = [IO.File]::OpenRead($f.FullName); $b = New-Object byte[] 4; [void]$fs.Read($b, 0, 4); $fs.Close()
    $m = [Text.Encoding]::ASCII.GetString($b)
    if ($m -eq "$([char]0x7F)ELF" -or $m -eq "SCE$([char]0)" -or $m -eq "$([char]0)PSF") { throw "Archivo del juego: $($f.FullName)" }
}
if (Get-ChildItem $out -Recurse -File | Where-Object { $_.Name -in 'ppu_recomp.h', 'ppu_hle_nids.cpp' -or (-not $private -and $_.Name -eq 'ReFine.exe') }) { throw 'ReFine.exe (en la publica) o codigo generado en el payload' }
'{0}  {1:N0} MB' -f $out, ((Get-ChildItem $out -Recurse -File | Measure-Object Length -Sum).Sum / 1MB)
