# Generates the XR Window Panels icon (multi-size .ico + preview PNGs) with System.Drawing.
#   powershell -ExecutionPolicy Bypass -File tools\make_icon.ps1 [-OutDir assets]
# Design: dark rounded square with two generic windows floating in perspective (one behind the
# other) - any desktop window placed in VR space. Amber title bar = the in-VR selection color.
param([string]$OutDir = (Join-Path (Split-Path $PSScriptRoot) 'assets'))

Add-Type -AssemblyName System.Drawing
New-Item -ItemType Directory -Force $OutDir | Out-Null

function New-RoundRect([float]$x, [float]$y, [float]$w, [float]$h, [float]$r) {
  $p = New-Object Drawing.Drawing2D.GraphicsPath
  $d = 2 * $r
  $p.AddArc($x, $y, $d, $d, 180, 90)
  $p.AddArc($x + $w - $d, $y, $d, $d, 270, 90)
  $p.AddArc($x + $w - $d, $y + $h - $d, $d, $d, 0, 90)
  $p.AddArc($x, $y + $h - $d, $d, $d, 90, 90)
  $p.CloseFigure()
  return $p
}

# Bilinear point inside the quad TL,TR,BR,BL for (u,v) in 0..1 (gives the perspective look).
function Get-QuadPoint($q, [float]$u, [float]$v, [float]$s) {
  $tx = $q[0][0] + ($q[1][0] - $q[0][0]) * $u; $ty = $q[0][1] + ($q[1][1] - $q[0][1]) * $u
  $bx = $q[3][0] + ($q[2][0] - $q[3][0]) * $u; $by = $q[3][1] + ($q[2][1] - $q[3][1]) * $u
  return New-Object Drawing.PointF((($tx + ($bx - $tx) * $v) * $s), (($ty + ($by - $ty) * $v) * $s))
}

# Sub-rectangle (u0..u1, v0..v1) of the quad, as a polygon in pixels.
function Get-Region($q, [float]$u0, [float]$u1, [float]$v0, [float]$v1, [float]$s) {
  return [Drawing.PointF[]]@((Get-QuadPoint $q $u0 $v0 $s), (Get-QuadPoint $q $u1 $v0 $s),
                             (Get-QuadPoint $q $u1 $v1 $s), (Get-QuadPoint $q $u0 $v1 $s))
}

function Fill($g, [int]$a, [int]$r, [int]$gr, [int]$b, $poly) {
  $brush = New-Object Drawing.SolidBrush([Drawing.Color]::FromArgb($a, $r, $gr, $b))
  $g.FillPolygon($brush, $poly)
  $brush.Dispose()
}

function Draw-Window($g, $q, [float]$s, [int]$size, [bool]$front) {
  $barV = if ($size -le 24) { 0.30 } else { 0.20 }
  # Drop shadow
  $off = [Math]::Max(1, $s * 0.03)
  $shadow = [Drawing.PointF[]]((Get-Region $q 0 1 0 1 $s) | ForEach-Object { New-Object Drawing.PointF(($_.X + $off * 0.5), ($_.Y + $off)) })
  Fill $g ($(if ($front) { 120 } else { 70 })) 0 0 0 $shadow

  if ($front) {
    Fill $g 255 238 242 247 (Get-Region $q 0 1 0 1 $s)       # body
    Fill $g 255 255 183 0 (Get-Region $q 0 1 0 $barV $s)     # amber title bar
    if ($size -ge 32) {
      # Generic content: an image block and text lines
      Fill $g 255 59 130 246 (Get-Region $q 0.08 0.42 ($barV + 0.12) 0.86 $s)
      foreach ($line in @(@(0.30, 0.92), @(0.46, 0.84), @(0.62, 0.92), @(0.78, 0.70))) {
        Fill $g 255 148 160 178 (Get-Region $q 0.50 $line[1] ($line[0] + $barV * 0.4) ($line[0] + $barV * 0.4 + 0.07) $s)
      }
    }
  } else {
    Fill $g 150 205 214 228 (Get-Region $q 0 1 0 1 $s)       # translucent body
    Fill $g 170 120 135 160 (Get-Region $q 0 1 0 $barV $s)   # muted title bar
  }
}

