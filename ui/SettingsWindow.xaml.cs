using Microsoft.Win32;
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Input;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Threading;

namespace AnkerClock
{
    public partial class SettingsWindow : Window
    {
        private readonly IntPtr _controller;
        private readonly byte[] _image = new byte[65520];
        private readonly DispatcherTimer _debounce = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(180) };
        private readonly DispatcherTimer _statusTimer = new DispatcherTimer { Interval = TimeSpan.FromSeconds(1) };
        private SettingsWire _saved;
        private IntPtr _preview, _window;
        private bool _ready, _paused;
        private bool _hasEdits;
        private string _fontName;
        private Control _invalidControl;
        private long _previewPeriod;
        private int _face;
        private string _netatmoOptions;
        private long _runtimeFlags;
        private string _netatmoRequestError;

        public SettingsWindow()
        {
            _controller = Native.Controller();
            if (_controller == IntPtr.Zero)
                throw new InvalidOperationException("Open AnkerClock.exe to start the clock and its settings.");
            var error = new StringBuilder(512);
            if (Native.clock_settings_load(out _saved, error, error.Capacity) == 0)
                throw new InvalidOperationException(error.ToString());
            InitializeComponent();
            ThemeChoice.ItemsSource = new[] { "Midnight", "Aurora", "Copper", "Ocean", "Graphite", "Custom image", "Pure black" };
            FitChoice.ItemsSource = new[] { "Fill screen (crop)", "Fit entire image" };
            DateChoice.ItemsSource = new[] { "Day and month", "Weekday, day and month", "Year-month-day", "Month/day/year" };
            AnalogFaceChoice.ItemsSource = new[] { "Quiet Classic", "Precision", "Soft Frame" };
            var zones = TimeZoneInfo.GetSystemTimeZones().Select(z => new Zone { Key = z.Id, Name = z.DisplayName }).ToList();
            zones.Insert(0, new Zone { Key = "", Name = "Windows time zone (automatic)" });
            ZoneChoice.ItemsSource = zones;
            LoadControls();
            _debounce.Tick += (sender, e) => { _debounce.Stop(); RefreshPreview(); };
            _statusTimer.Tick += (sender, e) => RefreshStatus();
            AddHandler(Selector.SelectionChangedEvent, new SelectionChangedEventHandler(Changed));
            AddHandler(TextBox.TextChangedEvent, new TextChangedEventHandler(Changed));
            AddHandler(ToggleButton.CheckedEvent, new RoutedEventHandler(Changed));
            AddHandler(ToggleButton.UncheckedEvent, new RoutedEventHandler(Changed));
            AddHandler(RangeBase.ValueChangedEvent, new RoutedPropertyChangedEventHandler<double>(Changed));
        }

        private sealed class Zone { public string Key { get; set; } public string Name { get; set; } }
        private sealed class WeatherModule
        {
            public string Key { get; set; }
            public string Name { get; set; }
            public int Readings { get; set; }
        }

