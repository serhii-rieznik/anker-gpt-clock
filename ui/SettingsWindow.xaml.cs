using Microsoft.Win32;
using System;
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
        private long _previewPeriod;
        private int _face;

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
            StyleChoice.ItemsSource = new[] { "Regular", "Bold", "Italic", "Bold italic" };
            FitChoice.ItemsSource = new[] { "Fill screen (crop)", "Fit entire image" };
            DateChoice.ItemsSource = new[] { "10 October", "Sat 10 Oct", "2026-10-10", "10/10/2026" };
            var fonts = Fonts.SystemFontFamilies.Select(f => f.Source).Distinct().OrderBy(f => f).ToList();
            if (fonts.Contains(_saved.Font) == false) fonts.Add(_saved.Font);
            FontChoice.ItemsSource = fonts;
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

        private void LoadControls()
        {
            ThemeChoice.SelectedIndex = _saved.Theme;
            FontChoice.SelectedItem = _saved.Font;
            StyleChoice.SelectedIndex = _saved.FontStyle;
            FitChoice.SelectedIndex = _saved.BackgroundFit;
            DateChoice.SelectedIndex = _saved.DateFormat;
            ZoneChoice.SelectedValue = _saved.TimeZone;
            SizeSlider.Value = _saved.FontSize;
            AnalogSizeSlider.Value = _saved.AnalogFontSize;
            DimSlider.Value = _saved.ImageDim;
            DitherSlider.Value = _saved.BackgroundDither;
            OffsetSlider.Value = _saved.VerticalOffset;
            BrightnessSlider.Value = _saved.Brightness;
            IntervalBox.Text = _saved.Interval.ToString(CultureInfo.InvariantCulture);
            CodexIntervalBox.Text = _saved.CodexRefreshSeconds.ToString(CultureInfo.InvariantCulture);
            LabelBox.Text = _saved.Label;
            TextColorBox.Text = ColorHex(_saved.TextColor);
            AccentColorBox.Text = ColorHex(_saved.AccentColor);
            ImagePath.Text = _saved.BackgroundPath;
            ImagePath.ToolTip = _saved.BackgroundPath;
            _face = _saved.Layout;
            (new[] { SignatureFace, DigitalFace, AnalogFace })[_face].IsChecked = true;
            var switches = new[] { HourSwitch, SecondsSwitch, DateSwitch, WeekdaySwitch, BatterySwitch, LaunchSwitch, ExclusiveSwitch, CodexSwitch };
            var bits = new[] { 1, 2, 4, 8, 16, 64, 128, 256 };
            for (int i = 0; i < switches.Length; i++) switches[i].IsChecked = (_saved.Flags & bits[i]) != 0;
            StartupSwitch.IsChecked = _saved.StartWindows != 0;
            UpdateAvailability();
        }

        private void InitializeWindow(object sender, EventArgs e)
        {
            _window = new WindowInteropHelper(this).Handle;
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

        private void Navigate(object sender, RoutedEventArgs e)
        {
            if (AppearancePanel == null) return;
            int tab = int.Parse((string)((RadioButton)sender).Tag, CultureInfo.InvariantCulture);
            var panels = new[] { AppearancePanel, ClockPanel, DevicePanel, StartupPanel };
            for (int i = 0; i < panels.Length; i++) panels[i].Visibility = i == tab ? Visibility.Visible : Visibility.Collapsed;
        }

        private void Changed(object sender, RoutedEventArgs e)
        {
            if (_ready == false) return;
            if (e.OriginalSource is RadioButton face && (string)face.GroupName == "Face" && face.IsChecked == true)
                _face = int.Parse((string)face.Tag, CultureInfo.InvariantCulture);
            UpdateAvailability();
            _debounce.Stop();
            _debounce.Start();
        }

        private void UpdateAvailability()
        {
            bool analog = _face == 2;
            DigitalSizeOptions.Visibility = analog ? Visibility.Collapsed : Visibility.Visible;
            AnalogSizeOptions.Visibility = analog ? Visibility.Visible : Visibility.Collapsed;
            OffsetSlider.IsEnabled = analog == false;
            HourSwitch.IsEnabled = analog == false;
            CodexIntervalBox.IsEnabled = CodexSwitch.IsChecked == true;
            DateSwitch.IsEnabled = analog == false || CodexSwitch.IsChecked != true;
            AnalogTypographyHint.Visibility = analog ? Visibility.Visible : Visibility.Collapsed;
            ImageOptions.Visibility = ThemeChoice.SelectedIndex == 5 ? Visibility.Visible : Visibility.Collapsed;
            DitherSlider.IsEnabled = ThemeChoice.SelectedIndex != 6;
        }

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
            if (ParseColor(TextColorBox.Text, out int text) == false) return Invalid("Use a six-digit hex color, such as #F5F8FF.", TextColorBox);
            TextColorBox.ClearValue(BorderBrushProperty);
            if (ParseColor(AccentColorBox.Text, out int accent) == false) return Invalid("Use a six-digit hex color, such as #90B8FF.", AccentColorBox);
            AccentColorBox.ClearValue(BorderBrushProperty);
            draft.Theme = ThemeChoice.SelectedIndex;
            draft.Font = (string)FontChoice.SelectedItem;
            draft.FontStyle = StyleChoice.SelectedIndex;
            draft.FontSize = (int)SizeSlider.Value;
            draft.AnalogFontSize = (int)AnalogSizeSlider.Value;
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
            draft.Label = LabelBox.Text;
            draft.TextColor = text;
            draft.AccentColor = accent;
            draft.BackgroundPath = ImagePath.Text;
            draft.StartWindows = StartupSwitch.IsChecked == true ? 1 : 0;
            draft.Flags = 0;
            var switches = new[] { HourSwitch, SecondsSwitch, DateSwitch, WeekdaySwitch, BatterySwitch, LaunchSwitch, ExclusiveSwitch, CodexSwitch };
            var bits = new[] { 1, 2, 4, 8, 16, 64, 128, 256 };
            for (int i = 0; i < switches.Length; i++) if (switches[i].IsChecked == true) draft.Flags |= bits[i];
            if (draft.Theme == 5 && string.IsNullOrEmpty(draft.BackgroundPath)) return Invalid("Choose a background image first.", ThemeChoice);
            ThemeChoice.ClearValue(BorderBrushProperty);
            return true;
        }

        private bool Invalid(string message, Control control)
        {
            ValidationMessage.Text = message;
            ValidationMessage.Foreground = new SolidColorBrush(Color.FromRgb(255, 165, 156));
            control.BorderBrush = ValidationMessage.Foreground;
            SaveButton.IsEnabled = false;
            return false;
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
            ValidationMessage.Text = dirty ? "Unsaved changes" : "Preview reflects your saved settings.";
            SaveButton.IsEnabled = dirty;
        }

        private void RefreshStatus()
        {
            if (Native.IsWindow(_controller) == false) { Close(); return; }
            string[] status = Native.Status(_controller).Split('\n');
            ConnectionStatus.Text = status[0];
            TimingStatus.Text = status.Length > 1 ? status[1] : "";
            long flags = Native.SendMessage(_controller, Native.Query, IntPtr.Zero, IntPtr.Zero).ToInt64();
            _paused = (flags & 1) != 0;
            PauseButton.Content = _paused ? "Resume" : "Pause";
            ConnectionDot.Fill = new SolidColorBrush((flags & 2) != 0 ? Color.FromRgb(123, 212, 175) : Color.FromRgb(155, 165, 183));
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
            if ((_saved.Flags & 128) == 0 && (draft.Flags & 128) != 0 &&
                MessageBox.Show(this, "Delete all saved photos from the hub and use its gallery only for this clock?", "Use clock-only gallery", MessageBoxButton.YesNo, MessageBoxImage.Warning) != MessageBoxResult.Yes)
                return false;
            if (Native.Save(_controller, _window, draft) == false)
                return Invalid("The settings could not be saved. Please try again.", SaveButton);
            _saved = draft;
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
        private void CloseSettings(object sender, RoutedEventArgs e) => Close();
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
