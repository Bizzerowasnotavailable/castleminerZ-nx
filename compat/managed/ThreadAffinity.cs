using System.Threading;

namespace CastleMinerZ.XnaCompat;

// Xbox 360 titles pin managed threads to Xenon hardware threads through
// Thread.SetProcessorAffinity(int[]). That member does not exist in .NET 9, so
// the managed relinker redirects calls to this helper. Switch thread placement
// belongs to Horizon, which does not expose an equivalent knob to user code, so
// the shim intentionally does nothing.
public static class ThreadAffinity
{
    public static void SetProcessorAffinity(Thread thread, int[] affinity)
    {
        ArgumentNullException.ThrowIfNull(thread);
        ArgumentNullException.ThrowIfNull(affinity);
    }
}
