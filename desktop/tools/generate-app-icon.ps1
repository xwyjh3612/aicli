param(
    [string]$OutputDirectory = (Join-Path $PSScriptRoot '..\assets')
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

function New-RoundedRectanglePath {
    param(
        [single]$X,
        [single]$Y,
        [single]$Width,
        [single]$Height,
        [single]$Radius
    )
    $diameter = $Radius * 2
    $path = [Drawing.Drawing2D.GraphicsPath]::new()
    $path.AddArc($X, $Y, $diameter, $diameter, 180, 90)
    $path.AddArc($X + $Width - $diameter, $Y, $diameter, $diameter, 270, 90)
    $path.AddArc($X + $Width - $diameter, $Y + $Height - $diameter, $diameter, $diameter, 0, 90)
    $path.AddArc($X, $Y + $Height - $diameter, $diameter, $diameter, 90, 90)
    $path.CloseFigure()
    return $path
}

function New-AppIconMaster {
    param([int]$Size = 1024)

    $bitmap = [Drawing.Bitmap]::new($Size, $Size, [Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $graphics = [Drawing.Graphics]::FromImage($bitmap)
    try {
        $graphics.Clear([Drawing.Color]::Transparent)
        $graphics.SmoothingMode = [Drawing.Drawing2D.SmoothingMode]::AntiAlias
        $graphics.CompositingQuality = [Drawing.Drawing2D.CompositingQuality]::HighQuality
        $graphics.InterpolationMode = [Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $graphics.PixelOffsetMode = [Drawing.Drawing2D.PixelOffsetMode]::HighQuality
        $scale = $Size / 256.0
        $graphics.ScaleTransform($scale, $scale)

        $surface = New-RoundedRectanglePath 22 22 212 212 56
        try {
            $surfaceBrush = [Drawing.Drawing2D.LinearGradientBrush]::new(
                [Drawing.RectangleF]::new(22, 22, 212, 212),
                [Drawing.Color]::FromArgb(255, 23, 35, 60),
                [Drawing.Color]::FromArgb(255, 23, 37, 84),
                45.0)
            try { $graphics.FillPath($surfaceBrush, $surface) } finally { $surfaceBrush.Dispose() }

            $graphics.SetClip($surface)
            try {
                $bluePath = [Drawing.Drawing2D.GraphicsPath]::new()
                $bluePath.AddEllipse(76, -30, 214, 214)
                $blueGlow = [Drawing.Drawing2D.PathGradientBrush]::new($bluePath)
                try {
                    $blueGlow.CenterColor = [Drawing.Color]::FromArgb(185, 59, 130, 246)
                    $blueGlow.SurroundColors = @([Drawing.Color]::FromArgb(0, 59, 130, 246))
                    $graphics.FillPath($blueGlow, $bluePath)
                } finally {
                    $blueGlow.Dispose()
                    $bluePath.Dispose()
                }

                $violetPath = [Drawing.Drawing2D.GraphicsPath]::new()
                $violetPath.AddEllipse(-48, 128, 190, 190)
                $violetGlow = [Drawing.Drawing2D.PathGradientBrush]::new($violetPath)
                try {
                    $violetGlow.CenterColor = [Drawing.Color]::FromArgb(125, 124, 58, 237)
                    $violetGlow.SurroundColors = @([Drawing.Color]::FromArgb(0, 124, 58, 237))
                    $graphics.FillPath($violetGlow, $violetPath)
                } finally {
                    $violetGlow.Dispose()
                    $violetPath.Dispose()
                }
            } finally {
                $graphics.ResetClip()
            }

            $border = [Drawing.Pen]::new([Drawing.Color]::FromArgb(148, 122, 168, 255), 3.0)
            try {
                $border.Alignment = [Drawing.Drawing2D.PenAlignment]::Inset
                $graphics.DrawPath($border, $surface)
            } finally { $border.Dispose() }
        } finally { $surface.Dispose() }

        $shadow = [Drawing.Pen]::new([Drawing.Color]::FromArgb(70, 0, 0, 0), 25.0)
        $chevron = [Drawing.Pen]::new([Drawing.Color]::FromArgb(255, 248, 250, 252), 23.0)
        try {
            foreach ($pen in @($shadow, $chevron)) {
                $pen.StartCap = [Drawing.Drawing2D.LineCap]::Round
                $pen.EndCap = [Drawing.Drawing2D.LineCap]::Round
                $pen.LineJoin = [Drawing.Drawing2D.LineJoin]::Round
            }
            $shadowPoints = @(
                [Drawing.PointF]::new(76, 81),
                [Drawing.PointF]::new(128, 131),
                [Drawing.PointF]::new(76, 181))
            $chevronPoints = @(
                [Drawing.PointF]::new(76, 78),
                [Drawing.PointF]::new(128, 128),
                [Drawing.PointF]::new(76, 178))
            $graphics.DrawLines($shadow, $shadowPoints)
            $graphics.DrawLines($chevron, $chevronPoints)
        } finally {
            $shadow.Dispose()
            $chevron.Dispose()
        }

        $cursorBrush = [Drawing.Drawing2D.LinearGradientBrush]::new(
            [Drawing.PointF]::new(141, 177),
            [Drawing.PointF]::new(187, 177),
            [Drawing.Color]::FromArgb(255, 103, 232, 249),
            [Drawing.Color]::FromArgb(255, 96, 165, 250))
        $cursor = [Drawing.Pen]::new($cursorBrush, 19.0)
        try {
            $cursor.StartCap = [Drawing.Drawing2D.LineCap]::Round
            $cursor.EndCap = [Drawing.Drawing2D.LineCap]::Round
            $graphics.DrawLine($cursor, 141, 177, 187, 177)
        } finally {
            $cursor.Dispose()
            $cursorBrush.Dispose()
        }

        $sparkPoints = @(
            [Drawing.PointF]::new(181, 61),
            [Drawing.PointF]::new(186, 72),
            [Drawing.PointF]::new(197, 77),
            [Drawing.PointF]::new(186, 82),
            [Drawing.PointF]::new(181, 93),
            [Drawing.PointF]::new(176, 82),
            [Drawing.PointF]::new(165, 77),
            [Drawing.PointF]::new(176, 72))
        $spark = [Drawing.SolidBrush]::new([Drawing.Color]::FromArgb(255, 196, 181, 253))
        try { $graphics.FillPolygon($spark, $sparkPoints) } finally { $spark.Dispose() }
    } finally {
        $graphics.Dispose()
    }
    return $bitmap
}

function Save-ScaledPng {
    param(
        [Drawing.Bitmap]$Master,
        [int]$Size,
        [string]$Path
    )
    $bitmap = [Drawing.Bitmap]::new($Size, $Size, [Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $graphics = [Drawing.Graphics]::FromImage($bitmap)
    try {
        $graphics.Clear([Drawing.Color]::Transparent)
        $graphics.CompositingMode = [Drawing.Drawing2D.CompositingMode]::SourceCopy
        $graphics.CompositingQuality = [Drawing.Drawing2D.CompositingQuality]::HighQuality
        $graphics.InterpolationMode = [Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $graphics.SmoothingMode = [Drawing.Drawing2D.SmoothingMode]::HighQuality
        $graphics.PixelOffsetMode = [Drawing.Drawing2D.PixelOffsetMode]::HighQuality
        $graphics.DrawImage($Master, [Drawing.Rectangle]::new(0, 0, $Size, $Size))
        $bitmap.Save($Path, [Drawing.Imaging.ImageFormat]::Png)
    } finally {
        $graphics.Dispose()
        $bitmap.Dispose()
    }
}

function Write-PngIcon {
    param(
        [string[]]$PngPaths,
        [int[]]$Sizes,
        [string]$OutputPath
    )
    $images = @()
    foreach ($path in $PngPaths) {
        $images += ,([IO.File]::ReadAllBytes($path))
    }
    $stream = [IO.File]::Create($OutputPath)
    $writer = [IO.BinaryWriter]::new($stream)
    try {
        $writer.Write([uint16]0)
        $writer.Write([uint16]1)
        $writer.Write([uint16]$images.Count)
        [uint32]$offset = 6 + 16 * $images.Count
        for ($index = 0; $index -lt $images.Count; $index++) {
            $size = $Sizes[$index]
            $writer.Write([byte]$(if ($size -ge 256) { 0 } else { $size }))
            $writer.Write([byte]$(if ($size -ge 256) { 0 } else { $size }))
            $writer.Write([byte]0)
            $writer.Write([byte]0)
            $writer.Write([uint16]1)
            $writer.Write([uint16]32)
            $writer.Write([uint32]$images[$index].Length)
            $writer.Write($offset)
            $offset += [uint32]$images[$index].Length
        }
        foreach ($image in $images) { $writer.Write($image) }
    } finally {
        $writer.Dispose()
        $stream.Dispose()
    }
}

$outputFull = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force -Path $outputFull | Out-Null
$sizes = @(16, 20, 24, 32, 40, 48, 64, 128, 256)
$master = New-AppIconMaster
try {
    $pngPaths = foreach ($size in $sizes) {
        $path = Join-Path $outputFull "aicli-logo-$size.png"
        Save-ScaledPng -Master $master -Size $size -Path $path
        $path
    }
    Copy-Item -LiteralPath (Join-Path $outputFull 'aicli-logo-256.png') `
              -Destination (Join-Path $outputFull 'aicli-logo.png') -Force
    Write-PngIcon -PngPaths $pngPaths -Sizes $sizes `
                  -OutputPath (Join-Path $outputFull 'aicli.ico')
} finally {
    $master.Dispose()
}

Write-Host "Generated AI CLI icon assets in $outputFull"
