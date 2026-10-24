# Put the original alpha back on AI-upscaled textures.
#   alpha_restore.ps1 <original dir> <scaled dir>
# For each <name>.png in both folders, the scaled picture keeps its colors and gets the original
# alpha resized smoothly (bicubic) to its size: the upscaler doesn't invent or move transparency,
# so cutouts and decals blend exactly as in the game.
param(
    [Parameter(Mandatory)][string]$Original,
    [Parameter(Mandatory)][string]$Scaled
)
Add-Type -AssemblyName System.Drawing
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @"
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;

public static class AlphaRestore {
  static int[] Pixels(Bitmap bmp) {
    var data = bmp.LockBits(new Rectangle(0, 0, bmp.Width, bmp.Height), ImageLockMode.ReadOnly,
                            PixelFormat.Format32bppArgb);
    var px = new int[bmp.Width * bmp.Height];
    Marshal.Copy(data.Scan0, px, 0, px.Length);
    bmp.UnlockBits(data);
    return px;
  }

  public static void Run(string orig, string scaled) {
    int w, h;
    int[] colors;
    using (var s = new Bitmap(scaled)) { w = s.Width; h = s.Height; colors = Pixels(s); }
    int[] alpha;
    using (var o = new Bitmap(orig)) {
      // the alpha as a grey picture, resized
      var grey = new Bitmap(o.Width, o.Height, PixelFormat.Format32bppArgb);
      var opx = Pixels(o);
      for (int i = 0; i < opx.Length; i++) {
        int a = (opx[i] >> 24) & 0xff;
        opx[i] = unchecked((int)0xff000000) | (a << 16) | (a << 8) | a;
      }
      var gd = grey.LockBits(new Rectangle(0, 0, o.Width, o.Height), ImageLockMode.WriteOnly,
                             PixelFormat.Format32bppArgb);
      Marshal.Copy(opx, 0, gd.Scan0, opx.Length);
      grey.UnlockBits(gd);
      using (var big = new Bitmap(w, h, PixelFormat.Format32bppArgb))
      using (var g = Graphics.FromImage(big)) {
        g.InterpolationMode = InterpolationMode.HighQualityBicubic;
        g.PixelOffsetMode = PixelOffsetMode.HighQuality;
        using (var wrap = new ImageAttributes()) {
          wrap.SetWrapMode(WrapMode.Tile);
          g.DrawImage(grey, new Rectangle(0, 0, w, h), 0, 0, o.Width, o.Height, GraphicsUnit.Pixel, wrap);
        }
        alpha = Pixels(big);
      }
      grey.Dispose();
    }
    for (int i = 0; i < colors.Length; i++) {
      colors[i] = (colors[i] & 0xffffff) | ((alpha[i] & 0xff) << 24);
    }
    using (var outBmp = new Bitmap(w, h, PixelFormat.Format32bppArgb)) {
      var od = outBmp.LockBits(new Rectangle(0, 0, w, h), ImageLockMode.WriteOnly, PixelFormat.Format32bppArgb);
      Marshal.Copy(colors, 0, od.Scan0, colors.Length);
      outBmp.UnlockBits(od);
      outBmp.Save(scaled + ".tmp", ImageFormat.Png);
    }
    System.IO.File.Delete(scaled);
    System.IO.File.Move(scaled + ".tmp", scaled);
  }
}
"@
foreach ($file in Get-ChildItem -Path $Original -Filter *.png) {
    $target = Join-Path $Scaled $file.Name
    if (Test-Path $target) {
        [AlphaRestore]::Run($file.FullName, $target)
    }
}
