using System.Diagnostics;
using System.Security.Cryptography;
using Mono.Cecil;
using Mono.Cecil.Cil;

if (args.Length != 4)
{
    Console.Error.WriteLine(
        "Usage: cmz-managed-relink-tests <Cmz.ManagedRelink.dll> <FNA.dll> " +
        "<CastleMinerZ.XnaCompat.dll> <scratch-directory>");
    return 2;
}

string relinker = Path.GetFullPath(args[0]);
string fna = Path.GetFullPath(args[1]);
string compat = Path.GetFullPath(args[2]);
string scratch = Path.GetFullPath(args[3]);

foreach (string path in new[] { relinker, fna, compat })
{
    if (!File.Exists(path))
    {
        Console.Error.WriteLine($"Missing test input: {path}");
        return 2;
    }
}

if (Directory.Exists(scratch))
{
    Directory.Delete(scratch, recursive: true);
}

Directory.CreateDirectory(scratch);

try
{
    string synthetic = Path.Combine(scratch, "SyntheticGame.dll");
    CreateSyntheticAssembly(synthetic);
    Check("synthetic assembly references the legacy XNA scope", HasLegacyReference(synthetic));

    string first = Path.Combine(scratch, "relinked-a");
    string second = Path.Combine(scratch, "relinked-b");
    string third = Path.Combine(scratch, "relinked-c");

    Run(relinker, fna, compat, first, synthetic);
    Run(relinker, fna, compat, second, synthetic);

    string firstAssembly = Path.Combine(first, "SyntheticGame.dll");
    string secondAssembly = Path.Combine(second, "SyntheticGame.dll");
    Check("relinked output exists", File.Exists(firstAssembly));
    Check("relinking is deterministic", Hash(firstAssembly) == Hash(secondAssembly));

    using (AssemblyDefinition assembly = AssemblyDefinition.ReadAssembly(firstAssembly))
    {
        ModuleDefinition module = assembly.MainModule;

        Check(
            "legacy assembly reference was pruned",
            module.AssemblyReferences.All(reference => reference.Name != "Microsoft.Xna.Framework"));
        Check(
            "FNA assembly reference was added",
            module.AssemblyReferences.Any(reference => reference.Name == "FNA"));
        Check(
            "compatibility assembly reference was added",
            module.AssemblyReferences.Any(reference => reference.Name == "CastleMinerZ.XnaCompat"));

        TypeReference gameTime = module.GetTypeReferences()
            .Single(type => type.FullName == "Microsoft.Xna.Framework.GameTime");
        Check(
            "GameTime routes to FNA",
            gameTime.Scope is AssemblyNameReference { Name: "FNA" });

        TypeReference guide = module.GetTypeReferences()
            .Single(type => type.FullName == "Microsoft.Xna.Framework.GamerServices.Guide");
        Check(
            "Guide routes to CastleMinerZ.XnaCompat",
            guide.Scope is AssemblyNameReference { Name: "CastleMinerZ.XnaCompat" });
    }

    // Running the relinker on its own output must be a byte-identical pass-through.
    Run(relinker, fna, compat, third, firstAssembly);
    string thirdAssembly = Path.Combine(third, "SyntheticGame.dll");
    Check("relinking an already-compatible assembly is idempotent", Hash(firstAssembly) == Hash(thirdAssembly));

    Console.WriteLine("All managed relinker tests passed.");
    return 0;
}
catch (Exception exception)
{
    Console.Error.WriteLine($"FAILED: {exception.Message}");
    return 1;
}