        private void LoadControls()
        {
            ThemeChoice.SelectedIndex = _saved.Theme;
            _fontName = _saved.Font;
            FontChoice.Content = _fontName;
            LoadFontStyles(_saved.FontStyle);
            FitChoice.SelectedIndex = _saved.BackgroundFit;
            DateChoice.SelectedIndex = _saved.DateFormat;
            ZoneChoice.SelectedValue = _saved.TimeZone;
            SizeSlider.Value = _saved.FontSize;
            AnalogSizeSlider.Value = _saved.AnalogFontSize;
            AnalogFaceChoice.SelectedIndex = _saved.AnalogFace;
            ReadingSizeSlider.Value = _saved.ReadingFontSize;
            DimSlider.Value = _saved.ImageDim;
            DitherSlider.Value = _saved.BackgroundDither;
            OffsetSlider.Value = _saved.VerticalOffset;
            BrightnessSlider.Value = _saved.Brightness;
            IntervalBox.Text = _saved.Interval.ToString(CultureInfo.InvariantCulture);
            CodexIntervalBox.Text = _saved.CodexRefreshSeconds.ToString(CultureInfo.InvariantCulture);
            NetatmoIntervalBox.Text = _saved.NetatmoRefreshSeconds.ToString(CultureInfo.InvariantCulture);
            var readings = new[] { TemperatureSwitch, HumiditySwitch, Co2Switch, NoiseSwitch };
            for (int i = 0; i < readings.Length; i++) readings[i].IsChecked = (_saved.NetatmoReadings & (1 << i)) != 0;
            LabelBox.Text = _saved.Label;
            TextColorBox.Text = ColorHex(_saved.TextColor);
            AccentColorBox.Text = ColorHex(_saved.AccentColor);
            ImagePath.Text = _saved.BackgroundPath;
            ImagePath.ToolTip = _saved.BackgroundPath;
            _face = _saved.Layout;
            (new[] { SignatureFace, DigitalFace, AnalogFace })[_face].IsChecked = true;
            var switches = new[] { HourSwitch, SecondsSwitch, DateSwitch, WeekdaySwitch, BatterySwitch, LaunchSwitch, CodexSwitch, NetatmoSwitch };
            var bits = new[] { 1, 2, 4, 8, 16, 64, 256, 512 };
            for (int i = 0; i < switches.Length; i++) switches[i].IsChecked = (_saved.Flags & bits[i]) != 0;
            StartupSwitch.IsChecked = _saved.StartWindows != 0;
            LoadNetatmoModules();
            UpdateAvailability();
        }

        private void InitializeWindow(object sender, EventArgs e)
        {
            _window = new WindowInteropHelper(this).Handle;
            HwndSource.FromHwnd(_window).AddHook(HandleControllerMessage);
            if (Native.SendMessage(_controller, Native.Register, _window, IntPtr.Zero) != (IntPtr)1)
            {
                MessageBox.Show(this, "Open settings from the Anker Clock tray icon.", "Anker Clock");
                Close();
                return;
            }
            int dark = 1, rounded = 2;
            Native.DwmSetWindowAttribute(_window, 20, ref dark, sizeof(int));
            Native.DwmSetWindowAttribute(_window, 33, ref rounded, sizeof(int));
            var error = new StringBuilder(512);
            _preview = Native.clock_preview_open(error, error.Capacity);
            if (_preview == IntPtr.Zero)
            {
                MessageBox.Show(this, error.ToString(), "Anker Clock");
                Close();
                return;
            }
            _ready = true;
            RefreshPreview();
            RefreshStatus();
            _statusTimer.Start();
        }

        private void ReleaseWindow(object sender, EventArgs e)
        {
            _ready = false;
            _debounce.Stop();
            _statusTimer.Stop();
            if (_preview != IntPtr.Zero) Native.clock_preview_close(_preview);
            _preview = IntPtr.Zero;
        }

        private IntPtr HandleControllerMessage(IntPtr window, int message, IntPtr sender, IntPtr parameter, ref bool handled)
        {
            if (message == Native.Shutdown && sender == _controller)
            {
                _hasEdits = false;
                _ready = false;
                Close();
                handled = true;
            }
            return IntPtr.Zero;
        }

        private void Navigate(object sender, RoutedEventArgs e)
        {
            if (AppearancePanel == null) return;
            int tab = int.Parse((string)((RadioButton)sender).Tag, CultureInfo.InvariantCulture);
            var panels = new[] { AppearancePanel, ClockPanel, DevicePanel, StartupPanel, NetatmoPanel };
            for (int i = 0; i < panels.Length; i++) panels[i].Visibility = i == tab ? Visibility.Visible : Visibility.Collapsed;
        }

        private void Changed(object sender, RoutedEventArgs e)
        {
            if (_ready == false) return;
            if (e.OriginalSource is RadioButton navigation && navigation.GroupName == "Navigation") return;
            _hasEdits = true;
            if (e.OriginalSource is RadioButton face && (string)face.GroupName == "Face" && face.IsChecked == true)
                _face = int.Parse((string)face.Tag, CultureInfo.InvariantCulture);
            UpdateAvailability();
            _debounce.Stop();
            _debounce.Start();
        }

