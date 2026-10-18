using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Drawing;
using System.Drawing.Text;
using System.IO;
using System.Linq;
using System.Text;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Interop;
using System.Windows.Media.Imaging;
using System.Windows.Threading;

namespace AnkerClock
{
    internal sealed class FontStyleOption
    {
        public int Key { get; set; }
        public string Name { get; set; }
    }

    internal static class FontCatalog
    {
        internal static List<string> Families()
        {
            var names = new List<string>();
            using (var installed = new InstalledFontCollection())
                foreach (FontFamily family in installed.Families)
                    using (family) names.Add(family.Name);
            return names.Distinct(StringComparer.OrdinalIgnoreCase).OrderBy(name => name, StringComparer.CurrentCultureIgnoreCase).ToList();
        }

        internal static List<FontStyleOption> Styles(string name)
        {
            var result = new List<FontStyleOption>();
            string[] names = { "Regular", "Bold", "Italic", "Bold italic" };
            try
            {
                using (var family = new FontFamily(name))
                    for (int style = 0; style < names.Length; style++)
                        if (family.IsStyleAvailable((System.Drawing.FontStyle)style)) result.Add(new FontStyleOption { Key = style, Name = names[style] });
            }
            catch (ArgumentException) { }
            if (result.Count == 0) result.Add(new FontStyleOption { Key = 0, Name = "Regular" });
            return result;
        }
    }

    public partial class FontPickerWindow : Window
    {
        private const int PageSize = 6;
        private readonly SettingsWire _settings;
        private readonly long _frameUtcTicks = DateTime.UtcNow.ToFileTimeUtc();
        private readonly List<string> _fonts;
        private readonly List<FontPreview> _pinned = new List<FontPreview>();
        private readonly Queue<FontPreview> _pending = new Queue<FontPreview>();
        private readonly byte[] _image = new byte[65520];
        private readonly DispatcherTimer _renderTimer = new DispatcherTimer(DispatcherPriority.Background) { Interval = TimeSpan.FromMilliseconds(20) };
        private readonly DispatcherTimer _searchTimer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(150) };
        private List<string> _filtered;
        private List<FontPreview> _pageFonts;
        private IntPtr _preview;
        private int _page;
        private bool _ready;
        public string SelectedFont { get; private set; }

        internal FontPickerWindow(SettingsWire settings)
        {
            _settings = settings;
            SelectedFont = settings.Font;
            _fonts = FontCatalog.Families();
            _filtered = _fonts;
            _page = Math.Max(0, _fonts.FindIndex(name => name == SelectedFont)) / PageSize;
            InitializeComponent();
            var error = new StringBuilder(512);
            _preview = Native.clock_preview_open(error, error.Capacity);
            if (_preview == IntPtr.Zero) throw new InvalidOperationException(error.ToString());
            _renderTimer.Tick += RenderNext;
            _searchTimer.Tick += (sender, e) => { _searchTimer.Stop(); _filtered = _fonts.Where(name => name.IndexOf(SearchBox.Text.Trim(), StringComparison.CurrentCultureIgnoreCase) >= 0).ToList(); _page = 0; ShowPage(); };
            _ready = true;
            ShowPage();
            Loaded += (sender, e) => SearchBox.Focus();
        }