function New-IconBitmap([int]$size) {
  $s = [float]$size
  $bmp = New-Object Drawing.Bitmap $size, $size, ([Drawing.Imaging.PixelFormat]::Format32bppArgb)
  $g = [Drawing.Graphics]::FromImage($bmp)
  $g.SmoothingMode = 'AntiAlias'
  $g.PixelOffsetMode = 'HighQuality'
  $g.Clear([Drawing.Color]::Transparent)

  # Background
  $m = [Math]::Max(0.5, $s * 0.03)
  $bg = New-RoundRect $m $m ($s - 2 * $m) ($s - 2 * $m) ($s * 0.21)
  $grad = New-Object Drawing.Drawing2D.LinearGradientBrush((New-Object Drawing.PointF(0, 0)), (New-Object Drawing.PointF($s, $s)),
    [Drawing.Color]::FromArgb(255, 37, 58, 99), [Drawing.Color]::FromArgb(255, 12, 18, 34))
  $g.FillPath($grad, $bg)

  # Two windows in perspective (left edges nearer = taller), back one up-right
  $back  = @(@(0.40, 0.14), @(0.88, 0.20), @(0.88, 0.54), @(0.40, 0.60))
  $front = @(@(0.12, 0.32), @(0.70, 0.39), @(0.70, 0.80), @(0.12, 0.87))
  Draw-Window $g $back $s $size $false
  Draw-Window $g $front $s $size $true

  $g.Dispose()
  return $bmp
}

function Get-PngBytes($bmp) {
  $ms = New-Object IO.MemoryStream
  $bmp.Save($ms, [Drawing.Imaging.ImageFormat]::Png)
  return , $ms.ToArray()
}

# Multi-size .ico (PNG-compressed entries, supported since Windows Vista)
$sizes = 16, 20, 24, 32, 40, 48, 64, 128, 256
$images = foreach ($sz in $sizes) { , (Get-PngBytes (New-IconBitmap $sz)) }
$ico = New-Object IO.MemoryStream
$bw = New-Object IO.BinaryWriter($ico)
$bw.Write([uint16]0); $bw.Write([uint16]1); $bw.Write([uint16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
  $sz = $sizes[$i]
  $bw.Write([byte]($sz % 256)); $bw.Write([byte]($sz % 256))  # 256 is stored as 0
  $bw.Write([byte]0); $bw.Write([byte]0)
  $bw.Write([uint16]1); $bw.Write([uint16]32)
  $bw.Write([uint32]$images[$i].Length); $bw.Write([uint32]$offset)
  $offset += $images[$i].Length
}
foreach ($img in $images) { $bw.Write($img) }
$bw.Flush()
[IO.File]::WriteAllBytes((Join-Path $OutDir 'icon.ico'), $ico.ToArray())

# Previews
(New-IconBitmap 256).Save((Join-Path $OutDir 'icon-256.png'), [Drawing.Imaging.ImageFormat]::Png)
$sheet = New-Object Drawing.Bitmap 520, 280
$gs = [Drawing.Graphics]::FromImage($sheet)
$gs.Clear([Drawing.Color]::FromArgb(255, 240, 240, 240))
$gs.FillRectangle((New-Object Drawing.SolidBrush([Drawing.Color]::FromArgb(255, 32, 32, 32))), 0, 140, 520, 140)
$x = 8
foreach ($sz in 16, 24, 32, 48, 64, 128) {
  $b = New-IconBitmap $sz
  $gs.DrawImage($b, $x, [int](70 - $sz / 2), $sz, $sz)
  $gs.DrawImage($b, $x, [int](210 - $sz / 2), $sz, $sz)
  $x += $sz + 18
}
$gs.Dispose()
$sheet.Save((Join-Path $OutDir 'icon-sizes.png'), [Drawing.Imaging.ImageFormat]::Png)
Write-Host "Icon written to $OutDir"
