# window_shot.ps1 - Guarda PNG de la ventana de un proceso, sin mirar la pantalla.
#
# Uso:  powershell -ExecutionPolicy Bypass -File tools\window_shot.ps1 -Name rpcs3 -At "125,135" -Out work\shots\rpcs3
#
# Espera a que arranque el proceso -Name y, a los segundos -At desde su inicio, guarda
# <Out>_<s>.png. Usa PrintWindow con PW_RENDERFULLCONTENT: pide la imagen a esa ventana, asi
# que no hace falta traerla al frente y nunca captura lo que el usuario tenga en pantalla
# (CopyFromScreen si lo hace: no usarlo).

param(
    [switch]$FullSize,   # captura a tamano completo (por defecto 960 px de ancho)
    [Parameter(Mandatory = $true)][string]$Name,
    [Parameter(Mandatory = $true)][string]$At,
    [Parameter(Mandatory = $true)][string]$Out,
    [string]$Client = ''   # 6b+6c: "2560x1440" pone el area cliente de la ventana a ese tamano antes de capturar
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type -Namespace ReFine -Name Shot -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
[DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
[StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
[DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
[DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int w, int ht, uint f);
'@

$deadline = (Get-Date).AddSeconds(60)
while (-not ($p = Get-Process $Name -ErrorAction SilentlyContinue | Select-Object -First 1)) {
    if ((Get-Date) -gt $deadline) { throw "no aparecio el proceso $Name" }
    Start-Sleep -Milliseconds 200
}
$t0 = $p.StartTime
if ($Client) {
    $cw, $ch = $Client -split 'x' | ForEach-Object { [int]$_ }
    while (($p.Refresh()) -or $p.MainWindowHandle -eq [IntPtr]::Zero) { Start-Sleep -Milliseconds 200 }
    Start-Sleep -Seconds 3
    $h = $p.MainWindowHandle
    $w = New-Object ReFine.Shot+RECT; $c = New-Object ReFine.Shot+RECT
    $null = [ReFine.Shot]::GetWindowRect($h, [ref]$w); $null = [ReFine.Shot]::GetClientRect($h, [ref]$c)
    $fw = ($w.R - $w.L) - $c.R; $fh = ($w.B - $w.T) - $c.B
    $null = [ReFine.Shot]::SetWindowPos($h, [IntPtr]::Zero, 0, 0, $cw + $fw, $ch + $fh, 0x0014)   # NOZORDER | NOACTIVATE
    Write-Host "ventana a ${cw}x${ch}"
}
foreach ($s in ($At -split ',' | ForEach-Object { [int]$_ } | Sort-Object)) {
    $ms = [int](($t0.AddSeconds($s) - (Get-Date)).TotalMilliseconds)
    if ($ms -gt 0 -and $p.WaitForExit($ms)) { Write-Host "$Name termino antes de $s s"; break }
    $p.Refresh(); $h = $p.MainWindowHandle
    if ($h -eq [IntPtr]::Zero) { Write-Host "$Name sin ventana a los $s s"; continue }
    $r = New-Object ReFine.Shot+RECT; $null = [ReFine.Shot]::GetClientRect($h, [ref]$r)
    $bmp = New-Object Drawing.Bitmap $r.R, $r.B
    $g = [Drawing.Graphics]::FromImage($bmp); $dc = $g.GetHdc()
    $ok = [ReFine.Shot]::PrintWindow($h, $dc, 3)   # PW_CLIENTONLY | PW_RENDERFULLCONTENT
    $g.ReleaseHdc($dc); $g.Dispose()
    $png = "${Out}_$s.png"
    if (-not $FullSize -and $bmp.Width -gt 960) {   # por defecto 960 px de ancho (ahorro de tokens); -FullSize para el tamano real
        $h2 = [int][math]::Round($bmp.Height * 960 / $bmp.Width); $sm = New-Object Drawing.Bitmap 960, $h2
        $g2 = [Drawing.Graphics]::FromImage($sm); $g2.InterpolationMode = 'HighQualityBicubic'; $g2.DrawImage($bmp, 0, 0, 960, $h2); $g2.Dispose()
        $bmp.Dispose(); $bmp = $sm }
    $bmp.Save($png, [Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
    Write-Host "captura $png ($($r.R)x$($r.B)) PrintWindow=$ok"
}
