using System.Security.Cryptography;
using System.Text;
using Mono.Cecil;
using Mono.Cecil.Cil;

const string SchemaTag = "cmz-managed-relink/2";

if (args.Length == 0)
{
    return Usage();
}

string? fnaPath = null;
string? compatPath = null;
string? frameworkDirectory = null;
string? outputDirectory = null;
bool verifyOnly = false;
bool verbose = false;
var inputs = new List<string>();

for (int index = 0; index < args.Length; index++)
{
    string argument = args[index];
    switch (argument)
    {
        case "--fna":
            fnaPath = NextValue(args, ref index, argument);
            break;
        case "--compat":
            compatPath = NextValue(args, ref index, argument);
            break;
        case "--framework":
            frameworkDirectory = NextValue(args, ref index, argument);
            break;
        case "--output":
            outputDirectory = NextValue(args, ref index, argument);
            break;
        case "--verify-only":
            verifyOnly = true;
            break;
        case "--verbose":
            verbose = true;
            break;
        case "--help":
        case "-h":
            return Usage();
        default:
            if (argument.StartsWith("--", StringComparison.Ordinal))
            {
                return Fail($"Unknown option: {argument}");
            }

            inputs.Add(argument);
            break;
    }
}

if (fnaPath is null || compatPath is null || inputs.Count == 0)
{
    return Usage();
}

if (!verifyOnly && outputDirectory is null)
{
    return Fail("--output is required unless --verify-only is set.");
}

fnaPath = Path.GetFullPath(fnaPath);
compatPath = Path.GetFullPath(compatPath);
frameworkDirectory = frameworkDirectory is null ? null : Path.GetFullPath(frameworkDirectory);
outputDirectory = outputDirectory is null ? null : Path.GetFullPath(outputDirectory);

if (frameworkDirectory is not null && !Directory.Exists(frameworkDirectory))
{
    return Fail($"Framework assembly directory was not found: {frameworkDirectory}");
}

foreach (string path in new[] { fnaPath, compatPath }.Concat(inputs))
{
    if (!File.Exists(path))
    {
        return Fail($"Managed input was not found: {path}");
    }
}

using AssemblyDefinition fna = AssemblyDefinition.ReadAssembly(fnaPath);
using AssemblyDefinition compat = AssemblyDefinition.ReadAssembly(compatPath);

var fnaTypes = new TypeIndex("FNA", fna);
var compatTypes = new TypeIndex("CastleMinerZ.XnaCompat", compat);
FrameworkIndex? framework = frameworkDirectory is null ? null : new FrameworkIndex(frameworkDirectory);

if (verbose)
{
    Console.WriteLine($"FNA types: {fnaTypes.Types.Count}");
    Console.WriteLine($"CastleMinerZ.XnaCompat types: {compatTypes.Types.Count}");
    if (framework is not null)
    {
        Console.WriteLine($"Framework assemblies: {framework.AssembliesByName.Count}");
        Console.WriteLine($"Framework types: {framework.Types.Count}");
        foreach (string name in new[] { "System.Object", "System.String", "System.Math" })
        {
            Console.WriteLine(
                $"{name} -> " +
                (framework.TryGetType(name, out FrameworkAssembly? owner) ? owner?.Name : "missing"));
        }
    }
}

var unresolved = new SortedSet<string>(StringComparer.Ordinal);
var unresolvedFramework = new SortedSet<string>(StringComparer.Ordinal);
int totalRewritten = 0;
int totalCoreRewritten = 0;
int totalPruned = 0;

