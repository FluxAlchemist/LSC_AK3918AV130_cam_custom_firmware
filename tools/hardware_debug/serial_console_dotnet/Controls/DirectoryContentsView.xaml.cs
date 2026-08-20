using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;

namespace serial_console_dotnet.Controls
{
    public sealed partial class DirectoryContentsView : UserControl
    {
        public event Action<ExplorerItem>? DirectoryDoubleTapped;
        public event Action? SelectionChanged;
        public event Action? DownloadRequested;
        public event Action? CopyRequested;
        public event Action? DeleteRequested;
        public event Action? PropertiesRequested;
        public event Action? PasteRequested;
        public event Action<string>? NewFolderRequested;
        public event Action<IReadOnlyList<Windows.Storage.IStorageItem>>? ItemsDropped;
        public event Action<ExplorerItem>? CopyPathRequested;
        public event Action<ExplorerItem>? ExecuteRequested;
        public event Action<ExplorerItem, string, string>? RenameCommitted; // item, oldName, newName

        private string _viewMode = "list";
        private List<ExplorerItem> _items = new List<ExplorerItem>();
        private string _sortColumn = "Name";
        private bool _sortAscending = true;

        private string _originalNameBeforeEdit = "";
        private bool _isCommitting = false;

        public DirectoryContentsView()
        {
            this.InitializeComponent();
        }

        public void SetItems(List<ExplorerItem> items)
        {
            _items = items ?? new List<ExplorerItem>();
            ApplySort();
        }

        public void SetLoading(bool isLoading)
        {
            LoadingRing.IsActive = isLoading;
            ItemsListView.Opacity = isLoading ? 0.4 : 1.0;
            ItemsGridView.Opacity = isLoading ? 0.4 : 1.0;
        }

        public void SetViewMode(string mode)
        {
            _viewMode = mode;
            if (_viewMode == "list")
            {
                ItemsGridView.Visibility = Visibility.Collapsed;
                ItemsListView.Visibility = Visibility.Visible;
                ListHeaderGrid.Visibility = Visibility.Visible;
            }
            else
            {
                ItemsListView.Visibility = Visibility.Collapsed;
                ItemsGridView.Visibility = Visibility.Visible;
                ListHeaderGrid.Visibility = Visibility.Collapsed;
            }
        }

        public void SetPasteEnabled(bool isEnabled)
        {
            PasteContextBtn.IsEnabled = isEnabled;
        }

        public List<ExplorerItem> GetSelectedItems()
        {
            var list = new List<ExplorerItem>();
            var view = _viewMode == "list" ? (ListViewBase)ItemsListView : (ListViewBase)ItemsGridView;
            if (view.SelectedItems != null)
            {
                foreach (var item in view.SelectedItems)
                {
                    if (item is ExplorerItem expItem)
                    {
                        list.Add(expItem);
                    }
                }
            }
            return list;
        }

        public bool ContainsItem(string name)
        {
            return _items != null && _items.Any(i => string.Equals(i.Name, name, StringComparison.OrdinalIgnoreCase));
        }

        private void ApplySort()
        {
            if (_items == null) _items = new List<ExplorerItem>();

            if (_items.Count > 0)
            {

            // Sort directories first, then sort by selected column
            IEnumerable<ExplorerItem> sorted;
            
            if (_sortColumn == "Size")
            {
                sorted = _sortAscending
                    ? _items.OrderBy(i => !i.IsDirectory).ThenBy(i => i.Size)
                    : _items.OrderBy(i => !i.IsDirectory).ThenByDescending(i => i.Size);
            }
            else if (_sortColumn == "Type")
            {
                sorted = _sortAscending
                    ? _items.OrderBy(i => !i.IsDirectory).ThenBy(i => i.ItemTypeLabel, StringComparer.OrdinalIgnoreCase)
                    : _items.OrderBy(i => !i.IsDirectory).ThenByDescending(i => i.ItemTypeLabel, StringComparer.OrdinalIgnoreCase);
            }
            else // Default: Name
            {
                sorted = _sortAscending
                    ? _items.OrderBy(i => !i.IsDirectory).ThenBy(i => i.Name, StringComparer.OrdinalIgnoreCase)
                    : _items.OrderBy(i => !i.IsDirectory).ThenByDescending(i => i.Name, StringComparer.OrdinalIgnoreCase);
            }

            _items = sorted.ToList();
            }

            // Refresh List/GridView ItemsSource
            ItemsListView.ItemsSource = null;
            ItemsListView.ItemsSource = _items;

            ItemsGridView.ItemsSource = null;
            ItemsGridView.ItemsSource = _items;

            // Update Header Sort Icons
            UpdateSortIcons();
        }

