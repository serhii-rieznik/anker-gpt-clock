param([string]$Exe = "$PSScriptRoot\..\build\Release\AnkerClock.exe", [switch]$Hardware, [switch]$InspectOnly, [switch]$Netatmo)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient,UIAutomationTypes,System.Drawing
Add-Type @'
using System; using System.Runtime.InteropServices; using System.Text;
public static class ClockUi {
 [StructLayout(LayoutKind.Sequential)] public struct Rect { public int Left, Top, Right, Bottom; }
 [DllImport("user32.dll",CharSet=CharSet.Unicode)] private static extern IntPtr FindWindow(string cls,string title);
 [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h,uint m,IntPtr w,IntPtr l);
 [DllImport("user32.dll",EntryPoint="SendMessageW")] public static extern IntPtr SendMessage(IntPtr h,uint m,IntPtr w,IntPtr l);
 [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h,int n);
 [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h,out Rect r);
 [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h,IntPtr dc,uint flags);
 [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr d);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)] private static extern int GetWindowText(IntPtr h,StringBuilder s,int n);
 [DllImport("wtsapi32.dll",CharSet=CharSet.Unicode)] private static extern bool WTSQuerySessionInformation(IntPtr server,uint session,int info,out IntPtr data,out uint bytes);
 [DllImport("wtsapi32.dll")] private static extern void WTSFreeMemory(IntPtr data);
 public static bool SessionLocked(){IntPtr data;uint bytes;if(!WTSQuerySessionInformation(IntPtr.Zero,0xffffffff,25,out data,out bytes))throw new InvalidOperationException("Could not query session state");try{return bytes>=20&&Marshal.ReadInt32(data)==1&&Marshal.ReadInt32(data,16)==0;}finally{WTSFreeMemory(data);}}
 public static IntPtr Controller(){return FindWindow("AnkerClock.Controller",null);}
 public static IntPtr FontPicker(){return FindWindow(null,"Choose clock font");}
 public static IntPtr UnsavedPrompt(){return FindWindow(null,"Unsaved changes");}
 public static string Status(){var s=new StringBuilder(2048);GetWindowText(Controller(),s,s.Capacity);return s.ToString();}
}
'@
[ClockUi]::SetProcessDpiAwarenessContext([IntPtr](-4)) | Out-Null
$outputDir = [IO.Path]::GetFullPath("$PSScriptRoot\..\test-output")
[IO.Directory]::CreateDirectory($outputDir) | Out-Null
$script:assertions = 0
function Assert($value, [string]$message) { if (!$value) { throw $message }; $script:assertions++ }
function Wait-For([scriptblock]$condition, [string]$message, [int]$timeout = 5000) {
    $timer = [Diagnostics.Stopwatch]::StartNew()
    do { if (& $condition) { return }; Start-Sleep -Milliseconds 40 } while ($timer.ElapsedMilliseconds -lt $timeout)
    $detail = if ($script:window) { try { (Element 'Validation').Current.Name } catch { '' } } else { '' }
    throw "$message`: $detail"
}
function Element([string]$id) {
    $e = $script:window.FindFirst([Windows.Automation.TreeScope]::Descendants,
        (New-Object Windows.Automation.PropertyCondition([Windows.Automation.AutomationElement]::AutomationIdProperty,$id)))
    if (!$e) { throw "Missing control: $id" }; return $e
}
function Visible([string]$id) {
    $e = $script:window.FindFirst([Windows.Automation.TreeScope]::Descendants,
        (New-Object Windows.Automation.PropertyCondition([Windows.Automation.AutomationElement]::AutomationIdProperty,$id)))
    return $e -and !$e.Current.IsOffscreen
}
function Font-Picker {
    $script:settingsWindow = $script:window
    Invoke 'Font'
    Wait-For {
        $handle = [ClockUi]::FontPicker()
        $script:fontWindow = if ($handle -ne [IntPtr]::Zero) { [Windows.Automation.AutomationElement]::FromHandle($handle) } else { $null }
        $null -ne $script:fontWindow
    } 'Font browser did not open'
    $script:window = $script:fontWindow
}
function Font-Item([string]$name) {
    $item = $script:window.FindFirst([Windows.Automation.TreeScope]::Descendants,
        (New-Object Windows.Automation.AndCondition(
            (New-Object Windows.Automation.PropertyCondition([Windows.Automation.AutomationElement]::ControlTypeProperty,[Windows.Automation.ControlType]::ListItem)),
            (New-Object Windows.Automation.PropertyCondition([Windows.Automation.AutomationElement]::NameProperty,$name)))))
    return $item
}
function Compare-Font([string]$name) {
    $toggle = $script:window.FindFirst([Windows.Automation.TreeScope]::Descendants,
        (New-Object Windows.Automation.PropertyCondition([Windows.Automation.AutomationElement]::NameProperty,"Compare $name")))
    (Pattern $toggle 'Toggle').Toggle()
}
function Pattern($element, [string]$type) {
    return $element.GetCurrentPattern(([type]"Windows.Automation.${type}Pattern")::Pattern)
}
function Invoke([string]$id) { (Pattern (Element $id) 'Invoke').Invoke() }
function Select-Item([string]$id) { (Pattern (Element $id) 'SelectionItem').Select() }
function Set-Text([string]$id, [string]$text) { (Pattern (Element $id) 'Value').SetValue($text) }
function Text([string]$id) { return (Pattern (Element $id) 'Value').Current.Value }
function Select-Combo([string]$id, [string]$name) {
    $combo = Element $id
    (Pattern $combo 'ExpandCollapse').Expand()
    $matches = $combo.FindAll([Windows.Automation.TreeScope]::Descendants,
        (New-Object Windows.Automation.PropertyCondition([Windows.Automation.AutomationElement]::NameProperty,$name)))
    $item = $null
    foreach ($match in $matches) {
        $candidate = $match
        while ($candidate -and !$candidate.Equals($combo)) {
            if ($candidate.GetCurrentPropertyValue([Windows.Automation.AutomationElement]::IsSelectionItemPatternAvailableProperty)) { $item = $candidate; break }
            $candidate = [Windows.Automation.TreeWalker]::ControlViewWalker.GetParent($candidate)
        }
        if ($item) { break }
    }
    if (!$item) { throw "Missing choice $name" }
    (Pattern $item 'SelectionItem').Select()
    (Pattern $combo 'ExpandCollapse').Collapse()
}
function Set-Switch([string]$id, [bool]$value) {
    $p = Pattern (Element $id) 'Toggle'
    if (($p.Current.ToggleState -eq [Windows.Automation.ToggleState]::On) -ne $value) { $p.Toggle() }
}
function Open-Settings {
    [ClockUi]::PostMessage([ClockUi]::Controller(),0x111,[IntPtr]4001,[IntPtr]::Zero) | Out-Null
    Wait-For { $p = Get-Process AnkerClock.Settings -ErrorAction SilentlyContinue; $p -and $p.MainWindowHandle -ne 0 } 'Settings did not open' 10000
    $script:uiProcess = Get-Process AnkerClock.Settings
    [ClockUi]::ShowWindow($script:uiProcess.MainWindowHandle,5) | Out-Null
    $script:window = [Windows.Automation.AutomationElement]::FromHandle($script:uiProcess.MainWindowHandle)
    Assert ($script:window.Current.FrameworkId -eq 'WPF') 'The window is not using WPF'
    Wait-For { (Element 'Status').Current.Name -ne '' } 'Status did not arrive'
}
function Close-Settings {
    Invoke 'Hide'
    Assert ($script:uiProcess.WaitForExit(5000)) 'The UI process remained resident'
    Assert ([ClockUi]::Controller() -ne [IntPtr]::Zero) 'Closing settings stopped the tray engine'
}
function Save { Start-Sleep -Milliseconds 250; Invoke 'Save'; Wait-For { (Element 'Validation').Current.Name -eq 'Settings saved' } 'Settings were not saved' }
function Escape {
    [ClockUi]::PostMessage($script:uiProcess.MainWindowHandle,0x100,[IntPtr]27,[IntPtr]1) | Out-Null
    [ClockUi]::PostMessage($script:uiProcess.MainWindowHandle,0x101,[IntPtr]27,[IntPtr]0xC0000001L) | Out-Null
}
function Capture([string]$name, [IntPtr]$handle = [IntPtr]::Zero) {
    Start-Sleep -Milliseconds 250
    $rect = New-Object ClockUi+Rect
    if ($handle -eq [IntPtr]::Zero) { $handle = $script:uiProcess.MainWindowHandle }
    [ClockUi]::GetWindowRect($handle,[ref]$rect) | Out-Null
    $bitmap = New-Object Drawing.Bitmap(($rect.Right-$rect.Left),($rect.Bottom-$rect.Top))
    $g = [Drawing.Graphics]::FromImage($bitmap); $dc = $g.GetHdc()
    try { Assert ([ClockUi]::PrintWindow($handle,$dc,2)) 'Capture failed' } finally { $g.ReleaseHdc($dc) }
    try {
        $colors = New-Object 'Collections.Generic.HashSet[int]'
        for ($y=0; $y -lt $bitmap.Height; $y+=8) {
            for ($x=0; $x -lt $bitmap.Width; $x+=8) { $colors.Add($bitmap.GetPixel($x,$y).ToArgb()) | Out-Null }
        }
        Assert ($colors.Count -ge 16) 'Capture returned a blank image'
        $bitmap.Save("$outputDir\$name.png",[Drawing.Imaging.ImageFormat]::Png)
    } finally { $g.Dispose(); $bitmap.Dispose() }
}
$keyPath = 'Software\AnkerClock'
$desktopLocked = [ClockUi]::SessionLocked()
Assert (!$desktopLocked -or $InspectOnly) 'Unlock Windows before running the interactive UI smoke test'
$key = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey($keyPath)
$hadPreferences = $null -ne $key; $backup = @{}
if ($key) { foreach ($name in $key.GetValueNames()) { if ($name -ne 'PendingFrame') { $backup[$name] = @($key.GetValue($name),$key.GetValueKind($name)) } }; $key.Dispose() }
$runPath = 'Software\Microsoft\Windows\CurrentVersion\Run'
$runKey = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey($runPath)
$runValue = if ($runKey) { $runKey.GetValue('AnkerClock') } else { $null }
$runKind = if ($null -ne $runValue) { $runKey.GetValueKind('AnkerClock') } else { [Microsoft.Win32.RegistryValueKind]::String }
if ($runKey) { $runKey.Dispose() }
$process = $null
try {
    Assert ([ClockUi]::Controller() -eq [IntPtr]::Zero) 'Quit the existing clock before testing'
    $key = [Microsoft.Win32.Registry]::CurrentUser.CreateSubKey($keyPath)
    foreach ($name in @('Theme','Layout','AnalogFace','FontStyle','PauseOnBattery')) {
        $value = switch ($name) { 'Theme' { 6 } 'Layout' { 2 } default { 0 } }
        $key.SetValue($name,$value,[Microsoft.Win32.RegistryValueKind]::DWord)
    }
    $key.Dispose()
    $process = Start-Process -FilePath $Exe -ArgumentList '--startup --paused' -WindowStyle Hidden -PassThru
    Wait-For { [ClockUi]::Controller() -ne [IntPtr]::Zero } 'Tray did not start'
    Assert ([ClockUi]::SendMessage([ClockUi]::Controller(),0x4a,[IntPtr]::Zero,[IntPtr]::Zero) -eq [IntPtr]::Zero) 'Malformed settings message was accepted'
    Assert (!$process.HasExited) 'Malformed settings message crashed the tray engine'
    $watch = [Diagnostics.Stopwatch]::StartNew(); Open-Settings; $openMs = $watch.ElapsedMilliseconds
    Wait-For { (Element 'Status').Current.Name -eq 'Updates paused' } 'The clock was not paused' 10000
    Assert (([ClockUi]::SendMessage([ClockUi]::Controller(),0x8004,[IntPtr]::Zero,[IntPtr]::Zero).ToInt64() -band 1) -eq 1) 'The clock was not paused'
    if ($Netatmo) {
        $codexEnabled = if ($backup.ContainsKey('ShowCodexUsage')) { [bool]$backup['ShowCodexUsage'][0] } else { $false }
        Select-Item 'ClockTab'; Set-Switch 'ShowCodexUsage' $false
        Select-Item 'NetatmoTab'
        Invoke 'RefreshNetatmo'
        Wait-For { (Element 'NetatmoStatus').Current.Name.Contains('Connected to Netatmo') } 'Netatmo did not connect' 20000
        Assert ((Element 'NetatmoAge').Current.Name.Contains('Last successful fetch:')) 'Weather diagnostics omit the fetch timestamp'
        Assert ((Element 'NetatmoAge').Current.Name.Contains('measurement:')) 'Weather diagnostics omit the measurement timestamp'
        Assert ((Element 'NetatmoPolling').Current.Name.StartsWith('Automatic refresh paused.')) 'Paused polling is not explained'
        $weather = Get-ItemProperty 'HKCU:\Software\AnkerClock\Netatmo'
        $station = (ConvertFrom-Json $weather.Data).body.devices[0]
        $indoor = @($station.modules | Where-Object { $_.reachable -and $_.data_type -contains 'Temperature' -and $_.data_type -notcontains 'Noise' })[0]
        Assert ($null -ne $indoor) 'This check needs a second reachable indoor module'
        Set-Switch 'ShowNetatmo' $true
        Select-Combo 'NetatmoModule' $station.module_name
        Assert ((Element 'NetatmoNoise').Current.IsEnabled) 'Main station noise is disabled'
        foreach ($id in @('NetatmoTemperature','NetatmoHumidity','NetatmoCo2','NetatmoNoise')) { Set-Switch $id $true }
        Set-Text 'NetatmoRefreshSeconds' '60'; Save
        $saved = Get-ItemProperty 'HKCU:\Software\AnkerClock'
        Assert ($saved.NetatmoModule -eq $station._id) 'Selected station was not saved'
        Assert ($saved.NetatmoReadings -eq 15 -and $saved.NetatmoRefreshSeconds -eq 60) 'Reading choices or interval were not saved'
        Select-Combo 'NetatmoModule' $indoor.module_name
        Assert (!(Visible 'NetatmoNoise')) 'Indoor module incorrectly offers noise'
        Select-Item 'ClockTab'; Set-Switch 'ShowCodexUsage' $true; Save
        Assert ((Get-ItemProperty 'HKCU:\Software\AnkerClock').NetatmoReadings -eq 7) 'An unavailable noise reading counted toward the Codex display limit'
        Set-Switch 'ShowCodexUsage' $false; Save; Select-Item 'NetatmoTab'
        Set-Switch 'NetatmoCo2' $false; Save
        Assert ((Get-ItemProperty 'HKCU:\Software\AnkerClock').NetatmoModule -eq $indoor._id) 'Room selection was not saved'
        Assert (((Get-ItemProperty 'HKCU:\Software\AnkerClock').NetatmoReadings -band 7) -eq 3) 'Individual reading selection was ignored'
        Set-Text 'NetatmoRefreshSeconds' '59'
        Wait-For { (Element 'Validation').Current.Name.Contains('60') -and !(Element 'Save').Current.IsEnabled } 'Invalid refresh interval was accepted'
        Set-Text 'NetatmoRefreshSeconds' '60'
        Select-Combo 'NetatmoModule' $station.module_name
        foreach ($id in @('NetatmoTemperature','NetatmoHumidity','NetatmoCo2','NetatmoNoise')) { Set-Switch $id $false }
        Wait-For { (Element 'Validation').Current.Name.StartsWith('Choose at least one') } 'Empty measurement selection was accepted'
        foreach ($id in @('NetatmoTemperature','NetatmoHumidity','NetatmoCo2','NetatmoNoise')) { Set-Switch $id $true }
        Set-Switch 'NetatmoHumidity' $false
        Select-Item 'ClockTab'; Set-Switch 'ShowCodexUsage' $codexEnabled
        foreach ($choice in @('Quiet Classic','Precision','Soft Frame')) {
            Select-Combo 'AnalogFace' $choice; Save
            $index = [Array]::IndexOf(@('Quiet Classic','Precision','Soft Frame'),$choice)
            Assert ((Get-ItemProperty 'HKCU:\Software\AnkerClock').AnalogFace -eq $index) 'Analog style was not saved'
        }
        Select-Item 'NetatmoTab'
        (Pattern (Element 'ReadingFontSize') 'RangeValue').SetValue(30)
        Save
        Assert ((Get-ItemProperty 'HKCU:\Software\AnkerClock').ReadingFontSize -eq 30) 'Reading value size was not saved'
        Capture 'netatmo-settings'
        Close-Settings; Open-Settings; Select-Item 'NetatmoTab'
        Assert ((Text 'NetatmoRefreshSeconds') -eq '60') 'Netatmo interval was lost when reopening'
        Assert ((Pattern (Element 'ReadingFontSize') 'RangeValue').Current.Value -eq 30) 'Reading size was lost when reopening'
        Assert ((Pattern (Element 'NetatmoNoise') 'Toggle').Current.ToggleState -eq [Windows.Automation.ToggleState]::On) 'Noise selection was lost when reopening'
        Select-Item 'ClockTab'
        Assert ((Pattern (Element 'AnalogFace') 'Selection').Current.GetSelection()[0].Current.Name -eq 'Soft Frame') 'Analog style was lost when reopening'
        Assert (((Pattern (Element 'ShowCodexUsage') 'Toggle').Current.ToggleState -eq [Windows.Automation.ToggleState]::On) -eq $codexEnabled) 'Netatmo changed the Codex usage preference'
        Close-Settings
        [ordered]@{assertions=$script:assertions;settingsOpenMs=$openMs;netatmo=$true} | ConvertTo-Json
        return
    }
    if ($InspectOnly) {
        Select-Item 'ClockTab'
        Assert ((Element 'ShowCodexUsage').Current.IsEnabled) 'Codex option is disabled on the analog face'
        $codex = if ($backup.ContainsKey('ShowCodexUsage')) { [bool]$backup['ShowCodexUsage'][0] } else { $false }
        Assert (((Pattern (Element 'ShowCodexUsage') 'Toggle').Current.ToggleState -eq [Windows.Automation.ToggleState]::On) -eq $codex) 'Codex display flag did not load'
        $codexInterval = if ($backup.ContainsKey('CodexRefreshSeconds')) { [string]$backup['CodexRefreshSeconds'][0] } else { '60' }
        Assert ((Text 'CodexRefreshSeconds') -eq $codexInterval) 'Codex refresh interval did not load'
        Assert ((Element 'CodexRefreshSeconds').Current.IsEnabled -eq $codex) 'Codex refresh availability differs'
        Assert ((Element 'ShowDate').Current.IsEnabled) 'Codex incorrectly disables the date switch'
        Select-Item 'StartupTab'
        $removed = $script:window.FindFirst([Windows.Automation.TreeScope]::Descendants,
            (New-Object Windows.Automation.PropertyCondition([Windows.Automation.AutomationElement]::AutomationIdProperty,'PauseWhenLocked')))
        Assert ($null -eq $removed) 'The lock-pause option is still visible'
        $launch = if ($backup.ContainsKey('ShowSettingsOnLaunch')) { [bool]$backup['ShowSettingsOnLaunch'][0] } else { $true }
        Assert (((Pattern (Element 'OpenSettings') 'Toggle').Current.ToggleState -eq [Windows.Automation.ToggleState]::On) -eq $launch) 'Removing the lock option changed the launch flag'
        Select-Item 'DeviceTab'
        $galleryToggle = $script:window.FindFirst([Windows.Automation.TreeScope]::Descendants,
            (New-Object Windows.Automation.PropertyCondition([Windows.Automation.AutomationElement]::AutomationIdProperty,'ExclusiveGallery')))
        Assert ($null -eq $galleryToggle) 'The obsolete gallery preservation option is still offered'
        Assert ((Element 'GalleryPolicy').Current.Name.Contains('no image backups')) 'Settings does not explain clock-only gallery cleanup'
        Assert ((Element 'Validation').Current.Name -eq 'Preview reflects your saved settings.') 'The settings preview failed'
        Select-Item 'AppearanceTab'
        Capture 'settings'
        Close-Settings
        $result = [ordered]@{assertions=$script:assertions;settingsOpenMs=$openMs;desktopLocked=$desktopLocked;removedLockOption=$true}
        $result | ConvertTo-Json | Set-Content "$outputDir\modern-ui-inspection.json"
        $result | ConvertTo-Json
        return
    }
    $theme = Element 'Theme'; $theme.SetFocus()
    (Pattern $theme 'ExpandCollapse').Expand(); Escape; Start-Sleep -Milliseconds 200
    $script:uiProcess.Refresh()
    Assert (!$script:uiProcess.HasExited) 'Escape in a dropdown closed settings and discarded the draft'
    Assert ((Pattern $theme 'ExpandCollapse').Current.ExpandCollapseState -eq [Windows.Automation.ExpandCollapseState]::Collapsed) 'Escape did not close the dropdown'
    Select-Combo 'Theme' 'Pure black'; Select-Item 'ClockTab'; Select-Item 'Analog'; Select-Item 'AppearanceTab'
    Assert (!(Visible 'BackgroundDither')) 'Pure black shows irrelevant gradient smoothing'
    Assert ((Element 'AnalogFontSize').Current.IsEnabled) 'Analog numeral size is disabled'
    (Pattern (Element 'AnalogFontSize') 'RangeValue').SetValue(28)
    Assert ((Element 'Font').Current.IsEnabled) 'Analog font is disabled'
    $second = Start-Process -FilePath $Exe -WindowStyle Hidden -PassThru
    Assert ($second.WaitForExit(5000)) 'A second tray engine started'
    Assert ((Get-Process AnkerClock.Settings).Count -eq 1) 'A second settings window started'
    Capture 'modern-appearance'
    Select-Combo 'Theme' 'Aurora'; Wait-For { Visible 'BackgroundDither' } 'Gradient controls did not appear'
    Assert ((Element 'BackgroundDither').Current.IsEnabled) 'Gradient smoothing is disabled for a gradient'
    (Pattern (Element 'BackgroundDither') 'RangeValue').SetValue(6)
    Select-Combo 'Theme' 'Pure black'
    Font-Picker
    Set-Text 'FontSearch' 'Arial'
    Wait-For { (Element 'FontPageStatus').Current.Name -like '*fonts' -and $null -ne (Font-Item 'Arial') } 'Font filtering failed'
    (Pattern (Font-Item 'Arial') 'SelectionItem').Select()
    Wait-For { (Element 'UseFont').Current.IsEnabled } 'Clock font preview did not render'
    Capture 'font-gallery' ([ClockUi]::FontPicker())
    Compare-Font 'Arial'; Compare-Font 'Arial Black'
    Assert ((Element 'CompareFonts').Current.IsEnabled) 'Two pinned fonts did not enable comparison'
    Invoke 'CompareFonts'; Assert (Visible 'FontComparison') 'Font comparison did not open'
    Capture 'font-comparison' ([ClockUi]::FontPicker())
    Invoke 'CompareFonts'
    (Pattern (Font-Item 'Arial') 'SelectionItem').Select()
    Invoke 'UseFont'; $script:window = $script:settingsWindow
    Wait-For { [ClockUi]::FontPicker() -eq [IntPtr]::Zero } 'Font browser did not close'
    Start-Sleep -Milliseconds 100
    Wait-For { (Element 'Font').Current.HelpText -eq 'Arial' } 'Font selection did not return to Settings'
    Select-Combo 'FontStyle' 'Bold'; Save
    Select-Item 'ClockTab'; Select-Item 'Digital'
    Assert ((Element 'ShowCodexUsage').Current.IsEnabled) 'Digital mode disables usage on the lock display'
    Set-Switch 'ShowCodexUsage' $true
    Assert ((Element 'CodexRefreshSeconds').Current.IsEnabled) 'Digital mode disables locked usage refresh controls'
    Assert ((Element 'ShowDate').Current.IsEnabled) 'Locked usage hides the digital date option'
    Set-Text 'CodexRefreshSeconds' '59'
    Wait-For { (Element 'Validation').Current.Name -like 'Codex refresh must*' } 'Invalid locked usage interval was not rejected'
    Assert ((Element 'Save').Current.IsEnabled -eq $false) 'Invalid locked usage interval can be saved'
    Set-Text 'CodexRefreshSeconds' '60'
    Select-Item 'NetatmoTab'
    Assert ((Element 'NetatmoDisplayStatus').Current.Name.Contains('this clock face')) 'Weather diagnostics omit the unsupported face reason'
    Assert (Visible 'UseAnalog') 'Digital mode does not explain how to enable station readings'
    Assert (!(Visible 'NetatmoModule')) 'Digital exposes unused station display controls'
    Select-Item 'ClockTab'
    Select-Item 'AppearanceTab'
    Assert ((Element 'FontSize').Current.IsEnabled) 'Digital font size is disabled'
    Select-Combo 'FontStyle' 'Regular'
    (Pattern (Element 'AppearancePanel') 'Scroll').SetScrollPercent(-1,100)
    Set-Text 'TextColor' '#XYZXYZ'
    Wait-For { (Element 'Validation').Current.Name -like 'Use a six-digit*' } 'Invalid color was not rejected'
    Assert ((Element 'Save').Current.IsEnabled -eq $false) 'Invalid color can be saved'
    Set-Text 'TextColor' '#E8D9B7'; Set-Text 'AccentColor' '#B7A3F4'; Save
    Select-Item 'ClockTab'; Set-Text 'Label' 'Час · 東京'; Set-Switch 'ShowSeconds' $true; Select-Item 'Analog'; Save
    Assert (!(Visible 'DateFormat')) 'Analog exposes an unused date format'
    Assert (!(Visible 'VerticalOffset')) 'Analog exposes an unused position slider'
    Set-Switch 'ShowCodexUsage' $true
    Assert ((Element 'ShowDate').Current.IsEnabled) 'Codex incorrectly disables the date option'
    Set-Text 'CodexRefreshSeconds' '59'
    Wait-For { (Element 'Validation').Current.Name -like 'Codex refresh must*' } 'Invalid Codex interval was not rejected'
    Assert ((Element 'Save').Current.IsEnabled -eq $false) 'Invalid Codex interval can be saved'
    Set-Text 'CodexRefreshSeconds' '120'; Save
    Set-Switch 'ShowCodexUsage' $false
    Assert ((Element 'ShowDate').Current.IsEnabled) 'Disabling Codex did not restore the date option'
    Assert (!(Visible 'CodexRefreshSeconds')) 'Disabled Codex still shows refresh controls'
    Save
    Set-Switch 'ShowCodexUsage' $true; Save
    Capture 'modern-clock'
    Select-Item 'DeviceTab'; Set-Text 'Interval' '86401'
    Wait-For { (Element 'Validation').Current.Name -like 'Update interval must*' } 'Invalid interval was not rejected'
    Assert ((Element 'Save').Current.IsEnabled -eq $false) 'Invalid interval can be saved'
    Set-Text 'Interval' '0'; Start-Sleep -Milliseconds 250
    Assert ((Element 'Save').Current.IsEnabled -eq $false) 'Zero interval can be saved'
    Set-Text 'Interval' '17'; Save; Capture 'modern-device'
    Set-Text 'Interval' '0'
    Wait-For { (Visible 'ReviewError') } 'Invalid input did not expose error navigation'
    Select-Item 'AppearanceTab'; Invoke 'ReviewError'
    Assert ((Element 'Interval').Current.HasKeyboardFocus) 'Review did not focus the invalid setting'
    Set-Text 'Interval' '17'
    Wait-For { !(Visible 'ReviewError') } 'Corrected input left an error link'
    Select-Item 'StartupTab'; Set-Switch 'PauseOnBattery' $true; Save; Capture 'modern-startup'
    $removedOption = $script:window.FindFirst([Windows.Automation.TreeScope]::Descendants,
        (New-Object Windows.Automation.PropertyCondition([Windows.Automation.AutomationElement]::AutomationIdProperty,'PauseWhenLocked')))
    Assert ($null -eq $removedOption) 'The removed lock-pause option is still visible'
    Close-Settings; Open-Settings
    Select-Item 'ClockTab'; Select-Combo 'AnalogFace' 'Soft Frame'
    Select-Item 'AppearanceTab'
    if ($backup.ContainsKey('ShowNetatmo') -and $backup['ShowNetatmo'][0]) {
        Assert (!(Visible 'AnalogFontSize')) 'Soft Frame exposes unused numeral size'
        Select-Item 'ClockTab'; Set-Switch 'ShowDate' $false
        Select-Item 'AppearanceTab'; Assert (!(Visible 'Font')) 'Soft Frame without a date exposes unused typography'
        Select-Item 'ClockTab'; Set-Switch 'ShowDate' $true
    }
    Select-Item 'ClockTab'; Select-Combo 'AnalogFace' 'Quiet Classic'
    Wait-For { (Element 'Validation').Current.Name -ne 'Unsaved changes' } 'Reverted face left a stale dirty state'
    Select-Item 'DeviceTab'; Assert ((Text 'Interval') -eq '17') 'Interval did not persist'
    Select-Item 'ClockTab'; Assert ((Get-ItemProperty 'HKCU:\Software\AnkerClock').Label -eq 'Час · 東京') 'Unicode heading did not persist'
    Assert (!(Visible 'Use24Hour')) 'Analog still shows irrelevant hour format'
    Assert ((Text 'CodexRefreshSeconds') -eq '120') 'Codex refresh interval did not persist'
    Assert ((Pattern (Element 'ShowCodexUsage') 'Toggle').Current.ToggleState -eq [Windows.Automation.ToggleState]::On) 'Codex display flag did not persist'
    Select-Item 'AppearanceTab'
    Assert ((Pattern (Element 'AnalogFontSize') 'RangeValue').Current.Value -eq 28) 'Analog numeral size did not persist'
    Assert ((Get-ItemProperty 'HKCU:\Software\AnkerClock').BackgroundDither -eq 6) 'Hidden gradient smoothing preference did not persist'
    $key = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey($keyPath)
    Assert ($key.GetValue('Layout') -eq 2) 'Layout did not reach native preferences'
    Assert ($key.GetValue('AnalogFontSize') -eq 28) 'Analog size did not reach native preferences'
    Assert ($key.GetValue('BackgroundDither') -eq 6) 'Gradient smoothing did not reach native preferences'
    Assert ($key.GetValue('TextColor') -eq 0xB7D9E8) 'Text color did not reach native preferences'
    Assert ($key.GetValue('AccentColor') -eq 0xF4A3B7) 'Accent color did not reach native preferences'; $key.Dispose()
    if ($Hardware) {
        Select-Combo 'Theme' 'Graphite'; Save
        Select-Item 'ClockTab'; Select-Combo 'TimeZone' 'Windows time zone (automatic)'
        Select-Item 'StartupTab'; Set-Switch 'PauseOnBattery' $false
        Select-Item 'DeviceTab'; Set-Text 'Interval' '86400'; Invoke 'UpdateNow'
        Wait-For { [ClockUi]::Status().StartsWith('Paused · updated ') } 'Manual update failed' 15000
        $stamp = [TimeSpan]::Parse([ClockUi]::Status().Substring('Paused · updated '.Length))
        $difference = [Math]::Abs(([DateTime]::Now.TimeOfDay-$stamp).TotalSeconds)
        Assert ([Math]::Min($difference,86400-$difference) -lt 10) 'Update now rendered the start of the interval instead of the current time'
        Assert ((Element 'Pause').Current.Name -eq 'Resume') 'Manual update resumed the worker'
        & (Join-Path (Split-Path $Exe) 'AnkerClockTests.exe') --probe $outputDir
        Assert ($LASTEXITCODE -eq 0) 'Paused update did not release USB'
        Invoke 'Pause'
        Wait-For { [ClockUi]::Status().StartsWith('Connected · updated ') } 'Resume waited for a day boundary instead of showing the clock' 15000
        Invoke 'Pause'; Wait-For { [ClockUi]::Status() -eq 'Updates paused' } 'Long-interval pause did not finish'
        Invoke 'Interval10'; Save
        Invoke 'Pause'; $scheduled = @(); $deadline = [Diagnostics.Stopwatch]::StartNew()
        while ($scheduled.Count -lt 3 -and $deadline.Elapsed.TotalSeconds -lt 45) {
            $status = [ClockUi]::Status()
            if ($status -match '^Connected · updated (\d\d:\d\d:\d\d)\nSent ([\d.]+) s before boundary$' -and
                ($scheduled.Count -eq 0 -or $scheduled[-1].stamp -ne $Matches[1])) {
                $scheduled += [pscustomobject]@{stamp=$Matches[1];lead=[double]$Matches[2];observedAt=[DateTimeOffset]::Now.ToString('o')}
            }
            Start-Sleep -Milliseconds 100
        }
        Assert ($scheduled.Count -eq 3) 'Three scheduled updates did not complete'
        for ($i=0; $i -lt $scheduled.Count; $i++) {
            $time = [TimeSpan]::Parse($scheduled[$i].stamp)
            Assert (($time.Seconds % 10) -eq 0) 'Update was not aligned to a 10-second boundary'
            Assert ($scheduled[$i].lead -ge 0.45 -and $scheduled[$i].lead -le 0.55) 'Selection was not 0.5 seconds early'
            if ($i -gt 0) { Assert (((($time-[TimeSpan]::Parse($scheduled[$i-1].stamp)).TotalSeconds+86400) % 86400) -eq 10) 'An update interval was skipped' }
        }
        Invoke 'Pause'; Wait-For { [ClockUi]::Status() -eq 'Updates paused' } 'Pause did not finish'
        & (Join-Path (Split-Path $Exe) 'AnkerClockTests.exe') --capture-current $outputDir
        Assert ($LASTEXITCODE -eq 0) 'Hub image CRC or dimensions differ'
        $scheduled | ConvertTo-Json | Set-Content "$outputDir\modern-hub-timing.json"
    }
    Close-Settings
    for ($i=0; $i -lt 3; $i++) { Open-Settings; Close-Settings }
    Assert (!(Get-Process AnkerClock.Settings -ErrorAction SilentlyContinue)) 'A UI process remained after reopen cycles'
    Open-Settings; Select-Item 'DeviceTab'; Set-Text 'Interval' '19'
    Wait-For { (Element 'Validation').Current.Name -eq 'Unsaved changes' } 'Draft change was not detected'
    Invoke 'Close'
    Wait-For { [ClockUi]::UnsavedPrompt() -ne [IntPtr]::Zero } 'Closing discarded an unsaved draft'
    [ClockUi]::PostMessage([ClockUi]::UnsavedPrompt(),0x111,[IntPtr]2,[IntPtr]::Zero) | Out-Null
    Wait-For { [ClockUi]::UnsavedPrompt() -eq [IntPtr]::Zero } 'Cancel did not dismiss the close prompt'
    Assert (!$script:uiProcess.HasExited) 'Cancel closed Settings'
    Assert ((Text 'Interval') -eq '19') 'Cancel lost the unsaved draft'
    Assert ((Get-ItemProperty 'HKCU:\Software\AnkerClock').Interval -eq 17) 'Cancel unexpectedly saved the draft'
    Select-Item 'AppearanceTab'; (Pattern (Element 'AppearancePanel') 'Scroll').SetScrollPercent(-1,0)
    Font-Picker
    [ClockUi]::PostMessage([ClockUi]::Controller(),0x111,[IntPtr]4004,[IntPtr]::Zero) | Out-Null
    Assert ($process.WaitForExit(5000)) 'Tray shutdown did not finish'
    Assert ($script:uiProcess.WaitForExit(5000)) 'Tray shutdown left a font dialog or close prompt resident'
    $result = [ordered]@{assertions=$script:assertions;settingsOpenMs=$openMs;hardware=[bool]$Hardware;framework='WPF on demand'}
    $result | ConvertTo-Json | Set-Content "$outputDir\modern-ui-results.json"; $result | ConvertTo-Json
} finally {
    if ($process) { [ClockUi]::PostMessage([ClockUi]::Controller(),0x111,[IntPtr]4004,[IntPtr]::Zero) | Out-Null }
    if ($process -and !$process.HasExited) { if (!$process.WaitForExit(5000)) { Stop-Process -Id $process.Id } }
    if ($hadPreferences) {
        $key = [Microsoft.Win32.Registry]::CurrentUser.CreateSubKey($keyPath)
        foreach ($name in $key.GetValueNames()) { if ($name -ne 'PendingFrame' -and !$backup.ContainsKey($name)) { $key.DeleteValue($name) } }
        foreach ($name in $backup.Keys) { $key.SetValue($name,$backup[$name][0],$backup[$name][1]) }; $key.Dispose()
    } else {
        $key = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey($keyPath,$true)
        if ($key) { foreach ($name in $key.GetValueNames()) { if ($name -ne 'PendingFrame') { $key.DeleteValue($name) } }; $key.Dispose() }
    }
    $runKey = [Microsoft.Win32.Registry]::CurrentUser.CreateSubKey($runPath)
    if ($null -eq $runValue) { $runKey.DeleteValue('AnkerClock',$false) } else { $runKey.SetValue('AnkerClock',$runValue,$runKind) }
    $runKey.Dispose()
}
