using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using serial_console_dotnet.Services;

namespace serial_console_dotnet.Controls
{
    public sealed partial class FavoritesSidebar : UserControl
    {
        public event Action<string>? FolderSelected;

        private readonly List<FavoriteFolderItem> _systemFolders = new List<FavoriteFolderItem>
        {
            new FavoriteFolderItem { Name = "Root (/) ", Path = "/" },
            new FavoriteFolderItem { Name = "Temporary (/tmp)", Path = "/tmp" },
            new FavoriteFolderItem { Name = "Config (/etc)", Path = "/etc" },
            new FavoriteFolderItem { Name = "Var (/var)", Path = "/var" },
            new FavoriteFolderItem { Name = "SD Card (/mnt)", Path = "/mnt" },
            new FavoriteFolderItem { Name = "App (/usr)", Path = "/usr" },
            new FavoriteFolderItem { Name = "Bin (/bin)", Path = "/bin" }
        };

        private List<FavoriteFolderItem> _favorites = new List<FavoriteFolderItem>();
        private List<FavoriteFolderItem> _recents = new List<FavoriteFolderItem>();
        private bool _isInitializing = true;

        public FavoritesSidebar()
        {
            this.InitializeComponent();

            SystemListView.ItemsSource = _systemFolders;
            LoadFromSettings();
            _isInitializing = false;
        }

        public void SetEnabled(bool isEnabled)
        {
            SystemListView.IsEnabled = isEnabled;
            FavoritesListView.IsEnabled = isEnabled;
            RecentsListView.IsEnabled = isEnabled;
        }

        public void LoadFromSettings()
        {
            try
            {
                var settings = SettingsService.Load();
                
                _favorites = settings.FavoriteFolders ?? new List<FavoriteFolderItem>();
                _recents = settings.RecentFolders ?? new List<FavoriteFolderItem>();

                // Restore expander states
                SystemExpander.IsExpanded = settings.SidebarSystemExpanded;
                FavoritesExpander.IsExpanded = settings.SidebarFavoritesExpanded;
                RecentsExpander.IsExpanded = settings.SidebarRecentsExpanded;

                RefreshLists();
            }
            catch (Exception ex)
            {
                System.Diagnostics.Debug.WriteLine($"Error loading sidebar settings: {ex.Message}");
            }
        }

        private void RefreshLists()
        {
            FavoritesListView.ItemsSource = null;
            FavoritesListView.ItemsSource = _favorites;
            NoFavoritesText.Visibility = _favorites.Count == 0 ? Visibility.Visible : Visibility.Collapsed;

            RecentsListView.ItemsSource = null;
            RecentsListView.ItemsSource = _recents;
            NoRecentsText.Visibility = _recents.Count == 0 ? Visibility.Visible : Visibility.Collapsed;
        }

        public void AddFavoriteFolder(string name, string path)
        {
            if (string.IsNullOrEmpty(path)) return;

            // Strip trailing slash if not root
            if (path.Length > 1 && path.EndsWith("/")) path = path.TrimEnd('/');

            // Check if already in favorites
            if (_favorites.Any(f => f.Path == path)) return;

            _favorites.Add(new FavoriteFolderItem { Name = name, Path = path });

            SaveToSettings();
            RefreshLists();
        }

        public void AddRecentFolder(string path)
        {
            if (string.IsNullOrEmpty(path)) return;

            // Strip trailing slash if not root
            if (path.Length > 1 && path.EndsWith("/")) path = path.TrimEnd('/');

            // Remove existing duplicate
            var existing = _recents.FirstOrDefault(f => f.Path == path);
            if (existing != null)
            {
                _recents.Remove(existing);
            }

            // Derive name
            string name = path == "/" ? "Root (/)" : path.Split('/').LastOrDefault() ?? path;
            if (string.IsNullOrEmpty(name)) name = path;

            // Insert at top
            _recents.Insert(0, new FavoriteFolderItem { Name = name, Path = path });

            // Limit to 5
            if (_recents.Count > 5)
            {
                _recents = _recents.Take(5).ToList();
            }

            SaveToSettings();
            RefreshLists();
        }

        private void SaveToSettings()
        {
            try
            {
                var settings = SettingsService.Load();
                settings.FavoriteFolders = _favorites;
                settings.RecentFolders = _recents;
                SettingsService.Save(settings);
            }
            catch (Exception ex)
            {
                System.Diagnostics.Debug.WriteLine($"Error saving sidebar settings: {ex.Message}");
            }
        }

        private void SystemListView_SelectionChanged(object sender, SelectionChangedEventArgs e)
        {
            if (SystemListView.SelectedItem is FavoriteFolderItem item)
            {
                SystemListView.SelectedItem = null;
                FolderSelected?.Invoke(item.Path);
            }
        }

        private void FavoritesListView_SelectionChanged(object sender, SelectionChangedEventArgs e)
        {
            if (FavoritesListView.SelectedItem is FavoriteFolderItem item)
            {
                FavoritesListView.SelectedItem = null;
                FolderSelected?.Invoke(item.Path);
            }
        }

        private void RecentsListView_SelectionChanged(object sender, SelectionChangedEventArgs e)
        {
            if (RecentsListView.SelectedItem is FavoriteFolderItem item)
            {
                RecentsListView.SelectedItem = null;
                FolderSelected?.Invoke(item.Path);
            }
        }

        private void FavoriteItem_RightTapped(object sender, RightTappedRoutedEventArgs e)
        {
            if (sender is FrameworkElement element && element.DataContext is FavoriteFolderItem item)
            {
                // Select the item so the ContextFlyout operates on it
                FavoritesListView.SelectedItem = item;
            }
        }

        private void RemoveFavorite_Click(object sender, RoutedEventArgs e)
        {
            if (sender is MenuFlyoutItem menuItem && menuItem.Tag is FavoriteFolderItem item)
            {
                var match = _favorites.FirstOrDefault(f => f.Path == item.Path);
                if (match != null)
                {
                    _favorites.Remove(match);
                    SaveToSettings();
                    RefreshLists();
                }
            }
        }

        private void Expander_Expanding(Expander sender, ExpanderExpandingEventArgs args)
        {
            SaveExpanderStates();
        }

        private void Expander_Collapsed(Expander sender, ExpanderCollapsedEventArgs args)
        {
            SaveExpanderStates();
        }

        private void SaveExpanderStates()
        {
            if (_isInitializing || SystemExpander == null || FavoritesExpander == null || RecentsExpander == null) return;

            try
            {
                var settings = SettingsService.Load();
                settings.SidebarSystemExpanded = SystemExpander.IsExpanded;
                settings.SidebarFavoritesExpanded = FavoritesExpander.IsExpanded;
                settings.SidebarRecentsExpanded = RecentsExpander.IsExpanded;
                SettingsService.Save(settings);
            }
            catch {}
        }
    }
}