        private void UpdateSortIcons()
        {
            if (NameSortIcon == null || SizeSortIcon == null || TypeSortIcon == null) return;

            NameSortIcon.Visibility = Visibility.Collapsed;
            SizeSortIcon.Visibility = Visibility.Collapsed;
            TypeSortIcon.Visibility = Visibility.Collapsed;

            string glyph = _sortAscending ? "\uE1FD" : "\uE1FC"; // Up arrow or Down arrow

            if (_sortColumn == "Name")
            {
                NameSortIcon.Glyph = glyph;
                NameSortIcon.Visibility = Visibility.Visible;
            }
            else if (_sortColumn == "Size")
            {
                SizeSortIcon.Glyph = glyph;
                SizeSortIcon.Visibility = Visibility.Visible;
            }
            else if (_sortColumn == "Type")
            {
                TypeSortIcon.Glyph = glyph;
                TypeSortIcon.Visibility = Visibility.Visible;
            }
        }

        private void SortByName_Click(object sender, RoutedEventArgs e)
        {
            if (_sortColumn == "Name")
            {
                _sortAscending = !_sortAscending;
            }
            else
            {
                _sortColumn = "Name";
                _sortAscending = true;
            }
            ApplySort();
        }

        private void SortBySize_Click(object sender, RoutedEventArgs e)
        {
            if (_sortColumn == "Size")
            {
                _sortAscending = !_sortAscending;
            }
            else
            {
                _sortColumn = "Size";
                _sortAscending = true;
            }
            ApplySort();
        }

        private void SortByType_Click(object sender, RoutedEventArgs e)
        {
            if (_sortColumn == "Type")
            {
                _sortAscending = !_sortAscending;
            }
            else
            {
                _sortColumn = "Type";
                _sortAscending = true;
            }
            ApplySort();
        }

        private void ItemsListView_DoubleTapped(object sender, DoubleTappedRoutedEventArgs e)
        {
            if (ItemsListView.SelectedItem is ExplorerItem item)
            {
                DirectoryDoubleTapped?.Invoke(item);
            }
        }

        private void ItemsGridView_DoubleTapped(object sender, DoubleTappedRoutedEventArgs e)
        {
            if (ItemsGridView.SelectedItem is ExplorerItem item)
            {
                DirectoryDoubleTapped?.Invoke(item);
            }
        }

        private void ItemsListView_SelectionChanged(object sender, SelectionChangedEventArgs e)
        {
            SelectionChanged?.Invoke();
        }

        private void ItemsGridView_SelectionChanged(object sender, SelectionChangedEventArgs e)
        {
            SelectionChanged?.Invoke();
        }

        private void Item_RightTapped(object sender, RightTappedRoutedEventArgs e)
        {
            e.Handled = true; // Prevent bubbling to background grid context menu

            if (sender is FrameworkElement element && element.DataContext is ExplorerItem item)
            {
                var view = _viewMode == "list" ? (ListViewBase)ItemsListView : (ListViewBase)ItemsGridView;
                if (!view.SelectedItems.Contains(item))
                {
                    view.SelectedItems.Clear();
                    view.SelectedItems.Add(item);
                }
            }
        }

        private void ContentsGrid_RightTapped(object sender, RightTappedRoutedEventArgs e)
        {
            if (e.Handled) return;
            BackgroundContextMenu.ShowAt(ContentsGrid, e.GetPosition(ContentsGrid));
            e.Handled = true;
        }

        public event Action<ExplorerItem>? AddToFavoritesRequested;
        public event Action<ExplorerItem>? EditFileRequested;

        private void ItemContextMenu_Opening(object sender, object e)
        {
            if (sender is MenuFlyout menu)
            {
                foreach (var flyoutItem in menu.Items)
                {
                    if (flyoutItem is MenuFlyoutItem menuItem)
                    {
                        if (menuItem.Text == "Add to Favorites")
                        {
                            if (menuItem.DataContext is ExplorerItem explorerItem)
                            {
                                menuItem.Visibility = explorerItem.IsDirectory ? Visibility.Visible : Visibility.Collapsed;
                            }
                        }
                        else if (menuItem.Text == "Edit File")
                        {
                            if (menuItem.DataContext is ExplorerItem explorerItem)
                            {
                                menuItem.Visibility = !explorerItem.IsDirectory ? Visibility.Visible : Visibility.Collapsed;
                            }
                        }
                        else if (menuItem.Text == "Execute")
                        {
                            if (menuItem.DataContext is ExplorerItem explorerItem)
                            {
                                string ext = System.IO.Path.GetExtension(explorerItem.Name).ToLower();
                                bool canExecute = !explorerItem.IsDirectory && (ext == ".sh" || ext == ".bin" || string.IsNullOrEmpty(ext));
                                menuItem.Visibility = canExecute ? Visibility.Visible : Visibility.Collapsed;
                            }
                        }
                    }
                }
            }
        }

