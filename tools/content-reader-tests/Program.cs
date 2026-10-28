using Microsoft.Xna.Framework.Content;
using Microsoft.Xna.Framework.Graphics;

// Compile the real dependency reader against a recording graphics boundary.
// Every byte in these fixtures is synthetic; no game content or GPU is needed.
foreach (char platform in new[] { 'x', 'w' })
foreach (SurfaceFormat format in new[] { SurfaceFormat.Color, SurfaceFormat.ColorBgraEXT,
    SurfaceFormat.Dxt1, SurfaceFormat.Dxt3, SurfaceFormat.Dxt5 })
foreach (bool reuse in new[] { false, true })
{
    const int size = 8;
    const int levels = 4;
    var expected = new List<TextureUpload>();
    using var stream = new MemoryStream();
    using (var writer = new BinaryWriter(stream, System.Text.Encoding.UTF8, leaveOpen: true))
    {
        writer.Write((int)format);
        writer.Write(size);
        writer.Write(levels);
        for (int face = 0; face < 6; face++)
        for (int level = 0; level < levels; level++)
        {
            int dimension = Math.Max(1, size >> level);
            bool color = format is SurfaceFormat.Color or SurfaceFormat.ColorBgraEXT;
            int blocks = ((dimension + 3) / 4) * ((dimension + 3) / 4);
            int length = color ? dimension * dimension * 4 : blocks * (format == SurfaceFormat.Dxt1 ? 8 : 16);
            byte[] linear = Enumerable.Range(0, length)
                .Select(index => unchecked((byte)(face * 37 + level * 19 + index * 7)))
                .ToArray();
            byte[] stored = (byte[])linear.Clone();
            if (platform == 'x')
            {
                int wordSize = color ? 4 : 2;
                for (int offset = 0; offset < length; offset += wordSize)
                    Array.Reverse(stored, offset, wordSize);
            }
            writer.Write(stored.Length);
            writer.Write(stored);
            expected.Add(new TextureUpload((CubeMapFace)face, level, linear));
        }
    }
    stream.Position = 0;
    using var reader = new ContentReader(stream, platform);
    TextureCube existing = reuse ? new TextureCube(new GraphicsDevice(), size, true, format) : null;
    TextureCube actual = new TextureCubeReader().Read(reader, existing);
    Check("Existing cube instance is retained", !reuse || ReferenceEquals(existing, actual));
    Check("Cube properties are retained", actual.Size == size && actual.MipMap && actual.Format == format);
    Check("All six faces and four mips are uploaded", actual.Uploads.Count == expected.Count);
    for (int index = 0; index < expected.Count; index++)
    {
        TextureUpload wanted = expected[index];
        TextureUpload upload = actual.Uploads[index];
        Check($"{platform}/{format}, face {wanted.Face}, mip {wanted.Level}: linear bytes",
            upload.Face == wanted.Face && upload.Level == wanted.Level && upload.Data.SequenceEqual(wanted.Data));
    }
    Check("Reader consumes exactly the cube payload", stream.Position == stream.Length);
    Console.WriteLine($"ok: platform={platform} format={format} reuse={reuse}, six faces and four mip levels");
}
Console.WriteLine("PASS: Xbox cubemap byte order and desktop passthrough (480 face/mip uploads)");
BufferTests.Run();

static void Check(string name, bool valid)
{
    if (!valid) throw new Exception(name);
}