        private void UpdateAvailability()
        {
            bool analog = _face == 2;
            bool weather = analog && NetatmoSwitch.IsChecked == true;
            bool numerals = analog && AnalogFaceChoice.SelectedIndex != 2;
            bool usageInset = analog && weather == false && CodexSwitch.IsChecked == true;
            bool heading = weather == false;
            bool fontUsed = analog == false || numerals || DateSwitch.IsChecked == true ||
                (heading && (string.IsNullOrWhiteSpace(LabelBox.Text) == false || WeekdaySwitch.IsChecked == true || CodexSwitch.IsChecked == true));
            DigitalSizeOptions.Visibility = analog ? Visibility.Collapsed : Visibility.Visible;
            AnalogSizeOptions.Visibility = numerals || usageInset ? Visibility.Visible : Visibility.Collapsed;
            AnalogSizeName.Text = numerals ? "Numerals" : "Usage scale";
            AnalogSizeUnit.Text = numerals ? " px" : "";
            AnalogFaceOptions.Visibility = analog ? Visibility.Visible : Visibility.Collapsed;
            TypographyOptions.Visibility = fontUsed ? Visibility.Visible : Visibility.Collapsed;
            FontStyleOptions.Visibility = StyleChoice.Items.Count > 1 &&
                (analog == false || numerals || DateSwitch.IsChecked == true || usageInset) ? Visibility.Visible : Visibility.Collapsed;
            OffsetOptions.Visibility = analog ? Visibility.Collapsed : Visibility.Visible;
            HourSwitch.Visibility = analog ? Visibility.Collapsed : Visibility.Visible;
            SecondsSwitch.Content = analog ? "Show second hand" : "Show seconds";
            DateFormatOptions.Visibility = analog == false && DateSwitch.IsChecked == true ? Visibility.Visible : Visibility.Collapsed;
            HeadingOptions.Visibility = heading ? Visibility.Visible : Visibility.Collapsed;
            WeekdaySwitch.Visibility = string.IsNullOrWhiteSpace(LabelBox.Text) ? Visibility.Visible : Visibility.Collapsed;
            CodexIntervalBox.IsEnabled = CodexSwitch.IsChecked == true;
            CodexOptions.Visibility = CodexSwitch.IsChecked == true ? Visibility.Visible : Visibility.Collapsed;
            CodexSwitch.Content = analog ? "Show Codex usage" : "Show Codex usage when locked";
            CodexHint.Text = analog ? "Shows the lowest remaining allowance. Works while locked, too." : "Usage appears while Windows is locked.";
            AnalogTypographyHint.Visibility = analog ? Visibility.Visible : Visibility.Collapsed;
            AnalogTypographyHint.Text = numerals ? "Font applies to the numerals and date." : "Font applies to the date and any heading or usage inset.";
            ImageOptions.Visibility = ThemeChoice.SelectedIndex == 5 ? Visibility.Visible : Visibility.Collapsed;
            DitherOptions.Visibility = ThemeChoice.SelectedIndex == 6 ? Visibility.Collapsed : Visibility.Visible;
            NetatmoSwitch.Visibility = analog ? Visibility.Visible : Visibility.Collapsed;
            NetatmoStyleHint.Visibility = analog ? Visibility.Collapsed : Visibility.Visible;
            NetatmoOptions.Visibility = weather ? Visibility.Visible : Visibility.Collapsed;
            var module = NetatmoChoice.SelectedItem as WeatherModule;
            var readings = new[] { TemperatureSwitch, HumiditySwitch, Co2Switch, NoiseSwitch };
            int supported = module?.Readings ?? 0;
            int count = readings.Where((reading, index) => reading.IsChecked == true && (supported & (1 << index)) != 0).Count();
            for (int i = 0; i < readings.Length; i++)
            {
                bool available = (supported & (1 << i)) != 0;
                readings[i].Visibility = available ? Visibility.Visible : Visibility.Collapsed;
                readings[i].IsEnabled = weather && available && (CodexSwitch.IsChecked != true || count < 3 || readings[i].IsChecked == true);
                readings[i].ToolTip = readings[i].IsEnabled ? null : "Choose up to three readings with Codex usage.";
            }
            ReadingsHint.Text = module == null ? "Connect your station and choose a module." :
                (CodexSwitch.IsChecked == true ? "Choose up to three readings with Codex usage." : "Only readings supported by this module are shown.");
            FaceSummary.Text = analog ? "Analog · " + AnalogFaceChoice.SelectedItem : (_face == 0 ? "Signature" : "Digital");
            UpdateNetatmoDiagnostics();
        }