        private void AddToFavoritesItem_Click(object sender, RoutedEventArgs e)
        {
            if (sender is MenuFlyoutItem menu && menu.Tag is ExplorerItem item)
            {
                AddToFavoritesRequested?.Invoke(item);
            }
        }

        private void EditFileItem_Click(object sender, RoutedEventArgs e)
        {
            if (sender is MenuFlyoutItem menu && menu.Tag is ExplorerItem item)
            {
                EditFileRequested?.Invoke(item);
            }
        }

        // ================= IN-PLACE EDITING (RENAME / MKDIR) =================

        public void StartRename(ExplorerItem item)
        {
            if (item == null) return;
            _originalNameBeforeEdit = item.Name;
            item.IsEditing = true;

            FocusTextBox(item);
        }

        private void RenameItem_Click(object sender, RoutedEventArgs e)
        {
            if (sender is MenuFlyoutItem menu && menu.Tag is ExplorerItem item)
            {
                StartRename(item);
            }
        }

        private void FocusTextBox(ExplorerItem item)
        {
            var view = _viewMode == "list" ? (ListViewBase)ItemsListView : (ListViewBase)ItemsGridView;
            view.ScrollIntoView(item);

            DispatcherQueue.TryEnqueue(async () =>
            {
                await System.Threading.Tasks.Task.Delay(50);
                var container = view.ContainerFromItem(item);
                if (container != null)
                {
                    var textBox = FindTextBoxInContainer(container);
                    if (textBox != null)
                    {
                        textBox.Focus(FocusState.Programmatic);
                        textBox.SelectAll();
                    }
                }
            });
        }

        private TextBox? FindTextBoxInContainer(DependencyObject parent)
        {
            for (int i = 0; i < Microsoft.UI.Xaml.Media.VisualTreeHelper.GetChildrenCount(parent); i++)
            {
                var child = Microsoft.UI.Xaml.Media.VisualTreeHelper.GetChild(parent, i);
                if (child is TextBox textBox && textBox.Name == "RenameTextBox")
                {
                    return textBox;
                }
                var result = FindTextBoxInContainer(child);
                if (result != null) return result;
            }
            return null;
        }

        private async void CommitRename(ExplorerItem item, string newName)
        {
            if (_isCommitting || !item.IsEditing) return;
            _isCommitting = true;
            item.IsEditing = false; // Turn editing off immediately to prevent lost-focus re-entry during directory refresh

            try
            {
                string cleanName = (newName ?? "").Trim();
                if (string.IsNullOrEmpty(cleanName))
                {
                    CancelRename(item);
                    return;
                }

                // Unchanged rename check
                if (cleanName == _originalNameBeforeEdit && !item.IsNewItem)
                {
                    item.IsEditing = false;
                    return;
                }

                // Duplicate naming collision checks
                bool hasCollision = _items.Any(i => i != item && string.Equals(i.Name, cleanName, StringComparison.OrdinalIgnoreCase));
                if (hasCollision)
                {
                    var dialog = new ContentDialog
                    {
                        Title = "Name Collision",
                        Content = $"A directory or file with the name '{cleanName}' already exists. What would you like to do?",
                        PrimaryButtonText = "Postfix with '_new'",
                        CloseButtonText = "Cancel",
                        XamlRoot = this.XamlRoot
                    };

                    var result = await dialog.ShowAsync();
                    if (result == ContentDialogResult.Primary)
                    {
                        string candidate = cleanName;
                        bool stillCollides = true;
                        for (int depth = 1; depth <= 5; depth++)
                        {
                            string ext = Path.GetExtension(candidate);
                            string baseName = Path.GetFileNameWithoutExtension(candidate);
                            candidate = baseName + "_new" + ext;

                            if (!_items.Any(i => i != item && string.Equals(i.Name, candidate, StringComparison.OrdinalIgnoreCase)))
                            {
                                stillCollides = false;
                                break;
                            }
                        }

                        if (stillCollides)
                        {
                            var errDialog = new ContentDialog
                            {
                                Title = "Rename Error",
                                Content = "Too many collisions (exceeded max depth of 5). Operation cancelled.",
                                CloseButtonText = "OK",
                                XamlRoot = this.XamlRoot
                            };
                            await errDialog.ShowAsync();
                            CancelRename(item);
                            return;
                        }
                        cleanName = candidate;
                    }
                    else
                    {
                        CancelRename(item);
                        return;
                    }
                }

                // Proceed with saving modifications
                if (item.IsNewItem)
                {
                    _items.Remove(item);
                    NewFolderRequested?.Invoke(cleanName);
                }
                else
                {
                    string oldName = _originalNameBeforeEdit;
                    item.Name = cleanName;
                    item.IsEditing = false;
                    RenameCommitted?.Invoke(item, oldName, cleanName);
                }
            }
            finally
            {
                _isCommitting = false;
            }
        }

