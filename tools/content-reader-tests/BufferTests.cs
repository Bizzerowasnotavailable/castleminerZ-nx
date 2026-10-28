using System.Buffers.Binary;
using Microsoft.Xna.Framework.Content;
using Microsoft.Xna.Framework.Graphics;

internal static class BufferTests
{
    internal static void Run()
    {
        foreach (char platform in new[] { 'x', 'w' })
        {
            foreach (VertexElementFormat format in Enum.GetValues<VertexElementFormat>())
            {
                int stride = Shape(format).Bytes + 8;
                var elements = new[] { new VertexElement(4, format, VertexElementUsage.TextureCoordinate, 0) };
                CheckVertices(platform, stride, elements, count: 3);
            }
            CheckVertices(platform, 36, new[] {
                new VertexElement(0, VertexElementFormat.Vector3, VertexElementUsage.Position, 0),
                new VertexElement(12, VertexElementFormat.Vector3, VertexElementUsage.Normal, 0),
                new VertexElement(24, VertexElementFormat.Vector2, VertexElementUsage.TextureCoordinate, 0)
            }, count: 3);
            CheckVertices(platform, 16, new[] {
                new VertexElement(0, VertexElementFormat.Vector3, VertexElementUsage.Position, 0)
            }, count: 0);
            foreach (bool shortIndices in new[] { false, true })
            foreach (bool reuse in new[] { false, true })
                CheckIndices(platform, shortIndices, reuse);
        }

        foreach (VertexElement element in new[] {
            new VertexElement(8, VertexElementFormat.Vector3, VertexElementUsage.Position, 0),
            new VertexElement(-1, VertexElementFormat.Color, VertexElementUsage.Color, 0),
            new VertexElement(0, (VertexElementFormat)999, VertexElementUsage.Position, 0)
        })
        {
            using var stream = VertexPayload(16, new[] { element }, 1, new byte[16]);
            using var reader = new ContentReader(stream, 'x');
            ExpectContentError(() => new VertexBufferReader().Read(reader, null));
        }
        foreach (bool shortIndices in new[] { false, true })
        {
            using var stream = IndexPayload(shortIndices, new byte[3]);
            using var reader = new ContentReader(stream, 'x');
            ExpectContentError(() => new IndexBufferReader().Read(reader, null));
        }
        Console.WriteLine("PASS: Xbox/desktop vertex formats, interleaved layouts, padding, empty buffers, " +
            "16/32-bit indices, index reuse, and malformed declarations");
    }

    private static void CheckVertices(char platform, int stride, VertexElement[] elements, int count)
    {
        byte[] expected = Enumerable.Range(0, stride * count)
            .Select(index => unchecked((byte)(0x53 + index * 11))).ToArray();
        byte[] stored = (byte[])expected.Clone();
        if (platform == 'x')
        {
            for (int vertex = 0; vertex < count; vertex++)
            foreach (VertexElement element in elements)
            {
                (int bytes, int word) = Shape(element.VertexElementFormat);
                for (int offset = 0; offset < bytes; offset += word)
                    Array.Reverse(stored, vertex * stride + element.Offset + offset, word);
            }
        }
        using var stream = VertexPayload(stride, elements, count, stored);
        using var reader = new ContentReader(stream, platform);
        VertexBuffer actual = new VertexBufferReader().Read(reader, null);
        Check(actual.Data.SequenceEqual(expected), $"{platform}: vertex bytes or padding changed incorrectly");
        Check(actual.VertexCount == count && actual.VertexDeclaration.VertexStride == stride,
            "Vertex count and declaration are retained");
        Check(stream.Position == stream.Length, "Vertex reader consumes exactly the payload");
    }

    private static MemoryStream VertexPayload(int stride, VertexElement[] elements, int count, byte[] stored)
    {
        var stream = new MemoryStream();
        using (var writer = new BinaryWriter(stream, System.Text.Encoding.UTF8, leaveOpen: true))
        {
            writer.Write(stride);
            writer.Write(elements.Length);
            foreach (VertexElement element in elements)
            {
                writer.Write(element.Offset);
                writer.Write((int)element.VertexElementFormat);
                writer.Write((int)element.VertexElementUsage);
                writer.Write(element.UsageIndex);
            }
            writer.Write(count);
            writer.Write(stored);
        }
        stream.Position = 0;
        return stream;
    }

    private static void CheckIndices(char platform, bool shortIndices, bool reuse)
    {
        uint[] values = shortIndices ? new uint[] { 0, 2, 255, 256, 65535 }
            : new uint[] { 0, 255, 256, 65536, 0x87654321 };
        int width = shortIndices ? 2 : 4;
        byte[] expected = new byte[values.Length * width];
        for (int index = 0; index < values.Length; index++)
        {
            if (shortIndices) BinaryPrimitives.WriteUInt16LittleEndian(expected.AsSpan(index * width), (ushort)values[index]);
            else BinaryPrimitives.WriteUInt32LittleEndian(expected.AsSpan(index * width), values[index]);
        }
        byte[] stored = (byte[])expected.Clone();
        if (platform == 'x')
            for (int offset = 0; offset < stored.Length; offset += width) Array.Reverse(stored, offset, width);
        using var stream = IndexPayload(shortIndices, stored);
        using var reader = new ContentReader(stream, platform);
        IndexElementSize size = shortIndices ? IndexElementSize.SixteenBits : IndexElementSize.ThirtyTwoBits;
        IndexBuffer existing = reuse ? new IndexBuffer(new GraphicsDevice(), size, values.Length, BufferUsage.None) : null;
        IndexBuffer actual = new IndexBufferReader().Read(reader, existing);
        Check(actual.Data.SequenceEqual(expected), $"{platform}: {width * 8}-bit index bytes differ");
        Check(!reuse || ReferenceEquals(actual, existing), "Existing index buffer is retained");
        Check(actual.IndexCount == values.Length && actual.IndexElementSize == size, "Index properties are retained");
        Check(stream.Position == stream.Length, "Index reader consumes exactly the payload");
    }

    private static MemoryStream IndexPayload(bool shortIndices, byte[] data)
    {
        var stream = new MemoryStream();
        using (var writer = new BinaryWriter(stream, System.Text.Encoding.UTF8, leaveOpen: true))
        {
            writer.Write(shortIndices);
            writer.Write(data.Length);
            writer.Write(data);
        }
        stream.Position = 0;
        return stream;
    }

    private static (int Bytes, int Word) Shape(VertexElementFormat format) => format switch
    {
        VertexElementFormat.Single or VertexElementFormat.Color or VertexElementFormat.Byte4 => (4, 4),
        VertexElementFormat.Vector2 => (8, 4),
        VertexElementFormat.Vector3 => (12, 4),
        VertexElementFormat.Vector4 => (16, 4),
        VertexElementFormat.Short2 or VertexElementFormat.NormalizedShort2 or VertexElementFormat.HalfVector2 => (4, 2),
        VertexElementFormat.Short4 or VertexElementFormat.NormalizedShort4 or VertexElementFormat.HalfVector4 => (8, 2),
        _ => throw new ArgumentOutOfRangeException(nameof(format))
    };

    private static void Check(bool valid, string message)
    {
        if (!valid) throw new Exception(message);
    }

    private static void ExpectContentError(Action action)
    {
        try { action(); }
        catch (ContentLoadException) { return; }
        throw new Exception("Malformed Xbox buffer did not raise ContentLoadException.");
    }
}
