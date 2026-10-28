using CastleMinerZ.XnaCompat;
using Microsoft.Xna.Framework;
using Microsoft.Xna.Framework.GamerServices;
using Microsoft.Xna.Framework.Net;

if (args.Length != 2 || args[0] is not ("selected" or "fallback"))
    throw new ArgumentException("usage: Cmz.LocalProfileTests <selected|fallback> <scratch-directory>");

string scratch = Path.GetFullPath(args[1]);
Directory.CreateDirectory(scratch);
Environment.SetEnvironmentVariable("CMZ_LOCAL_PROFILE_ID", null);
Environment.SetEnvironmentVariable("CMZ_LOCAL_PROFILE_NAME", null);
Environment.SetEnvironmentVariable("CMZ_LOCAL_PROFILE_PICTURE", null);
foreach (string invalid in new[] { "", "../../other", new string('0', 32), new string('z', 32) })
{
    Environment.SetEnvironmentVariable("CMZ_LOCAL_PROFILE_ID", invalid);
    Check("Invalid local user IDs are rejected", LocalUserProfile.ReadSelected() is null);
}
Environment.SetEnvironmentVariable("CMZ_LOCAL_PROFILE_ID", null);

byte[] picture = { 0xff, 0xd8, 0xff, 0xd9 }; // Synthetic stream fixture, not a user image.
string picturePath = Path.Combine(scratch, args[0] + "-picture.jpg");
bool selected = args[0] == "selected";
if (selected)
{
    File.WriteAllBytes(picturePath, picture);
    Environment.SetEnvironmentVariable("CMZ_LOCAL_PROFILE_ID", "0123456789ABCDEF0123456789ABCDEF");
    Environment.SetEnvironmentVariable("CMZ_LOCAL_PROFILE_NAME", "Test Switch User");
    Environment.SetEnvironmentVariable("CMZ_LOCAL_PROFILE_PICTURE", picturePath);
    Check("Local UID is canonicalized", LocalUserProfile.ReadSelected()?.Id == "0123456789abcdef0123456789abcdef");
}
Guide.ShowSignIn(1, onlineOnly: false);
SignedInGamer gamer = Gamer.SignedInGamers[PlayerIndex.One] ?? throw new Exception("No local gamer");
Check("Nickname/fallback is exposed", gamer.Gamertag == (selected ? "Test Switch User" : "Player 1"));
Check("No Xbox Live or online account is fabricated", !gamer.IsSignedInToLive && !gamer.Privileges.AllowOnlineSessions);

GamerProfile profile = gamer.EndGetProfile(gamer.BeginGetProfile(null, null));
using (Stream stream = profile.GetGamerPicture())
{
    byte[] bytes = Read(stream);
    Check("Picture uses local image or PNG fallback", selected ? bytes.SequenceEqual(picture) : IsPng(bytes));
}
File.Delete(picturePath);
using (Stream stream = profile.GetGamerPicture())
    Check("Missing local image falls back to PNG", IsPng(Read(stream)));

using NetworkSession session = NetworkSession.EndCreate(NetworkSession.BeginCreate(
    NetworkSessionType.Local, new[] { gamer }, 1, 0, new NetworkSessionProperties(), null, null));
LocalNetworkGamer networkGamer = session.LocalGamers[0];
Check("Offline session keeps the local nickname", networkGamer.Gamertag == gamer.Gamertag);
AvatarDescription avatar1 = AvatarDescription.EndGetFromGamer(AvatarDescription.BeginGetFromGamer(gamer, null, null));
AvatarDescription avatar2 = AvatarDescription.EndGetFromGamer(AvatarDescription.BeginGetFromGamer(networkGamer, null, null));
Check("Avatar remains a valid project placeholder", avatar1.IsValid && avatar2.IsValid);
if (selected)
    Check("Local placeholder is stable across gamer APIs", avatar1.Description.SequenceEqual(avatar2.Description));

using (var renderer = new AvatarRenderer(avatar1))
{
    Check("Avatar renderer reports Unavailable so the game uses its own fallback skeleton",
        renderer.State == AvatarRendererState.Unavailable);
    Check("Avatar renderer keeps the 71-bone API shape", renderer.BindPose.Count == 71 && renderer.ParentBones.Count == 71);
    renderer.Draw(Enumerable.Repeat(Matrix.Identity, 71).ToArray(), default);
    Check("Avatar renderer rejects torn bone buffers",
        Throws<ArgumentException>(() => renderer.Draw(new Matrix[70], default)));
}

Console.WriteLine($"PASS: {args[0]} local-profile integration");
return;

static byte[] Read(Stream stream)
{
    using var output = new MemoryStream();
    stream.CopyTo(output);
    return output.ToArray();
}

static bool IsPng(byte[] bytes) => bytes.AsSpan().StartsWith(new byte[] { 137, 80, 78, 71, 13, 10, 26, 10 });

static bool Throws<T>(Action action)
    where T : Exception
{
    try { action(); }
    catch (T) { return true; }
    return false;
}

static void Check(string name, bool valid)
{
    if (!valid) throw new Exception(name);
    Console.WriteLine("ok: " + name);
}
