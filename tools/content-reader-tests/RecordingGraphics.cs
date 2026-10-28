// These minimal test doubles record the real reader's calls without a native
// graphics library. They are not a replacement runtime or content decoder.
namespace Microsoft.Xna.Framework.Content
{
    internal abstract class ContentTypeReader<T>
    {
        protected internal abstract T Read(ContentReader reader, T existingInstance);
    }

    internal sealed class ContentReader : BinaryReader
    {
        internal readonly char platform;
        internal readonly ContentManager ContentManager = new();

        internal ContentReader(Stream stream, char platform)
            : base(stream, System.Text.Encoding.UTF8, leaveOpen: true)
        {
            this.platform = platform;
        }

        internal T ReadRawObject<T>()
        {
            if (typeof(T) != typeof(Graphics.VertexDeclaration))
                throw new NotSupportedException("Only raw vertex declarations are expected.");
            return (T)(object)new VertexDeclarationReader().Read(this, null);
        }
    }

    internal sealed class ContentManager
    {
        internal Graphics.GraphicsDevice GetGraphicsDevice() => new();
    }

    internal sealed class ContentLoadException : Exception
    {
        internal ContentLoadException(string message) : base(message) { }
    }
}

namespace Microsoft.Xna.Framework.Graphics
{
    internal sealed class GraphicsDevice { }
    internal sealed record TextureUpload(CubeMapFace Face, int Level, byte[] Data);

    internal sealed class TextureCube
    {
        internal int Size { get; }
        internal bool MipMap { get; }
        internal SurfaceFormat Format { get; }
        internal List<TextureUpload> Uploads { get; } = new();

        internal TextureCube(GraphicsDevice device, int size, bool mipMap, SurfaceFormat format)
        {
            Size = size;
            MipMap = mipMap;
            Format = format;
        }

        internal void SetData<T>(CubeMapFace face, int level, object rectangle, T[] data, int offset, int count)
        {
            if (typeof(T) != typeof(byte)) throw new NotSupportedException("The reader should upload bytes.");
            byte[] bytes = (byte[])(object)data;
            Uploads.Add(new TextureUpload(face, level, bytes.AsSpan(offset, count).ToArray()));
        }
    }

    internal readonly record struct VertexElement(int Offset, VertexElementFormat VertexElementFormat,
        VertexElementUsage VertexElementUsage, int UsageIndex);

    internal sealed class VertexDeclaration
    {
        private readonly VertexElement[] elements;
        internal int VertexStride { get; }

        internal VertexDeclaration(int stride, VertexElement[] elements)
        {
            VertexStride = stride;
            this.elements = elements;
        }

        internal VertexElement[] GetVertexElements() => (VertexElement[])elements.Clone();
    }

    internal sealed class VertexBuffer
    {
        internal VertexDeclaration VertexDeclaration { get; }
        internal int VertexCount { get; }
        internal byte[] Data { get; private set; }

        internal VertexBuffer(GraphicsDevice device, VertexDeclaration declaration, int count, BufferUsage usage)
        {
            VertexDeclaration = declaration;
            VertexCount = count;
        }

        internal void SetData<T>(T[] data) => Data = (byte[])((byte[])(object)data).Clone();
    }

    internal sealed class IndexBuffer
    {
        internal IndexElementSize IndexElementSize { get; }
        internal int IndexCount { get; }
        internal byte[] Data { get; private set; }

        internal IndexBuffer(GraphicsDevice device, IndexElementSize size, int count, BufferUsage usage)
        {
            IndexElementSize = size;
            IndexCount = count;
        }

        internal void SetData<T>(T[] data) => Data = (byte[])((byte[])(object)data).Clone();
    }
}
