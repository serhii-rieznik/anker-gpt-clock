param([string]$Exe = "$PSScriptRoot\..\build\Release\AnkerClock.exe", [switch]$Locked)
$ErrorActionPreference = 'Stop'
Add-Type @'
using System;using System.Runtime.InteropServices;using System.Text;
public static class CodexClockSmoke {
 [DllImport("user32.dll",CharSet=CharSet.Unicode)]private static extern IntPtr FindWindow(string c,string t);
 [DllImport("user32.dll")]public static extern bool PostMessage(IntPtr h,uint m,IntPtr w,IntPtr l);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)]private static extern int GetWindowText(IntPtr h,StringBuilder s,int n);
 [DllImport("wtsapi32.dll",CharSet=CharSet.Unicode)]private static extern bool WTSQuerySessionInformation(IntPtr server,uint session,int info,out IntPtr data,out uint bytes);
 [DllImport("wtsapi32.dll")]private static extern void WTSFreeMemory(IntPtr data);
 public static IntPtr Controller(){return FindWindow("AnkerClock.Controller",null);}
 public static string Status(){var s=new StringBuilder(2048);GetWindowText(Controller(),s,s.Capacity);return s.ToString();}
 public static bool SessionLocked(){IntPtr data;uint bytes;if(!WTSQuerySessionInformation(IntPtr.Zero,0xffffffff,25,out data,out bytes))throw new InvalidOperationException("Could not query lock state");try{return bytes>=20&&Marshal.ReadInt32(data)==1&&Marshal.ReadInt32(data,16)==0;}finally{WTSFreeMemory(data);}}
}
'@
$outputDir = [IO.Path]::GetFullPath("$PSScriptRoot\..\test-output")
$resultPrefix = if($Locked){'codex-locked'}else{'codex'}
$assertions = 0
function Assert($value,[string]$message) { if (!$value){throw $message}; $script:assertions++ }
function Wait-For([scriptblock]$condition,[string]$message,[int]$seconds=15) {
    $watch=[Diagnostics.Stopwatch]::StartNew()
    do { if (& $condition){return}; Start-Sleep -Milliseconds 25 } while($watch.Elapsed.TotalSeconds -lt $seconds)
    throw "$message`: $([CodexClockSmoke]::Status())"
}
function Command([int]$id) { [CodexClockSmoke]::PostMessage([CodexClockSmoke]::Controller(),0x111,[IntPtr]$id,[IntPtr]::Zero) | Out-Null }
function Read-Usage {
    $key=[Microsoft.Win32.Registry]::CurrentUser.OpenSubKey('Software\AnkerClock\CodexUsage')
    try {
        if (!$key){return $null}
        $data=$key.GetValue('Snapshot')
        if (!$data -or $data.Length -lt 52){return $null}
        return [pscustomobject]@{count=[BitConverter]::ToInt32($data,4);fetched=[DateTime]::FromFileTimeUtc([BitConverter]::ToInt64($data,8));remaining=[BitConverter]::ToInt32($data,16);failed=[BitConverter]::ToInt32($data,48)}
    } finally {if($key){$key.Dispose()}}
}
function Probe {
    & (Join-Path (Split-Path $Exe) 'AnkerClockTests.exe') --probe $outputDir | Out-Null
    Assert ($LASTEXITCODE -eq 0) 'Hub query failed'
    return Get-Content "$outputDir\hub-probe.json" -Raw | ConvertFrom-Json
}
Assert ([CodexClockSmoke]::Controller() -eq [IntPtr]::Zero) 'Quit the clock before testing'
$keyPath='Software\AnkerClock'
$key=[Microsoft.Win32.Registry]::CurrentUser.OpenSubKey($keyPath)
$backup=@{}
if($key){foreach($name in $key.GetValueNames()){if($name -ne 'PendingFrame'){$backup[$name]=@($key.GetValue($name),$key.GetValueKind($name))}};$key.Dispose()}
$engine=$null
try {
    $before=Probe
    $key=[Microsoft.Win32.Registry]::CurrentUser.CreateSubKey($keyPath)
    foreach($name in @('Layout','Interval','ShowCodexUsage','CodexRefreshSeconds','PauseOnBattery','ExclusiveGallery')){
        $value=switch($name){'Layout'{if($Locked){0}else{2}} 'Interval'{10} 'ShowCodexUsage'{1} 'CodexRefreshSeconds'{60} default{0}}
        $key.SetValue($name,$value,[Microsoft.Win32.RegistryValueKind]::DWord)
    }
    $key.Dispose()
    $started=[DateTime]::UtcNow
    $watch=[Diagnostics.Stopwatch]::StartNew()
    $arguments=if($Locked){'--startup --paused'}else{'--startup'}
    $engine=Start-Process $Exe -ArgumentList $arguments -WindowStyle Hidden -PassThru
    Wait-For {[CodexClockSmoke]::Controller() -ne [IntPtr]::Zero} 'Clock did not start'
    $startupMs=$watch.Elapsed.TotalMilliseconds
    $desktopLocked=[CodexClockSmoke]::SessionLocked()
    if($desktopLocked){[CodexClockSmoke]::PostMessage([CodexClockSmoke]::Controller(),0x2b1,[IntPtr]8,[IntPtr]::Zero) | Out-Null}
    if($Locked){
        [CodexClockSmoke]::PostMessage([CodexClockSmoke]::Controller(),0x2b1,[IntPtr]7,[IntPtr]::Zero) | Out-Null
        Command 4003
        Wait-For {[CodexClockSmoke]::Status() -match '^Locked at \d\d:\d\d:\d\d · 0 min$'} 'Initial lock frame did not finish'
    }
    $queryStarts=@(); $observedPids=@{}; $frames=@(); $lastStamp=''; $helperPeak=0; $usageSnapshots=@(); $lastFetched=0L
    $cpuBefore=$engine.TotalProcessorTime.TotalMilliseconds
    while($watch.Elapsed.TotalSeconds -lt 140) {
        foreach($child in @(Get-CimInstance Win32_Process -Filter "ParentProcessId=$($engine.Id) AND Name='codex.exe'")) {
            if(!$observedPids.ContainsKey($child.ProcessId)) {
                $observedPids[$child.ProcessId]=$true
                $queryStarts += [pscustomobject]@{pid=$child.ProcessId;startUtc=$child.CreationDate.ToUniversalTime();second=($child.CreationDate.Second + $child.CreationDate.Millisecond/1000.0)}
            }
            try {$helper=[Diagnostics.Process]::GetProcessById($child.ProcessId);$helperPeak=[Math]::Max($helperPeak,$helper.PeakWorkingSet64/1MB);$helper.Dispose()}catch{}
        }
        $usage=Read-Usage
        if($usage -and $usage.fetched.ToFileTimeUtc() -ne $lastFetched){$usageSnapshots+=$usage;$lastFetched=$usage.fetched.ToFileTimeUtc()}
        $status=[CodexClockSmoke]::Status()
        if($Locked -and $status -match '^Locked at (\d\d:\d\d:\d\d) · (\d+) min\nSent ([\d.]+) s before boundary$' -and $Matches[2] -ne $lastStamp) {
            $lastStamp=$Matches[2];$frames += [pscustomobject]@{stamp=$Matches[1];minute=[int]$Matches[2];lead=[double]$Matches[3]}
        } elseif(!$Locked -and $status -match '^Connected · updated (\d\d:\d\d:\d\d)\nSent ([\d.]+) s before boundary$' -and $Matches[1] -ne $lastStamp) {
            $lastStamp=$Matches[1];$frames += [pscustomobject]@{stamp=$Matches[1];lead=[double]$Matches[2]}
        }
        $aligned=@($queryStarts | Where-Object {$_.startUtc -gt $started.AddSeconds(3)})
        if($aligned.Count -ge 2 -and $usage -and $usage.fetched -gt $aligned[-1].startUtc -and $usage.failed -eq 0){break}
        Start-Sleep -Milliseconds 80
    }
    $engine.Refresh()
    $elapsedMs=$watch.Elapsed.TotalMilliseconds
    $cpuMs=$engine.TotalProcessorTime.TotalMilliseconds-$cpuBefore
    Assert ($aligned.Count -ge 2) 'Two aligned Codex queries did not run'
    foreach($query in $aligned){Assert ($query.second -ge 57.2 -and $query.second -le 57.9) 'Codex query did not start about 2.5 seconds before :00'}
    Assert ([Math]::Abs(($aligned[1].startUtc-$aligned[0].startUtc).TotalSeconds-60) -lt 0.2) 'Codex refresh drifted from minute boundaries'
    Assert ($usage.fetched -gt $started -and $usage.failed -eq 0 -and $usage.count -gt 0) 'The running clock did not fetch live Codex usage'
    if($Locked){
        Assert ($frames.Count -ge 1) 'Elapsed lock time stopped during usage refreshes'
        foreach($frame in $frames){Assert ($frame.stamp -eq $frames[0].stamp) 'Usage refresh changed the lock timestamp';Assert ($frame.lead -ge 0.44 -and $frame.lead -le 0.56) 'Locked selection lost its 0.5-second lead'}
    }else{
        Assert ($frames.Count -ge 4) 'Clock frames stopped during usage refreshes'
        foreach($frame in $frames){Assert (($frame.stamp.Split(':')[-1] -as [int]) % 10 -eq 0) 'Clock time lost its 10-second alignment';Assert ($frame.lead -ge 0.44 -and $frame.lead -le 0.56) 'Clock selection lost its 0.5-second lead'}
    }
    $timing=[ordered]@{queryStarts=$queryStarts;usageSnapshots=$usageSnapshots;clockFrames=$frames}
    $timing|ConvertTo-Json -Depth 5|Set-Content "$outputDir\$resultPrefix-timing-results.json"
    if($Locked){
        $lastMinute=if([CodexClockSmoke]::Status() -match '^Locked at \d\d:\d\d:\d\d · (\d+) min') {[int]$Matches[1]}else{throw 'Lock state was lost'}
        Wait-For {[CodexClockSmoke]::Status() -match '^Locked at \d\d:\d\d:\d\d · (\d+) min\nSent [\d.]+ s before boundary$' -and [int]$Matches[1] -gt $lastMinute} 'Next locked minute frame did not finish before readback' 75
        $lockedStatus=[CodexClockSmoke]::Status()
    }else{
        Wait-For {[CodexClockSmoke]::Status() -match '^Connected · updated \d\d:\d\d:00\nSent [\d.]+ s before boundary$'} 'Minute frame did not finish before readback'
    }
    Command 4003
    Wait-For {[CodexClockSmoke]::Status() -eq 'Updates paused'} 'Clock did not pause'
    Wait-For {@(Get-CimInstance Win32_Process -Filter "ParentProcessId=$($engine.Id) AND Name='codex.exe'").Count -eq 0} 'Codex helper stayed resident'
    $after=Probe
    $protected=$before.galleryBitmap -band (-bnot (1 -shl $before.selectedSlot))
    Assert ($before.galleryCount -eq $after.galleryCount -and ($after.galleryBitmap -band $protected) -eq $protected) 'Unrelated gallery images changed'
    & (Join-Path (Split-Path $Exe) 'AnkerClockTests.exe') --capture-current $outputDir | Out-Null
    Assert ($LASTEXITCODE -eq 0) 'Physical image readback failed'
    Copy-Item "$outputDir\physical-app-clock.jpg" "$outputDir\$resultPrefix-physical-clock.jpg" -Force
    $result=[ordered]@{assertions=$assertions;desktopLocked=$desktopLocked;injectedUnlock=$desktopLocked;injectedLock=[bool]$Locked;lockedStatus=$lockedStatus;startupToControllerMs=[Math]::Round($startupMs,1);workingSetMiB=[Math]::Round($engine.WorkingSet64/1MB,2);clockCpuPercentOfOneCore=[Math]::Round(100*$cpuMs/$elapsedMs,3);helperPeakWorkingSetMiB=[Math]::Round($helperPeak,1);queryStarts=$queryStarts;usageSnapshots=$usageSnapshots;clockFrames=$frames;galleryBefore=$before;galleryAfter=$after}
    $result|ConvertTo-Json -Depth 5|Set-Content "$outputDir\$resultPrefix-smoke-results.json"
    $result|ConvertTo-Json -Depth 5
} finally {
    if($engine){Command 4004;if(!$engine.WaitForExit(5000)){Stop-Process -Id $engine.Id}}
    $key=[Microsoft.Win32.Registry]::CurrentUser.CreateSubKey($keyPath)
    foreach($name in $key.GetValueNames()){if($name -ne 'PendingFrame' -and !$backup.ContainsKey($name)){$key.DeleteValue($name)}}
    foreach($name in $backup.Keys){$key.SetValue($name,$backup[$name][0],$backup[$name][1])}
    $key.Dispose()
}
