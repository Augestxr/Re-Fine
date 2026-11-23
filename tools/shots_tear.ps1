# shots_tear.ps1 - 4c.3: busca un corte horizontal en capturas.
#
# Uso:  powershell -ExecutionPolicy Bypass -File tools\shots_tear.ps1 -Glob 'work\shots\g1_t*.png'
#
# Por cada PNG, la diferencia media entre cada fila y la siguiente. Un corte entre dos fotogramas sale
# como una sola fila mucho mayor que sus vecinas: se da la fila y su cociente frente a ellas.

param([Parameter(Mandatory = $true)][string]$Glob)
Add-Type -AssemblyName System.Drawing
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System; using System.Drawing; using System.Drawing.Imaging; using System.Runtime.InteropServices;
public static class Tear {
  public static double[] Rows(Bitmap b) {
    var d = b.LockBits(new Rectangle(0,0,b.Width,b.Height), ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
    var a = new byte[d.Stride*b.Height]; Marshal.Copy(d.Scan0,a,0,a.Length); b.UnlockBits(d);
    var r = new double[b.Height-1];
    for (int y=0;y<b.Height-1;y++) {
      int p=y*d.Stride, q=p+d.Stride; long s=0;
      for (int x=0;x<b.Width*4;x+=4) s+=Math.Abs(a[p+x]-a[q+x])+Math.Abs(a[p+x+1]-a[q+x+1])+Math.Abs(a[p+x+2]-a[q+x+2]);
      r[y]=s/(double)b.Width;
    }
    return r;
  }
}
'@
Get-ChildItem $Glob | ForEach-Object {
    $b = New-Object Drawing.Bitmap $_.FullName
    $r = [Tear]::Rows($b); $b.Dispose()
    $best = 1; $bv = 0.0
    for ($y = 2; $y -lt $r.Length - 2; $y++) {
        $v = $r[$y] / [math]::Max(($r[$y - 2] + $r[$y + 2]) / 2, 0.5)
        if ($v -gt $bv) { $bv = $v; $best = $y }
    }
    '{0}  fila {1,3}  cociente {2,6:N1}{3}' -f $_.Name, $best, $bv, $(if ($bv -gt 6) { '  CORTE' } else { '' })
}