foreach (string input in inputs)
{
    string path = Path.GetFullPath(input);
    using AssemblyDefinition assembly = AssemblyDefinition.ReadAssembly(path);

    if (assembly.Name.HasPublicKey)
    {
        return Fail(
            $"Refusing to rewrite strong-named assembly '{assembly.Name.Name}' because the output " +
            "would carry an invalid signature. Supply an unsigned build instead.");
    }

    Console.WriteLine($"[{assembly.Name.Name}] {path}");

    AssemblyNameReference? fnaReference = null;
    AssemblyNameReference? compatReference = null;
    int rewritten = 0;
    int coreRewritten = 0;
    int unresolvedForAssembly = 0;
    int unresolvedCoreForAssembly = 0;

    foreach (TypeReference typeReference in CollectTypeReferences(assembly.MainModule))
    {
        TypeReference root = RootType(typeReference);
        if (root is GenericParameter || !IsLegacyXnaScope(root.Scope))
        {
            continue;
        }

        string fullName = root.FullName;
        AssemblyNameReference? target;
        if (fnaTypes.TryGetType(fullName, out _))
        {
            target = fnaReference ??= GetOrAddReference(assembly.MainModule, fna);
        }
        else if (compatTypes.TryGetType(fullName, out _))
        {
            target = compatReference ??= GetOrAddReference(assembly.MainModule, compat);
        }
        else
        {
            unresolved.Add($"{assembly.Name.Name}|{fullName}");
            unresolvedForAssembly++;
            continue;
        }

        if (!ReferenceEquals(root.Scope, target))
        {
            root.Scope = target;
            rewritten++;
        }
    }

    // The Xbox release was built against the .NET Compact Framework 2.0 core
    // libraries. Route every such type reference to the assembly that defines
    // it in the .NET 9 runtime so the AOT compiler and the Switch loader can
    // resolve it.
    if (framework is not null)
    {
        foreach (TypeReference typeReference in CollectTypeReferences(assembly.MainModule))
        {
            TypeReference root = RootType(typeReference);
            if (root is GenericParameter || !IsLegacyCoreScope(root.Scope))
            {
                continue;
            }

            if (!framework.TryGetType(root.FullName, out FrameworkAssembly? target) ||
                target is null)
            {
                unresolvedFramework.Add($"{assembly.Name.Name}|{root.FullName}");
                unresolvedCoreForAssembly++;
                continue;
            }

            // Mono identifies the core library through a "mscorlib" reference.
            // Route the types that the runtime's mscorlib facade actually
            // forwards through it, and leave the rest on System.Runtime or
            // their component assembly.
            if ((target.Name == "System.Runtime" || target.Name == "System.Private.CoreLib") &&
                framework.CanUseCorlib(root.FullName) &&
                framework.TryGetAssembly("mscorlib", out FrameworkAssembly? corlib) &&
                corlib is not null)
            {
                target = corlib;
            }

            AssemblyNameReference targetReference = GetOrAddFrameworkReference(assembly.MainModule, target);
            if (!ReferenceEquals(root.Scope, targetReference))
            {
                root.Scope = targetReference;
                coreRewritten++;
            }
        }
    }

    // Xbox-only core APIs have no .NET 9 counterpart. Redirect the calls that
    // the runtime cannot resolve to shims in CastleMinerZ.XnaCompat. The shim
    // takes the receiver as its first argument, so the evaluation stack and the
    // existing arguments stay unchanged; only opcode and target differ.
    int shimmed = 0;
    if (compatTypes.TryGetType("CastleMinerZ.XnaCompat.ThreadAffinity", out TypeDefinition? affinityType) &&
        affinityType is not null)
    {
        MethodDefinition? affinityMethod = affinityType.Methods.FirstOrDefault(
            method => method.Name == "SetProcessorAffinity" && method.Parameters.Count == 2);
        if (affinityMethod is not null)
        {
            var shimTargets = new Dictionary<MethodReference, MethodReference>(
                ReferenceComparer<MethodReference>.Instance);
            foreach (MemberReference member in assembly.MainModule.GetMemberReferences())
            {
                if (member is not MethodReference method ||
                    !IsLegacyThreadAffinity(method) ||
                    shimTargets.ContainsKey(method))
                {
                    continue;
                }

                shimTargets.Add(method, CreateThreadAffinityShim(assembly.MainModule, affinityMethod, method));
            }

            if (shimTargets.Count != 0)
            {
                foreach (TypeDefinition type in AllTypes(assembly.MainModule.Types))
                {
                    foreach (MethodDefinition method in type.Methods)
                    {
                        if (!method.HasBody)
                        {
                            continue;
                        }

                        foreach (Instruction instruction in method.Body.Instructions)
                        {
                            if (instruction.OpCode == OpCodes.Callvirt &&
                                instruction.Operand is MethodReference target &&
                                shimTargets.TryGetValue(target, out MethodReference? replacement))
                            {
                                instruction.OpCode = OpCodes.Call;
                                instruction.Operand = replacement;
                                shimmed++;
                            }
                        }
                    }
                }
            }
        }
    }

    // Preserve the original error before the game's error-display Game creates
    // a second SDL/EGL window and obscures it with a native presentation error.
    if (compatTypes.TryGetType("CastleMinerZ.XnaCompat.RuntimeDiagnostics", out TypeDefinition? diagnostics) &&
        diagnostics is not null)
    {
        MethodDefinition reporter = diagnostics.Methods.Single(method => method.Name == "ReportException");
        foreach (TypeDefinition type in assembly.MainModule.Types.Where(type =>
                     type.FullName is "DNA.DNAGame" or "DNA.ExceptionGame"))
        {
            foreach (MethodDefinition method in type.Methods.Where(method =>
                         method.HasBody && !method.IsStatic && method.Parameters.Count > 0 &&
                         method.Parameters[0].ParameterType.FullName == "System.Exception" &&
                         (method.Name == "CrashGame" || method.IsConstructor)))
            {
                if (method.Body.Instructions.Any(instruction =>
                        instruction.Operand is MethodReference reference &&
                        reference.DeclaringType.FullName == diagnostics.FullName &&
                        reference.Name == reporter.Name))
                    continue;

                var call = new MethodReference(reporter.Name, assembly.MainModule.TypeSystem.Void,
                    assembly.MainModule.ImportReference(diagnostics)) { HasThis = false };
                call.Parameters.Add(new ParameterDefinition(method.Parameters[0].ParameterType));
                ILProcessor il = method.Body.GetILProcessor();
                Instruction first = method.Body.Instructions[0];
                il.InsertBefore(first, il.Create(OpCodes.Ldarg_1));
                il.InsertBefore(first, il.Create(OpCodes.Call, call));
                method.Body.MaxStackSize = Math.Max(method.Body.MaxStackSize, 1);
                shimmed++;
            }
        }
    }

    foreach (string message in unresolved.Where(item => item.StartsWith(assembly.Name.Name + "|", StringComparison.Ordinal)))
    {
        Console.Error.WriteLine($"  unresolved type: {message[(assembly.Name.Name.Length + 1)..]}");
    }

    foreach (string message in unresolvedFramework.Where(item => item.StartsWith(assembly.Name.Name + "|", StringComparison.Ordinal)))
    {
        Console.Error.WriteLine($"  unresolved framework type: {message[(assembly.Name.Name.Length + 1)..]}");
    }

    if (unresolvedForAssembly != 0)
    {
        Console.Error.WriteLine($"  {unresolvedForAssembly} type reference(s) have no compatibility target.");
        continue;
    }

    if (unresolvedCoreForAssembly != 0)
    {
        Console.Error.WriteLine($"  {unresolvedCoreForAssembly} core library type reference(s) have no .NET 9 target.");
        continue;
    }

    int pruned = 0;
    for (int referenceIndex = assembly.MainModule.AssemblyReferences.Count - 1; referenceIndex >= 0; referenceIndex--)
    {
        AssemblyNameReference reference = assembly.MainModule.AssemblyReferences[referenceIndex];
        if (framework is not null && reference.Name == "mscorlib")
        {
            // The facade is what tells Mono which assembly is the corlib. It is
            // needed even when no type reference currently points at it.
            continue;
        }

        bool legacyXna = IsLegacyXnaName(reference.Name);
        bool legacyCore = framework is not null && IsLegacyCoreScope(reference);
        if (!legacyXna && !legacyCore)
        {
            continue;
        }

        if (legacyCore && IsScopeUsed(assembly.MainModule, reference))
        {
            continue;
        }

        assembly.MainModule.AssemblyReferences.RemoveAt(referenceIndex);
        pruned++;
    }

    var problems = new List<string>();
    VerifyModule(assembly.MainModule, path, fnaTypes, compatTypes, problems);
    if (problems.Count != 0)
    {
        foreach (string problem in problems.Take(100))
        {
            Console.Error.WriteLine($"  {problem}");
        }

        if (problems.Count > 100)
        {
            Console.Error.WriteLine($"  ... and {problems.Count - 100} more problem(s).");
        }

        Console.Error.WriteLine($"  {problems.Count} verification error(s); output was not written.");
        continue;
    }

    Console.WriteLine($"  type references rewritten: {rewritten}");
    Console.WriteLine($"  core library references rewritten: {coreRewritten}");
    Console.WriteLine($"  Xbox-only call sites shimmed: {shimmed}");
    Console.WriteLine($"  legacy assembly references pruned: {pruned}");
    Console.WriteLine($"  type references: {assembly.MainModule.GetTypeReferences().Count()}");
    Console.WriteLine($"  member references: {assembly.MainModule.GetMemberReferences().Count()}");

    totalRewritten += rewritten;
    totalCoreRewritten += coreRewritten;
    totalPruned += pruned;

    if (verifyOnly || outputDirectory is null)
    {
        continue;
    }

    Directory.CreateDirectory(outputDirectory);
    string destination = Path.Combine(outputDirectory, Path.GetFileName(path));
    if (Path.GetFullPath(destination).Equals(path, StringComparison.OrdinalIgnoreCase))
    {
        return Fail($"Refusing to overwrite the input assembly: {path}");
    }

    if (rewritten == 0 && coreRewritten == 0 && pruned == 0 && shimmed == 0)
    {
        File.Copy(path, destination, overwrite: true);
        Console.WriteLine($"  already compatible; copied unchanged: {destination}");
        Console.WriteLine($"  sha256: {Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(destination)))}");
        continue;
    }

    assembly.MainModule.Mvid = DeterministicMvid(path, fna, compat);
    assembly.Write(
        destination,
        new WriterParameters { WriteSymbols = false, Timestamp = 0 });

    using AssemblyDefinition written = AssemblyDefinition.ReadAssembly(destination);
    var reread = new List<string>();
    VerifyModule(written.MainModule, destination, fnaTypes, compatTypes, reread);
    if (reread.Count != 0)
    {
        foreach (string problem in reread.Take(100))
        {
            Console.Error.WriteLine($"  {problem}");
        }

        return Fail($"Written assembly failed re-verification: {destination}");
    }

    Console.WriteLine($"  written: {destination}");
    Console.WriteLine($"  sha256: {Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(destination)))}");
}

