using System;
using System.Threading.Tasks;

namespace serial_console_dotnet.Services
{
    public class SdCardService
    {
        private readonly ConnectionService _connectionService;
        private readonly ShellExecutionService _shellExecutionService;

        public SdCardService(ConnectionService connectionService, ShellExecutionService shellExecutionService)
        {
            _connectionService = connectionService;
            _shellExecutionService = shellExecutionService;
        }

        public async Task MountSdCardAsync()
        {
            if (!_connectionService.IsConnected) return;

            try
            {
                string[] commands = new string[]
                {
                    "insmod /usr/modules/mmc_core.ko",
                    "insmod /usr/modules/mmc_block.ko",
                    "insmod /usr/modules/ak_mci.ko",
                    "insmod /usr/modules/exfat.ko"
                };

                _shellExecutionService.LogMessage("--- Loading MMC/SD Drivers & Preparing Mount Point ---", "info");

                foreach (var cmd in commands)
                {
                    _connectionService.Write(cmd, "lf");
                    await Task.Delay(300);
                }

                // Wait 1.2s for device nodes to register in /dev before mounting
                await Task.Delay(1200);

                _shellExecutionService.LogMessage("--- Mounting SD Card Partition ---", "info");
                _connectionService.Write("mount -t vfat /dev/mmcblk0p1 /mnt || mount -t exfat /dev/mmcblk0p1 /mnt || mount /dev/mmcblk0p1 /mnt || mount /dev/mmcblk0 /mnt", "lf");
            }
            catch (Exception ex)
            {
                _shellExecutionService.LogMessage($"SD card mounting sequence failed: {ex.Message}", "error");
            }
        }

        public async Task UnmountSdCardAsync()
        {
            if (!_connectionService.IsConnected) return;

            try
            {
                _shellExecutionService.LogMessage("--- Safely Unmounting SD Card ---", "info");
                _connectionService.Write("cd /", "lf");
                await Task.Delay(200);
                _connectionService.Write("sync", "lf");
                await Task.Delay(200);
                _connectionService.Write("umount /mnt", "lf");
            }
            catch (Exception ex)
            {
                _shellExecutionService.LogMessage($"SD card unmount sequence failed: {ex.Message}", "error");
            }
        }
    }
}
