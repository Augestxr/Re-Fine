# fetch_vulkan.ps1 - N1.0 (docs/plan_nativo.md): lo que hace falta para compilar el backend Vulkan.
#
# Uso:  powershell -ExecutionPolicy Bypass -File tools\fetch_vulkan.ps1
#
# - Vulkan-Headers de Khronos (etiqueta vulkan-sdk-1.4.363.0): cabeceras, nada que ejecutar -> work\vulkan-headers\
# - DXC de Microsoft (release v1.9.2609, x64): HLSL -> SPIR-V para los programas del juego. El dxc.exe del Windows
#   SDK no trae SPIR-V ("SPIR-V CodeGen not available") -> work\dxc\ (con inc\dxcapi.h: el backend usa dxcompiler.dll)
# Comprueba el SHA-256 de cada zip antes de abrirlo y la firma de Microsoft de dxc.exe y dxcompiler.dll.
# Nada va a git. El driver (vulkan-1.dll) es del sistema y no se descarga.

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$ProgressPreference = 'SilentlyContinue'

function Get-Zip($name, $url, $sha256) {
    $dl = Join-Path $root "work\${name}_dl"
    if (Test-Path $dl) { Remove-Item -Recurse -Force $dl }
    New-Item -ItemType Directory -Force $dl | Out-Null
    $zip = Join-Path $dl "$name.zip"
    Invoke-WebRequest -Uri $url -OutFile $zip
    $got = (Get-FileHash -Algorithm SHA256 $zip).Hash.ToLower()
    if ($got -ne $sha256) { throw "${name}: SHA-256 distinto: $got, se esperaba $sha256" }
    $x = Join-Path $dl 'x'
    Expand-Archive -Path $zip -DestinationPath $x
    return $dl, $x
}

# --- Vulkan-Headers
$vhTag = 'vulkan-sdk-1.4.363.0'
$vhSha = 'f4be95220ff0ee0b1c620301ff4fe9ae48592d71f4c30f2d5c571351a153ae81'
$vhOut = Join-Path $root 'work\vulkan-headers'
$vhStamp = Join-Path $vhOut 'version.txt'
if ((Test-Path $vhStamp) -and ((Get-Content $vhStamp -Raw).Trim() -eq $vhSha)) {
    Write-Output "Vulkan-Headers ya estan en $vhOut ($vhTag)"
} else {
    $dl, $x = Get-Zip 'vulkan-headers' "https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/$vhTag.zip" $vhSha
    $top = (Get-ChildItem $x -Directory | Select-Object -First 1).FullName
    if (Test-Path $vhOut) { Remove-Item -Recurse -Force $vhOut }
    New-Item -ItemType Directory -Force $vhOut | Out-Null
    Copy-Item -Recurse (Join-Path $top 'include') $vhOut
    Copy-Item (Join-Path $top 'LICENSE.md') $vhOut
    Set-Content -Path $vhStamp -Value $vhSha -Encoding ascii
    Remove-Item -Recurse -Force $dl
    Write-Output "Vulkan-Headers $vhTag en $vhOut"
}

# --- DXC
$dxcVer = 'v1.9.2609'
$dxcZip = 'dxc_2026_09_29.zip'
$dxcSha = 'ad31b1fc8443175d204f77a611fdb3ef2ec42759bdc2f1167368de24a4a7e7f1'
$dxcOut = Join-Path $root 'work\dxc'
$dxcStamp = Join-Path $dxcOut 'version.txt'
if ((Test-Path $dxcStamp) -and ((Get-Content $dxcStamp -Raw).Trim() -eq $dxcSha) -and (Test-Path (Join-Path $dxcOut 'inc\dxcapi.h'))) {
    Write-Output "DXC ya esta en $dxcOut ($dxcVer)"
} else {
    $dl, $x = Get-Zip 'dxc' "https://github.com/microsoft/DirectXShaderCompiler/releases/download/$dxcVer/$dxcZip" $dxcSha
    foreach ($f in 'dxc.exe', 'dxcompiler.dll') {
        $sig = Get-AuthenticodeSignature (Join-Path $x "bin\x64\$f")
        if ($sig.Status -ne 'Valid' -or $sig.SignerCertificate.Subject -notmatch 'O=Microsoft Corporation') {
            throw "DXC: $f sin firma valida de Microsoft ($($sig.Status))"
        }
    }
    if (Test-Path $dxcOut) { Remove-Item -Recurse -Force $dxcOut }
    New-Item -ItemType Directory -Force $dxcOut | Out-Null
    foreach ($f in 'dxc.exe', 'dxcompiler.dll', 'dxil.dll') { Copy-Item (Join-Path $x "bin\x64\$f") $dxcOut }
    foreach ($f in 'LICENSE-MS.txt', 'LICENSE-LLVM.txt', 'LICENCE-MIT.txt') { Copy-Item (Join-Path $x $f) $dxcOut }
    New-Item -ItemType Directory -Force (Join-Path $dxcOut 'inc') | Out-Null   # dxcapi.h: the backend calls dxcompiler.dll in process
    foreach ($f in 'dxcapi.h', 'dxcerrors.h') { Copy-Item (Join-Path $x "inc\$f") (Join-Path $dxcOut 'inc') }
    Set-Content -Path $dxcStamp -Value $dxcSha -Encoding ascii
    Remove-Item -Recurse -Force $dl
    Write-Output "DXC $dxcVer en $dxcOut"
}

# --- Comprobacion: un shader de prueba a SPIR-V (empieza por la palabra magica 0x07230203)
$t = Join-Path $root 'work\dxc\spirv_check'
New-Item -ItemType Directory -Force $t | Out-Null
Set-Content -Path "$t\t.hlsl" -Value 'float4 main(float4 p : POSITION) : SV_Position { return p; }' -Encoding ascii
& (Join-Path $dxcOut 'dxc.exe') -T vs_6_0 -E main -spirv '-fspv-target-env=vulkan1.2' "$t\t.hlsl" -Fo "$t\t.spv" | Out-Null
$b = [IO.File]::ReadAllBytes("$t\t.spv")
if ($b.Length -lt 4 -or [BitConverter]::ToUInt32($b, 0) -ne 0x07230203) { throw 'DXC no genero SPIR-V' }
Write-Output "SPIR-V OK ($($b.Length) bytes)"
