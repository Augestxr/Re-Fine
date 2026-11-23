# elegir_carpeta.ps1 - primer arranque: pide la carpeta del juego y la guarda en ruta_juego.txt.
# Sale con 0 si queda una carpeta valida, con 1 si el usuario cancela.
param([string]$Ruta)

Add-Type -AssemblyName System.Windows.Forms
$owner = New-Object Windows.Forms.Form -Property @{ TopMost = $true }
function Aviso([string]$texto, $icono = 'Information') {
    [void][Windows.Forms.MessageBox]::Show($owner, $texto, 'Re:Fine', 'OK', $icono)
}

# SHA-256 del EBOOT.elf de BLUS31319 (USA), APP_VER 01.00: el unico con el que Re:Fine funciona
$hash = 'eba75a0503788b8a9e62545f8b810152d0a6adb0fb4ac488477b79280e0d8dbe'

$dlg = New-Object Windows.Forms.FolderBrowserDialog
$dlg.Description = 'Elige la carpeta de tu copia del juego: la que contiene PS3_GAME'
$dlg.ShowNewFolderButton = $false
while ($true) {
    if ($dlg.ShowDialog($owner) -ne 'OK') { exit 1 }
    $game = $dlg.SelectedPath.TrimEnd('\')
    if ((Split-Path $game -Leaf) -eq 'PS3_GAME') { $game = Split-Path $game -Parent }
    $elf = "$game\PS3_GAME\USRDIR\EBOOT.elf"
    if (Test-Path -LiteralPath $elf) { break }
    if (Test-Path -LiteralPath "$game\PS3_GAME\USRDIR\EBOOT.BIN") {
        Aviso "Falta EBOOT.elf en PS3_GAME\USRDIR. El disco trae EBOOT.BIN, que está cifrado: descífralo con RPCS3 como explica el LEEME." 'Warning'
    } else {
        Aviso "Esa carpeta no contiene PS3_GAME\USRDIR. Elige la carpeta que contiene PS3_GAME." 'Warning'
    }
}
if ((Get-FileHash -LiteralPath $elf -Algorithm SHA256).Hash -ne $hash) {
    Aviso "Este EBOOT.elf no es el de BLUS31319 (USA) 01.00. Re:Fine solo está hecho para esa versión, sin actualizaciones: lo más probable es que falle." 'Warning'
}
New-Item -ItemType Directory -Force (Split-Path $Ruta -Parent) | Out-Null
[IO.File]::WriteAllText($Ruta, $game)
exit 0