        private void LoadFontStyles(int preferred)
        {
            bool ready = _ready;
            _ready = false;
            var styles = FontCatalog.Styles(_fontName);
            StyleChoice.ItemsSource = styles;
            StyleChoice.SelectedValue = styles.Any(style => style.Key == preferred) ? preferred : styles[0].Key;
            _ready = ready;
        }

        private void ChooseFont(object sender, RoutedEventArgs e)
        {
            if (Collect(out var draft) == false) { ReviewError(sender, e); return; }
            try
            {
                var picker = new FontPickerWindow(draft) { Owner = this };
                if (picker.ShowDialog() != true) return;
                _fontName = picker.SelectedFont;
            }
            catch (Exception error) { Invalid(error.Message, FontChoice); return; }
            FontChoice.Content = _fontName;
            LoadFontStyles(draft.FontStyle);
            Changed(FontChoice, new RoutedEventArgs());
        }

        private void UseAnalog(object sender, RoutedEventArgs e) => AnalogFace.IsChecked = true;

        private bool Collect(out SettingsWire draft)
        {
            draft = _saved;
            if (int.TryParse(IntervalBox.Text, NumberStyles.None, CultureInfo.InvariantCulture, out int interval) == false || interval < 1 || interval > 86400)
                return Invalid("Update interval must be 1–86,400 seconds.", IntervalBox);
            IntervalBox.ClearValue(BorderBrushProperty);
            if (int.TryParse(CodexIntervalBox.Text, NumberStyles.None, CultureInfo.InvariantCulture, out int codexInterval) == false || codexInterval < 60 || codexInterval > 86400)
            {
                if (CodexSwitch.IsChecked == true)
                    return Invalid("Codex refresh must be 60–86,400 seconds.", CodexIntervalBox);
                codexInterval = _saved.CodexRefreshSeconds;
            }
            CodexIntervalBox.ClearValue(BorderBrushProperty);
            if (int.TryParse(NetatmoIntervalBox.Text, NumberStyles.None, CultureInfo.InvariantCulture, out int netatmoInterval) == false || netatmoInterval < 60 || netatmoInterval > 86400)
            {
                if (_face == 2 && NetatmoSwitch.IsChecked == true) return Invalid("Netatmo refresh must be 60–86,400 seconds.", NetatmoIntervalBox);
                netatmoInterval = _saved.NetatmoRefreshSeconds;
            }
            NetatmoIntervalBox.ClearValue(BorderBrushProperty);
            if (ParseColor(TextColorBox.Text, out int text) == false) return Invalid("Use a six-digit hex color, such as #F5F8FF.", TextColorBox);
            TextColorBox.ClearValue(BorderBrushProperty);
            if (ParseColor(AccentColorBox.Text, out int accent) == false) return Invalid("Use a six-digit hex color, such as #90B8FF.", AccentColorBox);
            AccentColorBox.ClearValue(BorderBrushProperty);
            draft.Theme = ThemeChoice.SelectedIndex;
            draft.Font = _fontName;
            draft.FontStyle = (int)StyleChoice.SelectedValue;
            draft.FontSize = (int)SizeSlider.Value;
            draft.AnalogFontSize = (int)AnalogSizeSlider.Value;
            draft.AnalogFace = AnalogFaceChoice.SelectedIndex;
            draft.ReadingFontSize = (int)ReadingSizeSlider.Value;
            draft.BackgroundFit = FitChoice.SelectedIndex;
            draft.DateFormat = DateChoice.SelectedIndex;
            draft.TimeZone = (string)ZoneChoice.SelectedValue ?? "";
            draft.ImageDim = (int)DimSlider.Value;
            draft.BackgroundDither = (int)DitherSlider.Value;
            draft.VerticalOffset = (int)OffsetSlider.Value;
            draft.Brightness = (int)BrightnessSlider.Value;
            draft.Layout = _face;
            draft.Interval = interval;
            draft.CodexRefreshSeconds = codexInterval;
            draft.NetatmoRefreshSeconds = netatmoInterval;
            draft.NetatmoModule = (string)NetatmoChoice.SelectedValue ?? _saved.NetatmoModule;
            draft.NetatmoReadings = 0;
            var weatherReadings = new[] { TemperatureSwitch, HumiditySwitch, Co2Switch, NoiseSwitch };
            for (int i = 0; i < weatherReadings.Length; i++) if (weatherReadings[i].IsChecked == true) draft.NetatmoReadings |= 1 << i;
            var weatherModule = NetatmoChoice.SelectedItem as WeatherModule;
            bool weather = _face == 2 && NetatmoSwitch.IsChecked == true;
            if (weather && weatherModule == null)
                return Invalid("Connect your station and choose a module.", NetatmoChoice);
            if (weather && weatherModule != null) draft.NetatmoReadings &= weatherModule.Readings;
            if (weather && draft.NetatmoReadings == 0)
                return Invalid("Choose at least one reading available on this module.", NetatmoChoice);
            if (draft.NetatmoReadings == 0) draft.NetatmoReadings = _saved.NetatmoReadings;
            int selectedReadings = draft.NetatmoReadings;
            int readingCount = Enumerable.Range(0, 4).Count(index => (selectedReadings & (1 << index)) != 0);
            if (weather && CodexSwitch.IsChecked == true && readingCount > 3)
                return Invalid("Choose up to three Netatmo readings while Codex is shown.", NetatmoChoice);
            TemperatureSwitch.ClearValue(BorderBrushProperty);
            draft.Label = LabelBox.Text.Trim();
            draft.TextColor = text;
            draft.AccentColor = accent;
            draft.BackgroundPath = ImagePath.Text;
            draft.StartWindows = StartupSwitch.IsChecked == true ? 1 : 0;
            draft.Flags = 0;
            var switches = new[] { HourSwitch, SecondsSwitch, DateSwitch, WeekdaySwitch, BatterySwitch, LaunchSwitch, CodexSwitch, NetatmoSwitch };
            var bits = new[] { 1, 2, 4, 8, 16, 64, 256, 512 };
            for (int i = 0; i < switches.Length; i++) if (switches[i].IsChecked == true) draft.Flags |= bits[i];
            if (draft.Theme == 5 && string.IsNullOrEmpty(draft.BackgroundPath)) return Invalid("Choose a background image first.", ThemeChoice);
            ThemeChoice.ClearValue(BorderBrushProperty);
            return true;
        }

