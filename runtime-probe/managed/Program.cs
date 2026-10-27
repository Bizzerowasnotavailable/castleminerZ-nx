using System.Diagnostics;
using System.Globalization;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Xna.Framework;
using Microsoft.Xna.Framework.Content;
using Microsoft.Xna.Framework.GamerServices;
using Microsoft.Xna.Framework.Graphics;
using Microsoft.Xna.Framework.Net;

[DllImport("__Internal")]
static extern int cmz_vfs_mounted();

[DllImport("__Internal")]
static extern IntPtr cmz_vfs_package_path();

[DllImport("__Internal")]
static extern IntPtr cmz_vfs_content_directory();

[DllImport("__Internal")]
static extern void cmz_host_log(string message);

bool isSwitch = OperatingSystem.IsOSPlatform("libnx");

// The probe reports through the native host instead of System.IO. SDL and the
// libnx console both want the default NWindow, so no console is used; the host
// writes straight to sdmc:/switch/castleminerz-logs/host.log.
if (isSwitch)
{
    Console.SetOut(new HostLogWriter());
    Console.SetError(new HostLogWriter());

    cmz_host_log("managed entry point reached");
    cmz_host_log($"cwd: {Environment.CurrentDirectory}");
    cmz_host_log($"fullpath game root: {Path.GetFullPath("/game")}");
    cmz_host_log($"fullpath save root: {Path.GetFullPath("/save")}");
    cmz_host_log($"fullpath logs root: {Path.GetFullPath("/logs")}");
    cmz_host_log($"fullpath sdmc device path: {Path.GetFullPath("sdmc:/switch/castleminerz-logs")}");
    cmz_host_log($"rooted /game: {Path.IsPathRooted("/game")}");
    cmz_host_log($"rooted sdmc: {Path.IsPathRooted("sdmc:/switch")}");

    try
    {
        File.WriteAllText("sdmc:/switch/castleminerz-logs/path-test.txt", "ok");
        cmz_host_log("write sdmc device path: ok");
    }
    catch (Exception exception)
    {
        cmz_host_log($"write sdmc device path failed: {exception.GetType().Name}: {exception.Message}");
    }

    try
    {
        Directory.CreateDirectory("/save");
        cmz_host_log("create virtual /save: ok");
    }
    catch (Exception exception)
    {
        cmz_host_log($"create virtual /save failed: {exception.GetType().Name}: {exception.Message}");
    }

    try
    {
        File.WriteAllText("/logs/path-test.txt", "ok");
        cmz_host_log("write virtual /logs path: ok");
    }
    catch (Exception exception)
    {
        cmz_host_log($"write virtual /logs path failed: {exception.GetType().Name}: {exception.Message}");
    }

    try
    {
        using FileStream stream = File.OpenRead("/game/Content/LoadScreen.xnb");
        byte[] header = new byte[8];
        int read = stream.Read(header, 0, header.Length);
        cmz_host_log(
            $"read rooted /game/Content/LoadScreen.xnb: {stream.Length} bytes, header={Convert.ToHexString(header)}");
    }
    catch (Exception exception)
    {
        cmz_host_log(
            $"read rooted /game/Content/LoadScreen.xnb failed: {exception.GetType().Name}: {exception.Message}");
    }
}

Console.WriteLine("CastleMiner Z NX managed runtime probe");
Console.WriteLine($"OS: {RuntimeInformation.OSDescription}");
Console.WriteLine($"Architecture: {RuntimeInformation.ProcessArchitecture}");
Console.WriteLine($"Runtime: {RuntimeInformation.FrameworkDescription}");
Console.WriteLine($"Environment version: {Environment.Version}");
Console.WriteLine($"Switch platform: {isSwitch}");

Assembly assembly = Assembly.GetExecutingAssembly();
MethodInfo? reflectedMethod = typeof(ProbeTarget).GetMethod(nameof(ProbeTarget.Double));
object? reflectedResult = reflectedMethod?.Invoke(null, new object[] { 21 });
Console.WriteLine($"Reflection: {assembly.GetName().Name}, result={reflectedResult}");

int threadResult = 0;
Thread worker = new(() => threadResult = ProbeTarget.Double(21));
worker.Start();
worker.Join();
Console.WriteLine($"Threading: result={threadResult}");

