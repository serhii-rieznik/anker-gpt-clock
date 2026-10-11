$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$payloads = @()
$sizes = @(16,24,32,48,64,128,256)
foreach ($size in $sizes) {
    $image = [Drawing.Bitmap]::new($size,$size)
    $g = [Drawing.Graphics]::FromImage($image)
    $g.SmoothingMode = 'AntiAlias'
    $g.PixelOffsetMode = 'HighQuality'
    $g.Clear([Drawing.Color]::Transparent)
    $g.ScaleTransform([single]($size/256),[single]($size/256))
    $shape = [Drawing.Drawing2D.GraphicsPath]::new()
    foreach ($arc in @(@(16,16,112,112,180,90),@(128,16,112,112,270,90),@(128,128,112,112,0,90),@(16,128,112,112,90,90))) {
        $shape.AddArc($arc[0],$arc[1],$arc[2],$arc[3],$arc[4],$arc[5])
    }
    $shape.CloseFigure()
    $surface = [Drawing.SolidBrush]::new([Drawing.Color]::FromArgb(46,58,85))
    $g.FillPath($surface,$shape)
    $ring = [Drawing.Pen]::new([Drawing.Color]::FromArgb(168,190,255),12)
    $g.DrawEllipse($ring,56,56,144,144)
    $marks = [Drawing.Pen]::new($ring.Color,9)
    $marks.StartCap = 'Round'; $marks.EndCap = 'Round'
    foreach ($line in @(@(128,64,128,74),@(192,128,182,128),@(128,192,128,182),@(64,128,74,128))) {
        $g.DrawLine($marks,$line[0],$line[1],$line[2],$line[3])
    }
    $hands = [Drawing.Pen]::new([Drawing.Color]::FromArgb(242,244,248),14)
    $hands.StartCap = 'Round'; $hands.EndCap = 'Round'; $hands.LineJoin = 'Round'
    $g.DrawLines($hands,[Drawing.Point[]]@([Drawing.Point]::new(92,99),[Drawing.Point]::new(128,128),[Drawing.Point]::new(164,92)))
    $stream = [IO.MemoryStream]::new()
    $image.Save($stream,[Drawing.Imaging.ImageFormat]::Png)
    $payloads += ,$stream.ToArray()
    $stream.Dispose(); $hands.Dispose(); $marks.Dispose(); $ring.Dispose(); $surface.Dispose(); $shape.Dispose(); $g.Dispose(); $image.Dispose()
}
$writer = [IO.BinaryWriter]::new([IO.File]::Create((Join-Path $PSScriptRoot '..\resources\clock.ico')))
$writer.Write([uint16]0); $writer.Write([uint16]1); $writer.Write([uint16]$sizes.Count)
$offset = 6 + 16*$sizes.Count
for ($i=0; $i -lt $sizes.Count; $i++) {
    $dimension = if ($sizes[$i] -eq 256) { 0 } else { $sizes[$i] }
    $writer.Write([byte]$dimension); $writer.Write([byte]$dimension); $writer.Write([uint16]0)
    $writer.Write([uint16]1); $writer.Write([uint16]32)
    $writer.Write([uint32]$payloads[$i].Length); $writer.Write([uint32]$offset)
    $offset += $payloads[$i].Length
}
foreach ($payload in $payloads) { $writer.Write([byte[]]$payload) }
$writer.Dispose()
