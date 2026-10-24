# Lists the PNGs in a folder that have any translucent texel (PS2 alpha below 120 of 128).
#   translucent.ps1 <dir>
# Those are decals, cutouts and glass, often drawn with additive or multiplied blending where
# even the color of "transparent" texels shows: AI upscaling changes that color, so they are left
# at their original resolution.
param([Parameter(Mandatory)][string]$Dir)
Add-Type -AssemblyName System.Drawing
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @"
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;

public static class Translucent {
  public static bool Check(string path) {
    using (var bmp = new Bitmap(path)) {
      var data = bmp.LockBits(new Rectangle(0, 0, bmp.Width, bmp.Height), ImageLockMode.ReadOnly,
                              PixelFormat.Format32bppArgb);
      var px = new int[bmp.Width * bmp.Height];
      Marshal.Copy(data.Scan0, px, 0, px.Length);
      bmp.UnlockBits(data);
      foreach (var p in px) {
        if (((p >> 24) & 0xff) < 120) return true;
      }
      return false;
    }
  }
}
"@
foreach ($file in Get-ChildItem -Path $Dir -Filter *.png) {
    if ([Translucent]::Check($file.FullName)) {
        $file.Name
    }
}