long before = GC.GetTotalMemory(false);
byte[][] allocations = new byte[16][];
for (int index = 0; index < allocations.Length; index++)
{
    allocations[index] = new byte[64 * 1024];
}
GC.Collect();
GC.WaitForPendingFinalizers();
long after = GC.GetTotalMemory(true);
Console.WriteLine($"GC: before={before}, after={after}");

Stopwatch timer = Stopwatch.StartNew();
Thread.Sleep(100);
timer.Stop();
Console.WriteLine($"Timer: {timer.ElapsedMilliseconds} ms");

try
{
    Console.WriteLine($"SD root visible: {Directory.Exists("sdmc:/")}");
}
catch (Exception exception)
{
    Console.WriteLine($"Filesystem probe failed: {exception.GetType().Name}: {exception.Message}");
}

ProbePackageMount();
ProbeXnaCompatibility();

if (isSwitch)
{
    RunFnaGraphicsProbe();
}

Console.WriteLine();
Console.WriteLine("PASS: managed entry point completed.");
Console.WriteLine("This is a runtime test only; CastleMiner Z is not running yet.");

Thread.Sleep(TimeSpan.FromSeconds(15));

// Reads a real asset out of the user's STFS package through System.IO, which on
// mono-nx goes through newlib open/read and therefore through the host's
// read-only "stfs:" devoptab. This is the end-to-end proof that managed file
// APIs see the package without it being copied or unpacked.
static void ProbePackageMount()
{
    if (!OperatingSystem.IsOSPlatform("libnx"))
    {
        return;
    }

    try
    {
        if (cmz_vfs_mounted() == 0)
        {
            Console.WriteLine("VFS: not mounted (no CastMiner Z package found)");
            return;
        }

        string package = Marshal.PtrToStringUTF8(cmz_vfs_package_path()) ?? "";
        string content = Marshal.PtrToStringUTF8(cmz_vfs_content_directory()) ?? "";
        Console.WriteLine($"VFS: mounted, package={package}");
        Console.WriteLine($"VFS: content={content}");

        string asset = Path.Combine(content, "LoadScreen.xnb");
        using FileStream stream = File.OpenRead(asset);
        byte[] header = new byte[8];
        int read = stream.Read(header, 0, header.Length);
        Console.WriteLine(
            $"VFS: read {read} bytes from {asset}, header={Convert.ToHexString(header)}");
        Console.WriteLine($"VFS: length={stream.Length}");
    }
    catch (Exception exception)
    {
        Console.WriteLine(
            $"VFS probe failed: {exception.GetType().FullName}: {exception.Message}");
    }
}

static void RunFnaGraphicsProbe()
{
    // FNA reads these before any platform type is touched. FNA3D's driver
    // selection and its ES3 requirement are SDL hints, which fall back to the
    // process environment when they have not been set programmatically.
    Environment.SetEnvironmentVariable("FNA_PLATFORM_BACKEND", "SDL2");
    Environment.SetEnvironmentVariable("FNA3D_FORCE_DRIVER", "OpenGL");
    Environment.SetEnvironmentVariable("FNA3D_OPENGL_FORCE_ES3", "1");
    Environment.SetEnvironmentVariable("FNA3D_OPENGL_WINDOW_DEPTHSTENCILFORMAT", "Depth24Stencil8");

    Console.WriteLine();
    Console.WriteLine("FNA graphics probe: SDL2 -> EGL -> switch-mesa -> FNA3D (GLES3)");

    FnaProbeGame? game = null;
    try
    {
        game = new FnaProbeGame();
        game.Run();
    }
    catch (Exception exception)
    {
        Console.WriteLine($"FNA probe failed: {exception.GetType().FullName}: {exception.Message}");
        Console.WriteLine(exception.StackTrace);
    }
    finally
    {
        try
        {
            game?.Dispose();
        }
        catch (Exception disposeError)
        {
            Console.WriteLine($"FNA probe dispose failed: {disposeError.Message}");
        }
    }

    Console.WriteLine($"FNA device: {FnaProbeGame.DeviceSummary ?? "not created"}");
    Console.WriteLine($"FNA sprites: {FnaProbeGame.SpriteStatus ?? "not attempted"}");
    Console.WriteLine($"FNA title location: {FnaProbeGame.TitleStatus ?? "not attempted"}");
    Console.WriteLine(
        $"FNA frames: {FnaProbeGame.FramesPresented} in " +
        $"{FnaProbeGame.ElapsedMilliseconds} ms");

    if (FnaProbeGame.FramesPresented > 0 && FnaProbeGame.ElapsedMilliseconds > 0)
    {
        double fps = FnaProbeGame.FramesPresented * 1000.0 / FnaProbeGame.ElapsedMilliseconds;
        Console.WriteLine($"FNA frame rate: {fps.ToString("0.0", CultureInfo.InvariantCulture)} fps");
    }

    WriteProbeLog();
}