        private bool Invalid(string message, Control control)
        {
            ValidationMessage.Text = message;
            ValidationMessage.Foreground = new SolidColorBrush(Color.FromRgb(255, 165, 156));
            _invalidControl?.ClearValue(BorderBrushProperty);
            control.BorderBrush = ValidationMessage.Foreground;
            _invalidControl = control;
            ReviewButton.Visibility = Visibility.Visible;
            SaveButton.IsEnabled = false;
            UpdateButton.IsEnabled = false;
            return false;
        }

        private void ReviewError(object sender, RoutedEventArgs e)
        {
            if (_invalidControl == null) return;
            var panels = new[] { AppearancePanel, ClockPanel, DevicePanel, StartupPanel, NetatmoPanel };
            var tabs = new[] { AppearanceTab, ClockTab, DeviceTab, StartupTab, NetatmoTab };
            for (int i = 0; i < panels.Length; i++)
                if (panels[i].IsAncestorOf(_invalidControl)) tabs[i].IsChecked = true;
            _invalidControl.BringIntoView();
            _invalidControl.Focus();
        }

        private void RefreshPreview()
        {
            if (_ready == false || Collect(out var draft) == false) return;
            var error = new StringBuilder(512);
            int count = Native.clock_preview_render(_preview, ref draft, _image, _image.Length, error, error.Capacity);
            if (count == 0) { Invalid(error.ToString(), ThemeChoice); return; }
            var bitmap = new BitmapImage();
            using (var stream = new MemoryStream(_image, 0, count, false))
            {
                bitmap.BeginInit();
                bitmap.CacheOption = BitmapCacheOption.OnLoad;
                bitmap.StreamSource = stream;
                bitmap.EndInit();
            }
            bitmap.Freeze();
            ClockPreview.Source = bitmap;
            _previewPeriod = DateTime.UtcNow.ToFileTimeUtc() / (draft.Interval * 10000000L);
            ValidationMessage.Foreground = (Brush)FindResource("Muted");
            SaveButton.ClearValue(BorderBrushProperty);
            bool dirty = draft.Equals(_saved) == false;
            _hasEdits = dirty;
            _invalidControl?.ClearValue(BorderBrushProperty);
            _invalidControl = null;
            ReviewButton.Visibility = Visibility.Collapsed;
            HideButton.Content = dirty ? "Discard & close" : "Hide to tray";
            UpdateButton.Content = dirty ? "Save & update" : "Update now";
            UpdateButton.IsEnabled = true;
            ValidationMessage.Text = dirty ? "Unsaved changes" : "Preview reflects your saved settings.";
            SaveButton.IsEnabled = dirty;
        }

