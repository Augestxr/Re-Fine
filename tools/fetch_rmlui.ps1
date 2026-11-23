# fetch_rmlui.ps1 - 6b+6c: descarga las fuentes de RmlUi, FreeType y la fuente Inter del menu del port.
#
# Uso:  powershell -ExecutionPolicy Bypass -File tools\fetch_rmlui.ps1
#
# Versiones fijadas, con SHA-256 comprobado antes de abrir nada. Todo queda en work\rmlui\ (fuera de
# git); CMakeLists.txt compila RmlUi y FreeType desde ahi con el mismo compilador que el port.
# Licencias en third_party\ (copiadas a mano de estos mismos paquetes) y en THIRD_PARTY_NOTICES.md.

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$pkgs = @(
    @{ name = 'RmlUi 6.3'; file = 'RmlUi-6.3.zip'
       url = 'https://github.com/mikke89/RmlUi/archive/refs/tags/6.3.zip'
       sha = 'bfb340660e4bce38d4211cffc836cfbe1be676aa36ef9cc48e7df3855ee74fe3' },
    @{ name = 'FreeType 2.14.3'; file = 'freetype-2.14.3.tar.gz'
       url = 'https://download.savannah.gnu.org/releases/freetype/freetype-2.14.3.tar.gz'
       sha = 'e61b31ab26358b946e767ed7eb7f4bb2e507da1cfefeb7a8861ace7fd5c899a1' },
    @{ name = 'Inter 4.1'; file = 'Inter-4.1.zip'
       url = 'https://github.com/rsms/inter/releases/download/v4.1/Inter-4.1.zip'
       sha = '9883fdd4a49d4fb66bd8177ba6625ef9a64aa45899767dde3d36aa425756b11e' },
    # 6b+6c parte 2: Barlow Condensed (OFL 1.1), google/fonts fijado al commit 60824dce48f7dd28fe7d65559f2da1f6e04e585b.
    @{ name = 'Barlow Condensed SemiBold'; file = 'BarlowCondensed-SemiBold.ttf'; font = $true
       url = 'https://raw.githubusercontent.com/google/fonts/60824dce48f7dd28fe7d65559f2da1f6e04e585b/ofl/barlowcondensed/BarlowCondensed-SemiBold.ttf'
       sha = '7b619d14bc2327509a9ef32b0890f709626f7ecc9ff61191c2a4314c5499d2d9' },
    @{ name = 'Barlow Condensed Bold'; file = 'BarlowCondensed-Bold.ttf'; font = $true
       url = 'https://raw.githubusercontent.com/google/fonts/60824dce48f7dd28fe7d65559f2da1f6e04e585b/ofl/barlowcondensed/BarlowCondensed-Bold.ttf'
       sha = 'e476562ec9c1e16cf16475895b511f08c804f438cc9a9f80a44ea50a0eeb5b65' },
    # 2026-11-12, menu "SETLIST" (docs/diseno_menu.md): Dela Gothic One, Chakra Petch, IBM Plex Mono (OFL 1.1), mismo commit.
    @{ name = 'Dela Gothic One'; file = 'DelaGothicOne-Regular.ttf'; font = $true
       url = 'https://raw.githubusercontent.com/google/fonts/60824dce48f7dd28fe7d65559f2da1f6e04e585b/ofl/delagothicone/DelaGothicOne-Regular.ttf'
       sha = '4ff87a0965f1b0505e5a2c58424bc6ad3cff27e56a82f21c2fc9d6b0e3857ee2' },
    @{ name = 'Chakra Petch Medium'; file = 'ChakraPetch-Medium.ttf'; font = $true
       url = 'https://raw.githubusercontent.com/google/fonts/60824dce48f7dd28fe7d65559f2da1f6e04e585b/ofl/chakrapetch/ChakraPetch-Medium.ttf'
       sha = 'd480f4f97405fac3600652e2e14fd0b14339031c0af4da11582994878f04d919' },
    @{ name = 'Chakra Petch SemiBold'; file = 'ChakraPetch-SemiBold.ttf'; font = $true
       url = 'https://raw.githubusercontent.com/google/fonts/60824dce48f7dd28fe7d65559f2da1f6e04e585b/ofl/chakrapetch/ChakraPetch-SemiBold.ttf'
       sha = '45264de3204ddbd5fb3e14a2402acd5c630d16650ae5fc221d2c52da46a6734b' },
    @{ name = 'Chakra Petch Bold Italic'; file = 'ChakraPetch-BoldItalic.ttf'; font = $true
       url = 'https://raw.githubusercontent.com/google/fonts/60824dce48f7dd28fe7d65559f2da1f6e04e585b/ofl/chakrapetch/ChakraPetch-BoldItalic.ttf'
       sha = 'bf012ce1894ffcf7d32d3e6548b90df7c40f30161b5cc295eb54ff61c21fab19' },
    @{ name = 'IBM Plex Mono Medium'; file = 'IBMPlexMono-Medium.ttf'; font = $true
       url = 'https://raw.githubusercontent.com/google/fonts/60824dce48f7dd28fe7d65559f2da1f6e04e585b/ofl/ibmplexmono/IBMPlexMono-Medium.ttf'
       sha = 'a9b4c49bb299e05b5f6c481e7fb5e78943d2793249a0c8874ab574a2d1ea6755' }
)
$out = Join-Path $root 'work\rmlui'
$stamp = Join-Path $out 'version.txt'
$want = ($pkgs | ForEach-Object { $_.sha }) -join ','
if ((Test-Path $stamp) -and ((Get-Content $stamp -Raw).Trim() -eq $want)) {
    Write-Output "RmlUi, FreeType e Inter ya estan en $out"
    exit 0
}

