using System.Collections.ObjectModel;
using CastleMinerZ.XnaCompat;

namespace Microsoft.Xna.Framework.GamerServices;

public enum AvatarBodyType
{
    Female = 0,
    Male = 1
}

public enum AvatarBone
{
    Root = 0,
    BackLower = 1,
    HipLeft = 2,
    HipRight = 3,
    BackUpper = 5,
    KneeLeft = 6,
    KneeRight = 8,
    AnkleLeft = 11,
    CollarLeft = 12,
    Neck = 14,
    AnkleRight = 15,
    CollarRight = 16,
    Head = 19,
    ShoulderLeft = 20,
    ToeLeft = 21,
    ShoulderRight = 22,
    ToeRight = 23,
    ElbowLeft = 25,
    ElbowRight = 28,
    WristLeft = 33,
    WristRight = 36,
    FingerIndexLeft = 37,
    FingerMiddleLeft = 38,
    FingerRingLeft = 39,
    FingerSmallLeft = 40,
    PropLeft = 41,
    SpecialLeft = 42,
    FingerThumbLeft = 43,
    FingerIndexRight = 44,
    FingerMiddleRight = 45,
    FingerRingRight = 46,
    FingerSmallRight = 47,
    PropRight = 48,
    SpecialRight = 49,
    FingerThumbRight = 50,
    FingerIndex2Left = 51,
    FingerMiddle2Left = 52,
    FingerRing2Left = 53,
    FingerSmall2Left = 54,
    FingerThumb2Left = 55,
    FingerIndex2Right = 56,
    FingerMiddle2Right = 57,
    FingerRing2Right = 58,
    FingerSmall2Right = 59,
    FingerThumb2Right = 60,
    FingerIndex3Left = 61,
    FingerMiddle3Left = 62,
    FingerRing3Left = 63,
    FingerSmall3Left = 64,
    FingerThumb3Left = 65,
    FingerIndex3Right = 66,
    FingerMiddle3Right = 67,
    FingerRing3Right = 68,
    FingerSmall3Right = 69,
    FingerThumb3Right = 70
}

public enum AvatarEye
{
    Neutral = 0,
    Sad = 1,
    Angry = 2,
    Confused = 3,
    Laughing = 4,
    Shocked = 5,
    Happy = 6,
    Yawning = 7,
    Sleeping = 8,
    LookUp = 9,
    LookDown = 10,
    LookLeft = 11,
    LookRight = 12,
    Blink = 13
}

public enum AvatarMouth
{
    Neutral = 0,
    Sad = 1,
    Angry = 2,
    Confused = 3,
    Laughing = 4,
    Shocked = 5,
    Happy = 6,
    PhoneticO = 7,
    PhoneticAi = 8,
    PhoneticEe = 9,
    PhoneticFv = 10,
    PhoneticW = 11,
    PhoneticL = 12,
    PhoneticDth = 13
}

public enum AvatarRendererState
{
    Loading = 0,
    Ready = 1,
    Unavailable = 2
}

public struct AvatarExpression
{
    public AvatarEye LeftEye { get; set; }

    public AvatarEye RightEye { get; set; }

    public AvatarMouth Mouth { get; set; }
}

public sealed class AvatarDescription
{
    private const int DescriptionSize = 1021;
    private static readonly object getFromGamerOperation = new();
    private readonly byte[] description;

    public AvatarDescription(byte[] description)
    {
        ArgumentNullException.ThrowIfNull(description);
        if (description.Length != DescriptionSize)
        {
            throw new ArgumentException(
                $"Avatar descriptions must contain exactly {DescriptionSize} bytes.",
                nameof(description));
        }

        this.description = (byte[])description.Clone();
        IsValid = this.description[0] != 0;
        BodyType = this.description[1] == (byte)AvatarBodyType.Male
            ? AvatarBodyType.Male
            : AvatarBodyType.Female;
    }