static void Run(string relinker, string fna, string compat, string output, params string[] inputs)
{
    var startInfo = new ProcessStartInfo("dotnet")
    {
        RedirectStandardOutput = true,
        RedirectStandardError = true,
        UseShellExecute = false
    };

    startInfo.ArgumentList.Add(relinker);
    startInfo.ArgumentList.Add("--fna");
    startInfo.ArgumentList.Add(fna);
    startInfo.ArgumentList.Add("--compat");
    startInfo.ArgumentList.Add(compat);
    startInfo.ArgumentList.Add("--output");
    startInfo.ArgumentList.Add(output);
    foreach (string input in inputs)
    {
        startInfo.ArgumentList.Add(input);
    }

    using Process process = Process.Start(startInfo)
        ?? throw new InvalidOperationException("Could not start the relinker.");

    string standardOutput = process.StandardOutput.ReadToEnd();
    string standardError = process.StandardError.ReadToEnd();
    process.WaitForExit();

    if (process.ExitCode != 0)
    {
        throw new InvalidOperationException(
            $"Relinker exited with {process.ExitCode}:{Environment.NewLine}" +
            $"{standardOutput}{Environment.NewLine}{standardError}");
    }
}

static bool HasLegacyReference(string path)
{
    using AssemblyDefinition assembly = AssemblyDefinition.ReadAssembly(path);
    return assembly.MainModule.AssemblyReferences
        .Any(reference => reference.Name == "Microsoft.Xna.Framework");
}

static string Hash(string path) =>
    Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(path)));

static void Check(string description, bool condition)
{
    if (!condition)
    {
        throw new InvalidOperationException(description);
    }

    Console.WriteLine($"ok: {description}");
}

static void CreateSyntheticAssembly(string path)
{
    var name = new AssemblyNameDefinition("SyntheticGame", new Version(1, 0, 0, 0));
    using AssemblyDefinition assembly = AssemblyDefinition.CreateAssembly(
        name,
        "SyntheticGame.dll",
        ModuleKind.Dll);
    ModuleDefinition module = assembly.MainModule;

    var legacyScope = new AssemblyNameReference("Microsoft.Xna.Framework", new Version(4, 0, 0, 0));
    module.AssemblyReferences.Add(legacyScope);

    var gameTime = new TypeReference(
        "Microsoft.Xna.Framework",
        "GameTime",
        module,
        legacyScope);

    var guide = new TypeReference(
        "Microsoft.Xna.Framework.GamerServices",
        "Guide",
        module,
        legacyScope);

    var timeSpan = new TypeReference(
        "System",
        "TimeSpan",
        module,
        module.TypeSystem.CoreLibrary);

    var probe = new TypeDefinition(
        "Synthetic",
        "Probe",
        TypeAttributes.Public | TypeAttributes.Class | TypeAttributes.BeforeFieldInit,
        module.TypeSystem.Object);
    module.Types.Add(probe);

    probe.Fields.Add(new FieldDefinition("Time", FieldAttributes.Public, gameTime));

    var readGuide = new MethodDefinition(
        "ReadGuide",
        MethodAttributes.Public | MethodAttributes.Static,
        module.TypeSystem.Boolean);
    probe.Methods.Add(readGuide);

    ILProcessor guideBody = readGuide.Body.GetILProcessor();
    var isTrialMode = new MethodReference("get_IsTrialMode", module.TypeSystem.Boolean, guide)
    {
        HasThis = false
    };
    guideBody.Emit(OpCodes.Call, isTrialMode);
    guideBody.Emit(OpCodes.Ret);

    var readTime = new MethodDefinition(
        "ReadTime",
        MethodAttributes.Public | MethodAttributes.Static,
        timeSpan);
    probe.Methods.Add(readTime);

    ILProcessor timeBody = readTime.Body.GetILProcessor();
    var constructor = new MethodReference(".ctor", module.TypeSystem.Void, gameTime)
    {
        HasThis = true
    };
    var totalGameTime = new MethodReference("get_TotalGameTime", timeSpan, gameTime)
    {
        HasThis = true
    };
    timeBody.Emit(OpCodes.Newobj, constructor);
    timeBody.Emit(OpCodes.Callvirt, totalGameTime);
    timeBody.Emit(OpCodes.Ret);

    assembly.Write(path, new WriterParameters { WriteSymbols = false, Timestamp = 0 });
}
