using System;

namespace serial_console_dotnet.Services
{
    public class AnsiInputEncoder
    {
        public byte[]? EncodeKey(Windows.System.VirtualKey key, string lineEnding)
        {
            switch (key)
            {
                case Windows.System.VirtualKey.Enter:
                    if (lineEnding == "crlf") return new byte[] { 13, 10 };
                    if (lineEnding == "cr") return new byte[] { 13 };
                    return new byte[] { 10 };

                case Windows.System.VirtualKey.Back:
                    return new byte[] { 127 };

                case Windows.System.VirtualKey.Tab:
                    return new byte[] { 9 };

                case Windows.System.VirtualKey.Escape:
                    return new byte[] { 27 };

                case Windows.System.VirtualKey.Up:
                    return new byte[] { 27, 91, 65 };

                case Windows.System.VirtualKey.Down:
                    return new byte[] { 27, 91, 66 };

                case Windows.System.VirtualKey.Right:
                    return new byte[] { 27, 91, 67 };

                case Windows.System.VirtualKey.Left:
                    return new byte[] { 27, 91, 68 };

                default:
                    return null;
            }
        }
    }
}
