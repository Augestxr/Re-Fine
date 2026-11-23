# refine_shots.ps1 - 4i.2: capturas de Re:Fine a los mismos segundos que tools\rpcs3_shots.ps1.
#
# Uso (con tools\run.ps1 -FsLog -Tag <Tag> ya lanzado):
#   powershell -ExecutionPolicy Bypass -File tools\refine_shots.ps1 -Tag k1 [-At "5,10,15,20,25,30,36"]
#
# Sigue work\run\<Tag>.err.log hasta la primera mencion de pv_600.ogg (t = 0, el ancla de RPCS3) y
# guarda work\shots\<Tag>_tNN.png con PrintWindow: pide la imagen a esa ventana, nunca a la pantalla.

param(
    [switch]$FullSize,   # captura a tamano completo (por defecto 960 px de ancho)
    [Parameter(Mandatory = $true)][string]$Tag,
    [string]$At = '5,10,15,20,25,30,36',
    [double]$Shift = 0,   # desfase de nuestra PV frente al ancla de RPCS3, en s
    [string]$Anchor = 'pv_600\.ogg',    # 4c.3: 'adv\.pam' para la pelicula (hace falta -FsLog)
    [int]$Timeout = 300,
    [string]$Wav = ''     # 4j.1: el AUDIO_WAV de la corrida; apunta su tamano en el ancla
)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
Add-Type -AssemblyName System.Drawing
Add-Type -Namespace ReFine -Name Shot2 -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
[DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
[StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
'@

$deadline = (Get-Date).AddSeconds($Timeout)
$log = Join-Path $root "work\run\$Tag.err.log"
while (-not (Test-Path $log)) { Start-Sleep -Milliseconds 200; if ((Get-Date) -gt $deadline) { throw "sin $log" } }
$fs = New-Object IO.FileStream($log, 'Open', 'Read', 'ReadWrite,Delete')
$sr = New-Object IO.StreamReader($fs)
$t0 = $null
while (-not $t0) {
    if ((Get-Date) -gt $deadline) { throw "No llego a $Anchor" }
    $l = $sr.ReadLine()
    if ($null -eq $l) { Start-Sleep -Milliseconds 50; continue }
    if ($l -match $Anchor) { $t0 = Get-Date }
}
$sr.Close()
if ($Wav) {
    # El tamano por el handle abierto: el del directorio va atrasado mientras el juego escribe.
    $wf = New-Object IO.FileStream($Wav, 'Open', 'Read', 'ReadWrite,Delete')
    Write-Host "wav en el ancla: $($wf.Length) bytes"; $wf.Close()
}
$p = Get-Process ReFine | Select-Object -First 1
$h = $p.MainWindowHandle
Write-Host "ancla $Anchor a $([int]($t0 - $p.StartTime).TotalSeconds) s del proceso"
foreach ($s in ($At -split ',' | ForEach-Object { [double]$_ })) {
    $ms = [int](($t0.AddSeconds($s + $Shift) - (Get-Date)).TotalMilliseconds)
    if ($ms -gt 0) { Start-Sleep -Milliseconds $ms }
    $r = New-Object ReFine.Shot2+RECT; $null = [ReFine.Shot2]::GetClientRect($h, [ref]$r)
    $bmp = New-Object Drawing.Bitmap $r.R, $r.B
    $g = [Drawing.Graphics]::FromImage($bmp); $dc = $g.GetHdc()
    $ok = [ReFine.Shot2]::PrintWindow($h, $dc, 3)
    $g.ReleaseHdc($dc); $g.Dispose()
    $name = if ($s -eq [math]::Floor($s)) { '{0}_t{1:00}.png' -f $Tag, [int]$s } else { '{0}_t{1:00.0}.png' -f $Tag, $s }
    $png = Join-Path $root "work\shots\$name"
    if (-not $FullSize -and $bmp.Width -gt 960) {   # por defecto 960 px de ancho (ahorro de tokens); -FullSize para el tamano real
        $h2 = [int][math]::Round($bmp.Height * 960 / $bmp.Width); $sm = New-Object Drawing.Bitmap 960, $h2
        $g2 = [Drawing.Graphics]::FromImage($sm); $g2.InterpolationMode = 'HighQualityBicubic'; $g2.DrawImage($bmp, 0, 0, 960, $h2); $g2.Dispose()
        $bmp.Dispose(); $bmp = $sm }
    $bmp.Save($png, [Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
    Write-Host ("+{0,4:0.0} s {1} ok={2}" -f ((Get-Date) - $t0).TotalSeconds, (Split-Path $png -Leaf), $ok)
}