        private void RefreshStatus()
        {
            if (Native.IsWindow(_controller) == false) { Close(); return; }
            string[] status = Native.Status(_controller).Split('\n');
            ConnectionStatus.Text = status[0];
            TimingStatus.Text = string.Join("\n", status.Skip(1));
            long flags = Native.SendMessage(_controller, Native.Query, IntPtr.Zero, IntPtr.Zero).ToInt64();
            _runtimeFlags = flags;
            _paused = (flags & 1) != 0;
            PauseButton.Content = _paused ? "Resume" : "Pause";
            ConnectionDot.Fill = new SolidColorBrush((flags & 2) != 0 ? Color.FromRgb(123, 212, 175) : Color.FromRgb(155, 165, 183));
            LoadNetatmoModules();
            RefreshNetatmoButton.IsEnabled = (flags & 4) == 0;
            if ((flags & 4) != 0) NetatmoStatus.Text = "Refreshing readings…";
            if (int.TryParse(IntervalBox.Text, out int interval) && interval > 0 && interval <= 86400 &&
                DateTime.UtcNow.ToFileTimeUtc() / (interval * 10000000L) != _previewPeriod)
                RefreshPreview();
        }

        private bool Save()
        {
            _debounce.Stop();
            if (Collect(out var draft) == false) return false;
            // Validate image decoding before changing the running clock's settings.
            var error = new StringBuilder(512);
            if (Native.clock_preview_render(_preview, ref draft, _image, _image.Length, error, error.Capacity) == 0)
                return Invalid(error.ToString(), ThemeChoice);
            if (Native.Save(_controller, _window, draft) == false)
                return Invalid("The settings could not be saved. Please try again.", SaveButton);
            _saved = draft;
            _hasEdits = false;
            RefreshPreview();
            ValidationMessage.Text = "Settings saved";
            return true;
        }

