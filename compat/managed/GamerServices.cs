using System.Collections;
using System.Collections.ObjectModel;
using CastleMinerZ.XnaCompat;
using Microsoft.Xna.Framework.Audio;

namespace Microsoft.Xna.Framework.GamerServices;

public enum GamerPrivilegeSetting
{
    Blocked = 0,
    FriendsOnly = 1,
    Everyone = 2
}

public enum MessageBoxIcon
{
    None = 0,
    Error = 1,
    Warning = 2,
    Alert = 3
}

public enum NotificationPosition
{
    TopLeft = 0,
    TopCenter = 1,
    TopRight = 2,
    CenterLeft = 3,
    Center = 4,
    CenterRight = 5,
    BottomLeft = 6,
    BottomCenter = 7,
    BottomRight = 8
}

public class GamerCollection<T> : ReadOnlyCollection<T>, IEnumerable<Gamer>
    where T : Gamer
{
    internal GamerCollection(IList<T> gamers)
        : base(gamers)
    {
    }

    public new GamerCollectionEnumerator GetEnumerator() => new(this);

    IEnumerator<Gamer> IEnumerable<Gamer>.GetEnumerator() => GetEnumerator();

    public struct GamerCollectionEnumerator : IEnumerator<T>
    {
        private readonly GamerCollection<T> collection;
        private int index;

        internal GamerCollectionEnumerator(GamerCollection<T> collection)
        {
            this.collection = collection;
            index = -1;
        }

        public T Current => collection[index];

        object IEnumerator.Current => Current;

        public bool MoveNext() => ++index < collection.Count;

        public void Reset() => index = -1;

        public void Dispose()
        {
        }
    }
}

public abstract class Gamer
{
    private readonly object profileOperation = new();
    private bool disposed;

    protected Gamer(string gamertag)
    {
        Gamertag = gamertag;
    }

    public static SignedInGamerCollection SignedInGamers => GamerServicesRuntime.SignedInGamers;

    public string Gamertag { get; internal set; }

    public bool IsDisposed => disposed;

    public object? Tag { get; set; }

    internal LocalUserProfile? LocalProfile { get; set; }

    public IAsyncResult BeginGetProfile(AsyncCallback? callback, object? asyncState) =>
        CompletedAsyncResult<GamerProfile>.Run(
            profileOperation,
            () => new GamerProfile(LocalProfile?.PicturePath),
            callback,
            asyncState);

    public GamerProfile EndGetProfile(IAsyncResult result) =>
        CompletedAsyncResult<GamerProfile>.End(result, profileOperation);

    internal void MarkDisposed()
    {
        disposed = true;
    }

    public override string ToString() => Gamertag;
}

public sealed class FriendGamer : Gamer
{
    internal FriendGamer(string gamertag)
        : base(gamertag)
    {
    }
}

public sealed class FriendCollection : GamerCollection<FriendGamer>
{
    internal FriendCollection()
        : base(new List<FriendGamer>())
    {
    }
}

public sealed class GamerProfile
{
    private readonly string? picturePath;

    internal GamerProfile(string? picturePath = null)
    {
        this.picturePath = picturePath;
    }

    // A valid transparent 1x1 PNG keeps callers that immediately decode a
    // profile image on the normal success path.
    private const string EmptyPicture =
        "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVQIHWP4z8DwHwAFgAI/ScL5WQAAAABJRU5ErkJggg==";

    public Stream GetGamerPicture()
    {
        if (!string.IsNullOrEmpty(picturePath))
        {
            try
            {
                return File.OpenRead(picturePath);
            }
            catch (IOException)
            {
            }
            catch (UnauthorizedAccessException)
            {
            }
        }
        return new MemoryStream(Convert.FromBase64String(EmptyPicture), writable: false);
    }
}

public sealed class GamerPrivileges
{
    internal GamerPrivileges()
    {
    }

    public GamerPrivilegeSetting AllowCommunication => GamerPrivilegeSetting.Everyone;

    public bool AllowOnlineSessions => false;

    public bool AllowPurchaseContent => false;
}

public sealed class GameDefaults
{
    internal GameDefaults()
    {
    }

    public bool InvertYAxis => false;
}

public sealed class SignedInGamer : Gamer
{
    private readonly FriendCollection friends = new();

    internal SignedInGamer(PlayerIndex playerIndex, string gamertag)
        : base(gamertag)
    {
        PlayerIndex = playerIndex;
    }

    public static event EventHandler<SignedInEventArgs>? SignedIn;

    public static event EventHandler<SignedOutEventArgs>? SignedOut;

    public PlayerIndex PlayerIndex { get; }

    public bool IsGuest => false;

    public bool IsSignedInToLive => false;

    public GamerPrivileges Privileges { get; } = new();

    public GameDefaults GameDefaults { get; } = new();

    public FriendCollection GetFriends() => friends;

    public bool IsHeadset(Microphone microphone) => false;

    internal static void RaiseSignedIn(SignedInGamer gamer) =>
        SignedIn?.Invoke(null, new SignedInEventArgs(gamer));