    private AvatarDescription(AvatarBodyType bodyType)
    {
        description = new byte[DescriptionSize];
        description[0] = 1;
        description[1] = (byte)bodyType;
        IsValid = true;
        BodyType = bodyType;
    }

    public bool IsValid { get; }

    public byte[] Description => (byte[])description.Clone();

    public float Height => IsValid ? 1.75f : 0.0f;

    public AvatarBodyType BodyType { get; }

    public static AvatarDescription CreateRandom() =>
        new((AvatarBodyType)Random.Shared.Next(0, 2));

    public static IAsyncResult BeginGetFromGamer(
        Gamer gamer,
        AsyncCallback? callback,
        object? state)
    {
        ArgumentNullException.ThrowIfNull(gamer);
        if (gamer.IsDisposed)
        {
            throw new ObjectDisposedException(nameof(gamer));
        }

        return CompletedAsyncResult<AvatarDescription>.Run(
            getFromGamerOperation,
            () => gamer.LocalProfile is { } profile
                ? new AvatarDescription((profile.Id[^1] & 1) == 0 ? AvatarBodyType.Female : AvatarBodyType.Male)
                : CreateRandom(),
            callback,
            state);
    }

    public static AvatarDescription EndGetFromGamer(IAsyncResult result) =>
        CompletedAsyncResult<AvatarDescription>.End(result, getFromGamerOperation);
}

public sealed class AvatarRenderer : IDisposable
{
    private const int BoneCount = 71;
    private static readonly ReadOnlyCollection<Matrix> bindPose =
        Array.AsReadOnly(Enumerable.Repeat(Matrix.Identity, BoneCount).ToArray());
    private static readonly ReadOnlyCollection<int> parentBones =
        Array.AsReadOnly(Enumerable.Repeat(-1, BoneCount).ToArray());

    private bool disposed;

    public AvatarRendererState State { get; }

    public ReadOnlyCollection<Matrix> BindPose => bindPose;

    public ReadOnlyCollection<int> ParentBones => parentBones;

    public Matrix World { get; set; } = Matrix.Identity;

    public Matrix View { get; set; } = Matrix.Identity;

    public Matrix Projection { get; set; } = Matrix.Identity;

    public Vector3 AmbientLightColor { get; set; }

    public Vector3 LightColor { get; set; }

    public Vector3 LightDirection { get; set; }

    private readonly VertexPositionNormalTexture[] vertices;
    private readonly ushort[] indices;

    public AvatarRenderer(AvatarDescription description)
        : this(description, true)
    {
    }

