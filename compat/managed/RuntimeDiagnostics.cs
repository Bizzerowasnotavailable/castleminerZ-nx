using System.Runtime.InteropServices;

namespace CastleMinerZ.XnaCompat;

public static class RuntimeDiagnostics
{
    [DllImport("__Internal", EntryPoint = "cmz_host_log")]
    private static extern void HostLog(string message);

    // Log before the game's Xbox error UI creates another GraphicsDevice. SDL's
    // Switch backend has one native window, so that UI can mask the real error.
    public static void ReportException(Exception exception)
    {
        try
        {
            string message = "Game exception: " + exception;
            if (OperatingSystem.IsOSPlatform("libnx"))
                HostLog(message);
            else
                Console.Error.WriteLine(message);
        }
        catch
        {
            // Reporting must never replace the original exception.
        }
    }
}
