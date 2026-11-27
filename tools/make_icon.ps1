# Generates the XR Window Panels icon (multi-size .ico + preview PNGs) with System.Drawing.
#   powershell -ExecutionPolicy Bypass -File tools\make_icon.ps1 [-OutDir assets]
# Design: dark rounded square, a panel floating in perspective with an amber title bar and
# throttle (green) / brake (red) traces - a window placed inside VR.
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
function Get-QuadPoint($q, [float]$u, [float]$v) {
  $top = @(($q[0][0] + ($q[1][0] - $q[0][0]) * $u), ($q[0][1] + ($q[1][1] - $q[0][1]) * $u))
  $bot = @(($q[3][0] + ($q[2][0] - $q[3][0]) * $u), ($q[3][1] + ($q[2][1] - $q[3][1]) * $u))
  return New-Object Drawing.PointF(($top[0] + ($bot[0] - $top[0]) * $v), ($top[1] + ($bot[1] - $top[1]) * $v))
}

function Get-QuadPolygon($q, [float]$v0, [float]$v1, [float]$s) {
  $pts = @((Get-QuadPoint $q 0 $v0), (Get-QuadPoint $q 1 $v0), (Get-QuadPoint $q 1 $v1), (Get-QuadPoint $q 0 $v1))
  return [Drawing.PointF[]]($pts | ForEach-Object { New-Object Drawing.PointF(($_.X * $s), ($_.Y * $s)) })
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

  # Floating panel (left edge nearer = taller), slightly raised
  $q = @(@(0.16, 0.20), @(0.86, 0.29), @(0.86, 0.71), @(0.16, 0.80))
  $shadow = Get-QuadPolygon $q 0 1 $s
  $off = [Math]::Max(1, $s * 0.035)
  $shadow = [Drawing.PointF[]]($shadow | ForEach-Object { New-Object Drawing.PointF(($_.X + $off * 0.4), ($_.Y + $off)) })
  $g.FillPolygon((New-Object Drawing.SolidBrush([Drawing.Color]::FromArgb(110, 0, 0, 0))), $shadow)
  $g.FillPolygon((New-Object Drawing.SolidBrush([Drawing.Color]::FromArgb(255, 238, 242, 247))), (Get-QuadPolygon $q 0 1 $s))

  # Amber title bar (same amber as the in-VR selection border)
  $barV = if ($size -le 20) { 0.30 } else { 0.22 }
  $g.FillPolygon((New-Object Drawing.SolidBrush([Drawing.Color]::FromArgb(255, 255, 183, 0))), (Get-QuadPolygon $q 0 $barV $s))

  # Throttle / brake traces (only where they stay legible)
  if ($size -ge 32) {
    $w = [Math]::Max(1.2, $s * 0.045)
    # Full throttle -> lift + brake spike with trail-braking release -> back on throttle (x2)
    $throttle = @(@(0.06, 0.40), @(0.22, 0.40), @(0.26, 0.88), @(0.40, 0.88), @(0.50, 0.40), @(0.64, 0.40), @(0.68, 0.88), @(0.78, 0.88), @(0.88, 0.40), @(0.94, 0.40))
    $brake    = @(@(0.06, 0.92), @(0.23, 0.92), @(0.26, 0.46), @(0.38, 0.92), @(0.66, 0.92), @(0.69, 0.56), @(0.78, 0.92), @(0.94, 0.92))
    foreach ($line in @(@($throttle, [Drawing.Color]::FromArgb(255, 34, 197, 94)), @($brake, [Drawing.Color]::FromArgb(255, 239, 68, 68)))) {
      $pts = [Drawing.PointF[]]($line[0] | ForEach-Object {
        $v = $barV + 0.08 + ($_[1] - 0.3) * (1 - $barV - 0.08) / 0.7
        $p = Get-QuadPoint $q $_[0] ([Math]::Min(0.95, $v))
        New-Object Drawing.PointF(($p.X * $s), ($p.Y * $s))
      })
      $pen = New-Object Drawing.Pen($line[1], $w)
      $pen.LineJoin = 'Round'; $pen.StartCap = 'Round'; $pen.EndCap = 'Round'
      $g.DrawLines($pen, $pts)
    }
  }
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