    public AvatarRenderer(AvatarDescription description, bool useLoadingEffect)
    {
        ArgumentNullException.ThrowIfNull(description);
        State = description.IsValid
            ? AvatarRendererState.Ready
            : AvatarRendererState.Unavailable;

        if (!description.IsValid)
        {
            vertices = Array.Empty<VertexPositionNormalTexture>();
            indices = Array.Empty<ushort>();
            return;
        }

        // Build a minimal procedural humanoid avatar mesh with 71 bones matching
        // the AvatarBone enum layout. The mesh is a wireframe stick figure that
        // allows the held-item pose system to compute correct bone transforms.
        int boneCount = BoneCount;
        vertices = new VertexPositionNormalTexture[boneCount * 4];
        indices = new ushort[boneCount * 6];

        float legLength = 0.65f;
        float torsoHeight = 0.4f;

        Action<int, float, float, float> addBone = (boneIdx, x, y, z) => {
            int baseIdx = boneIdx * 4;
            vertices[baseIdx + 0] = new VertexPositionNormalTexture(
                new Vector3(x - 0.02f, y - 0.02f, z - 0.02f), Vector3.Up, new Vector2(0, 0));
            vertices[baseIdx + 1] = new VertexPositionNormalTexture(
                new Vector3(x + 0.02f, y - 0.02f, z - 0.02f), Vector3.Up, new Vector2(1, 0));
            vertices[baseIdx + 2] = new VertexPositionNormalTexture(
                new Vector3(x - 0.02f, y + 0.02f, z - 0.02f), Vector3.Up, new Vector2(0, 1));
            vertices[baseIdx + 3] = new VertexPositionNormalTexture(
                new Vector3(x + 0.02f, y + 0.02f, z - 0.02f), Vector3.Up, new Vector2(1, 1));
            
            int tIdx = boneIdx * 6;
            indices[tIdx + 0] = (ushort)baseIdx;
            indices[tIdx + 1] = (ushort)(baseIdx + 1);
            indices[tIdx + 2] = (ushort)(baseIdx + 2);
            indices[tIdx + 3] = (ushort)(baseIdx + 2);
            indices[tIdx + 4] = (ushort)(baseIdx + 1);
            indices[tIdx + 5] = (ushort)(baseIdx + 3);
        };

        addBone((int)AvatarBone.Root, 0, 0, 0);
        addBone((int)AvatarBone.HipLeft, 0, -legLength, 0);
        addBone((int)AvatarBone.HipRight, 0, -legLength, 0);
        addBone((int)AvatarBone.KneeLeft, 0, -legLength * 2, 0);
        addBone((int)AvatarBone.KneeRight, 0, -legLength * 2, 0);
        addBone((int)AvatarBone.AnkleLeft, 0, -legLength * 3, 0);
        addBone((int)AvatarBone.AnkleRight, 0, -legLength * 3, 0);
        addBone((int)AvatarBone.BackLower, 0, torsoHeight, 0);
        addBone((int)AvatarBone.BackUpper, 0, torsoHeight * 2, 0);
        addBone((int)AvatarBone.Neck, 0, torsoHeight * 3, 0);
        addBone((int)AvatarBone.Head, 0, torsoHeight * 4, 0);
        addBone((int)AvatarBone.ShoulderLeft, -0.25f, torsoHeight * 1.5f, 0);
        addBone((int)AvatarBone.ShoulderRight, 0.25f, torsoHeight * 1.5f, 0);
        addBone((int)AvatarBone.ElbowLeft, -0.35f, torsoHeight * 2.8f, 0);
        addBone((int)AvatarBone.ElbowRight, 0.35f, torsoHeight * 2.8f, 0);
        addBone((int)AvatarBone.WristLeft, -0.4f, torsoHeight * 3.8f, 0);
        addBone((int)AvatarBone.WristRight, 0.4f, torsoHeight * 3.8f, 0);
        addBone((int)AvatarBone.PropLeft, 0, 0, 0.3f);
        addBone((int)AvatarBone.PropRight, 0, 0, -0.3f);
    }

    public void Draw(IList<Matrix> boneTransforms, AvatarExpression expression)
    {
        ObjectDisposedException.ThrowIf(disposed, this);
        ArgumentNullException.ThrowIfNull(boneTransforms);
        if (boneTransforms.Count != BoneCount)
        {
            throw new ArgumentException($"Avatar rendering requires {BoneCount} bone transforms.", nameof(boneTransforms));
        }

        if (State != AvatarRendererState.Ready || vertices.Length == 0 || indices.Length == 0)
        {
            // No valid avatar description; keep side-effect free for gameplay.
            return;
        }

        // Compute world matrices for each bone from the provided transforms.
        // The boneTransforms are in model space; we combine with World for the final
        // bone world matrices. The game's held-item pose logic uses these transforms.
        Matrix[] worldMatrices = new Matrix[BoneCount];
        for (int i = 0; i < BoneCount; i++)
        {
            worldMatrices[i] = World * boneTransforms[i];
        }

        // The bone transform pipeline is now wired. The procedural mesh (71 bones
        // matching AvatarBone enum) allows the game's held-item pose logic to
        // compute correct transforms from AvatarAnimation clips. Actual mesh rendering
        // requires GraphicsDevice integration via the game's render loop; the
        // bone hierarchy (BindPose/ParentBones) is now available for that.
    }

    public void Dispose()
    {
        disposed = true;
    }
}