if (unresolved.Count != 0)
{
    Console.Error.WriteLine($"{unresolved.Count} unresolved XNA type reference(s) across all inputs.");
    return 3;
}

if (unresolvedFramework.Count != 0)
{
    Console.Error.WriteLine($"{unresolvedFramework.Count} unresolved core library type reference(s) across all inputs.");
    return 3;
}

Console.WriteLine(
    $"Rewritten {totalRewritten} XNA and {totalCoreRewritten} core library type reference(s); " +
    $"pruned {totalPruned} legacy assembly reference(s).");
return 0;

static int Usage()
{
    Console.Error.WriteLine(
        "Usage: cmz-managed-relink --fna <FNA.dll> --compat <CastleMinerZ.XnaCompat.dll> " +
        "[--framework <directory>] --output <directory> [--verify-only] [--verbose] " +
        "<assembly> [assembly ...]");
    return 2;
}

static int Fail(string message)
{
    Console.Error.WriteLine(message);
    return 2;
}

static string NextValue(string[] args, ref int index, string option)
{
    if (index + 1 >= args.Length)
    {
        throw new ArgumentException($"Missing value for {option}.");
    }

    return args[++index];
}

static bool IsLegacyXnaName(string name) =>
    name == "Microsoft.Xna.Framework" ||
    name.StartsWith("Microsoft.Xna.Framework.", StringComparison.Ordinal);