        private void CancelRename(ExplorerItem item)
        {
            if (item.IsNewItem)
            {
                _items.Remove(item);
                ApplySort();
            }
            else
            {
                item.Name = _originalNameBeforeEdit;
                item.IsEditing = false;
            }
        }

        private void RenameTextBox_LostFocus(object sender, RoutedEventArgs e)
        {
            if (sender is TextBox textBox && textBox.DataContext is ExplorerItem item)
            {
                CommitRename(item, textBox.Text);
            }
        }

        private void RenameTextBox_KeyDown(object sender, KeyRoutedEventArgs e)
        {
            if (sender is TextBox textBox && textBox.DataContext is ExplorerItem item)
            {
                if (e.Key == Windows.System.VirtualKey.Enter)
                {
                    e.Handled = true;
                    CommitRename(item, textBox.Text);
                }
                else if (e.Key == Windows.System.VirtualKey.Escape)
                {
                    e.Handled = true;
                    CancelRename(item);
                }
            }
        }

        // ================= CONTEXT MENU HANDLERS =================

        private void CopyPathItem_Click(object sender, RoutedEventArgs e)
        {
            if (sender is MenuFlyoutItem menu && menu.Tag is ExplorerItem item)
            {
                CopyPathRequested?.Invoke(item);
            }
        }

        private void ExecuteItem_Click(object sender, RoutedEventArgs e)
        {
            if (sender is MenuFlyoutItem menu && menu.Tag is ExplorerItem item)
            {
                ExecuteRequested?.Invoke(item);
            }
        }

        private void DownloadItem_Click(object sender, RoutedEventArgs e)
        {
            DownloadRequested?.Invoke();
        }

        private void CopyItem_Click(object sender, RoutedEventArgs e)
        {
            CopyRequested?.Invoke();
        }

        private void DeleteItem_Click(object sender, RoutedEventArgs e)
        {
            DeleteRequested?.Invoke();
        }

        private void PropertiesItem_Click(object sender, RoutedEventArgs e)
        {
            PropertiesRequested?.Invoke();
        }

        private void PasteContextBtn_Click(object sender, RoutedEventArgs e)
        {
            PasteRequested?.Invoke();
        }

        private void NewFolderContextBtn_Click(object sender, RoutedEventArgs e)
        {
            var newItem = new ExplorerItem
            {
                IsDirectory = true,
                Name = "New Directory",
                IsNewItem = true,
                IsEditing = true
            };

            _items.Add(newItem);
            ApplySort();

            _originalNameBeforeEdit = "New Directory";
            FocusTextBox(newItem);
        }

        // ================= DRAG AND DROP =================

        private void ContentsGrid_DragOver(object sender, DragEventArgs e)
        {
            e.AcceptedOperation = Windows.ApplicationModel.DataTransfer.DataPackageOperation.Copy;
            e.DragUIOverride.Caption = "Upload to device";
            e.DragUIOverride.IsCaptionVisible = true;
        }

        private async void ContentsGrid_Drop(object sender, DragEventArgs e)
        {
            if (e.DataView.Contains(Windows.ApplicationModel.DataTransfer.StandardDataFormats.StorageItems))
            {
                var storageItems = await e.DataView.GetStorageItemsAsync();
                if (storageItems != null && storageItems.Count > 0)
                {
                    ItemsDropped?.Invoke(storageItems);
                }
            }
        }

        private void Items_KeyDown(object sender, Microsoft.UI.Xaml.Input.KeyRoutedEventArgs e)
        {
            var ctrl = Microsoft.UI.Input.InputKeyboardSource.GetKeyStateForCurrentThread(Windows.System.VirtualKey.Control);
            bool isCtrlPressed = ctrl.HasFlag(Windows.UI.Core.CoreVirtualKeyStates.Down);

            if (isCtrlPressed)
            {
                if (e.Key == Windows.System.VirtualKey.C)
                {
                    e.Handled = true;
                    CopyRequested?.Invoke();
                }
                else if (e.Key == Windows.System.VirtualKey.V)
                {
                    e.Handled = true;
                    PasteRequested?.Invoke();
                }
            }
            else if (e.Key == Windows.System.VirtualKey.Delete)
            {
                e.Handled = true;
                DeleteRequested?.Invoke();
            }
        }
    }
}
