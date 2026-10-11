using System;
using System.Windows;

namespace AnkerClock
{
    public partial class App : Application
    {
        private void Start(object sender, StartupEventArgs e)
        {
            try
            {
                new SettingsWindow().Show();
            }
            catch (Exception error)
            {
                MessageBox.Show(error.Message, "Anker Clock", MessageBoxButton.OK, MessageBoxImage.Error);
                Shutdown(1);
            }
        }
    }
}
