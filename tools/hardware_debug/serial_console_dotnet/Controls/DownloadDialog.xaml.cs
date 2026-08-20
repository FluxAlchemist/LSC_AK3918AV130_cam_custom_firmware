using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using System;

namespace serial_console_dotnet.Controls
{
    public sealed partial class DownloadDialog : UserControl
    {
        public event Action<string>? DownloadRequested;
        public event Action<string, string, string>? FtpDownloadRequested;
        public event Action? DownloadCancelled;

        public bool IsFtpMode => UseFtpToggle.IsOn;
        public string FtpUsername => FtpUsernameTextBox.Text;
        public string FtpPassword => FtpPasswordBox.Password;

        public DownloadDialog()
        {
            this.InitializeComponent();
        }

        public void Show()
        {
            DownloadProgressPanel.Visibility = Visibility.Collapsed;
            DownloadProgressBar.Value = 0;
            DownloadProgressBar.IsIndeterminate = false;
            UseFtpToggle.IsOn = (FtpSwitchPanel.Visibility == Visibility.Visible);
            StartDownloadBtn.IsEnabled = true;
            DownloadTargetPathTextBox.IsEnabled = true;

            this.Visibility = Visibility.Visible;
        }

        public void Hide()
        {
            this.Visibility = Visibility.Collapsed;
        }

        public void SetTargetPath(string path)
        {
            DownloadTargetPathTextBox.Text = path;
        }

        public void SetProgress(double progress)
        {
            DownloadProgressPanel.Visibility = Visibility.Visible;
            if (progress < 0)
            {
                DownloadProgressBar.IsIndeterminate = true;
            }
            else
            {
                DownloadProgressBar.IsIndeterminate = false;
                DownloadProgressBar.Value = progress;
            }
        }

        public void SetStatus(string status)
        {
            DownloadProgressPanel.Visibility = Visibility.Visible;
            DownloadStatusText.Text = status;
        }

        public void SetDownloadingState(bool isDownloading)
        {
            DownloadTargetPathTextBox.IsEnabled = !isDownloading;
            UseFtpToggle.IsEnabled = !isDownloading;
            FtpUsernameTextBox.IsEnabled = !isDownloading;
            FtpPasswordBox.IsEnabled = !isDownloading;
            StartDownloadBtn.IsEnabled = !isDownloading;
        }

        public void SetIsTelnet(bool isTelnet)
        {
            FtpSwitchPanel.Visibility = isTelnet ? Visibility.Visible : Visibility.Collapsed;
            UseFtpToggle.IsOn = isTelnet;
        }

        public void SetFtpCredentials(string username, string password)
        {
            FtpUsernameTextBox.Text = string.IsNullOrEmpty(username) ? "root" : username;
            FtpPasswordBox.Password = password ?? "";
        }

        private void UseFtpToggle_Toggled(object sender, RoutedEventArgs e)
        {
            bool active = UseFtpToggle.IsOn;
            FtpCredentialsPanel.Visibility = active ? Visibility.Visible : Visibility.Collapsed;
        }

        private void CloseDownloadDialog_Click(object sender, RoutedEventArgs e)
        {
            DownloadCancelled?.Invoke();
            Hide();
        }

        private void StartDownload_Click(object sender, RoutedEventArgs e)
        {
            string remotePath = DownloadTargetPathTextBox.Text;
            if (string.IsNullOrWhiteSpace(remotePath)) return;

            SetDownloadingState(true);

            if (UseFtpToggle.IsOn)
            {
                FtpDownloadRequested?.Invoke(remotePath, FtpUsername, FtpPassword);
            }
            else
            {
                DownloadRequested?.Invoke(remotePath);
            }
        }
    }
}
