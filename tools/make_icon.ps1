# make_icon.ps1 - build res/syndicate64.ico (PNG images, 16 to 256 pixels) and res/syndicate64-256.png from the
# artwork res/syndicate-icon.png, with high-quality bicubic downscaling.
#
#   powershell -ExecutionPolicy Bypass -File tools/make_icon.ps1
Add-Type -AssemblyName System.Drawing
$root = Split-Path -Parent $PSScriptRoot
$src = [System.Drawing.Image]::FromFile((Join-Path $root 'res\syndicate-icon.png'))
$sizes = 16, 20, 24, 32, 40, 48, 64, 96, 128, 256
$images = @()
foreach ($s in $sizes) {
    $bmp = New-Object System.Drawing.Bitmap $s, $s, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
    $g.CompositingQuality = [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
    $attr = New-Object System.Drawing.Imaging.ImageAttributes
    $attr.SetWrapMode([System.Drawing.Drawing2D.WrapMode]::TileFlipXY) # no dark fringe at the edges
    $g.DrawImage($src, (New-Object System.Drawing.Rectangle 0, 0, $s, $s), 0, 0, $src.Width, $src.Height,
        [System.Drawing.GraphicsUnit]::Pixel, $attr)
    $g.Dispose()
    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    if ($s -eq 256) { $bmp.Save((Join-Path $root 'res\syndicate64-256.png'), [System.Drawing.Imaging.ImageFormat]::Png) }
    $bmp.Dispose()
    $images += , $ms.ToArray()
}
$src.Dispose()

# ICONDIR, one ICONDIRENTRY per image, then the PNG data
$out = New-Object System.IO.MemoryStream
$w = New-Object System.IO.BinaryWriter $out
$w.Write([uint16]0); $w.Write([uint16]1); $w.Write([uint16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $s = $sizes[$i]; $d = $images[$i]
    $w.Write([byte]($s % 256)); $w.Write([byte]($s % 256)); $w.Write([byte]0); $w.Write([byte]0)
    $w.Write([uint16]1); $w.Write([uint16]32); $w.Write([uint32]$d.Length); $w.Write([uint32]$offset)
    $offset += $d.Length
}
foreach ($d in $images) { $w.Write($d) }
$w.Flush()
[System.IO.File]::WriteAllBytes((Join-Path $root 'res\syndicate64.ico'), $out.ToArray())
"wrote res\syndicate64.ico ($($out.Length) bytes, $($sizes.Count) sizes)"