        private void SaveChanges(object sender, RoutedEventArgs e) => Save();
        private void UpdateNow(object sender, RoutedEventArgs e)
        {
            if (Save()) Native.PostMessage(_controller, 0x111, (IntPtr)4002, IntPtr.Zero);
        }
        private void TogglePause(object sender, RoutedEventArgs e)
        {
            Native.SendMessage(_controller, 0x111, (IntPtr)4003, IntPtr.Zero);
            RefreshStatus();
        }
        private void ChooseInterval(object sender, RoutedEventArgs e) => IntervalBox.Text = (string)((Button)sender).Tag;
        private void ChooseNetatmoInterval(object sender, RoutedEventArgs e) => NetatmoIntervalBox.Text = (string)((Button)sender).Tag;
        private void ImportNetatmo(object sender, RoutedEventArgs e)
        {
            var dialog = new OpenFileDialog { Filter = "Netatmo credentials|*.txt;*.json|All files|*.*", CheckFileExists = true, Title = "Import Netatmo credentials" };
            if (dialog.ShowDialog(this) != true) return;
            var error = new StringBuilder(512);
            if (Native.clock_netatmo_import(dialog.FileName, error, error.Capacity) == 0)
            {
                _netatmoRequestError = "Import failed: " + error;
                NetatmoStatus.Text = _netatmoRequestError;
                return;
            }
            NetatmoSwitch.IsChecked = true;
            RefreshNetatmo(sender, e);
        }
        private void RefreshNetatmo(object sender, RoutedEventArgs e)
        {
            _netatmoRequestError = null;
            NetatmoStatus.Text = "Connecting to Netatmo…";
            if (Native.SendMessage(_controller, Native.NetatmoRefresh, _window, IntPtr.Zero) != (IntPtr)1)
            {
                _netatmoRequestError = "Could not request a refresh. Close and reopen Settings, then try again.";
                NetatmoStatus.Text = _netatmoRequestError;
                return;
            }
            RefreshStatus();
        }
        private void LoadNetatmoModules()
        {
            var output = new StringBuilder(16384);
            if (Native.clock_netatmo_modules(output, output.Capacity) == 0) return;
            string source = output.ToString();
            string[] lines = source.Split('\n');
            if (source != _netatmoOptions)
            {
                string selected = (string)NetatmoChoice.SelectedValue ?? _saved.NetatmoModule;
                var modules = new List<WeatherModule>();
                foreach (string line in lines.Skip(1))
                {
                    string[] parts = line.Split('\t');
                    if (parts.Length == 5 && int.TryParse(parts[2], out int readings) && long.TryParse(parts[3], out long measuredAt))
                        modules.Add(new WeatherModule { Key = parts[0], Name = parts[1] + (parts[4] == "1" ? "" : " (offline)"), Readings = readings });
                }
                if (string.IsNullOrEmpty(selected) == false && modules.All(m => m.Key != selected))
                    modules.Add(new WeatherModule { Key = selected, Name = "Selected module (unavailable)", Readings = _saved.NetatmoReadings });
                bool ready = _ready;
                _ready = false;
                NetatmoChoice.ItemsSource = modules;
                NetatmoChoice.SelectedValue = selected;
                if (NetatmoChoice.SelectedIndex < 0 && modules.Count > 0) NetatmoChoice.SelectedIndex = 0;
                _ready = ready;
                _netatmoOptions = source;
                if (ready) { UpdateAvailability(); _debounce.Stop(); _debounce.Start(); }
            }
            NetatmoStatus.Text = _netatmoRequestError ?? (string.IsNullOrEmpty(lines[0]) ? "No request completed yet." : "Last request: " + lines[0]);
            UpdateNetatmoDiagnostics();
        }

        private void UpdateNetatmoDiagnostics()
        {
            var draft = _saved;
            draft.Layout = _face;
            draft.Flags = NetatmoSwitch.IsChecked == true ? draft.Flags | 512 : draft.Flags & ~512;
            draft.NetatmoModule = (string)NetatmoChoice.SelectedValue ?? _saved.NetatmoModule;
            draft.NetatmoReadings = 0;
            var readings = new[] { TemperatureSwitch, HumiditySwitch, Co2Switch, NoiseSwitch };
            for (int i = 0; i < readings.Length; i++) if (readings[i].IsChecked == true) draft.NetatmoReadings |= 1 << i;
            bool noReadings = draft.NetatmoReadings == 0;
            if (noReadings) draft.NetatmoReadings = 1;
            if (int.TryParse(NetatmoIntervalBox.Text, out int interval) && interval >= 60 && interval <= 86400)
                draft.NetatmoRefreshSeconds = interval;
            string[] diagnostics = Native.NetatmoDiagnostics(draft).Split('\n');
            string summary = diagnostics[0];
            string action = diagnostics.Length > 1 ? diagnostics[1] : "";
            if (noReadings && _face == 2 && NetatmoSwitch.IsChecked == true)
            {
                summary = "Weather hidden: no readings are selected.";
                action = "Choose at least one reading available on this module.";
            }
            bool hidden = summary.StartsWith("Weather hidden", StringComparison.Ordinal);
            bool stale = summary.StartsWith("Weather stale", StringComparison.Ordinal);
            NetatmoDisplayStatus.Text = summary;
            NetatmoDisplayStatus.Foreground = new SolidColorBrush(hidden || stale ? Color.FromRgb(213, 184, 137) : Color.FromRgb(123, 212, 175));
            NetatmoAction.Text = action;
            NetatmoAction.Visibility = string.IsNullOrEmpty(action) ? Visibility.Collapsed : Visibility.Visible;
            NetatmoAge.Text = string.Join("\n", diagnostics.Skip(2));
            NetatmoPolling.Text = _paused ? "Automatic refresh paused. Resume updates to keep readings current." :
                ((_runtimeFlags & 8) != 0 ? "Automatic refresh paused while on battery." :
                (_saved.Layout != 2 || (_saved.Flags & 512) == 0 ? "Automatic refresh off in saved settings." :
                "Automatic refresh every " + _saved.NetatmoRefreshSeconds.ToString(CultureInfo.InvariantCulture) + " seconds."));
            if (_hasEdits) NetatmoPolling.Text += " Save to apply your changes.";
            WeatherWarningText.Text = stale ? "Weather readings are old · view details" : "Weather unavailable · view details";
            WeatherWarning.Visibility = _face == 2 && NetatmoSwitch.IsChecked == true && (hidden || stale) ? Visibility.Visible : Visibility.Collapsed;
        }

