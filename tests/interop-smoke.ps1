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
    'CodexRefreshSeconds','BackgroundDither')
for ($i = 0; $i -lt $numbers.Length; $i++) { $wireType.GetField($numbers[$i]).SetValue($settings, [int]($i + 1)) }
$texts = @(
    @{Name='Font'; Text='Georgia'; Offset=64; Capacity=128},
    @{Name='BackgroundPath'; Text=''; Offset=320; Capacity=32768},
    @{Name='TimeZone'; Text='UTC'; Offset=65856; Capacity=128},
    @{Name='Label'; Text='Clock'; Offset=66112; Capacity=41}
)
foreach ($field in $texts) { $wireType.GetField($field.Name).SetValue($settings, $field.Text) }
$size = 66204
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
    "$assertions interop assertions passed"
} finally { [Runtime.InteropServices.Marshal]::FreeHGlobal($buffer) }
