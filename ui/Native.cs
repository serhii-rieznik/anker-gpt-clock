using System;
using System.Runtime.InteropServices;
using System.Text;

namespace AnkerClock
{
    internal struct SettingsWire
    {
        public int Version, Theme, FontSize, FontStyle, Layout, DateFormat, Interval, Brightness,
            VerticalOffset, BackgroundFit, TextColor, AccentColor, ImageDim, Flags, StartWindows, AnalogFontSize;
        public int CodexRefreshSeconds, BackgroundDither;
        public string Font;
        public string BackgroundPath;
        public string TimeZone;
        public string Label;
    }

    internal static class Native
    {
        internal const uint Query = 0x8004, Register = 0x8005;
        [StructLayout(LayoutKind.Sequential)]
        private struct CopyData { public IntPtr Kind; public int Size; public IntPtr Data; }

        [DllImport("AnkerClock.Native.dll", CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
        internal static extern IntPtr clock_preview_open(StringBuilder error, int capacity);
        [DllImport("AnkerClock.Native.dll", CallingConvention = CallingConvention.Cdecl)]
        internal static extern void clock_preview_close(IntPtr preview);
        [DllImport("AnkerClock.Native.dll", CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
        private static extern int clock_settings_load(IntPtr settings, StringBuilder error, int capacity);
        [DllImport("AnkerClock.Native.dll", CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
        private static extern int clock_preview_render(IntPtr preview, IntPtr settings,
            [Out] byte[] bytes, int capacity, StringBuilder error, int errorCapacity);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        private static extern IntPtr FindWindow(string className, string title);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        private static extern int GetWindowText(IntPtr window, StringBuilder text, int capacity);
        [DllImport("user32.dll")] internal static extern bool IsWindow(IntPtr window);
        [DllImport("user32.dll")] internal static extern IntPtr SendMessage(IntPtr window, uint message, IntPtr w, IntPtr l);
        [DllImport("user32.dll")] internal static extern bool PostMessage(IntPtr window, uint message, IntPtr w, IntPtr l);
        [DllImport("user32.dll", EntryPoint = "SendMessageW")]
        private static extern IntPtr SendCopyData(IntPtr window, uint message, IntPtr sender, ref CopyData data);
        [DllImport("dwmapi.dll")]
        internal static extern int DwmSetWindowAttribute(IntPtr window, int attribute, ref int value, int size);

        internal static IntPtr Controller() => FindWindow("AnkerClock.Controller", null);
        internal static string Status(IntPtr controller)
        {
            var text = new StringBuilder(2048);
            GetWindowText(controller, text, text.Capacity);
            return text.ToString();
        }

        // 16 Int32 fields, fixed UTF-16 buffers, then the usage interval and dithering strength.
        private const int WireSize = 66204;
        internal static int clock_settings_load(out SettingsWire settings, StringBuilder error, int capacity)
        {
            IntPtr buffer = Marshal.AllocHGlobal(WireSize);
            try
            {
                int result = clock_settings_load(buffer, error, capacity);
                settings = result == 0 ? default : ReadSettings(buffer);
                return result;
            }
            finally { Marshal.FreeHGlobal(buffer); }
        }
        internal static int clock_preview_render(IntPtr preview, ref SettingsWire settings, byte[] bytes,
            int capacity, StringBuilder error, int errorCapacity)
        {
            IntPtr buffer = Marshal.AllocHGlobal(WireSize);
            try
            {
                WriteSettings(settings, buffer);
                return clock_preview_render(preview, buffer, bytes, capacity, error, errorCapacity);
            }
            finally { Marshal.FreeHGlobal(buffer); }
        }
        private static SettingsWire ReadSettings(IntPtr buffer)
        {
            var n = new int[16];
            Marshal.Copy(buffer, n, 0, n.Length);
            return new SettingsWire
            {
                Version = n[0],
                Theme = n[1],
                FontSize = n[2],
                FontStyle = n[3],
                Layout = n[4],
                DateFormat = n[5],
                Interval = n[6],
                Brightness = n[7],
                VerticalOffset = n[8],
                BackgroundFit = n[9],
                TextColor = n[10],
                AccentColor = n[11],
                ImageDim = n[12],
                Flags = n[13],
                StartWindows = n[14],
                AnalogFontSize = n[15],
                CodexRefreshSeconds = Marshal.ReadInt32(buffer, 66196),
                BackgroundDither = Marshal.ReadInt32(buffer, 66200),
                Font = Marshal.PtrToStringUni(IntPtr.Add(buffer, 64)),
                BackgroundPath = Marshal.PtrToStringUni(IntPtr.Add(buffer, 320)),
                TimeZone = Marshal.PtrToStringUni(IntPtr.Add(buffer, 65856)),
                Label = Marshal.PtrToStringUni(IntPtr.Add(buffer, 66112))
            };
        }
        private static void WriteSettings(SettingsWire s, IntPtr buffer)
        {
            Marshal.Copy(new byte[WireSize], 0, buffer, WireSize);
            var n = new[] { s.Version, s.Theme, s.FontSize, s.FontStyle, s.Layout, s.DateFormat,
                s.Interval, s.Brightness, s.VerticalOffset, s.BackgroundFit, s.TextColor,
                s.AccentColor, s.ImageDim, s.Flags, s.StartWindows, s.AnalogFontSize };
            Marshal.Copy(n, 0, buffer, n.Length);
            WriteText(s.Font, buffer, 64, 128);
            WriteText(s.BackgroundPath, buffer, 320, 32768);
            WriteText(s.TimeZone, buffer, 65856, 128);
            WriteText(s.Label, buffer, 66112, 41);
            Marshal.WriteInt32(buffer, 66196, s.CodexRefreshSeconds);
            Marshal.WriteInt32(buffer, 66200, s.BackgroundDither);
        }
        private static void WriteText(string text, IntPtr buffer, int offset, int capacity)
        {
            if (text.Length >= capacity) throw new ArgumentException("A clock setting is too long.");
            char[] chars = (text + "\0").ToCharArray();
            Marshal.Copy(chars, 0, IntPtr.Add(buffer, offset), chars.Length);
        }

        internal static bool Save(IntPtr controller, IntPtr sender, SettingsWire settings)
        {
            int size = WireSize;
            IntPtr buffer = Marshal.AllocHGlobal(size);
            try
            {
                WriteSettings(settings, buffer);
                var data = new CopyData { Kind = (IntPtr)0x41434C4B, Size = size, Data = buffer };
                return SendCopyData(controller, 0x4a, sender, ref data) == (IntPtr)1;
            }
            finally { Marshal.FreeHGlobal(buffer); }
        }
    }
}