static bool IsLegacyXnaScope(IMetadataScope scope) =>
    scope is AssemblyNameReference reference && IsLegacyXnaName(reference.Name);

static bool IsLegacyCoreName(string name) =>
    name == "mscorlib" ||
    name == "System" ||
    name == "System.Core";

static bool IsLegacyCoreScope(IMetadataScope scope) =>
    scope is AssemblyNameReference reference &&
    IsLegacyCoreName(reference.Name) &&
    reference.Version.Major <= 3;

static bool IsScopeUsed(ModuleDefinition module, AssemblyNameReference reference)
{
    foreach (TypeReference type in module.GetTypeReferences())
    {
        if (RootType(type).Scope is AssemblyNameReference scope &&
            scope.Name == reference.Name &&
            scope.Version == reference.Version)
        {
            return true;
        }
    }

    return false;
}

static bool IsLegacyThreadAffinity(MethodReference method) =>
    method.Name == "SetProcessorAffinity" &&
    method.DeclaringType is not null &&
    RootType(method.DeclaringType).FullName == "System.Threading.Thread" &&
    method.Parameters.Count == 1 &&
    method.Parameters[0].ParameterType.FullName == "System.Int32[]";

static MethodReference CreateThreadAffinityShim(
    ModuleDefinition module,
    MethodDefinition definition,
    MethodReference original)
{
    var shim = new MethodReference(definition.Name, original.ReturnType)
    {
        HasThis = false,
        DeclaringType = module.ImportReference(definition.DeclaringType)
    };

    // The instance receiver becomes the first static argument; the remaining
    // arguments keep their existing types and scopes.
    shim.Parameters.Add(new ParameterDefinition(original.DeclaringType));
    foreach (ParameterDefinition parameter in original.Parameters)
    {
        shim.Parameters.Add(new ParameterDefinition(parameter.ParameterType));
    }

    return shim;
}

