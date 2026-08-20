using System;
using System.Collections.Generic;
using System.Threading.Tasks;
using Windows.Storage;
using Windows.Storage.Pickers;

namespace serial_console_dotnet.Services
{
    public interface IDialogService
    {
        Task<string?> PickFolderAsync();
        Task<string?> PickSaveFileAsync(string suggestedFileName, string defaultExt);
    }

    public class WindowsDialogService : IDialogService
    {
        private readonly IntPtr _hwnd;

        public WindowsDialogService(IntPtr hwnd)
        {
            _hwnd = hwnd;
        }

        public async Task<string?> PickFolderAsync()
        {
            var picker = new FolderPicker();
            picker.SuggestedStartLocation = PickerLocationId.ComputerFolder;
            picker.FileTypeFilter.Add("*");

            WinRT.Interop.InitializeWithWindow.Initialize(picker, _hwnd);

            StorageFolder folder = await picker.PickSingleFolderAsync();
            return folder?.Path;
        }

        public async Task<string?> PickSaveFileAsync(string suggestedFileName, string defaultExt)
        {
            var picker = new FileSavePicker();
            picker.SuggestedStartLocation = PickerLocationId.ComputerFolder;
            picker.SuggestedFileName = suggestedFileName;
            
            string ext = System.IO.Path.GetExtension(suggestedFileName);
            if (string.IsNullOrEmpty(ext)) ext = defaultExt;
            
            picker.FileTypeChoices.Add("File", new List<string> { ext });

            WinRT.Interop.InitializeWithWindow.Initialize(picker, _hwnd);

            StorageFile file = await picker.PickSaveFileAsync();
            return file?.Path;
        }
    }
}
