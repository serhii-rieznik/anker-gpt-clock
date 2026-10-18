param([string]$Exe = "$PSScriptRoot\..\build\Release\AnkerClock.exe", [switch]$Hardware)
$ErrorActionPreference = 'Stop'
Add-Type @'
using System; using System.Runtime.InteropServices; using System.Text;
public static class ClockWorker {
 [DllImport("user32.dll",CharSet=CharSet.Unicode)] private static extern IntPtr FindWindow(string cls,string title);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)] private static extern int GetWindowText(IntPtr h,StringBuilder s,int n);
 [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h,uint m,IntPtr w,IntPtr l);
 [DllImport("user32.dll",EntryPoint="SendMessageW")] public static extern IntPtr SendMessage(IntPtr h,uint m,IntPtr w,IntPtr l);
 [DllImport("wtsapi32.dll",CharSet=CharSet.Unicode)] private static extern bool WTSQuerySessionInformation(IntPtr server,uint session,int info,out IntPtr data,out uint bytes);
 [DllImport("wtsapi32.dll")] private static extern void WTSFreeMemory(IntPtr data);
 public static bool SessionLocked(){IntPtr data;uint bytes;if(!WTSQuerySessionInformation(IntPtr.Zero,0xffffffff,25,out data,out bytes))throw new InvalidOperationException("Could not query session state");try{return bytes>=20&&Marshal.ReadInt32(data)==1&&Marshal.ReadInt32(data,16)==0;}finally{WTSFreeMemory(data);}}
 public static IntPtr Controller(){return FindWindow("AnkerClock.Controller",null);}
 public static string Status(){var s=new StringBuilder(2048);GetWindowText(Controller(),s,s.Capacity);return s.ToString();}
}
'@
$outputDir = [IO.Path]::GetFullPath("$PSScriptRoot\..\test-output")
[IO.Directory]::CreateDirectory($outputDir) | Out-Null
$assertions = 0; $process = $null
function Assert($value, [string]$message) { if (!$value) { throw $message }; $script:assertions++ }
function Wait-For([scriptblock]$condition, [string]$message, [int]$timeout = 15000) {
    $watch = [Diagnostics.Stopwatch]::StartNew()
    do { if (& $condition) { return }; Start-Sleep -Milliseconds 20 } while ($watch.ElapsedMilliseconds -lt $timeout)
    throw "$message`: $([ClockWorker]::Status())"
}
function Command([int]$id) { [ClockWorker]::PostMessage([ClockWorker]::Controller(),0x111,[IntPtr]$id,[IntPtr]::Zero) | Out-Null }
function Start-Clock([string]$arguments) {
    $script:process = Start-Process -FilePath $Exe -ArgumentList $arguments -WindowStyle Hidden -PassThru
    Wait-For { [ClockWorker]::Controller() -ne [IntPtr]::Zero } 'Controller did not start'
}
function Stop-Clock {
    if ($script:process) { Command 4004; Assert ($script:process.WaitForExit(5000)) 'Worker did not exit'; $script:process = $null }
}
function Probe {
    & (Join-Path (Split-Path $Exe) 'AnkerClockTests.exe') --probe $outputDir | Out-Null
    Assert ($LASTEXITCODE -eq 0) 'Hub query failed'
    return Get-Content "$outputDir\hub-probe.json" -Raw | ConvertFrom-Json
}
function Assert-CurrentTime([string]$prefix) {
    $stamp = [TimeSpan]::Parse([ClockWorker]::Status().Substring($prefix.Length).Split("`n")[0])
    $difference = [Math]::Abs(([DateTime]::Now.TimeOfDay-$stamp).TotalSeconds)
    Assert ([Math]::Min($difference,86400-$difference) -lt 10) 'The frame did not show the current time'
}
Assert ([ClockWorker]::Controller() -eq [IntPtr]::Zero) 'Quit the existing clock before testing'
$keyPath = 'Software\AnkerClock'
$key = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey($keyPath)
$hadPreferences = $null -ne $key; $backup = @{}
if ($key) { foreach ($name in $key.GetValueNames()) { if ($name -ne 'PendingFrame') { $backup[$name] = @($key.GetValue($name),$key.GetValueKind($name)) } }; $key.Dispose() }
try {
    Start-Clock '--startup --paused'
    Assert ([ClockWorker]::SendMessage([ClockWorker]::Controller(),0x4a,[IntPtr]::Zero,[IntPtr]::Zero) -eq [IntPtr]::Zero) 'Malformed IPC was accepted'
    Assert (!$process.HasExited) 'Malformed IPC crashed the controller'
    Stop-Clock
    if ($Hardware) {
        $before = Probe
        $key = [Microsoft.Win32.Registry]::CurrentUser.CreateSubKey($keyPath)
        foreach ($name in @('Interval','PauseOnBattery','PauseWhenLocked','Theme','ShowSeconds')) {
            $value = switch ($name) { 'Interval' {86400} 'PauseWhenLocked' {1} 'Theme' {6} 'ShowSeconds' {1} default {0} }
            $key.SetValue($name,$value,[Microsoft.Win32.RegistryValueKind]::DWord)
        }
        $key.SetValue('TimeZone',''); $key.Dispose()
        $startedLocked = [ClockWorker]::SessionLocked()
        Start-Clock '--startup'
        if ($startedLocked) {
            Wait-For { [ClockWorker]::Status().StartsWith('Windows locked · observed for 0 min') } 'Startup ignored the existing Windows lock'
            Assert ([ClockWorker]::Status().StartsWith('Windows locked · observed for 0 min')) 'Locked startup invented a lock time'
            [ClockWorker]::PostMessage([ClockWorker]::Controller(),0x2b1,[IntPtr]8,[IntPtr]::Zero) | Out-Null
        }
        Wait-For { [ClockWorker]::Status().StartsWith('Connected · updated ') } 'Startup waited for the next day boundary'
        Assert-CurrentTime 'Connected · updated '
        Command 4003; Wait-For { [ClockWorker]::Status() -eq 'Updates paused' } 'Pause failed'
        Command 4002; Wait-For { [ClockWorker]::Status().StartsWith('Paused · updated ') } 'Manual update failed'
        Assert-CurrentTime 'Paused · updated '
        Stop-Clock
        $key = [Microsoft.Win32.Registry]::CurrentUser.CreateSubKey($keyPath)
        $key.SetValue('Interval',10,[Microsoft.Win32.RegistryValueKind]::DWord); $key.Dispose()
        Start-Clock '--startup'
        if ([ClockWorker]::SessionLocked()) { [ClockWorker]::PostMessage([ClockWorker]::Controller(),0x2b1,[IntPtr]8,[IntPtr]::Zero) | Out-Null }
        $scheduled = @(); $watch = [Diagnostics.Stopwatch]::StartNew()
        while ($scheduled.Count -lt 3 -and $watch.Elapsed.TotalSeconds -lt 45) {
            $status = [ClockWorker]::Status()
            if ($status -match '^Connected · updated (\d\d:\d\d:\d\d)\nSent ([\d.]+) s before boundary$' -and
                ($scheduled.Count -eq 0 -or $scheduled[-1].stamp -ne $Matches[1])) {
                $scheduled += [pscustomobject]@{stamp=$Matches[1];lead=[double]$Matches[2]}
            }
            Start-Sleep -Milliseconds 50
        }
        Assert ($scheduled.Count -eq 3) 'Three scheduled frames did not complete'
        $scheduled | ConvertTo-Json -Compress | Write-Host
        for ($i=0; $i -lt $scheduled.Count; $i++) {
            $time = [TimeSpan]::Parse($scheduled[$i].stamp)
            Assert (($time.Seconds % 10) -eq 0) 'A frame was not snapped to 10 seconds'
            Assert ($scheduled[$i].lead -ge 0.45 -and $scheduled[$i].lead -le 0.55) "Selection at $($scheduled[$i].stamp) was $($scheduled[$i].lead) seconds early"
            if ($i -gt 0) { Assert (((($time-[TimeSpan]::Parse($scheduled[$i-1].stamp)).TotalSeconds+86400) % 86400) -eq 10) 'An interval was skipped' }
        }
        Command 4003; Wait-For { [ClockWorker]::Status() -eq 'Updates paused' } 'Pause failed'
        $selected = Probe
        Command 4002
        Wait-For { $null -ne [Microsoft.Win32.Registry]::GetValue('HKEY_CURRENT_USER\Software\AnkerClock','PendingFrame',$null) } 'A transfer was not staged'
        $cancel = [Diagnostics.Stopwatch]::StartNew()
        [ClockWorker]::PostMessage([ClockWorker]::Controller(),0x2b1,[IntPtr]7,[IntPtr]::Zero) | Out-Null
        Start-Sleep -Milliseconds 250
        $afterCancel = Probe
        Assert ($afterCancel.selectedCrc -eq $selected.selectedCrc) 'An interrupted update changed the selected image'
        Assert ($afterCancel.galleryCount -eq $selected.galleryCount) 'Cancellation changed gallery occupancy'
        $cancelMs = $cancel.ElapsedMilliseconds
        [ClockWorker]::PostMessage([ClockWorker]::Controller(),0x2b1,[IntPtr]8,[IntPtr]::Zero) | Out-Null
        Command 4003
        Wait-For { [ClockWorker]::Status().StartsWith('Connected · updated ') } 'Interrupted transfer did not recover'
        $lockWatch = [Diagnostics.Stopwatch]::StartNew()
        [ClockWorker]::PostMessage([ClockWorker]::Controller(),0x2b1,[IntPtr]7,[IntPtr]::Zero) | Out-Null
        Wait-For { [ClockWorker]::Status() -match '^Locked at (\d\d:\d\d:\d\d) · 0 min\nUpdated ' } 'Lock event did not add the lock overlay'
        $initialLockStatus = [ClockWorker]::Status()
        $lockStamp = $initialLockStatus.Substring(10,8)
        $lockedWeatherBefore = [Microsoft.Win32.Registry]::GetValue('HKEY_CURRENT_USER\Software\AnkerClock\Netatmo','FetchedAt',0)
        $lockTimeDifference = [Math]::Abs(([DateTime]::Now.TimeOfDay - [TimeSpan]::Parse($lockStamp)).TotalSeconds)
        Assert ([Math]::Min($lockTimeDifference,86400-$lockTimeDifference) -lt 10) 'The captured lock time differs from the event'
        [ClockWorker]::PostMessage([ClockWorker]::Controller(),0x2b1,[IntPtr]7,[IntPtr]::Zero) | Out-Null
        Start-Sleep -Seconds 12
        $lockedStatus = [ClockWorker]::Status()
        Assert ($lockedStatus.StartsWith("Locked at $lockStamp · 0 min`nUpdated ")) 'A duplicate lock event reset the lock timestamp'
        Assert ($lockedStatus -ne $initialLockStatus) 'The clock stopped updating while locked'
        Assert ($lockedStatus -match '\nUpdated (\d\d:\d\d:\d\d)\nSent ([\d.]+) s before boundary$') 'A locked clock frame was not scheduled'
        $lockedClockStamp = $Matches[1]
        $lockedClockLead = [double]$Matches[2]
        Assert (([TimeSpan]::Parse($lockedClockStamp).Seconds % 10) -eq 0) 'The locked clock was not snapped to 10 seconds'
        Assert ($lockedClockLead -ge 0.45 -and $lockedClockLead -le 0.55) 'The locked clock was not selected half a second early'
        $lockedScheduled = @([pscustomobject]@{stamp=$lockedClockStamp;lead=$lockedClockLead})
        Wait-For {
            $currentStatus = [ClockWorker]::Status()
            if ($currentStatus -match '^Locked at .+\nUpdated (\d\d:\d\d:\d\d)\nSent ([\d.]+) s before boundary$' -and
                $lockedScheduled[-1].stamp -ne $Matches[1]) {
                $script:lockedScheduled += [pscustomobject]@{stamp=$Matches[1];lead=[double]$Matches[2]}
            }
            $currentStatus.StartsWith("Locked at $lockStamp · 1 min")
        } 'Elapsed lock minutes did not update' 65000
        $lockedMinuteAtSeconds = $lockWatch.Elapsed.TotalSeconds
        $lockedWeatherAfter = [Microsoft.Win32.Registry]::GetValue('HKEY_CURRENT_USER\Software\AnkerClock\Netatmo','FetchedAt',0)
        Assert ($lockedMinuteAtSeconds -ge 59 -and $lockedMinuteAtSeconds -le 71) 'The lock duration did not advance on the next clock frame'
        Assert ($lockedScheduled.Count -ge 4) 'The locked clock did not keep updating through its first minute'
        for ($i=0; $i -lt $lockedScheduled.Count; $i++) {
            $time = [TimeSpan]::Parse($lockedScheduled[$i].stamp)
            Assert (($time.Seconds % 10) -eq 0) 'A locked frame was not snapped to 10 seconds'
            Assert ($lockedScheduled[$i].lead -ge 0.45 -and $lockedScheduled[$i].lead -le 0.55) 'A locked frame was not selected half a second early'
            if ($i -gt 0) { Assert (((($time-[TimeSpan]::Parse($lockedScheduled[$i-1].stamp)).TotalSeconds+86400) % 86400) -eq 10) 'An interval was skipped while locked' }
        }
        Assert ([ClockWorker]::Status() -match 'Sent ([\d.]+) s before boundary$') 'The locked minute was selected late'
        $lockedMinuteLead = [double]$Matches[1]
        Assert ($lockedMinuteLead -ge 0.45 -and $lockedMinuteLead -le 0.55) 'The locked minute was not selected half a second early'
        Command 4003; Wait-For { [ClockWorker]::Status() -eq 'Updates paused' } 'Lock display did not release USB when paused'
        & (Join-Path (Split-Path $Exe) 'AnkerClockTests.exe') --capture-current $outputDir | Out-Null
        Assert ($LASTEXITCODE -eq 0) 'Locked image readback failed'
        Copy-Item "$outputDir\physical-app-clock.jpg" "$outputDir\physical-locked-clock.jpg" -Force
        [ClockWorker]::PostMessage([ClockWorker]::Controller(),0x2b1,[IntPtr]8,[IntPtr]::Zero) | Out-Null
        Command 4003
        Wait-For { [ClockWorker]::Status().StartsWith('Connected · updated ') } 'Unlock did not restore the normal clock immediately'
        Assert-CurrentTime 'Connected · updated '
        Command 4003; Wait-For { [ClockWorker]::Status() -eq 'Updates paused' } 'Final pause failed'
        $after = Probe
        Assert ($after.galleryCount -eq 1) 'Old images accumulated in the clock gallery'
        Assert ($null -eq [Microsoft.Win32.Registry]::GetValue('HKEY_CURRENT_USER\Software\AnkerClock','PendingFrame',$null)) 'Recovery left a pending transfer'
        & (Join-Path (Split-Path $Exe) 'AnkerClockTests.exe') --capture-current $outputDir | Out-Null
        Assert ($LASTEXITCODE -eq 0) 'Physical image readback failed'
    }
    Stop-Clock
    $result = [ordered]@{assertions=$assertions;hardware=[bool]$Hardware;startedLocked=$startedLocked;scheduled=$scheduled;cancelAndProbeMs=$cancelMs;injectedLockEvents=$true;lockStamp=$lockStamp;lockedScheduled=$lockedScheduled;lockedMinuteAtSeconds=$lockedMinuteAtSeconds;lockedMinuteLead=$lockedMinuteLead;weatherRefreshedWhileLocked=($lockedWeatherAfter -gt $lockedWeatherBefore)}
    $result | ConvertTo-Json -Depth 4 | Set-Content "$outputDir\review-worker-results.json"
    $result | ConvertTo-Json -Depth 4
} finally {
    if ($process) { Command 4004; if (!$process.WaitForExit(5000)) { Stop-Process -Id $process.Id } }
    $key = [Microsoft.Win32.Registry]::CurrentUser.CreateSubKey($keyPath)
    foreach ($name in $key.GetValueNames()) { if ($name -ne 'PendingFrame' -and !$backup.ContainsKey($name)) { $key.DeleteValue($name) } }
    foreach ($name in $backup.Keys) { $key.SetValue($name,$backup[$name][0],$backup[$name][1]) }
    $empty = $key.GetValueNames().Length -eq 0
    $key.Dispose()
    if (!$hadPreferences -and $empty) { [Microsoft.Win32.Registry]::CurrentUser.DeleteSubKey($keyPath,$false) }
}