        private void ShowWeatherDetails(object sender, RoutedEventArgs e)
        {
            NetatmoTab.IsChecked = true;
            NetatmoPanel.ScrollToTop();
        }
        private void ChoosePalette(object sender, RoutedEventArgs e)
        {
            string[] choice = ((string)((Button)sender).Tag).Split(':');
            (choice[0] == "text" ? TextColorBox : AccentColorBox).Text = "#" + choice[1];
        }
        private void ChooseImage(object sender, RoutedEventArgs e)
        {
            var dialog = new OpenFileDialog { Filter = "Images|*.jpg;*.jpeg;*.png;*.bmp", CheckFileExists = true, Title = "Choose clock background" };
            if (dialog.ShowDialog(this) == true)
            {
                ImagePath.Text = dialog.FileName;
                ImagePath.ToolTip = dialog.FileName;
                ThemeChoice.SelectedIndex = 5;
                _debounce.Stop();
                RefreshPreview();
            }
        }
        private void CloseSettings(object sender, RoutedEventArgs e)
        {
            if (ReferenceEquals(sender, HideButton)) _hasEdits = false;
            Close();
        }
        private void ConfirmClose(object sender, CancelEventArgs e)
        {
            if (_ready == false || _hasEdits == false || Native.IsWindow(_controller) == false) return;
            var answer = MessageBox.Show(this, "Save your changes before closing settings?", "Unsaved changes",
                MessageBoxButton.YesNoCancel, MessageBoxImage.Question);
            if (answer == MessageBoxResult.Cancel || (answer == MessageBoxResult.Yes && Save() == false))
            {
                e.Cancel = true;
                if (answer == MessageBoxResult.Yes) ReviewError(sender, new RoutedEventArgs());
            }
        }
        private void Minimize(object sender, RoutedEventArgs e) => WindowState = WindowState.Minimized;
        private void KeyPressed(object sender, KeyEventArgs e)
        {
            if (e.Key == Key.Escape) { Close(); e.Handled = true; }
            if (e.Key == Key.S && Keyboard.Modifiers == ModifierKeys.Control) { Save(); e.Handled = true; }
        }
        private static string ColorHex(int color) => $"#{color & 255:X2}{(color >> 8) & 255:X2}{(color >> 16) & 255:X2}";
        private static bool ParseColor(string text, out int color)
        {
            string hex = text.TrimStart('#');
            color = 0;
            if (hex.Length != 6 || int.TryParse(hex, NumberStyles.HexNumber, CultureInfo.InvariantCulture, out int rgb) == false) return false;
            color = ((rgb >> 16) & 255) | (rgb & 0x00ff00) | ((rgb & 255) << 16);
            return true;
        }
    }
}