$dl = Join-Path $root 'work\rmlui_dl'
if (Test-Path $dl) { Remove-Item -Recurse -Force $dl }
New-Item -ItemType Directory -Force $dl | Out-Null
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$ProgressPreference = 'SilentlyContinue'
foreach ($p in $pkgs) {
    $f = Join-Path $dl $p.file
    Invoke-WebRequest -Uri $p.url -OutFile $f
    $got = (Get-FileHash -Algorithm SHA256 $f).Hash.ToLower()
    if ($got -ne $p.sha) { throw "$($p.name): SHA-256 distinto: $got, se esperaba $($p.sha)" }
}

if (Test-Path $out) { Remove-Item -Recurse -Force $out }
New-Item -ItemType Directory -Force $out | Out-Null
Expand-Archive -Path (Join-Path $dl 'RmlUi-6.3.zip') -DestinationPath $out          # -> RmlUi-6.3\
tar -xzf (Join-Path $dl 'freetype-2.14.3.tar.gz') -C $out                          # -> freetype-2.14.3\
if ($LASTEXITCODE -ne 0) { throw "tar fallo con freetype: exit $LASTEXITCODE" }
$ix = Join-Path $dl 'inter'
Expand-Archive -Path (Join-Path $dl 'Inter-4.1.zip') -DestinationPath $ix
$fonts = Join-Path $out 'fonts'
New-Item -ItemType Directory -Force $fonts | Out-Null
foreach ($t in 'Inter-Regular.ttf', 'Inter-SemiBold.ttf') {
    Copy-Item (Join-Path $ix "extras\ttf\$t") $fonts
}
foreach ($p in ($pkgs | Where-Object { $_.font })) { Copy-Item (Join-Path $dl $p.file) $fonts }
Set-Content -Path $stamp -Value $want -Encoding ascii
Remove-Item -Recurse -Force $dl
Write-Output "Listo en ${out}: RmlUi-6.3, freetype-2.14.3, fonts\Inter-Regular.ttf, fonts\Inter-SemiBold.ttf, BarlowCondensed-*.ttf"