    internal static void RaiseSignedOut(SignedInGamer gamer) =>
        SignedOut?.Invoke(null, new SignedOutEventArgs(gamer));
}

public sealed class SignedInGamerCollection : GamerCollection<SignedInGamer>
{
    private readonly IList<SignedInGamer> gamers;

    internal SignedInGamerCollection(IList<SignedInGamer> gamers)
        : base(gamers)
    {
        this.gamers = gamers;
    }

    public SignedInGamer? this[PlayerIndex index] =>
        gamers.FirstOrDefault(gamer => gamer.PlayerIndex == index);
}

public sealed class SignedInEventArgs : EventArgs
{
    internal SignedInEventArgs(SignedInGamer gamer)
    {
        Gamer = gamer;
    }

    public SignedInGamer Gamer { get; }
}

public sealed class SignedOutEventArgs : EventArgs
{
    internal SignedOutEventArgs(SignedInGamer gamer)
    {
        Gamer = gamer;
    }

    public SignedInGamer Gamer { get; }
}

public sealed class InviteAcceptedEventArgs : EventArgs
{
    internal InviteAcceptedEventArgs(SignedInGamer gamer)
    {
        Gamer = gamer;
    }

    public SignedInGamer Gamer { get; }
}

public sealed class GuideAlreadyVisibleException : InvalidOperationException
{
    public GuideAlreadyVisibleException()
    {
    }

    public GuideAlreadyVisibleException(string message)
        : base(message)
    {
    }
}

public static class GamerServicesDispatcher
{
    public static bool IsInitialized { get; internal set; }
}

public sealed class GamerServicesComponent : GameComponent
{
    public GamerServicesComponent(Game game)
        : base(game)
    {
        GamerServicesDispatcher.IsInitialized = true;
        GamerServicesRuntime.EnsureSignedIn(1);
    }

    public override void Update(GameTime gameTime)
    {
    }
}

public static class Guide
{
    private static readonly object keyboardOperation = new();
    private static readonly object messageBoxOperation = new();

    public static bool IsTrialMode => false;

    public static bool IsVisible => false;

    public static NotificationPosition NotificationPosition { get; set; }

    public static void ShowSignIn(int paneCount, bool onlineOnly)
    {
        if (paneCount is < 1 or > 4)
        {
            throw new ArgumentOutOfRangeException(nameof(paneCount));
        }

        GamerServicesRuntime.EnsureSignedIn(paneCount);
    }

    public static IAsyncResult BeginShowKeyboardInput(
        PlayerIndex player,
        string title,
        string description,
        string defaultText,
        AsyncCallback? callback,
        object? state) =>
        CompletedAsyncResult<string>.Run(
            keyboardOperation,
            () => defaultText ?? string.Empty,
            callback,
            state);

    public static string EndShowKeyboardInput(IAsyncResult result) =>
        CompletedAsyncResult<string>.End(result, keyboardOperation);

    public static IAsyncResult BeginShowMessageBox(
        PlayerIndex player,
        string title,
        string text,
        IEnumerable<string> buttons,
        int focusButton,
        MessageBoxIcon icon,
        AsyncCallback? callback,
        object? state) =>
        BeginShowMessageBox(title, text, buttons, focusButton, icon, callback, state);

    public static IAsyncResult BeginShowMessageBox(
        string title,
        string text,
        IEnumerable<string> buttons,
        int focusButton,
        MessageBoxIcon icon,
        AsyncCallback? callback,
        object? state)
    {
        ArgumentNullException.ThrowIfNull(buttons);
        string[] choices = buttons.ToArray();
        int? selection = choices.Length == 0
            ? null
            : Math.Clamp(focusButton, 0, choices.Length - 1);
        return CompletedAsyncResult<int?>.Run(
            messageBoxOperation,
            () => selection,
            callback,
            state);
    }

    public static int? EndShowMessageBox(IAsyncResult result) =>
        CompletedAsyncResult<int?>.End(result, messageBoxOperation);

    public static void ShowGameInvite(PlayerIndex player, IEnumerable<Gamer> recipients)
    {
        ArgumentNullException.ThrowIfNull(recipients);
    }

    public static void ShowMarketplace(PlayerIndex player)
    {
    }
}

internal static class GamerServicesRuntime
{
    private static readonly List<SignedInGamer> gamers = new();

    internal static SignedInGamerCollection SignedInGamers { get; } = new(gamers);

    internal static void EnsureSignedIn(int count)
    {
        int target = Math.Clamp(count, 0, 4);
        while (gamers.Count < target)
        {
            int slot = gamers.Count;
            LocalUserProfile? profile = slot == 0 ? LocalUserProfile.ReadSelected() : null;
            var gamer = new SignedInGamer((PlayerIndex)slot, profile?.Nickname ?? $"Player {slot + 1}")
            {
                LocalProfile = profile
            };
            gamers.Add(gamer);
            SignedInGamer.RaiseSignedIn(gamer);
        }
    }
}
