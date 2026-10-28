using System.Buffers.Binary;
using System.Numerics;
using Microsoft.Xna.Framework.Content;

// Read-only XNB metadata diagnostics. Never write decompressed payloads, model
// vertices, textures, or converted assets. Unknown reader layouts fail closed.
if (args.Length == 0)
{
    Console.Error.WriteLine("Usage: Cmz.ContentAudit <private.xnb> [...]");
    return 2;
}
try
{
    foreach (string path in args)
    {
        using var payload = OpenPayload(path);
        using var reader = new BinaryReader(payload);
        int count = Count(reader.Read7BitEncodedInt());
        string[] readers = new string[count];
        for (int index = 0; index < count; index++)
        {
            readers[index] = reader.ReadString().Split(',')[0];
            reader.ReadInt32();
        }
        int shared = Count(reader.Read7BitEncodedInt());
        string root = ReaderName(reader, readers);
        Console.WriteLine($"{Path.GetFileName(path)}: reader={root}, shared={shared}");
        if (root == "TextureCubeReader")
        {
            int format = reader.ReadInt32();
            int size = Count(reader.ReadInt32());
            int levels = Count(reader.ReadInt32());
            Console.WriteLine($"  cube format={format}, size={size}, levels={levels}");
            for (int face = 0; face < 6; face++)
            for (int level = 0; level < levels; level++)
                Skip(reader, Count(reader.ReadInt32()));
        }
        else if (root == "ModelReader")
        {
            ReadModel(reader, readers);
        }
        else throw new NotSupportedException($"Root reader {root} is not supported.");

        int vertexCount = 0;
        for (int index = 0; index < shared; index++)
        {
            string name = ReaderName(reader, readers);
            switch (name)
            {
                case "VertexBufferReader": vertexCount = ReadVertices(reader); break;
                case "IndexBufferReader": ReadIndices(reader, vertexCount); break;
                case "BasicEffectReader":
                    reader.ReadString(); // External texture reference; never load it.
                    Skip(reader, 44);
                    reader.ReadBoolean();
                    break;
                case "EffectReader": Skip(reader, Count(reader.ReadInt32())); break;
                default: throw new NotSupportedException($"Shared reader {name} is not supported.");
            }
        }
        if (payload.Position != payload.Length)
            throw new InvalidDataException($"Unconsumed payload: {payload.Length - payload.Position} bytes.");
    }
    return 0;
}
catch (Exception error)
{
    Console.Error.WriteLine($"Content audit failed: {error.Message}");
    return 1;
}

static MemoryStream OpenPayload(string path)
{
    const int maximum = 64 * 1024 * 1024;
    long length = new FileInfo(path).Length;
    if (length < 10 || length > maximum) throw new InvalidDataException("XNB length is outside the audit limit.");
    byte[] file = File.ReadAllBytes(path);
    if (file[0] != 'X' || file[1] != 'N' || file[2] != 'B' || file[4] != 5 ||
        BinaryPrimitives.ReadInt32LittleEndian(file.AsSpan(6)) != file.Length)
        throw new InvalidDataException("Expected a complete XNB v5 file.");
    Console.WriteLine($"platform={(char)file[3]}, compressed={(file[5] & 0x80) != 0}");
    if ((file[5] & 0x80) == 0)
        return new MemoryStream(file, 10, file.Length - 10, writable: false);
    if (file.Length < 14) throw new InvalidDataException("Truncated compressed header.");
    int size = BinaryPrimitives.ReadInt32LittleEndian(file.AsSpan(10));
    if (size < 0 || size > maximum) throw new InvalidDataException("Decompressed size is outside the audit limit.");
    using var input = new MemoryStream(file, 14, file.Length - 14, writable: false);
    using var reader = new BinaryReader(input);
    var output = new MemoryStream(size);
    var decoder = new LzxDecoder(16);
    while (output.Position < size)
    {
        int high = reader.ReadByte();
        int low = reader.ReadByte();
        int block = (high << 8) | low;
        int frame = 0x8000;
        if (high == 0xff)
        {
            frame = (low << 8) | reader.ReadByte();
            block = (reader.ReadByte() << 8) | reader.ReadByte();
        }
        if (block <= 0 || block > input.Length - input.Position || frame <= 0 || frame > size - output.Position)
            throw new InvalidDataException("Invalid LZX frame bounds.");
        long end = input.Position + block;
        if (decoder.Decompress(input, block, output, frame) != 0)
            throw new InvalidDataException("LZX decompression failed.");
        input.Position = end;
    }
    if (output.Position != size) throw new InvalidDataException("LZX output size mismatch.");
    output.Position = 0;
    return output;
}

static string ReaderName(BinaryReader reader, string[] names)
{
    int index = reader.Read7BitEncodedInt();
    if (index == 0) return "null";
    if (index < 1 || index > names.Length) throw new InvalidDataException("Invalid type reader index.");
    return names[index - 1].Split('.').Last();
}

static void ObjectName(BinaryReader reader, string[] names)
{
    string name = ReaderName(reader, names);
    if (name == "StringReader") reader.ReadString();
    else if (name != "null") throw new NotSupportedException($"Expected a string or null, not {name}.");
}

static void NullTag(BinaryReader reader)
{
    if (reader.Read7BitEncodedInt() != 0) throw new NotSupportedException("Non-null model tags are not supported.");
}

