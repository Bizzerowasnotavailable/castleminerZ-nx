using System.Security.Cryptography;
using System.Text;
using Mono.Cecil;
using Mono.Cecil.Cil;

// Trims static-AOT-hostile code out of third-party managed assemblies.
//
// Three transforms are supported, all applied to the compiled dependency while
// the pinned source checkout stays untouched:
//
//   --stub Type::Method
//       Replace a body with "return;". Used for FNA's FNADllMap, which is a
//       [ModuleInitializer]: trimming keeps its whole body, including its
//       System.Xml path, and that drags in System.Private.Xml ->
//       System.Net.Http -> System.Net.Security and the OpenSSL CryptoNative_*
//       entry points mono-nx does not ship.
//
//   --stub-pinvoke-module <library>
//       Replace every DllImport in that native library with a managed
//       default(T) body. mono-aot-cross compiles all methods of an assembly and
//       the static AOT module table keeps every one alive, so FNA's unused SDL3
//       and Emscripten bindings would otherwise emit unresolvable references to
//       SDL_OpenGamepad, emscripten_set_main_loop and friends. FNA is forced
//       onto SDL2 on this target, so those bindings are unreachable.
//
//   --stub-pinvoke-name <entry point>
//       Replace one named DllImport with a managed default(T) body. The switch
//       SDL2 library only implements desktop bindings, while FNA also declares
//       Android, WinRT and Apple entry points that the static AOT module keeps
//       alive.
//
// ILLink's --substitutions could express the first transform, but this linker
// build does not match the documented signatures, and a checked, deterministic
// Cecil rewrite is easier to reason about than a silent no-op warning.

const string SchemaTag = "cmz-managed-aot-prep/2";

// The game build uses these to walk the managed assembly closure without
// dragging Mono.Cecil into the build scripts.
if (args.Length == 2 && args[0] == "--list-references")
{
    string referencePath = Path.GetFullPath(args[1]);
    if (!File.Exists(referencePath))
    {
        return Fail($"Assembly was not found: {referencePath}");
    }

    using AssemblyDefinition referenced = AssemblyDefinition.ReadAssembly(referencePath);
    foreach (AssemblyNameReference reference in referenced.MainModule.AssemblyReferences)
    {
        Console.WriteLine(reference.Name);
    }

    return 0;
}

if (args.Length == 2 && args[0] == "--list-type-scopes")
{
    string scopePath = Path.GetFullPath(args[1]);
    if (!File.Exists(scopePath))
    {
        return Fail($"Assembly was not found: {scopePath}");
    }

    using AssemblyDefinition referenced = AssemblyDefinition.ReadAssembly(scopePath);
    foreach (string scope in CollectTypeScopes(referenced.MainModule))
    {
        Console.WriteLine(scope);
    }

    return 0;
}

if (args.Length == 0)
{
    return Usage();
}

string? input = null;
string? output = null;
var stubs = new List<StubSpec>();
var stubModules = new List<string>();
var stubNames = new List<string>();

for (int index = 0; index < args.Length; index++)
{
    switch (args[index])
    {
        case "--input":
            input = NextValue(args, ref index, args[index]);
            break;
        case "--output":
            output = NextValue(args, ref index, args[index]);
            break;
        case "--stub-pinvoke-module":
            stubModules.Add(NextValue(args, ref index, args[index]));
            break;
        case "--stub-pinvoke-name":
            stubNames.Add(NextValue(args, ref index, args[index]));
            break;
        case "--stub":
            string spec = NextValue(args, ref index, args[index]);
            if (!StubSpec.TryParse(spec, out StubSpec? parsed, out string? specError))
            {
                return Fail(specError ?? $"Invalid stub target '{spec}'.");
            }

            stubs.Add(parsed!);
            break;
        case "--help":
        case "-h":
            return Usage();
        default:
            return Fail($"Unknown option: {args[index]}");
    }
}

if (input is null || output is null || (stubs.Count == 0 && stubModules.Count == 0 && stubNames.Count == 0))
{
    return Usage();
}

input = Path.GetFullPath(input);
output = Path.GetFullPath(output);
if (!File.Exists(input))
{
    return Fail($"Input assembly was not found: {input}");
}

if (string.Equals(input, output, StringComparison.OrdinalIgnoreCase))
{
    return Fail("Refusing to rewrite the input in place; use a separate --output path.");
}

using AssemblyDefinition assembly = AssemblyDefinition.ReadAssembly(input);

if (assembly.Name.HasPublicKey)
{
    return Fail("Refusing to rewrite a strong-named assembly; the signature would be invalid.");
}

var typesByName = new Dictionary<string, TypeDefinition>(StringComparer.Ordinal);
foreach (TypeDefinition type in AllTypes(assembly.MainModule.Types))
{
    typesByName[type.FullName] = type;
}

int stubbed = 0;
int alreadyStubbed = 0;
var failures = new List<string>();