static void WriteProbeLog()
{
    try
    {
        string directory = "/logs";
        Directory.CreateDirectory(directory);
        string path = Path.Combine(directory, "fna-graphics-probe.log");
        File.WriteAllLines(
            path,
            new[]
            {
                $"device: {FnaProbeGame.DeviceSummary ?? "not created"}",
                $"sprites: {FnaProbeGame.SpriteStatus ?? "not attempted"}",
                $"title: {FnaProbeGame.TitleStatus ?? "not attempted"}",
                $"frames: {FnaProbeGame.FramesPresented}",
                $"elapsedMs: {FnaProbeGame.ElapsedMilliseconds}",
            });
        Console.WriteLine($"FNA probe log: {path}");
    }
    catch (Exception exception)
    {
        Console.WriteLine($"FNA probe log failed: {exception.GetType().Name}: {exception.Message}");
    }
}

static void ProbeXnaCompatibility()
{
    Matrix rotation = Matrix.CreateRotationY(MathF.PI / 4.0f);
    Vector3 transformed = Vector3.Transform(Vector3.UnitX, rotation);
    Console.WriteLine(
        $"FNA math: x={transformed.X.ToString("0.000", CultureInfo.InvariantCulture)}, " +
        $"y={transformed.Y.ToString("0.000", CultureInfo.InvariantCulture)}, " +
        $"z={transformed.Z.ToString("0.000", CultureInfo.InvariantCulture)}");

    Console.WriteLine($"Guide trial mode: {Guide.IsTrialMode}");
    Guide.ShowSignIn(1, onlineOnly: false);
    SignedInGamer gamer = Gamer.SignedInGamers[0];
    Console.WriteLine($"Gamer: tag={gamer.Gamertag}, index={gamer.PlayerIndex}");

    IAsyncResult dialog = Guide.BeginShowMessageBox(
        "CastleMiner Z",
        "Choose an option.",
        new[] { "Continue", "Cancel" },
        focusButton: 0,
        MessageBoxIcon.None,
        callback: null,
        state: null);
    Console.WriteLine($"Guide dialog: selection={Guide.EndShowMessageBox(dialog)}");

    var properties = new NetworkSessionProperties { [0] = 42 };
    using NetworkSession session = NetworkSession.EndCreate(
        NetworkSession.BeginCreate(
            NetworkSessionType.Local,
            new[] { gamer },
            maxGamers: 4,
            privateGamerSlots: 0,
            properties,
            callback: null,
            state: null));

    LocalNetworkGamer local = session.LocalGamers[0];
    byte[] payload = { 1, 2, 3, 4 };
    local.SendData(payload, 0, payload.Length, SendDataOptions.ReliableInOrder);
    int received = local.ReceiveData(new byte[8], out NetworkGamer sender);
    Console.WriteLine(
        $"NetworkSession: type={session.SessionType}, host={session.IsHost}, " +
        $"received={received}, sender={sender.Gamertag}, property={session.SessionProperties[0]}");

    AvatarDescription description = AvatarDescription.CreateRandom();
    using var renderer = new AvatarRenderer(description);
    Console.WriteLine(
        $"Avatar: body={description.BodyType}, valid={description.IsValid}, " +
        $"state={renderer.State}, bones={renderer.BindPose.Count}");
}

static class ProbeTarget
{
    public static int Double(int value) => value * 2;
}

/// <summary>
/// Minimal FNA game: creates a real GraphicsDevice through FNA3D, clears and
/// presents a moving colour, and draws two sprites when texture creation works.
/// It exists to prove the managed graphics path on hardware, not to render
/// anything from CastleMiner Z.
/// </summary>
sealed class FnaProbeGame : Game
{
    private const int FrameLimit = 240;

