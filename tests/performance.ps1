param(
    [string]$Exe = "$PSScriptRoot\..\build\Release\AnkerClock.exe",
    [int]$Seconds = 30,
    [switch]$Active
)
$ErrorActionPreference = 'Stop'
Add-Type @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class ClockPerf {
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] private static extern IntPtr FindWindow(string cls, string title);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr window, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetGuiResources(IntPtr process, uint flags);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] private static extern int GetWindowText(IntPtr window, StringBuilder text, int capacity);
    public static IntPtr Controller() { return FindWindow("AnkerClock.Controller", null); }
    public static string Status() {
        var window = Controller();
        if (window == IntPtr.Zero) return "";
        var text = new StringBuilder(2048);
        GetWindowText(window, text, text.Capacity);
        return text.ToString();
    }
}
'@
if ([ClockPerf]::Controller() -ne [IntPtr]::Zero) { throw 'Quit the existing clock before benchmarking' }
$outputDir = [IO.Path]::GetFullPath("$PSScriptRoot\..\test-output")
[IO.Directory]::CreateDirectory($outputDir) | Out-Null
$samples = @()
for ($iteration = 0; $iteration -lt 3; $iteration++) {
    $process = $null
    $controller = [IntPtr]::Zero
    try {
        $arguments = if ($Active) { '--startup' } else { '--startup --paused' }
        $watch = [Diagnostics.Stopwatch]::StartNew()
        $process = Start-Process -FilePath $Exe -ArgumentList $arguments -WindowStyle Hidden -PassThru
        do {
            $controller = [ClockPerf]::Controller()
            if ($controller -ne [IntPtr]::Zero) { break }
            Start-Sleep -Milliseconds 5
        } while ($watch.ElapsedMilliseconds -lt 5000)
        if ($controller -eq [IntPtr]::Zero) { throw 'Tray startup failed' }
        $startupMs = $watch.Elapsed.TotalMilliseconds
        Start-Sleep -Milliseconds $(if ($Active) { 3000 } else { 500 })
        $process.Refresh()
        $row = [ordered]@{
            startupToControllerMs=[Math]::Round($startupMs, 1)
            workingSetMiB=[Math]::Round($process.WorkingSet64 / 1MB, 2)
            privateMiB=[Math]::Round($process.PrivateMemorySize64 / 1MB, 2)
            handles=$process.HandleCount
            gdiObjects=[ClockPerf]::GetGuiResources($process.Handle, 0)
        }
        if ($iteration -eq 2) {
            if ($Active) { Start-Sleep -Seconds 5 }
            $process.Refresh()
            $cpuBefore = $process.TotalProcessorTime.TotalMilliseconds
            $sampleWatch = [Diagnostics.Stopwatch]::StartNew()
            Start-Sleep -Seconds $Seconds
            $process.Refresh()
            $row.sampleSeconds = [Math]::Round($sampleWatch.Elapsed.TotalSeconds, 2)
            $row.cpuMs = [Math]::Round($process.TotalProcessorTime.TotalMilliseconds - $cpuBefore, 2)
            $row.cpuPercentOfOneCore = [Math]::Round(100 * $row.cpuMs / $sampleWatch.Elapsed.TotalMilliseconds, 4)
            $row.endWorkingSetMiB = [Math]::Round($process.WorkingSet64 / 1MB, 2)
            $row.endPrivateMiB = [Math]::Round($process.PrivateMemorySize64 / 1MB, 2)
            if ($Active) {
                $statusWatch = [Diagnostics.Stopwatch]::StartNew()
                do {
                    $row.endStatus = [ClockPerf]::Status()
                    if ($row.endStatus) { break }
                    Start-Sleep -Milliseconds 20
                } while ($statusWatch.ElapsedMilliseconds -lt 5000)
                if (!$row.endStatus.StartsWith('Connected · updated ') -and
                    !$row.endStatus.StartsWith('Locked at ') -and
                    !$row.endStatus.StartsWith('Windows locked · observed for ')) { throw "Active clock did not finish updates: $($row.endStatus)" }
            }
        }
        $samples += $row
    } finally {
        if ($controller -ne [IntPtr]::Zero) {
            [ClockPerf]::PostMessage($controller, 0x111, [IntPtr]4004, [IntPtr]::Zero) | Out-Null
        }
        if ($process -and !$process.HasExited) {
            if (!$process.WaitForExit(5000)) { Stop-Process -Id $process.Id }
        }
    }
}
$mode = if ($Active) { 'active' } else { 'paused' }
$result = [ordered]@{ mode=$mode; executableBytes=(Get-Item $Exe).Length; samples=$samples }
$result | ConvertTo-Json -Depth 5 | Set-Content "$outputDir\performance-$mode.json"
$result | ConvertTo-Json -Depth 5