static void Bone(BinaryReader reader, int bones)
{
    if (bones < 255) reader.ReadByte();
    else reader.ReadUInt32();
}

static void ReadModel(BinaryReader reader, string[] names)
{
    int bones = Count(reader.ReadInt32());
    for (int index = 0; index < bones; index++) { ObjectName(reader, names); Skip(reader, 64); }
    for (int index = 0; index < bones; index++)
    {
        Bone(reader, bones);
        int children = Count(reader.ReadInt32());
        for (int child = 0; child < children; child++) Bone(reader, bones);
    }
    int meshes = Count(reader.ReadInt32());
    int totalParts = 0;
    for (int mesh = 0; mesh < meshes; mesh++)
    {
        ObjectName(reader, names);
        Bone(reader, bones);
        Skip(reader, 16);
        NullTag(reader);
        int parts = Count(reader.ReadInt32());
        totalParts = checked(totalParts + parts);
        for (int part = 0; part < parts; part++)
        {
            Skip(reader, 16);
            NullTag(reader);
            for (int resource = 0; resource < 3; resource++) reader.Read7BitEncodedInt();
        }
    }
    Bone(reader, bones);
    NullTag(reader);
    Console.WriteLine($"  model bones={bones}, meshes={meshes}, parts={totalParts}");
}

static int ReadVertices(BinaryReader reader)
{
    int stride = Count(reader.ReadInt32());
    int elements = Count(reader.ReadInt32());
    int positionOffset = -1;
    for (int index = 0; index < elements; index++)
    {
        int offset = reader.ReadInt32();
        int format = reader.ReadInt32();
        int usage = reader.ReadInt32();
        int usageIndex = reader.ReadInt32();
        if (format == 2 && usage == 0 && usageIndex == 0) positionOffset = offset;
    }
    int count = Count(reader.ReadInt32());
    byte[] data = Bytes(reader, checked(count * stride));
    Console.WriteLine($"  vertices={count}, stride={stride}, elements={elements}");
    if (positionOffset >= 0 && positionOffset <= stride - 12)
    {
        foreach (bool bigEndian in new[] { false, true })
        {
            Vector3 minimum = new(float.PositiveInfinity);
            Vector3 maximum = new(float.NegativeInfinity);
            int invalid = 0;
            for (int index = 0; index < count; index++)
            {
                var values = new float[3];
                for (int axis = 0; axis < 3; axis++)
                {
                    ReadOnlySpan<byte> bytes = data.AsSpan(index * stride + positionOffset + axis * 4, 4);
                    int bits = bigEndian ? BinaryPrimitives.ReadInt32BigEndian(bytes) : BinaryPrimitives.ReadInt32LittleEndian(bytes);
                    values[axis] = BitConverter.Int32BitsToSingle(bits);
                }
                if (values.Any(value => !float.IsFinite(value))) { invalid++; continue; }
                var position = new Vector3(values[0], values[1], values[2]);
                minimum = Vector3.Min(minimum, position);
                maximum = Vector3.Max(maximum, position);
            }
            Console.WriteLine($"    {(bigEndian ? "BE" : "LE")} position bounds={minimum}..{maximum}, nonfinite={invalid}");
        }
    }
    return count;
}

static void ReadIndices(BinaryReader reader, int vertices)
{
    bool shortIndices = reader.ReadBoolean();
    byte[] data = Bytes(reader, Count(reader.ReadInt32()));
    int width = shortIndices ? 2 : 4;
    if (data.Length % width != 0) throw new InvalidDataException("Misaligned index buffer.");
    foreach (bool bigEndian in new[] { false, true })
    {
        uint maximum = 0;
        int invalid = 0;
        for (int index = 0; index < data.Length; index += width)
        {
            ReadOnlySpan<byte> bytes = data.AsSpan(index, width);
            uint value = width == 2
                ? (bigEndian ? BinaryPrimitives.ReadUInt16BigEndian(bytes) : BinaryPrimitives.ReadUInt16LittleEndian(bytes))
                : (bigEndian ? BinaryPrimitives.ReadUInt32BigEndian(bytes) : BinaryPrimitives.ReadUInt32LittleEndian(bytes));
            maximum = Math.Max(maximum, value);
            if (value >= vertices) invalid++;
        }
        Console.WriteLine($"  {(bigEndian ? "BE" : "LE")} {width * 8}-bit indices={data.Length / width}, max={maximum}, outside-last-vertex-buffer={invalid}");
    }
}

static byte[] Bytes(BinaryReader reader, int size)
{
    if (size < 0 || size > reader.BaseStream.Length - reader.BaseStream.Position)
        throw new InvalidDataException("Payload read exceeds stream bounds.");
    byte[] data = reader.ReadBytes(size);
    if (data.Length != size) throw new EndOfStreamException();
    return data;
}

static void Skip(BinaryReader reader, int size)
{
    if (size < 0 || size > reader.BaseStream.Length - reader.BaseStream.Position)
        throw new InvalidDataException("Payload skip exceeds stream bounds.");
    reader.BaseStream.Position += size;
}

static int Count(int count) => count >= 0 && count <= 16 * 1024 * 1024
    ? count : throw new InvalidDataException("Content count is outside the audit limit.");