    private readonly GraphicsDeviceManager graphics;
    private readonly Stopwatch clock = new();

    private SpriteBatch? spriteBatch;
    private Texture2D? pixel;
    private int frames;

    public static string? DeviceSummary { get; private set; }
    public static string? SpriteStatus { get; private set; }
    public static string? TitleStatus { get; private set; }
    public static int FramesPresented { get; private set; }
    public static long ElapsedMilliseconds { get; private set; }

    public FnaProbeGame()
    {
        graphics = new GraphicsDeviceManager(this)
        {
            PreferredBackBufferWidth = 1280,
            PreferredBackBufferHeight = 720,
            SynchronizeWithVerticalRetrace = true,
            IsFullScreen = false,
            PreferMultiSampling = false,
        };

        IsFixedTimeStep = false;
        Content.RootDirectory = "Content";
    }

    protected override void Initialize()
    {
        base.Initialize();

        GraphicsDevice device = GraphicsDevice;
        PresentationParameters presentation = device.PresentationParameters;
        DeviceSummary = string.Format(
            CultureInfo.InvariantCulture,
            "adapter='{0}', profile={1}, status={2}, backbuffer={3}x{4}, depth={5}",
            device.Adapter.Description,
            device.GraphicsProfile,
            device.GraphicsDeviceStatus,
            presentation.BackBufferWidth,
            presentation.BackBufferHeight,
            presentation.DepthStencilFormat);

        clock.Start();

        // This is the path the real game uses: ContentManager.RootDirectory is
        // resolved against FNA's TitleLocation, which the host points at the
        // mounted package through the replacement SDL_GetBasePath.
        try
        {
            using Stream stream = TitleContainer.OpenStream("Content/LoadScreen.xnb");
            byte[] header = new byte[8];
            int read = stream.Read(header, 0, header.Length);
            TitleStatus = $"read {read} bytes, header={Convert.ToHexString(header)}";
        }
        catch (Exception exception)
        {
            TitleStatus = $"{exception.GetType().Name}: {exception.Message}";
        }

        try
        {
            pixel = new Texture2D(device, 1, 1);
            pixel.SetData(new[] { Color.White });
            spriteBatch = new SpriteBatch(device);
            SpriteStatus = "ready";
        }
        catch (Exception exception)
        {
            // Keep the clear/present loop running even when the textured path
            // is not available yet: the frame count is the more useful signal.
            SpriteStatus = $"{exception.GetType().Name}: {exception.Message}";
            spriteBatch = null;
            pixel = null;
        }
    }

    protected override void Update(GameTime gameTime)
    {
        base.Update(gameTime);

        frames++;
        if (frames >= FrameLimit)
        {
            Exit();
        }
    }

    protected override void Draw(GameTime gameTime)
    {
        double phase = gameTime.TotalGameTime.TotalSeconds;
        Color background = new(
            (byte)(128 + 96 * Math.Sin(phase)),
            (byte)(128 + 96 * Math.Sin(phase + 2.0943951)),
            (byte)(128 + 96 * Math.Sin(phase + 4.1887902)));

        GraphicsDevice.Clear(background);

        if (spriteBatch != null && pixel != null)
        {
            int offset = (int)(160 * Math.Sin(phase * 2.0));
            spriteBatch.Begin();
            spriteBatch.Draw(pixel, new Rectangle(64 + offset, 64, 512, 256), Color.White);
            spriteBatch.Draw(pixel, new Rectangle(700 - offset, 400, 512, 256), Color.Black);
            spriteBatch.End();
        }

        base.Draw(gameTime);

        FramesPresented = frames;
        ElapsedMilliseconds = clock.ElapsedMilliseconds;
    }
}

// Console output for this probe goes straight to the native host log. That
// keeps progress observable even when System.IO path handling is what is being
// investigated.
sealed class HostLogWriter : TextWriter
{
    [DllImport("__Internal")]
    private static extern void cmz_host_log(string message);

    public override Encoding Encoding => Encoding.UTF8;

    public override void Write(string? value)
    {
        if (!string.IsNullOrEmpty(value))
        {
            cmz_host_log(value);
        }
    }

    public override void WriteLine(string? value)
    {
        cmz_host_log(value ?? string.Empty);
    }

    public override void WriteLine()
    {
    }
}
