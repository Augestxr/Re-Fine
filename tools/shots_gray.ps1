# shots_gray.ps1 - 4j.1: cada PNG a una fila "nombre mtime_s v1 v2 ..." (48x27, gris), para tools/pv_align.py.
param([Parameter(Mandatory = $true)][string]$Glob, [Parameter(Mandatory = $true)][string]$Out)
Add-Type -AssemblyName System.Drawing
$epoch = Get-Date '2026-01-01'
Get-ChildItem $Glob | ForEach-Object {
    $i = [Drawing.Image]::FromFile($_.FullName)
    $b = New-Object Drawing.Bitmap 48, 27
    $g = [Drawing.Graphics]::FromImage($b); $g.DrawImage($i, 0, 0, 48, 27); $g.Dispose(); $i.Dispose()
    $v = New-Object Collections.ArrayList
    for ($y = 0; $y -lt 27; $y++) { for ($x = 0; $x -lt 48; $x++) {
        $c = $b.GetPixel($x, $y); [void]$v.Add([int](0.3 * $c.R + 0.59 * $c.G + 0.11 * $c.B)) } }
    $b.Dispose()
    "$($_.Name) $(($_.LastWriteTime - $epoch).TotalSeconds) $($v -join ' ')"
} | Set-Content $Out -Encoding ascii
