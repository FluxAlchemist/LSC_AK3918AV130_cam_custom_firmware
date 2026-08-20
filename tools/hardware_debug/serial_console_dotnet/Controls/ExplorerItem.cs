using System.ComponentModel;
using System.Runtime.CompilerServices;
using Microsoft.UI.Xaml;

namespace serial_console_dotnet.Controls
{
    public class ExplorerItem : INotifyPropertyChanged
    {
        public bool IsDirectory { get; set; }
        public string Name { get; set; } = "";
        public long Size { get; set; }
        public bool IsSymlink { get; set; }
        public string SymlinkTarget { get; set; } = "";
        public bool IsNewItem { get; set; }

        private bool _isEditing;
        public bool IsEditing
        {
            get => _isEditing;
            set
            {
                if (_isEditing != value)
                {
                    _isEditing = value;
                    OnPropertyChanged(nameof(IsEditing));
                    OnPropertyChanged(nameof(EditingVisibility));
                    OnPropertyChanged(nameof(NormalVisibility));
                }
            }
        }

        public string DisplayName => IsSymlink ? $"{Name} -> {SymlinkTarget}" : Name;
        
        public Visibility EditingVisibility => IsEditing ? Visibility.Visible : Visibility.Collapsed;
        public Visibility NormalVisibility => IsEditing ? Visibility.Collapsed : Visibility.Visible;

        public string Icon
        {
            get
            {
                if (IsSymlink) return "\uE167"; // Link icon
                if (IsDirectory) return "\uE8B7"; // Folder icon
                
                string ext = System.IO.Path.GetExtension(Name).ToLower();
                switch (ext)
                {
                    case ".txt":
                    case ".md":
                        return "\uE15F"; // Page/Document
                    case ".sh":
                        return "\uE943"; // Code/Script
                    case ".log":
                        return "\uE9F9"; // History/Logs
                    case ".conf":
                    case ".config":
                    case ".cfg":
                    case ".ini":
                        return "\uE115"; // Setting/Config
                    case ".so":
                        return "\uE1DF"; // System Library
                    case ".ko":
                        return "\uEA86"; // Kernel Module (Plug-in/Add-in Puzzle)
                    case ".bin":
                        return "\uE7F4"; // Binary Tool/Build
                    default:
                        return "\uE102"; // Generic document
                }
            }
        }

        public string IconColor
        {
            get
            {
                if (IsSymlink) return "#38BDF8"; // Sky Blue (Link/Shortcut)
                if (IsDirectory) return "#00D2FF"; // Cyan Folder
                
                string ext = System.IO.Path.GetExtension(Name).ToLower();
                switch (ext)
                {
                    case ".txt":
                    case ".md":
                        return "#3B82F6"; // Blue (Text/Markdown)
                    case ".sh":
                        return "#10B981"; // Emerald Green (Scripts)
                    case ".log":
                        return "#A855F7"; // Purple (Log files)
                    case ".conf":
                    case ".config":
                    case ".cfg":
                    case ".ini":
                        return "#F59E0B"; // Amber (Configs)
                    case ".so":
                        return "#EF4444"; // Red/Rose (Shared library)
                    case ".ko":
                        return "#EC4899"; // Pink/Magenta (Kernel module)
                    case ".bin":
                        return "#14B8A6"; // Teal (Binary/Executables)
                    default:
                        return "#98A2B3"; // Slate Gray (Generic file)
                }
            }
        }

        public string SizeLabel
        {
            get
            {
                if (IsDirectory) return "";
                if (Size < 1024) return $"{Size} B";
                if (Size < 1048576) return $"{(Size / 1024.0):F1} KB";
                return $"{(Size / 1048576.0):F1} MB";
            }
        }

        public string ItemTypeLabel
        {
            get
            {
                if (IsSymlink) return "Symbolic Link";
                if (IsDirectory) return "Directory";
                string ext = System.IO.Path.GetExtension(Name).ToLower();
                switch (ext)
                {
                    case ".txt": return "Text Document";
                    case ".md": return "Markdown Document";
                    case ".log": return "Log File";
                    case ".sh": return "Shell Script";
                    case ".conf":
                    case ".config":
                    case ".cfg":
                    case ".ini": return "Configuration File";
                    case ".bin": return "Binary File";
                    case ".json": return "JSON File";
                    case ".xml": return "XML Document";
                    case ".ko": return "Kernel Module";
                    case ".so": return "Shared Library";
                    case "": return "File";
                    default: return $"{ext.TrimStart('.').ToUpper()} File";
                }
            }
        }

        public event PropertyChangedEventHandler? PropertyChanged;

        protected void OnPropertyChanged([CallerMemberName] string? propertyName = null)
        {
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(propertyName));
        }
    }
}