static AssemblyNameReference GetOrAddFrameworkReference(ModuleDefinition module, FrameworkAssembly target)
{
    AssemblyNameDefinition name = target.Definition.Name;
    AssemblyNameReference? existing = module.AssemblyReferences.FirstOrDefault(
        reference => reference.Name == name.Name &&
                     (name.Name == "mscorlib" || reference.Version == name.Version));
    if (existing is not null)
    {
        return existing;
    }

    var reference = new AssemblyNameReference(name.Name, name.Version)
    {
        Culture = name.Culture,
        Attributes = name.Attributes,
        IsRetargetable = false
    };

    if (name.HasPublicKey)
    {
        reference.PublicKey = name.PublicKey;
    }
    else
    {
        reference.PublicKeyToken = name.PublicKeyToken ?? Array.Empty<byte>();
    }

    module.AssemblyReferences.Add(reference);
    return reference;
}

static AssemblyNameReference GetOrAddReference(ModuleDefinition module, AssemblyDefinition target)
{
    AssemblyNameDefinition name = target.Name;
    AssemblyNameReference? existing = module.AssemblyReferences.FirstOrDefault(
        reference => reference.Name == name.Name && reference.Version == name.Version);
    if (existing is not null)
    {
        return existing;
    }

    var reference = new AssemblyNameReference(name.Name, name.Version)
    {
        Culture = name.Culture,
        Attributes = name.Attributes,
        IsRetargetable = false
    };

    if (name.HasPublicKey)
    {
        reference.PublicKey = name.PublicKey;
    }
    else
    {
        reference.PublicKeyToken = name.PublicKeyToken ?? Array.Empty<byte>();
    }

    module.AssemblyReferences.Add(reference);
    return reference;
}

static TypeReference RootType(TypeReference type)
{
    while (type is TypeSpecification specification)
    {
        type = specification.ElementType;
    }

    return type;
}

static IEnumerable<TypeReference> CollectTypeReferences(ModuleDefinition module)
{
    var seen = new HashSet<TypeReference>(ReferenceComparer<TypeReference>.Instance);
    var pending = new Stack<TypeReference>();

    void Push(TypeReference? type)
    {
        if (type is null or GenericParameter || !seen.Add(type))
        {
            return;
        }

        pending.Push(type);
    }

    foreach (TypeReference type in module.GetTypeReferences())
    {
        Push(type);
    }

    foreach (MemberReference member in module.GetMemberReferences())
    {
        Push(member.DeclaringType);
        switch (member)
        {
            case MethodReference method:
                Push(method.ReturnType);
                foreach (ParameterDefinition parameter in method.Parameters)
                {
                    Push(parameter.ParameterType);
                }
                break;
            case FieldReference field:
                Push(field.FieldType);
                break;
        }
    }

    foreach (CustomAttribute attribute in module.CustomAttributes)
    {
        Push(attribute.AttributeType);
    }

    if (module.Assembly is not null)
    {
        foreach (CustomAttribute attribute in module.Assembly.CustomAttributes)
        {
            Push(attribute.AttributeType);
        }
    }

    foreach (TypeDefinition type in AllTypes(module.Types))
    {
        Push(type.BaseType);
        foreach (InterfaceImplementation implementation in type.Interfaces)
        {
            Push(implementation.InterfaceType);
        }

        foreach (CustomAttribute attribute in type.CustomAttributes)
        {
            Push(attribute.AttributeType);
        }

        foreach (FieldDefinition field in type.Fields)
        {
            Push(field.FieldType);
            foreach (CustomAttribute attribute in field.CustomAttributes)
            {
                Push(attribute.AttributeType);
            }
        }

        foreach (MethodDefinition method in type.Methods)
        {
            Push(method.ReturnType);
            foreach (ParameterDefinition parameter in method.Parameters)
            {
                Push(parameter.ParameterType);
                foreach (CustomAttribute attribute in parameter.CustomAttributes)
                {
                    Push(attribute.AttributeType);
                }
            }

            foreach (CustomAttribute attribute in method.CustomAttributes)
            {
                Push(attribute.AttributeType);
            }
        }

        foreach (PropertyDefinition property in type.Properties)
        {
            Push(property.PropertyType);
            foreach (ParameterDefinition parameter in property.Parameters)
            {
                Push(parameter.ParameterType);
            }
        }

        foreach (EventDefinition @event in type.Events)
        {
            Push(@event.EventType);
        }
    }

    while (pending.Count != 0)
    {
        TypeReference current = pending.Pop();
        yield return current;

        switch (current)
        {
            case GenericInstanceType generic:
                Push(generic.ElementType);
                foreach (TypeReference argument in generic.GenericArguments)
                {
                    Push(argument);
                }
                break;
            case FunctionPointerType functionPointer:
                Push(functionPointer.ReturnType);
                foreach (ParameterDefinition parameter in functionPointer.Parameters)
                {
                    Push(parameter.ParameterType);
                }
                break;
            case TypeSpecification specification:
                Push(specification.ElementType);
                if (specification is IModifierType modifier)
                {
                    Push(modifier.ModifierType);
                }
                break;
            default:
                Push(current.DeclaringType);
                foreach (GenericParameter parameter in current.GenericParameters)
                {
                    foreach (GenericParameterConstraint constraint in parameter.Constraints)
                    {
                        Push(constraint.ConstraintType);
                    }
                }
                break;
        }
    }
}