// Neutralize every P/Invoke that belongs to a library this target does not
// link. mono-aot-cross compiles *all* methods of an assembly and the static AOT
// module table keeps every one of them alive, so an unused binding such as
// FNA's SDL3 surface still emits an external reference for SDL_OpenGamepad and
// friends. Those entry points are unreachable here (FNA is forced onto SDL2 and
// the Emscripten main loop is never selected), but they would fail the link.
foreach (string module in stubModules)
{
    foreach (TypeDefinition type in AllTypes(assembly.MainModule.Types))
    {
        foreach (MethodDefinition method in type.Methods)
        {
            if (method.PInvokeInfo is null ||
                method.PInvokeInfo.Module.Name != module)
            {
                continue;
            }

            EmitDefaultReturn(method);
            stubbed++;
        }
    }
}

// Some platform bindings live in an otherwise usable module. The Switch SDL2
// build only implements desktop entry points, so stub the named entry points
// that the static AOT module would keep alive.
if (stubNames.Count != 0)
{
    var requested = new HashSet<string>(stubNames, StringComparer.Ordinal);
    var found = new HashSet<string>(StringComparer.Ordinal);
    foreach (TypeDefinition type in AllTypes(assembly.MainModule.Types))
    {
        foreach (MethodDefinition method in type.Methods)
        {
            if (method.PInvokeInfo is null)
            {
                continue;
            }

            string entryPoint = method.PInvokeInfo.EntryPoint ?? method.Name;
            if (!requested.Contains(entryPoint))
            {
                continue;
            }

            EmitDefaultReturn(method);
            found.Add(entryPoint);
            stubbed++;
        }
    }

    foreach (string name in stubNames)
    {
        if (!found.Contains(name))
        {
            failures.Add($"P/Invoke not found: {name}");
        }
    }
}

foreach (StubSpec stub in stubs)
{
    if (!typesByName.TryGetValue(stub.TypeName, out TypeDefinition? type))
    {
        failures.Add($"type not found: {stub.TypeName}");
        continue;
    }

    MethodDefinition? method = type.Methods.FirstOrDefault(
        candidate =>
            candidate.Name == stub.MethodName &&
            candidate.Parameters.Count == 0 &&
            candidate.GenericParameters.Count == 0);

    if (method is null)
    {
        failures.Add($"method not found: {stub.TypeName}::{stub.MethodName}()");
        continue;
    }

    if (method.IsAbstract || method.IsPInvokeImpl)
    {
        failures.Add($"method has no IL body to stub: {stub.TypeName}::{stub.MethodName}()");
        continue;
    }

    if (IsBareReturn(method))
    {
        alreadyStubbed++;
        continue;
    }

    method.Body = new MethodBody(method);
    ILProcessor il = method.Body.GetILProcessor();
    il.Emit(OpCodes.Ret);
    method.Body.MaxStackSize = 8;
    stubbed++;
}

if (failures.Count != 0)
{
    foreach (string failure in failures)
    {
        Console.Error.WriteLine($"  {failure}");
    }

    return Fail($"{failures.Count} stub target(s) could not be resolved; output was not written.");
}

Directory.CreateDirectory(Path.GetDirectoryName(output) ?? ".");

if (stubbed == 0)
{
    File.Copy(input, output, overwrite: true);
    Console.WriteLine($"Already prepared; copied unchanged: {output}");
    Console.WriteLine($"SHA-256: {Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(output)))}");
    return 0;
}

assembly.MainModule.Mvid = DeterministicMvid(input, stubs, stubModules, stubNames);
assembly.Write(output, new WriterParameters { WriteSymbols = false, Timestamp = 0 });

using (AssemblyDefinition written = AssemblyDefinition.ReadAssembly(output))
{
    foreach (StubSpec stub in stubs)
    {
        TypeDefinition? type = AllTypes(written.MainModule.Types)
            .FirstOrDefault(candidate => candidate.FullName == stub.TypeName);
        MethodDefinition? method = type?.Methods.FirstOrDefault(
            candidate => candidate.Name == stub.MethodName && candidate.Parameters.Count == 0);

        if (method is null || !IsBareReturn(method))
        {
            return Fail(
                $"Written assembly failed re-verification for {stub.TypeName}::{stub.MethodName}().");
        }
    }

    foreach (string module in stubModules)
    {
        bool remaining = AllTypes(written.MainModule.Types)
            .Any(type => type.Methods.Any(
                method => method.PInvokeInfo?.Module.Name == module));

        if (remaining)
        {
            return Fail(
                $"Written assembly still imports native library '{module}'.");
        }
    }

    foreach (string name in stubNames)
    {
        bool remaining = AllTypes(written.MainModule.Types)
            .Any(type => type.Methods.Any(
                method => method.PInvokeInfo is not null &&
                          (method.PInvokeInfo.EntryPoint ?? method.Name) == name));

        if (remaining)
        {
            return Fail(
                $"Written assembly still imports native entry point '{name}'.");
        }
    }
}

