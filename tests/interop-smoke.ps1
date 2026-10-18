param([string]$SettingsExe = "$PSScriptRoot\..\build\Release\AnkerClock.Settings.exe")
$ErrorActionPreference = 'Stop'
$assembly = [Reflection.Assembly]::LoadFrom([IO.Path]::GetFullPath($SettingsExe))
$wireType = $assembly.GetType('AnkerClock.SettingsWire', $true)
$nativeType = $assembly.GetType('AnkerClock.Native', $true)
$flags = [Reflection.BindingFlags]'Static,NonPublic'
$write = $nativeType.GetMethod('WriteSettings', $flags)
$read = $nativeType.GetMethod('ReadSettings', $flags)
$settings = [Activator]::CreateInstance($wireType)
$numbers = @('Version','Theme','FontSize','FontStyle','Layout','DateFormat','Interval','Brightness',
    'VerticalOffset','BackgroundFit','TextColor','AccentColor','ImageDim','Flags','StartWindows','AnalogFontSize',
    'CodexRefreshSeconds','BackgroundDither','NetatmoRefreshSeconds','NetatmoReadings','AnalogFace','ReadingFontSize')
for ($i = 0; $i -lt $numbers.Length; $i++) { $wireType.GetField($numbers[$i]).SetValue($settings, [int]($i + 1)) }
$texts = @(
    @{Name='Font'; Text='Georgia'; Offset=64; Capacity=128},
    @{Name='BackgroundPath'; Text=''; Offset=320; Capacity=32768},
    @{Name='TimeZone'; Text='UTC'; Offset=65856; Capacity=128},
    @{Name='Label'; Text='Clock'; Offset=66112; Capacity=41},
    @{Name='NetatmoModule'; Text='station'; Offset=66212; Capacity=128}
)
foreach ($field in $texts) { $wireType.GetField($field.Name).SetValue($settings, $field.Text) }
$size = 66476
$buffer = [Runtime.InteropServices.Marshal]::AllocHGlobal($size)
$assertions = 0
function Assert($condition, [string]$message) { if (!$condition) { throw $message }; $script:assertions++ }
try {
    $bytes = New-Object byte[] $size
    for ($i = 0; $i -lt $size; $i++) { $bytes[$i] = 0xCC }
    [Runtime.InteropServices.Marshal]::Copy($bytes, 0, $buffer, $size)
    $write.Invoke($null, @($settings, $buffer)) | Out-Null
    [Runtime.InteropServices.Marshal]::Copy($buffer, $bytes, 0, $size)
    $roundTrip = $read.Invoke($null, @($buffer))
    foreach ($name in $numbers) {
        Assert ($wireType.GetField($name).GetValue($roundTrip) -eq $wireType.GetField($name).GetValue($settings)) "Wire changed $name"
    }
    foreach ($field in $texts) {
        Assert ($wireType.GetField($field.Name).GetValue($roundTrip) -eq $field.Text) "Wire changed $($field.Name)"
        $dirty = $false
        for ($i = $field.Offset + ($field.Text.Length * 2); $i -lt $field.Offset + ($field.Capacity * 2); $i++) {
            if ($bytes[$i] -ne 0) { $dirty = $true; break }
        }
        Assert (!$dirty) "Wire exposed uninitialized bytes after $($field.Name)"
    }
    Assert ($bytes[66194] -eq 0 -and $bytes[66195] -eq 0) 'Wire exposed uninitialized alignment padding'
    $wireType.GetField('Font').SetValue($settings, ('x' * 128))
    $rejected = $false
    try { $write.Invoke($null, @($settings, $buffer)) | Out-Null }
    catch { $rejected = $_.Exception.GetBaseException() -is [ArgumentException] }
    Assert $rejected 'Wire accepted a font longer than its fixed buffer'
    Add-Type @'
using System; using System.Runtime.InteropServices;
public static class PreviewCom {
 [DllImport("ole32.dll")] public static extern int CoInitializeEx(IntPtr reserved,uint mode);
 [DllImport("ole32.dll")] public static extern void CoUninitialize();
}
'@
    $com = [PreviewCom]::CoInitializeEx([IntPtr]::Zero,2)
    Assert ($com -ge 0) 'Preview COM initialization failed'
    $preview = [IntPtr]::Zero
    try {
        $errorText = [Text.StringBuilder]::new(512)
        $load = $nativeType.GetMethods($flags) | Where-Object {$_.Name -eq 'clock_settings_load' -and $_.GetParameters()[0].ParameterType.IsByRef}
        $arguments = [object[]]@($null,$errorText,512)
        Assert ($load.Invoke($null,$arguments) -ne 0) 'Could not load preview settings'
        $frame = $arguments[0]
        $diagnostics = $nativeType.GetMethod('NetatmoDiagnostics',$flags).Invoke($null,@($frame))
        Assert ($diagnostics.StartsWith('Weather ')) 'Weather diagnostics did not cross the native boundary'
        Assert ($diagnostics.Contains('Last successful fetch:')) 'Weather diagnostics lost its timestamp across the native boundary'
        $wireType.GetField('Theme').SetValue($frame,6)
        $wireType.GetField('Font').SetValue($frame,'Arial')
        $wireType.GetField('FontStyle').SetValue($frame,0)
        $preview = $nativeType.GetMethod('clock_preview_open',$flags).Invoke($null,@($errorText,512))
        Assert ($preview -ne [IntPtr]::Zero) 'Could not open font preview'
        $render = $nativeType.GetMethod('RenderFontPreview',$flags)
        $timestamp = [DateTime]::UtcNow.ToFileTimeUtc()
        $firstImage = [byte[]]::new(65520)
        $secondImage = [byte[]]::new(65520)
        $firstCount = $render.Invoke($null,[object[]]@($preview,$frame,$timestamp,$firstImage,$errorText))
        $secondCount = $render.Invoke($null,[object[]]@($preview,$frame,$timestamp,$secondImage,$errorText))
        Assert ($firstCount -gt 1000 -and $firstCount -eq $secondCount) 'Fixed-time font preview failed'
        $same = $true
        for ($i=0; $i -lt $firstCount; $i++) { if ($firstImage[$i] -ne $secondImage[$i]) { $same=$false; break } }
        Assert $same 'Font comparison does not preserve the same frame'
        Assert ($firstImage[0] -eq 255 -and $firstImage[1] -eq 216) 'Font preview was not a JPEG'
    } finally {
        if ($preview -ne [IntPtr]::Zero) { $nativeType.GetMethod('clock_preview_close',$flags).Invoke($null,@($preview)) | Out-Null }
        [PreviewCom]::CoUninitialize()
    }
    "$assertions interop assertions passed"
} finally { [Runtime.InteropServices.Marshal]::FreeHGlobal($buffer) }
