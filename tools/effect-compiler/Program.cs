using System.Runtime.InteropServices;
using System.Text;

// This build-time tool needs the installed DirectX June 2010 runtime. Neither
// the DLL nor any original game shader is copied into the port's outputs.
internal static class Program
{
    private const int CompileEffectSlot = 59;

    [DllImport("d3dx9_43.dll", CharSet = CharSet.Unicode, ExactSpelling = true)]
    private static extern int D3DXCreateEffectCompilerFromFileW(
        string file, IntPtr defines, IntPtr include, uint flags,
        out IntPtr compiler, out IntPtr errors);

    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    private delegate int CompileEffect(IntPtr self, uint flags, out IntPtr code, out IntPtr errors);

    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    private delegate IntPtr BufferPointer(IntPtr self);

    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    private delegate uint BufferSize(IntPtr self);

    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    private delegate uint ReleaseObject(IntPtr self);

    private static T Method<T>(IntPtr instance, int slot) where T : Delegate =>
        Marshal.GetDelegateForFunctionPointer<T>(
            Marshal.ReadIntPtr(Marshal.ReadIntPtr(instance), slot * IntPtr.Size));

    private static byte[] ReadBuffer(IntPtr buffer)
    {
        if (buffer == IntPtr.Zero)
            return Array.Empty<byte>();
        int length = checked((int)Method<BufferSize>(buffer, 4)(buffer));
        byte[] bytes = new byte[length];
        Marshal.Copy(Method<BufferPointer>(buffer, 3)(buffer), bytes, 0, length);
        return bytes;
    }

    private static void Release(IntPtr instance)
    {
        if (instance != IntPtr.Zero)
            Method<ReleaseObject>(instance, 2)(instance);
    }

    private static int Main(string[] args)
    {
        if (args.Length != 2)
        {
            Console.Error.WriteLine("usage: Cmz.EffectCompiler <source.fx> <output.fxb>");
            return 2;
        }

        IntPtr compiler = IntPtr.Zero, parseErrors = IntPtr.Zero;
        IntPtr code = IntPtr.Zero, compileErrors = IntPtr.Zero;
        try
        {
            if (!OperatingSystem.IsWindows())
                throw new PlatformNotSupportedException("D3DX9 effect compilation requires Windows.");

            string source = Path.GetFullPath(args[0]);
            string output = Path.GetFullPath(args[1]);
            if (source.Equals(output, StringComparison.OrdinalIgnoreCase))
                throw new ArgumentException("Source and output must be different files.");

            int hr = D3DXCreateEffectCompilerFromFileW(
                source, IntPtr.Zero, IntPtr.Zero, 0, out compiler, out parseErrors);
            WriteDiagnostics(parseErrors);
            Marshal.ThrowExceptionForHR(hr);
            if (compiler == IntPtr.Zero)
                throw new InvalidOperationException("D3DX9 returned no effect compiler.");

            hr = Method<CompileEffect>(compiler, CompileEffectSlot)(
                compiler, 0, out code, out compileErrors);
            WriteDiagnostics(compileErrors);
            Marshal.ThrowExceptionForHR(hr);
            byte[] bytes = ReadBuffer(code);
            if (bytes.Length < 8 || BitConverter.ToUInt32(bytes, 0) != 0xFEFF0901)
                throw new InvalidDataException("D3DX9 did not produce an Effects Framework binary.");

            Directory.CreateDirectory(Path.GetDirectoryName(output)!);
            File.WriteAllBytes(output, bytes);
            Console.WriteLine($"Compiled {Path.GetFileName(source)}: {bytes.Length} bytes");
            return 0;
        }
        catch (DllNotFoundException)
        {
            Console.Error.WriteLine("Install the DirectX June 2010 runtime (d3dx9_43.dll) for this host architecture.");
            return 1;
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine(ex.Message);
            return 1;
        }
        finally
        {
            Release(compileErrors);
            Release(code);
            Release(parseErrors);
            Release(compiler);
        }
    }

    private static void WriteDiagnostics(IntPtr buffer)
    {
        string text = Encoding.UTF8.GetString(ReadBuffer(buffer)).TrimEnd('\0', '\r', '\n');
        if (text.Length != 0)
            Console.Error.WriteLine(text);
    }
}