Console.WriteLine($"{assembly.Name.Name}: stubbed {stubbed}, already stubbed {alreadyStubbed}.");
Console.WriteLine($"Written: {output}");
Console.WriteLine($"SHA-256: {Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(output)))}");
return 0;

static int Usage()
{
    Console.Error.WriteLine(
        "Usage: cmz-managed-aot-prep --input <assembly> --output <assembly> " +
        "[--stub <Namespace.Type>::<Method> ...] " +
        "[--stub-pinvoke-module <native library> ...] " +
        "[--stub-pinvoke-name <entry point> ...]\n" +
        "       cmz-managed-aot-prep --list-references <assembly>\n" +
        "       cmz-managed-aot-prep --list-type-scopes <assembly>");
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

static bool IsBareReturn(MethodDefinition method)
{
    if (!method.HasBody)
    {
        return false;
    }

    var instructions = method.Body.Instructions;
    return instructions.Count == 1 && instructions[0].OpCode == OpCodes.Ret;
}

static Guid DeterministicMvid(
    string inputPath,
    IReadOnlyList<StubSpec> stubs,
    IReadOnlyList<string> stubModules,
    IReadOnlyList<string> stubNames)
{
    byte[] inputHash = SHA256.HashData(File.ReadAllBytes(inputPath));
    string identity = string.Join(
        '|',
        SchemaTag,
        Convert.ToHexString(inputHash),
        string.Join(',', stubs.Select(stub => stub.ToString())),
        "modules=" + string.Join(',', stubModules.OrderBy(module => module, StringComparer.Ordinal)),
        "names=" + string.Join(',', stubNames.OrderBy(name => name, StringComparer.Ordinal)));
    byte[] hash = SHA256.HashData(Encoding.UTF8.GetBytes(identity));
    byte[] value = hash.AsSpan(0, 16).ToArray();
    value[7] = (byte)((value[7] & 0x0F) | 0x40);
    value[8] = (byte)((value[8] & 0x3F) | 0x80);
    return new Guid(value);
}

// Replaces a DllImport with a managed body that returns default(T). Only usable
// for bindings that are never reached on the target platform.
static void EmitDefaultReturn(MethodDefinition method)
{
    method.PInvokeInfo = null;
    method.Attributes &= ~MethodAttributes.PInvokeImpl;
    method.ImplAttributes = MethodImplAttributes.IL | MethodImplAttributes.Managed;

    var body = new MethodBody(method);
    ILProcessor il = body.GetILProcessor();

    if (method.ReturnType.MetadataType == MetadataType.Void)
    {
        il.Emit(OpCodes.Ret);
    }
    else if (method.ReturnType.IsValueType)
    {
        var local = new VariableDefinition(method.ReturnType);
        body.Variables.Add(local);
        il.Emit(OpCodes.Ldloca, local);
        il.Emit(OpCodes.Initobj, method.ReturnType);
        il.Emit(OpCodes.Ldloc, local);
        il.Emit(OpCodes.Ret);
    }
    else
    {
        il.Emit(OpCodes.Ldnull);
        il.Emit(OpCodes.Ret);
    }

    body.MaxStackSize = 8;
    method.Body = body;
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

static TypeReference RootType(TypeReference type)
{
    while (type is TypeSpecification specification && specification.ElementType is not null)
    {
        type = specification.ElementType;
    }

    return type;
}

// Lists the assemblies that own types this assembly actually uses. Assembly
// references alone over-approximate the closure badly: facades such as
// mscorlib list dozens of implementation assemblies they never call into, and
// pulling those into a static AOT build drags in unrelated native entry points.
static IEnumerable<string> CollectTypeScopes(ModuleDefinition module)
{
    var scopes = new SortedSet<string>(StringComparer.Ordinal);
    var seen = new HashSet<TypeReference>();
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
        TypeReference root = RootType(current);
        if (root is not TypeDefinition && root.Scope is AssemblyNameReference reference)
        {
            scopes.Add(reference.Name);
        }

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

    return scopes;
}

internal sealed record StubSpec(string TypeName, string MethodName)
{
    public static bool TryParse(string value, out StubSpec? spec, out string? error)
    {
        spec = null;
        error = null;

        int separator = value.IndexOf("::", StringComparison.Ordinal);
        if (separator <= 0 || separator + 2 >= value.Length)
        {
            error = $"Invalid stub target '{value}'; expected Namespace.Type::Method.";
            return false;
        }

        string typeName = value[..separator];
        string methodName = value[(separator + 2)..];
        if (typeName.Length == 0 || methodName.Length == 0)
        {
            error = $"Invalid stub target '{value}'; expected Namespace.Type::Method.";
            return false;
        }

        spec = new StubSpec(typeName, methodName);
        return true;
    }

    public override string ToString() => $"{TypeName}::{MethodName}()";
}