        private sealed class FontPreview : INotifyPropertyChanged
        {
            public string Name { get; set; }
            public BitmapImage Image { get; private set; }
            public string Status { get; private set; } = "Preparing preview…";
            public bool Pinned { get; private set; }
            public bool CanPin { get; private set; } = true;
            public event PropertyChangedEventHandler PropertyChanged;
            public void SetImage(BitmapImage image, string status)
            {
                Image = image;
                Status = status;
                PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(Image)));
                PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(Status)));
            }
            public void SetPinned(bool pinned)
            {
                Pinned = pinned;
                PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(Pinned)));
            }
            public void SetCanPin(bool canPin)
            {
                CanPin = canPin;
                PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(CanPin)));
            }
        }

        private void InitializeWindow(object sender, EventArgs e)
        {
            int dark = 1;
            Native.DwmSetWindowAttribute(new WindowInteropHelper(this).Handle, 20, ref dark, sizeof(int));
        }

        private void ReleaseWindow(object sender, EventArgs e)
        {
            _ready = false;
            _renderTimer.Stop();
            _searchTimer.Stop();
            _pending.Clear();
            if (_preview != IntPtr.Zero) Native.clock_preview_close(_preview);
            _preview = IntPtr.Zero;
        }

        private void FilterFonts(object sender, TextChangedEventArgs e)
        {
            if (_ready == false) return;
            _renderTimer.Stop();
            _searchTimer.Stop();
            _searchTimer.Start();
        }

        private void ShowPage()
        {
            _renderTimer.Stop();
            _pending.Clear();
            _pageFonts = _filtered.Skip(_page * PageSize).Take(PageSize)
                .Select(name => _pinned.FirstOrDefault(font => font.Name == name) ?? new FontPreview { Name = name }).ToList();
            Gallery.ItemsSource = _pageFonts;
            Gallery.SelectedValue = SelectedFont;
            foreach (var font in _pageFonts) if (font.Image == null) _pending.Enqueue(font);
            foreach (var font in _pageFonts) font.SetCanPin(font.Pinned || _pinned.Count < 2);
            PreviousButton.IsEnabled = _page > 0;
            NextButton.IsEnabled = ((_page + 1) * PageSize) < _filtered.Count;
            EmptyMessage.Visibility = _filtered.Count == 0 ? Visibility.Visible : Visibility.Collapsed;
            PageStatus.Text = _filtered.Count == 0 ? "0 fonts" : $"{(_page * PageSize) + 1}–{Math.Min((_page + 1) * PageSize, _filtered.Count)} of {_filtered.Count} fonts";
            UpdateSelection();
            if (_pending.Count > 0) _renderTimer.Start();
        }

        private void RenderNext(object sender, EventArgs e)
        {
            if (_pending.Count == 0) { _renderTimer.Stop(); return; }
            var font = _pending.Dequeue();
            var frame = _settings;
            frame.Font = font.Name;
            var styles = FontCatalog.Styles(frame.Font);
            if (styles.Any(style => style.Key == frame.FontStyle) == false) frame.FontStyle = styles[0].Key;
            var error = new StringBuilder(512);
            int count = Native.RenderFontPreview(_preview, ref frame, _frameUtcTicks, _image, error);
            if (count == 0) font.SetImage(null, error.ToString());
            else
            {
                var bitmap = new BitmapImage();
                using (var stream = new MemoryStream(_image, 0, count, false))
                {
                    bitmap.BeginInit();
                    bitmap.CacheOption = BitmapCacheOption.OnLoad;
                    bitmap.StreamSource = stream;
                    bitmap.EndInit();
                }
                bitmap.Freeze();
                font.SetImage(bitmap, "");
            }
            UpdateSelection();
            if (_pending.Count == 0) _renderTimer.Stop();
        }

        private void SelectFont(object sender, SelectionChangedEventArgs e)
        {
            if (_ready == false || e.OriginalSource != sender) return;
            var selected = ((ListBox)sender).SelectedItem as FontPreview;
            if (selected != null) SelectedFont = selected.Name;
            UpdateSelection();
        }

        private void UpdateSelection()
        {
            var selected = _pageFonts?.Concat(_pinned).FirstOrDefault(font => font.Name == SelectedFont);
            UseButton.IsEnabled = selected?.Image != null;
            UseButton.ToolTip = SelectedFont;
        }

        private void PinFont(object sender, RoutedEventArgs e)
        {
            if (_ready == false) return;
            var checkbox = (CheckBox)sender;
            var font = (FontPreview)checkbox.DataContext;
            bool pinned = checkbox.IsChecked == true;
            if (pinned && font.Pinned == false && _pinned.Count < 2) _pinned.Add(font);
            else if (pinned == false) _pinned.Remove(font);
            font.SetPinned(_pinned.Contains(font));
            foreach (var candidate in _pageFonts)
                if (candidate.Name == font.Name && ReferenceEquals(candidate, font) == false) candidate.SetPinned(font.Pinned);
            CompareButton.Content = $"Compare ({_pinned.Count}/2)";
            CompareButton.IsEnabled = _pinned.Count == 2;
            foreach (var candidate in _pageFonts.Concat(_pinned)) candidate.SetCanPin(candidate.Pinned || _pinned.Count < 2);
            if (pinned && font.Pinned == false) checkbox.IsChecked = false;
        }

        private void ToggleComparison(object sender, RoutedEventArgs e)
        {
            bool compare = Comparison.Visibility != Visibility.Visible;
            Comparison.ItemsSource = _pinned.ToArray();
            Comparison.SelectedValue = SelectedFont;
            Comparison.Visibility = compare ? Visibility.Visible : Visibility.Collapsed;
            Gallery.Visibility = compare ? Visibility.Collapsed : Visibility.Visible;
            SearchBox.Visibility = compare ? Visibility.Collapsed : Visibility.Visible;
            PreviousButton.Visibility = compare ? Visibility.Collapsed : Visibility.Visible;
            NextButton.Visibility = compare ? Visibility.Collapsed : Visibility.Visible;
            CompareButton.Content = compare ? "Back to fonts" : $"Compare ({_pinned.Count}/2)";
            PageStatus.Text = compare ? "Side-by-side clock previews" : $"{(_page * PageSize) + 1}–{Math.Min((_page + 1) * PageSize, _filtered.Count)} of {_filtered.Count} fonts";
            _renderTimer.Stop();
            _pending.Clear();
            foreach (var font in compare ? _pinned : _pageFonts) if (font.Image == null) _pending.Enqueue(font);
            if (_pending.Count > 0) _renderTimer.Start();
        }

        private void PreviousPage(object sender, RoutedEventArgs e) { _page--; ShowPage(); }
        private void NextPage(object sender, RoutedEventArgs e) { _page++; ShowPage(); }
        private void UseFont(object sender, RoutedEventArgs e) => DialogResult = true;
    }
}
