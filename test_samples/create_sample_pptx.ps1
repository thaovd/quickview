Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.IO.Compression.FileSystem

$tmp = Join-Path $env:TEMP "pptx_build"
if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
$null = New-Item -ItemType Directory -Path "$tmp\docProps" -Force
$null = New-Item -ItemType Directory -Path "$tmp\ppt\slides" -Force

$bmp = New-Object System.Drawing.Bitmap 1280, 720
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
$g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAlias

$brush = New-Object System.Drawing.Drawing2D.LinearGradientBrush (New-Object System.Drawing.Point 0,0), (New-Object System.Drawing.Point 1280,720), ([System.Drawing.Color]::FromArgb(18, 24, 38)), ([System.Drawing.Color]::FromArgb(35, 45, 75))
$g.FillRectangle($brush, 0, 0, 1280, 720)
$brush.Dispose()

$fontTitle = New-Object System.Drawing.Font("Segoe UI", [single]42.0, [System.Drawing.FontStyle]::Bold)
$fontSub = New-Object System.Drawing.Font("Segoe UI", [single]22.0, [System.Drawing.FontStyle]::Regular)
$brushOrange = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(218, 59, 1))
$brushWhite = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::White)
$brushMuted = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(180, 190, 205))

$g.FillRectangle($brushOrange, 100, 220, 12, 110)
$g.DrawString("QuickView Presentation", $fontTitle, $brushWhite, [single]130.0, [single]210.0)
$g.DrawString("Native Windows Direct2D & DirectWrite Performance", $fontSub, $brushMuted, [single]130.0, [single]280.0)

$badgeFont = New-Object System.Drawing.Font("Segoe UI", [single]14.0, [System.Drawing.FontStyle]::Bold)
$g.DrawString("SLIDE 1 of 5 | Extracted docProps/thumbnail.jpeg", $badgeFont, $brushOrange, [single]130.0, [single]360.0)

$bmp.Save("$tmp\docProps\thumbnail.jpeg", [System.Drawing.Imaging.ImageFormat]::Jpeg)
$g.Dispose()
$bmp.Dispose()

1..5 | ForEach-Object {
    Set-Content -LiteralPath "$tmp\ppt\slides\slide$_.xml" -Value "<p:sld/>"
}
Set-Content -LiteralPath "$tmp\[Content_Types].xml" -Value "<Types/>"

$outPath = Join-Path (Get-Location) "test_samples\sample_presentation.pptx"
if (Test-Path $outPath) { Remove-Item -Force $outPath }

[System.IO.Compression.ZipFile]::CreateFromDirectory($tmp, $outPath)
Remove-Item -Recurse -Force $tmp
Write-Host "Created $outPath successfully"
