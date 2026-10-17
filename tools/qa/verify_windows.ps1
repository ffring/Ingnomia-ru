param(
    [Parameter(Mandatory=$true)][string]$GameRoot,
    [Parameter(Mandatory=$true)][string]$OutputDir,
    [switch]$StartColony
)
# Runtime proof for an isolated, disposable package copy on a Windows test runner.
# By default it captures the menu. -StartColony also clicks New Game in this process only,
# waits for world generation and captures the colony. It does not verify mood or job behavior.
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
if ($StartColony) {
    # Test-copy settings only; the original package and its default embark are untouched.
    $newGamePath = Join-Path $GameRoot 'content/JSON/newgame.json'
    $newGame = Get-Content -Raw -Encoding UTF8 $newGamePath | ConvertFrom-Json
    $newGame.dimX = 64
    $newGame.dimY = 64
    $newGame.numAnimals = 8
    $newGame.oceanSize = 0
    $newGame.numGnomes = 3
    $newGame.peaceful = $true
    $newGame | ConvertTo-Json -Depth 20 | Set-Content -Encoding UTF8 $newGamePath
}
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class GameWindowProof {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hwnd, out RECT rect);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr hwnd, out RECT rect);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr hwnd, ref POINT point);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern bool PostMessageW(IntPtr hwnd, uint msg, IntPtr wparam, IntPtr lparam);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hwnd, IntPtr hdc, uint flags);
}
'@

function Save-GameWindow([IntPtr]$Handle, [string]$Name) {
    if ($Handle -eq [IntPtr]::Zero) { throw 'Cannot capture a zero window handle' }
    $rect = New-Object GameWindowProof+RECT
    if (-not [GameWindowProof]::GetWindowRect($Handle, [ref]$rect)) { throw 'GetWindowRect failed' }
    $width = $rect.Right - $rect.Left
    $height = $rect.Bottom - $rect.Top
    if ($width -lt 100 -or $height -lt 100) { throw 'Game window dimensions are invalid' }
    $bitmap = New-Object System.Drawing.Bitmap($width, $height)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    try {
        $hdc = $graphics.GetHdc()
        try { $printed = [GameWindowProof]::PrintWindow($Handle, $hdc, 2) }
        finally { $graphics.ReleaseHdc($hdc) }
        $bitmap.Save((Join-Path $OutputDir $Name), [System.Drawing.Imaging.ImageFormat]::Png)
        $colors = New-Object 'System.Collections.Generic.HashSet[int]'
        for ($y=30; $y -lt $height; $y+=17) {
            for ($x=10; $x -lt $width; $x+=17) { [void]$colors.Add($bitmap.GetPixel($x,$y).ToArgb()) }
        }
        return [ordered]@{ width=$width; height=$height; printWindowSucceeded=$printed; sampledColors=$colors.Count; variedPixels=($colors.Count -gt 20) }
    } finally { $graphics.Dispose(); $bitmap.Dispose() }
}

# main.cpp opens log.txt before the user data folder exists; on a fresh machine the startup
# log is therefore written to the working directory (GameRoot), later runs use Documents.
$localLog = Join-Path $GameRoot 'log.txt'
$documentsLog = Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'My Games\Ingnomia\log.txt'

