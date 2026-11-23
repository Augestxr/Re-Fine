# win_burst.ps1 - rafaga de la ventana del juego para contar frames rotos (tools\video_glitch.py -Raw).
# Solo lee la ventana del proceso (PrintWindow): nunca el escritorio ni otras ventanas. No guarda imagenes:
# cada captura se reduce a 96x54 en gris y se anade a -Out (bajo work\, fuera de git).
# Uso:  powershell -ExecutionPolicy Bypass -File tools\win_burst.ps1 -At 46 -Secs 20 -Out work\n12\burst_a.raw
param(
    [string]$Name = 'ReFine',
    [Parameter(Mandatory)][int]$At,
    [int]$Secs = 20,
    [Parameter(Mandatory)][string]$Out
)
$ErrorActionPreference = 'Stop'
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System; using System.Drawing; using System.Drawing.Imaging; using System.IO; using System.Runtime.InteropServices; using System.Diagnostics;
public static class ReFineBurst {
    [DllImport("user32.dll")] static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
    [StructLayout(LayoutKind.Sequential)] struct RECT { public int L, T, R, B; }
    [DllImport("user32.dll")] static extern bool GetClientRect(IntPtr h, out RECT r);
    public static int Run(IntPtr h, double secs, string path) {
        const int W = 96, H = 54; int n = 0;
        RECT r; GetClientRect(h, out r);
        if (r.R <= 0 || r.B <= 0) return 0;
        using (var big = new Bitmap(r.R, r.B, PixelFormat.Format32bppRgb))
        using (var small = new Bitmap(W, H, PixelFormat.Format32bppRgb))
        using (var fs = new FileStream(path, FileMode.Create)) {
            var sw = Stopwatch.StartNew(); var row = new byte[W * H]; var px = new byte[W * H * 4];
            while (sw.Elapsed.TotalSeconds < secs) {
                using (var g = Graphics.FromImage(big)) { IntPtr dc = g.GetHdc(); PrintWindow(h, dc, 3); g.ReleaseHdc(dc); }
                using (var g2 = Graphics.FromImage(small)) { g2.InterpolationMode = System.Drawing.Drawing2D.InterpolationMode.Bilinear; g2.DrawImage(big, 0, 0, W, H); }
                var d = small.LockBits(new Rectangle(0, 0, W, H), ImageLockMode.ReadOnly, PixelFormat.Format32bppRgb);
                Marshal.Copy(d.Scan0, px, 0, px.Length); small.UnlockBits(d);
                for (int i = 0; i < W * H; i++) row[i] = (byte)((px[4 * i] + 2 * px[4 * i + 1] + px[4 * i + 2]) >> 2);
                fs.Write(row, 0, row.Length); n++;
            }
        }
        return n;
    }
}
'@
$deadline = (Get-Date).AddSeconds(60)
while (-not ($p = Get-Process $Name -ErrorAction SilentlyContinue | Select-Object -First 1)) {
    if ((Get-Date) -gt $deadline) { throw "no aparecio el proceso $Name" }
    Start-Sleep -Milliseconds 200
}
$ms = [int](($p.StartTime.AddSeconds($At) - (Get-Date)).TotalMilliseconds)
if ($ms -gt 0 -and $p.WaitForExit($ms)) { throw "$Name termino antes de $At s" }
$p.Refresh()
$n = [ReFineBurst]::Run($p.MainWindowHandle, $Secs, $Out)
Write-Host "rafaga: $n capturas en $Secs s ($([math]::Round($n / $Secs, 1)) por segundo) -> $Out"