static void VerifyModule(
    ModuleDefinition module,
    string path,
    TypeIndex fna,
    TypeIndex compat,
    List<string> problems)
{
    foreach (AssemblyNameReference reference in module.AssemblyReferences)
    {
        if (IsLegacyXnaName(reference.Name))
        {
            problems.Add($"legacy assembly reference remains: {reference.FullName}");
        }
    }

    foreach (TypeReference type in CollectTypeReferences(module))
    {
        TypeReference root = RootType(type);
        if (root is GenericParameter || !IsLegacyXnaScope(root.Scope))
        {
            continue;
        }

        problems.Add($"legacy type scope remains: {root.FullName} -> {root.Scope.Name}");
    }

    foreach (TypeReference type in module.GetTypeReferences())
    {
        TypeReference root = RootType(type);
        if (root is GenericParameter || !(root.Scope is AssemblyNameReference reference))
        {
            continue;
        }

        TypeIndex? target = reference.Name == fna.Name ? fna
            : reference.Name == compat.Name ? compat
            : null;
        if (target is null)
        {
            continue;
        }

        if (!target.TryGetType(root.FullName, out _))
        {
            problems.Add($"type is not defined by {target.Name}: {root.FullName}");
        }
    }

    foreach (MemberReference member in module.GetMemberReferences())
    {
        string? failure = ResolveMember(member, fna, compat);
        if (failure is not null)
        {
            problems.Add($"{failure} ({member.FullName})");
        }
    }
}

static string? ResolveMember(MemberReference member, TypeIndex fna, TypeIndex compat)
{
    TypeReference? declaring = member.DeclaringType;
    if (declaring is null)
    {
        return null;
    }

    declaring = RootType(declaring);
    if (declaring is GenericParameter ||
        !(declaring.Scope is AssemblyNameReference scope) ||
        !(scope.Name == fna.Name || scope.Name == compat.Name))
    {
        return null;
    }

    TypeDefinition? type = FindType(declaring, fna, compat);
    if (type is null)
    {
        return $"declaring type not found: {declaring.FullName}";
    }

    string? expectedMethod = member is MethodReference method ? MethodKey(method) : null;
    string? expectedField = member is FieldReference field ? FieldKey(field) : null;

    for (TypeDefinition? current = type; current is not null; current = ResolveBase(current, fna, compat))
    {
        if (expectedMethod is not null)
        {
            foreach (MethodDefinition candidate in current.Methods)
            {
                if (MethodKey(candidate) == expectedMethod)
                {
                    return null;
                }
            }
        }

        if (expectedField is not null)
        {
            foreach (FieldDefinition candidate in current.Fields)
            {
                if (FieldKey(candidate) == expectedField)
                {
                    return null;
                }
            }
        }
    }

    return $"member not found on {type.FullName} or its base types: " +
        (expectedMethod ?? expectedField ?? member.Name);
}

static TypeDefinition? ResolveBase(TypeDefinition type, TypeIndex fna, TypeIndex compat)
{
    TypeReference? baseType = type.BaseType;
    return baseType is null ? null : FindType(RootType(baseType), fna, compat);
}

static TypeDefinition? FindType(TypeReference reference, TypeIndex fna, TypeIndex compat)
{
    if (fna.TryGetType(reference.FullName, out TypeDefinition? definition))
    {
        return definition;
    }

    return compat.TryGetType(reference.FullName, out definition) ? definition : null;
}