$proc = $null
$startTime = Get-Date
$report = [ordered]@{
    executable = $exe
    startTime = $startTime.ToString('o')
    windowFound = $false
    screenshot = $false
    variedPixels = $false
    gameplayTested = $false
    colonyRequested = [bool]$StartColony
    colonyGenerated = $false
}
$failure = $null
try {
    $proc = Start-Process -FilePath $exe -WorkingDirectory $GameRoot -PassThru -RedirectStandardOutput (Join-Path $OutputDir 'stdout.log') -RedirectStandardError (Join-Path $OutputDir 'stderr.log')
    $report.pid = $proc.Id
    $deadline = (Get-Date).AddSeconds(45)
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Seconds 1
        $proc.Refresh()
        if ($proc.HasExited) { throw 'Game exited before a window was available' }
        if ($proc.MainWindowHandle -ne [IntPtr]::Zero) { break }
    }
    if ($proc.MainWindowHandle -eq [IntPtr]::Zero) { throw 'No game window appeared within 45 seconds' }
    $report.windowFound = $true
    $report.windowTitle = $proc.MainWindowTitle
    Start-Sleep -Seconds 12
    $menu = Save-GameWindow $proc.MainWindowHandle 'window.png'
    foreach ($key in $menu.Keys) { $report[$key] = $menu[$key] }
    $report.screenshot = $true
    $screen = [System.Windows.Forms.SystemInformation]::VirtualScreen
    $desktop = New-Object System.Drawing.Bitmap($screen.Width,$screen.Height)
    $dg = [System.Drawing.Graphics]::FromImage($desktop)
    try {
        $dg.CopyFromScreen($screen.Left,$screen.Top,0,0,$screen.Size)
        $desktop.Save((Join-Path $OutputDir 'desktop.png'),[System.Drawing.Imaging.ImageFormat]::Png)
    } finally { $dg.Dispose(); $desktop.Dispose() }
    $proc.Refresh()
    $report.runningAfterCapture = -not $proc.HasExited
    if ($proc.HasExited) { throw 'Game exited after menu capture' }
    if ($StartColony) {
        $client = New-Object GameWindowProof+RECT
        $handle = $proc.MainWindowHandle
        if ($handle -eq [IntPtr]::Zero -or -not [GameWindowProof]::GetClientRect($handle, [ref]$client)) { throw 'No owned game client area for New Game' }
        # MainPage.xaml second centered button, observed in the menu proof: New Game.
        $x = [int](($client.Right - $client.Left) * 0.5)
        $y = [int](($client.Bottom - $client.Top) * 0.4)
        $point = New-Object GameWindowProof+POINT
        $point.X = $x; $point.Y = $y
        if (-not [GameWindowProof]::ClientToScreen($handle, [ref]$point)) { throw 'ClientToScreen failed' }
        if (-not [GameWindowProof]::SetCursorPos($point.X, $point.Y)) { throw 'SetCursorPos failed on the test runner' }
        $xy = [IntPtr](($y -shl 16) -bor ($x -band 0xffff))
        foreach ($message in @(@(0x0200,0), @(0x0201,1), @(0x0202,0))) {
            if (-not [GameWindowProof]::PostMessageW($handle, [uint32]$message[0], [IntPtr]$message[1], $xy)) { throw 'New Game mouse message failed' }
            Start-Sleep -Milliseconds 150
        }
        $report.newGameClick = [ordered]@{ clientX=$x; clientY=$y; pid=$proc.Id }
        $deadline = (Get-Date).AddSeconds(120)
        while ((Get-Date) -lt $deadline) {
            Start-Sleep -Seconds 2
            $proc.Refresh()
            if ($proc.HasExited) { throw 'Game exited while creating the colony' }
            foreach ($path in @($localLog, $documentsLog)) {
                if ((Test-Path $path) -and (Select-String -Quiet -SimpleMatch 'world generator - life - done' -Path $path)) {
                    $report.colonyGenerated = $true
                }
            }
            if ($report.colonyGenerated) { break }
        }
        if (-not $report.colonyGenerated) { throw 'World generation did not finish within 120 seconds' }
        Start-Sleep -Seconds 10
        $proc.Refresh()
        if ($proc.HasExited) { throw 'Game exited after world generation' }
        $report.colonyCapture = Save-GameWindow $proc.MainWindowHandle 'colony.png'
        $report.runningAfterColonyCapture = $true
    }
} catch {
    $failure = $_
    $report.error = $_.Exception.Message
} finally {
    # tear down only the process this script started
    if ($null -ne $proc) {
        $proc.Refresh()
        if (-not $proc.HasExited) {
            Stop-Process -Id $proc.Id -Force
            $report.stoppedByHelper = $true
        } else {
            $report.stoppedByHelper = $false
        }
        try {
            $proc.WaitForExit(10000) | Out-Null
            $report.exitCode = $proc.ExitCode
            $unsignedExitCode = [BitConverter]::ToUInt32([BitConverter]::GetBytes([int]$proc.ExitCode), 0)
            $report.exitCodeHex = ('0x{0:X8}' -f $unsignedExitCode)
            $report.exitTime = $proc.ExitTime.ToString('o')
        } catch {
            $report.exitCodeError = $_.Exception.Message
        }
    }

    # both log locations, kept separately
    $report.startupLocalLogCopied = $false
    if (Test-Path $localLog) {
        Copy-Item $localLog (Join-Path $OutputDir 'startup-local-log.txt') -Force
        $report.startupLocalLogCopied = $true
    }
    $report.documentsLogCopied = $false
    if (Test-Path $documentsLog) {
        Copy-Item $documentsLog (Join-Path $OutputDir 'documents-log.txt') -Force
        $report.documentsLogCopied = $true
    }

    # Windows Application error events for Ingnomia.exe since the test started (WER needs a moment)
    $report.applicationEvents = 0
    try {
        Start-Sleep -Seconds 5
        # levels 1-4: Application Error (1000) is an error, the WER report (1001) is informational
        $events = @(Get-WinEvent -FilterHashtable @{ LogName = 'Application'; StartTime = $startTime.AddSeconds(-2); Level = 1,2,3,4 } -ErrorAction Stop |
            Where-Object { $_.Message -match 'Ingnomia\.exe' })
        if ($null -ne $report.pid -and $events.Count -gt 0) {
            # keep events that name our PID, or that carry no PID at all (e.g. WER summaries)
            $pidHex = '0x{0:x}' -f $report.pid
            $owned = @($events | Where-Object { $_.Message -match [regex]::Escape($pidHex) -or $_.Message -notmatch '(?i)process id' })
            if ($owned.Count -gt 0) { $events = $owned }
        }
        $report.applicationEvents = $events.Count
        if ($events.Count -gt 0) {
            $events | Select-Object TimeCreated, Id, ProviderName, LevelDisplayName, Message |
                ConvertTo-Json -Depth 4 | Set-Content -Encoding UTF8 (Join-Path $OutputDir 'application-events.json')
        }
    } catch [System.Exception] {
        # "no events found" also lands here; it must not replace the original failure
        $report.applicationEventsNote = $_.Exception.Message
    }

    $report | ConvertTo-Json -Depth 8 | Set-Content -Encoding UTF8 (Join-Path $OutputDir 'runtime-proof.json')
}
if ($null -ne $failure) { throw $failure }
