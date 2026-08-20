using System;
using System.IO;
using System.Text.Json;
using System.Collections.Generic;

namespace serial_console_dotnet.Services
{
    public class WindowLayoutSettings
    {
        public int Width { get; set; }
        public int Height { get; set; }
        public bool IsMaximized { get; set; }
    }

    public class AppSettings
    {
        public string ConnectionType { get; set; } = "serial";
        public string SerialPort { get; set; } = "";
        public string BaudRate { get; set; } = "115200";
        public string TelnetHost { get; set; } = "192.168.1.1";
        public string TelnetPort { get; set; } = "23";
        public string TelnetUsername { get; set; } = "root";
        public string TelnetPassword { get; set; } = "admin";
        public bool ShowTimestamps { get; set; } = true;
        public string FtpUsername { get; set; } = "root";
        public string FtpPassword { get; set; } = "";
        public List<string> CommandHistory { get; set; } = new List<string>();
        
        // Explorer Layout Settings
        public WindowLayoutSettings MainWindowLayout { get; set; } = new WindowLayoutSettings { Width = 1280, Height = 800, IsMaximized = false };
        public WindowLayoutSettings ExplorerWindowLayout { get; set; } = new WindowLayoutSettings { Width = 960, Height = 720, IsMaximized = false };
        
        // Sidebar expansion states
        public bool SidebarSystemExpanded { get; set; } = true;
        public bool SidebarFavoritesExpanded { get; set; } = true;
        public bool SidebarRecentsExpanded { get; set; } = true;

        public List<serial_console_dotnet.Controls.FavoriteFolderItem> FavoriteFolders { get; set; } = new List<serial_console_dotnet.Controls.FavoriteFolderItem>();
        public List<serial_console_dotnet.Controls.FavoriteFolderItem> RecentFolders { get; set; } = new List<serial_console_dotnet.Controls.FavoriteFolderItem>();
    }

    public static class SettingsService
    {
        private static readonly string FolderPath = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            "serial_console_dotnet"
        );
        private static readonly string FilePath = Path.Combine(FolderPath, "settings.json");

        public static AppSettings Load()
        {
            try
            {
                if (File.Exists(FilePath))
                {
                    string json = File.ReadAllText(FilePath);
                    var settings = JsonSerializer.Deserialize<AppSettings>(json);
                    if (settings != null) return settings;
                }
            }
            catch (Exception ex)
            {
                System.Diagnostics.Debug.WriteLine($"Error loading settings: {ex.Message}");
            }
            return new AppSettings();
        }

        public static void Save(AppSettings settings)
        {
            try
            {
                if (!Directory.Exists(FolderPath))
                {
                    Directory.CreateDirectory(FolderPath);
                }
                string json = JsonSerializer.Serialize(settings, new JsonSerializerOptions { WriteIndented = true });
                File.WriteAllText(FilePath, json);
            }
            catch (Exception ex)
            {
                System.Diagnostics.Debug.WriteLine($"Error saving settings: {ex.Message}");
            }
        }
    }
}
