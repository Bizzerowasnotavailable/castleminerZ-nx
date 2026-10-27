using Mono.Cecil;

var referencePaths = new List<string>();
var inputs = new List<string>();

for (int index = 0; index < args.Length; index++)
{
    if (args[index] == "--reference")
    {
        if (index + 1 >= args.Length)
        {
            Console.Error.WriteLine("Missing value for --reference.");
            return 2;
        }

        referencePaths.Add(args[++index]);
        continue;
    }

    if (args[index] is "--verbose" or "-v")
    {
        continue;
    }

    inputs.Add(args[index]);
}

if (referencePaths.Count == 0 || inputs.Count == 0)
{
    Console.Error.WriteLine(
        "Usage: cmz-managed-audit --reference <FNA.dll> [--reference <CastleMinerZ.XnaCompat.dll>] " +
        "<assembly> [assembly ...]");
    return 2;
}

var references = new Dictionary<string, TypeDefinition>(StringComparer.Ordinal);
var referenceNames = new List<string>();
var owners = new List<AssemblyDefinition>();

try
{
    foreach (string referencePath in referencePaths)
    {
        string path = Path.GetFullPath(referencePath);
        if (!File.Exists(path))
        {
            Console.Error.WriteLine($"Reference assembly was not found: {path}");
            return 2;
        }

        AssemblyDefinition assembly = AssemblyDefinition.ReadAssembly(path);
        owners.Add(assembly);
        referenceNames.Add(assembly.Name.Name);
        foreach (TypeDefinition type in AllTypes(assembly.MainModule.Types))
        {
            if (type.FullName == "<Module>")
            {
                continue;
            }

            if (!references.TryAdd(type.FullName, type))
            {
                Console.Error.WriteLine(
                    $"Duplicate reference type '{type.FullName}' in {path}.");
                return 2;
            }
        }
    }

    foreach (string input in inputs)
    {
        string path = Path.GetFullPath(input);
        if (!File.Exists(path))
        {
            Console.Error.WriteLine($"Managed input was not found: {path}");
            return 2;
        }

        using AssemblyDefinition assembly = AssemblyDefinition.ReadAssembly(path);
        AuditAssembly(assembly, references, referenceNames);
    }

    return 0;
}
finally
{
    foreach (AssemblyDefinition owner in owners)
    {
        owner.Dispose();
    }
}

static void AuditAssembly(
    AssemblyDefinition assembly,
    IReadOnlyDictionary<string, TypeDefinition> references,
    IReadOnlyList<string> referenceNames)
{
    Console.WriteLine($"[{assembly.Name.Name}]");

    AssemblyNameReference[] xnaReferences = assembly.MainModule.AssemblyReferences
        .Where(reference => IsXnaAssembly(reference.Name))
        .OrderBy(reference => reference.Name, StringComparer.Ordinal)
        .ToArray();

    Console.WriteLine($"XNA assembly references: {xnaReferences.Length}");
    foreach (AssemblyNameReference reference in xnaReferences)
    {
        string token = Convert.ToHexString(reference.PublicKeyToken ?? Array.Empty<byte>())
            .ToLowerInvariant();
        Console.WriteLine($"  {reference.Name}, version={reference.Version}, token={token}");
    }

    Dictionary<string, TypeReference> types = new(StringComparer.Ordinal);
    foreach (TypeReference type in assembly.MainModule.GetTypeReferences())
    {
        string? sourceAssembly = GetAssemblyScope(type);
        if (sourceAssembly is null || !IsXnaAssembly(sourceAssembly))
        {
            continue;
        }

        TypeReference elementType = GetElementType(type);
        string key = $"{sourceAssembly}|{elementType.FullName}";
        types.TryAdd(key, elementType);
    }

    Dictionary<string, MemberReference> members = new(StringComparer.Ordinal);
    foreach (MemberReference member in assembly.MainModule.GetMemberReferences())
    {
        string? sourceAssembly = GetAssemblyScope(member.DeclaringType);
        if (sourceAssembly is null || !IsXnaAssembly(sourceAssembly))
        {
            continue;
        }

        string key = $"{sourceAssembly}|{member.DeclaringType.FullName}|{MemberKey(member)}";
        members.TryAdd(key, member);
    }

    int presentTypes = 0;
    var missingTypes = new SortedSet<string>(StringComparer.Ordinal);
    foreach (TypeReference type in types.Values)
    {
        string fullName = GetElementType(type).FullName;
        if (references.TryGetValue(fullName, out _))
        {
            presentTypes++;
        }
        else
        {
            missingTypes.Add($"{GetAssemblyScope(type)}|{fullName}");
        }
    }

    int presentMembers = 0;
    int membersWithMissingTypes = 0;
    var missingMemberKeys = new SortedSet<string>(StringComparer.Ordinal);
    foreach (MemberReference member in members.Values)
    {
        string declaringName = GetElementType(member.DeclaringType).FullName;
        if (!references.TryGetValue(declaringName, out TypeDefinition? targetType))
        {
            membersWithMissingTypes++;
            missingMemberKeys.Add($"{GetAssemblyScope(member.DeclaringType)}|{declaringName}|{MemberKey(member)}");
            continue;
        }

        if (HasMember(targetType, member, references))
        {
            presentMembers++;
        }
        else
        {
            missingMemberKeys.Add($"{GetAssemblyScope(member.DeclaringType)}|{declaringName}|{MemberKey(member)}");
        }
    }

    Console.WriteLine(
        $"Unique XNA type references: {types.Count}; resolved: {presentTypes}; " +
        $"missing: {types.Count - presentTypes}");
    Console.WriteLine(
        $"Unique XNA member references: {members.Count}; compatible: {presentMembers}; " +
        $"missing type: {membersWithMissingTypes}; missing member: " +
        $"{members.Count - presentMembers - membersWithMissingTypes}");

    foreach (IGrouping<string, TypeReference> group in types.Values
        .GroupBy(type => GetAssemblyScope(type) ?? "<unknown>")
        .OrderBy(group => group.Key, StringComparer.Ordinal))
    {
        int available = group.Count(type => references.ContainsKey(GetElementType(type).FullName));
        Console.WriteLine($"  {group.Key}: types={group.Count()}, resolved={available}");
    }

    if (missingTypes.Count != 0)
    {
        Console.WriteLine($"  unresolved types: {string.Join(", ", missingTypes)}");
    }

    if (missingMemberKeys.Count != 0)
    {
        Console.WriteLine($"  unresolved members: {string.Join(", ", missingMemberKeys)}");
    }

    Console.WriteLine($"  references: {string.Join(", ", referenceNames)}");
    Console.WriteLine();
}

