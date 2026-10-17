param(
    [Parameter(Mandatory=$true)][string]$GameRoot,
    [Parameter(Mandatory=$true)][string]$OutputDir
)
$ErrorActionPreference = 'Stop'
$GameRoot = (Resolve-Path $GameRoot).Path
New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
$OutputDir = (Resolve-Path $OutputDir).Path
$exe = Join-Path $GameRoot 'Ingnomia.exe'
if (-not (Test-Path $exe)) { throw 'Ingnomia.exe is missing' }
$configPath = Join-Path $GameRoot 'content/JSON/config.json'
$config = Get-Content -Raw -Encoding UTF8 $configPath | ConvertFrom-Json
$config.WindowWidth = 1280
$config.WindowHeight = 720
$config.WindowPosX = 0
$config.WindowPosY = 0
$config.framelessWindow = 'false'
$config | ConvertTo-Json -Depth 20 | Set-Content -Encoding UTF8 $configPath
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class GameWindowProof {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hwnd, out RECT rect);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hwnd, IntPtr hdc, uint flags);
}
'@
$proc = $null
$report = [ordered]@{ executable = $exe; windowFound = $false; screenshot = $false; variedPixels = $false; gameplayTested = $false }
try {
    $proc = Start-Process -FilePath $exe -WorkingDirectory $GameRoot -PassThru -RedirectStandardOutput (Join-Path $OutputDir 'stdout.log') -RedirectStandardError (Join-Path $OutputDir 'stderr.log')
    $deadline = (Get-Date).AddSeconds(45)
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Seconds 1
        $proc.Refresh()
        if ($proc.HasExited) { $report.exitCode = $proc.ExitCode; throw 'Game exited before a window was available' }
        if ($proc.MainWindowHandle -ne [IntPtr]::Zero) { break }
    }
    if ($proc.MainWindowHandle -eq [IntPtr]::Zero) { throw 'No game window appeared within 45 seconds' }
    $report.windowFound = $true
    $report.windowTitle = $proc.MainWindowTitle
    Start-Sleep -Seconds 12
    $rect = New-Object GameWindowProof+RECT
    if (-not [GameWindowProof]::GetWindowRect($proc.MainWindowHandle, [ref]$rect)) { throw 'GetWindowRect failed' }
    $width = $rect.Right - $rect.Left
    $height = $rect.Bottom - $rect.Top
    if ($width -lt 100 -or $height -lt 100) { throw 'Game window dimensions are invalid' }
    $report.width = $width
    $report.height = $height
    $bitmap = New-Object System.Drawing.Bitmap($width, $height)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    try {
        $hdc = $graphics.GetHdc()
        try { $report.printWindowSucceeded = [GameWindowProof]::PrintWindow($proc.MainWindowHandle, $hdc, 2) }
        finally { $graphics.ReleaseHdc($hdc) }
        $bitmap.Save((Join-Path $OutputDir 'window.png'), [System.Drawing.Imaging.ImageFormat]::Png)
        $colors = New-Object 'System.Collections.Generic.HashSet[int]'
        for ($y=30; $y -lt $height; $y+=17) {
            for ($x=10; $x -lt $width; $x+=17) { [void]$colors.Add($bitmap.GetPixel($x,$y).ToArgb()) }
        }
        $report.sampledColors = $colors.Count
        $report.variedPixels = $colors.Count -gt 20
        $report.screenshot = $true
    } finally { $graphics.Dispose(); $bitmap.Dispose() }
    $screen = [System.Windows.Forms.SystemInformation]::VirtualScreen
    $desktop = New-Object System.Drawing.Bitmap($screen.Width,$screen.Height)
    $dg = [System.Drawing.Graphics]::FromImage($desktop)
    try {
        $dg.CopyFromScreen($screen.Left,$screen.Top,0,0,$screen.Size)
        $desktop.Save((Join-Path $OutputDir 'desktop.png'),[System.Drawing.Imaging.ImageFormat]::Png)
    } finally { $dg.Dispose(); $desktop.Dispose() }
    $proc.Refresh()
    $report.runningAfterCapture = -not $proc.HasExited
} catch {
    $report.error = $_.Exception.Message
    throw
} finally {
    if ($null -ne $proc) {
        $proc.Refresh()
        if (-not $proc.HasExited) { Stop-Process -Id $proc.Id -Force }
    }
    # Ingnomia is a GUI-subsystem app: its own log goes to Documents\My Games\Ingnomia\log.txt
    $gameLog = Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'My Games\Ingnomia\log.txt'
    $report.gameLogCopied = $false
    if (Test-Path $gameLog) {
        Copy-Item $gameLog (Join-Path $OutputDir 'ingnomia-log.txt') -Force
        $report.gameLogCopied = $true
    }
    $report | ConvertTo-Json -Depth 8 | Set-Content -Encoding UTF8 (Join-Path $OutputDir 'runtime-proof.json')
}