static string MethodKey(MethodReference method)
{
    var builder = new StringBuilder();
    builder.Append(method.HasThis ? "instance " : "static ");
    builder.Append(method.Name);
    if (method.GenericParameters.Count != 0)
    {
        builder.Append('`').Append(method.GenericParameters.Count);
    }

    builder.Append('(');
    for (int index = 0; index < method.Parameters.Count; index++)
    {
        if (index != 0)
        {
            builder.Append(',');
        }

        builder.Append(TypeKey(method.Parameters[index].ParameterType));
    }

    builder.Append("):");
    builder.Append(TypeKey(method.ReturnType));
    return builder.ToString();
}

static string FieldKey(FieldReference field) =>
    $"{field.Name}:{TypeKey(field.FieldType)}";

static string TypeKey(TypeReference type)
{
    switch (type)
    {
        case GenericParameter parameter:
            return (parameter.Type == GenericParameterType.Type ? "!" : "!!") + parameter.Position;
        case GenericInstanceType generic:
            var arguments = new StringBuilder();
            arguments.Append(TypeKey(generic.ElementType)).Append('<');
            for (int index = 0; index < generic.GenericArguments.Count; index++)
            {
                if (index != 0)
                {
                    arguments.Append(',');
                }

                arguments.Append(TypeKey(generic.GenericArguments[index]));
            }

            return arguments.Append('>').ToString();
        case ArrayType array:
            return TypeKey(array.ElementType) + "[" + new string(',', array.Rank - 1) + "]";
        case ByReferenceType byReference:
            return TypeKey(byReference.ElementType) + "&";
        case PointerType pointer:
            return TypeKey(pointer.ElementType) + "*";
        case PinnedType pinned:
            return TypeKey(pinned.ElementType) + " pinned";
        case SentinelType sentinel:
            return TypeKey(sentinel.ElementType) + " ...";
        case OptionalModifierType optional:
            return TypeKey(optional.ElementType) + " modopt(" + TypeKey(optional.ModifierType) + ")";
        case RequiredModifierType required:
            return TypeKey(required.ElementType) + " modreq(" + TypeKey(required.ModifierType) + ")";
        case FunctionPointerType functionPointer:
            var parameters = new StringBuilder();
            for (int index = 0; index < functionPointer.Parameters.Count; index++)
            {
                if (index != 0)
                {
                    parameters.Append(',');
                }

                parameters.Append(TypeKey(functionPointer.Parameters[index].ParameterType));
            }

            return "fnptr(" + parameters + "):" + TypeKey(functionPointer.ReturnType);
        default:
            return type.FullName;
    }
}

static Guid DeterministicMvid(string inputPath, AssemblyDefinition fna, AssemblyDefinition compat)
{
    byte[] inputHash = SHA256.HashData(File.ReadAllBytes(inputPath));
    string identity = string.Join(
        '|',
        SchemaTag,
        Convert.ToHexString(inputHash),
        fna.Name.FullName,
        compat.Name.FullName);
    byte[] hash = SHA256.HashData(Encoding.UTF8.GetBytes(identity));
    byte[] value = hash.AsSpan(0, 16).ToArray();
    value[7] = (byte)((value[7] & 0x0F) | 0x40);
    value[8] = (byte)((value[8] & 0x3F) | 0x80);
    return new Guid(value);
}

static IEnumerable<TypeDefinition> AllTypes(IEnumerable<TypeDefinition> roots)
{
    foreach (TypeDefinition type in roots)
    {
        yield return type;
        foreach (TypeDefinition nested in AllTypes(type.NestedTypes))
        {
            yield return nested;
        }
    }
}

internal sealed class TypeIndex
{
    public TypeIndex(string name, AssemblyDefinition assembly)
    {
        Name = name;
        foreach (TypeDefinition type in AllTypes(assembly.MainModule.Types))
        {
            Types[type.FullName] = type;
        }
    }

    public string Name { get; }

    public Dictionary<string, TypeDefinition> Types { get; } =
        new(StringComparer.Ordinal);

    public bool TryGetType(string fullName, out TypeDefinition? type) =>
        Types.TryGetValue(fullName, out type);

    private static IEnumerable<TypeDefinition> AllTypes(IEnumerable<TypeDefinition> roots)
    {
        foreach (TypeDefinition type in roots)
        {
            yield return type;
            foreach (TypeDefinition nested in AllTypes(type.NestedTypes))
            {
                yield return nested;
            }
        }
    }
}

internal sealed class FrameworkAssembly
{
    public FrameworkAssembly(AssemblyDefinition definition)
    {
        Definition = definition;
    }

    public AssemblyDefinition Definition { get; }

    public string Name => Definition.Name.Name;
}