static bool HasMember(
    TypeDefinition targetType,
    MemberReference reference,
    IReadOnlyDictionary<string, TypeDefinition> references)
{
    for (TypeDefinition? current = targetType; current is not null; current = ResolveBase(current, references))
    {
        if (reference is MethodReference method)
        {
            string key = MethodKey(method);
            if (current.Methods.Any(candidate => MethodKey(candidate) == key))
            {
                return true;
            }
        }
        else if (reference is FieldReference field)
        {
            string key = FieldKey(field);
            if (current.Fields.Any(candidate => FieldKey(candidate) == key))
            {
                return true;
            }
        }
        else
        {
            return false;
        }
    }

    return false;
}

static TypeDefinition? ResolveBase(
    TypeDefinition type,
    IReadOnlyDictionary<string, TypeDefinition> references)
{
    TypeReference? baseType = type.BaseType;
    return baseType is null || !references.TryGetValue(GetElementType(baseType).FullName, out TypeDefinition? definition)
        ? null
        : definition;
}

static string MemberKey(MemberReference member) => member switch
{
    MethodReference method => MethodKey(method),
    FieldReference field => FieldKey(field),
    _ => $"other:{member.Name}"
};

static string MethodKey(MethodReference method)
{
    MethodReference elementMethod = method is GenericInstanceMethod generic
        ? generic.ElementMethod
        : method;
    string parameters = string.Join(",", elementMethod.Parameters.Select(
        parameter => TypeKey(parameter.ParameterType)));
    return $"method:{elementMethod.Name}`{elementMethod.GenericParameters.Count}:" +
        $"{TypeKey(elementMethod.ReturnType)}({parameters})";
}

static string FieldKey(FieldReference field) =>
    $"field:{field.Name}:{TypeKey(field.FieldType)}";

static string TypeKey(TypeReference type) => type switch
{
    GenericParameter generic => generic.Type == GenericParameterType.Method
        ? $"!!{generic.Position}"
        : $"!{generic.Position}",
    GenericInstanceType generic =>
        $"{TypeKey(generic.ElementType)}<{string.Join(",", generic.GenericArguments.Select(TypeKey))}>",
    ArrayType array => $"{TypeKey(array.ElementType)}[{new string(',', array.Rank - 1)}]",
    ByReferenceType reference => $"{TypeKey(reference.ElementType)}&",
    PointerType pointer => $"{TypeKey(pointer.ElementType)}*",
    OptionalModifierType modifier => TypeKey(modifier.ElementType),
    RequiredModifierType modifier => TypeKey(modifier.ElementType),
    PinnedType pinned => TypeKey(pinned.ElementType),
    _ => type.FullName
};

static TypeReference GetElementType(TypeReference type)
{
    while (type is TypeSpecification specification)
    {
        type = specification.ElementType;
    }
    return type;
}

static string? GetAssemblyScope(TypeReference type)
{
    IMetadataScope? scope = GetElementType(type).Scope;
    while (scope is TypeReference declaringType)
    {
        scope = declaringType.Scope;
    }
    return scope is AssemblyNameReference assembly ? assembly.Name : null;
}

static bool IsXnaAssembly(string name) =>
    name.Equals("Microsoft.Xna.Framework", StringComparison.Ordinal) ||
    name.StartsWith("Microsoft.Xna.Framework.", StringComparison.Ordinal);

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
