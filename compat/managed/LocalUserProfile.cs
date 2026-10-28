namespace CastleMinerZ.XnaCompat;

/// <summary>
/// A snapshot of the selected console-local Switch user. The native host supplies
/// it before Mono starts; no Nintendo Account or network credentials are used.
/// </summary>
public sealed class LocalUserProfile
{
    private LocalUserProfile(string id, string nickname, string? picturePath)
    {
        Id = id;
        Nickname = nickname;
        PicturePath = picturePath;
    }

    public string Id { get; }

    public string Nickname { get; }

    internal string? PicturePath { get; }

    public static LocalUserProfile? ReadSelected()
    {
        string? id = Environment.GetEnvironmentVariable("CMZ_LOCAL_PROFILE_ID");
        if (id is null || id.Length != 32 || id.Any(c => !Uri.IsHexDigit(c)) ||
            id.All(c => c == '0'))
        {
            return null;
        }
        string? nickname = Environment.GetEnvironmentVariable("CMZ_LOCAL_PROFILE_NAME");
        if (string.IsNullOrWhiteSpace(nickname))
            nickname = "Switch User";
        return new LocalUserProfile(id.ToLowerInvariant(), nickname,
            Environment.GetEnvironmentVariable("CMZ_LOCAL_PROFILE_PICTURE"));
    }
}