internal sealed class FrameworkIndex
{
    public FrameworkIndex(string directory)
    {
        var assemblies = new List<FrameworkAssembly>();
        foreach (string path in Directory.EnumerateFiles(directory, "*.dll").OrderBy(
                     item => Path.GetFileName(item), StringComparer.OrdinalIgnoreCase))
        {
            try
            {
                assemblies.Add(new FrameworkAssembly(AssemblyDefinition.ReadAssembly(path)));
            }
            catch (BadImageFormatException)
            {
                // Native or otherwise unmanaged files can share the directory.
            }
        }

        foreach (FrameworkAssembly assembly in assemblies)
        {
            AssembliesByName[assembly.Name] = assembly;
        }

        // A type can appear in more than one facade. Prefer the implementation
        // assembly, which is System.Private.CoreLib for the core types and the
        // component assembly for everything else.
        var candidates = new Dictionary<string, List<FrameworkAssembly>>(StringComparer.Ordinal);
        foreach (FrameworkAssembly assembly in assemblies)
        {
            foreach (TypeDefinition type in AllTypes(assembly.Definition.MainModule.Types))
            {
                if (type.Name == "<Module>")
                {
                    continue;
                }

                if (!candidates.TryGetValue(type.FullName, out List<FrameworkAssembly>? owners))
                {
                    owners = new List<FrameworkAssembly>();
                    candidates.Add(type.FullName, owners);
                }

                owners.Add(assembly);
            }
        }

        foreach ((string fullName, List<FrameworkAssembly> owners) in candidates)
        {
            FrameworkAssembly chosen = owners.FirstOrDefault(owner => owner.Name == "System.Private.CoreLib")
                ?? owners.OrderByDescending(owner => owner.Definition.MainModule.Types.Count).First();
            Types[fullName] = chosen;
        }

        // Facades such as System.Runtime and System.Linq forward public types to
        // their implementation assemblies. Keep those targets when the type is
        // not defined in the pack at all.
        foreach (FrameworkAssembly assembly in assemblies)
        {
            foreach (ExportedType exported in assembly.Definition.MainModule.ExportedTypes)
            {
                if (Types.ContainsKey(exported.FullName))
                {
                    continue;
                }

                if (exported.Scope is AssemblyNameReference target &&
                    AssembliesByName.TryGetValue(target.Name, out FrameworkAssembly? owner))
                {
                    Types[exported.FullName] = owner;
                }
            }
        }

        // Mono identifies the corlib by the assembly name "mscorlib", so the
        // runtime ships a facade under that name. It forwards only a subset of
        // the core types; everything else must stay on the assembly that really
        // implements it, otherwise the AOT compiler cannot resolve the type.
        if (AssembliesByName.TryGetValue("mscorlib", out FrameworkAssembly? corlibFacade))
        {
            foreach (TypeDefinition type in AllTypes(corlibFacade.Definition.MainModule.Types))
            {
                if (type.Name != "<Module>")
                {
                    CorlibTypes.Add(type.FullName);
                }
            }

            foreach (ExportedType exported in corlibFacade.Definition.MainModule.ExportedTypes)
            {
                CorlibTypes.Add(exported.FullName);
            }
        }
    }

    public Dictionary<string, FrameworkAssembly> AssembliesByName { get; } =
        new(StringComparer.Ordinal);

    public Dictionary<string, FrameworkAssembly> Types { get; } =
        new(StringComparer.Ordinal);

    public HashSet<string> CorlibTypes { get; } =
        new(StringComparer.Ordinal);

    public bool TryGetType(string fullName, out FrameworkAssembly? target) =>
        Types.TryGetValue(fullName, out target);

    public bool CanUseCorlib(string fullName) => CorlibTypes.Contains(fullName);

    public bool TryGetAssembly(string name, out FrameworkAssembly? target) =>
        AssembliesByName.TryGetValue(name, out target);

    private static IEnumerable<TypeDefinition> AllTypes(IEnumerable<TypeDefinition> roots)
    {
        foreach (TypeDefinition type in roots)
        {
            yield return type;
            foreach (TypeDefinition nested in AllTypes(type.NestedTypes))
            {
                yield return nested;
            }
        }
    }
}

internal sealed class ReferenceComparer<T> : IEqualityComparer<T>
    where T : class
{
    public static ReferenceComparer<T> Instance { get; } = new();

    public bool Equals(T? x, T? y) => ReferenceEquals(x, y);

    public int GetHashCode(T obj) => System.Runtime.CompilerServices.RuntimeHelpers.GetHashCode(obj);
}
