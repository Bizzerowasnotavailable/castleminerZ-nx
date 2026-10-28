using System.Collections.ObjectModel;
using CastleMinerZ.XnaCompat;
using Microsoft.Xna.Framework.GamerServices;

namespace Microsoft.Xna.Framework.Net;

public enum NetworkSessionType
{
    Local = 0,
    SystemLink = 1,
    PlayerMatch = 2,
    Ranked = 3,
    LocalWithLeaderboards = 4
}

[Flags]
public enum SendDataOptions
{
    None = 0,
    Reliable = 1,
    InOrder = 2,
    ReliableInOrder = Reliable | InOrder,
    Chat = 4
}

public enum NetworkSessionEndReason
{
    ClientSignedOut = 0,
    HostEndedSession = 1,
    RemovedByHost = 2,
    Disconnected = 3
}

public sealed class NetworkSessionProperties
{
    private const int PropertyCount = 8;
    private readonly int?[] values = new int?[PropertyCount];

    public int? this[int index]
    {
        get
        {
            ValidateIndex(index);
            return values[index];
        }
        set
        {
            ValidateIndex(index);
            values[index] = value;
        }
    }

    internal NetworkSessionProperties Clone()
    {
        var clone = new NetworkSessionProperties();
        Array.Copy(values, clone.values, PropertyCount);
        return clone;
    }

    private static void ValidateIndex(int index)
    {
        if ((uint)index >= PropertyCount)
        {
            throw new ArgumentOutOfRangeException(nameof(index));
        }
    }
}

public sealed class AvailableNetworkSession
{
    internal AvailableNetworkSession(
        string hostGamertag,
        int currentGamerCount,
        NetworkSessionProperties properties)
    {
        HostGamertag = hostGamertag;
        CurrentGamerCount = currentGamerCount;
        SessionProperties = properties;
    }

    public string HostGamertag { get; }

    public int CurrentGamerCount { get; }

    public NetworkSessionProperties SessionProperties { get; }
}

public sealed class AvailableNetworkSessionCollection :
    ReadOnlyCollection<AvailableNetworkSession>, IDisposable
{
    internal AvailableNetworkSessionCollection(IList<AvailableNetworkSession> sessions)
        : base(sessions)
    {
    }

    public void Dispose()
    {
    }
}

public class NetworkGamer : Gamer
{
    private readonly NetworkSession session;

    internal NetworkGamer(
        NetworkSession session,
        byte id,
        string gamertag,
        bool isLocal,
        bool isHost)
        : base(gamertag)
    {
        this.session = session;
        Id = id;
        IsLocal = isLocal;
        IsHost = isHost;
        Machine = new NetworkMachine(session, this);
    }

    public byte Id { get; }

    public bool IsLocal { get; }

    public bool IsHost { get; internal set; }

    public bool IsTalking => false;

    public bool HasLeftSession { get; internal set; }

    public NetworkMachine Machine { get; }

    internal NetworkSession Session => session;
}

public sealed class LocalNetworkGamer : NetworkGamer
{
    private readonly Queue<Packet> received = new();

    internal LocalNetworkGamer(
        NetworkSession session,
        byte id,
        SignedInGamer signedInGamer,
        bool isHost)
        : base(session, id, signedInGamer.Gamertag, isLocal: true, isHost)
    {
        SignedInGamer = signedInGamer;
        LocalProfile = signedInGamer.LocalProfile;
    }

    public SignedInGamer SignedInGamer { get; }

    public bool IsDataAvailable => received.Count != 0;

    public void SendData(
        byte[] data,
        int offset,
        int count,
        SendDataOptions options) =>
        Session.Send(this, data, offset, count, target: null);

    public void SendData(
        byte[] data,
        int offset,
        int count,
        SendDataOptions options,
        NetworkGamer recipient) =>
        Session.Send(this, data, offset, count, recipient);

    public int ReceiveData(byte[] data, out NetworkGamer sender)
    {
        ArgumentNullException.ThrowIfNull(data);
        if (!received.TryDequeue(out Packet packet))
        {
            throw new InvalidOperationException("No network data is available.");
        }
        if (data.Length < packet.Data.Length)
        {
            throw new ArgumentException("The destination buffer is too small.", nameof(data));
        }

        packet.Data.CopyTo(data, 0);
        sender = packet.Sender;
        return packet.Data.Length;
    }

    internal void Enqueue(byte[] data, NetworkGamer sender) =>
        received.Enqueue(new Packet(data, sender));

    private readonly record struct Packet(byte[] Data, NetworkGamer Sender);
}

public sealed class NetworkMachine
{
    private readonly NetworkSession session;
    private readonly NetworkGamer gamer;

    internal NetworkMachine(NetworkSession session, NetworkGamer gamer)
    {
        this.session = session;
        this.gamer = gamer;
    }

    public void RemoveFromSession() => session.Remove(gamer);
}

public sealed class NetworkSession : IDisposable
{
    private static readonly object createOperation = new();
    private static readonly object findOperation = new();
    private static readonly object joinOperation = new();
    private static readonly object joinInvitedOperation = new();

    private readonly List<NetworkGamer> allGamers = new();
    private readonly List<LocalNetworkGamer> localGamers = new();
    private readonly List<NetworkGamer> remoteGamers = new();
    private EventHandler<GamerJoinedEventArgs>? gamerJoined;
    private bool disposed;
    private int bytesPerSecondSent;

    private NetworkSession(
        NetworkSessionType sessionType,
        IEnumerable<SignedInGamer> signedInGamers,
        int maxGamers,
        NetworkSessionProperties? properties)
    {
        SessionType = sessionType;
        MaxGamers = maxGamers;
        SessionProperties = properties?.Clone() ?? new NetworkSessionProperties();

        byte id = 0;
        foreach (SignedInGamer signedInGamer in signedInGamers)
        {
            var gamer = new LocalNetworkGamer(this, id++, signedInGamer, isHost: id == 1);
            localGamers.Add(gamer);
            allGamers.Add(gamer);
        }

        LocalGamers = new GamerCollection<LocalNetworkGamer>(localGamers);
        AllGamers = new GamerCollection<NetworkGamer>(allGamers);
        RemoteGamers = new GamerCollection<NetworkGamer>(remoteGamers);
    }

    public static event EventHandler<InviteAcceptedEventArgs>? InviteAccepted;

    public GamerCollection<LocalNetworkGamer> LocalGamers { get; }

    public GamerCollection<NetworkGamer> AllGamers { get; }

    public GamerCollection<NetworkGamer> RemoteGamers { get; }

    public NetworkSessionProperties SessionProperties { get; }

    public NetworkSessionType SessionType { get; }

    public bool IsHost => localGamers.Any(gamer => gamer.IsHost);

    public int BytesPerSecondSent => bytesPerSecondSent;

    public int MaxGamers { get; }

    public bool AllowHostMigration { get; set; }

    public bool AllowJoinInProgress { get; set; }

    public event EventHandler<GamerJoinedEventArgs>? GamerJoined
    {
        add
        {
            gamerJoined += value;
            if (value is not null)
            {
                foreach (NetworkGamer gamer in allGamers)
                {
                    value(this, new GamerJoinedEventArgs(gamer));
                }
            }
        }
        remove => gamerJoined -= value;
    }

    public event EventHandler<GamerLeftEventArgs>? GamerLeft;

    public event EventHandler<GameStartedEventArgs>? GameStarted;

    public event EventHandler<GameEndedEventArgs>? GameEnded;

    public event EventHandler<HostChangedEventArgs>? HostChanged;

    public event EventHandler<NetworkSessionEndedEventArgs>? SessionEnded;

    public static IAsyncResult BeginCreate(
        NetworkSessionType sessionType,
        IEnumerable<SignedInGamer> localGamers,
        int maxGamers,
        int privateGamerSlots,
        NetworkSessionProperties sessionProperties,
        AsyncCallback? callback,
        object? state) =>
        CompletedAsyncResult<NetworkSession>.Run(
            createOperation,
            () => CreateLocal(sessionType, localGamers, maxGamers, sessionProperties),
            callback,
            state);

    public static NetworkSession EndCreate(IAsyncResult result) =>
        CompletedAsyncResult<NetworkSession>.End(result, createOperation);

    public static IAsyncResult BeginFind(
        NetworkSessionType sessionType,
        IEnumerable<SignedInGamer> localGamers,
        NetworkSessionProperties searchProperties,
        AsyncCallback? callback,
        object? state) =>
        CompletedAsyncResult<AvailableNetworkSessionCollection>.Run(
            findOperation,
            static () => new AvailableNetworkSessionCollection(new List<AvailableNetworkSession>()),
            callback,
            state);

    public static AvailableNetworkSessionCollection EndFind(IAsyncResult result) =>
        CompletedAsyncResult<AvailableNetworkSessionCollection>.End(result, findOperation);

    public static IAsyncResult BeginJoin(
        AvailableNetworkSession availableSession,
        AsyncCallback? callback,
        object? state)
    {
        ArgumentNullException.ThrowIfNull(availableSession);
        return CompletedAsyncResult<NetworkSession>.Run(
            joinOperation,
            () => CreateLocal(
                NetworkSessionType.Local,
                CurrentSignedInGamers(),
                Math.Max(availableSession.CurrentGamerCount, 1),
                availableSession.SessionProperties),
            callback,
            state);
    }

    public static NetworkSession EndJoin(IAsyncResult result) =>
        CompletedAsyncResult<NetworkSession>.End(result, joinOperation);

    public static IAsyncResult BeginJoinInvited(
        IEnumerable<SignedInGamer> localGamers,
        AsyncCallback? callback,
        object? state) =>
        CompletedAsyncResult<NetworkSession>.Run(
            joinInvitedOperation,
            () => CreateLocal(
                NetworkSessionType.Local,
                localGamers,
                maxGamers: 4,
                new NetworkSessionProperties()),
            callback,
            state);

    public static NetworkSession EndJoinInvited(IAsyncResult result) =>
        CompletedAsyncResult<NetworkSession>.End(result, joinInvitedOperation);

    public NetworkGamer? FindGamerById(byte gamerId) =>
        allGamers.FirstOrDefault(gamer => gamer.Id == gamerId);

    public void Update()
    {
        ThrowIfDisposed();
    }

    public void Dispose()
    {
        if (disposed)
        {
            return;
        }

        disposed = true;
        foreach (NetworkGamer gamer in allGamers)
        {
            gamer.HasLeftSession = true;
            gamer.MarkDisposed();
        }
        SessionEnded?.Invoke(this, new NetworkSessionEndedEventArgs(NetworkSessionEndReason.HostEndedSession));
    }

    internal void Send(
        LocalNetworkGamer sender,
        byte[] data,
        int offset,
        int count,
        NetworkGamer? target)
    {
        ThrowIfDisposed();
        ArgumentNullException.ThrowIfNull(data);
        ArgumentOutOfRangeException.ThrowIfNegative(offset);
        ArgumentOutOfRangeException.ThrowIfNegative(count);
        if (offset > data.Length - count)
        {
            throw new ArgumentException("The packet range lies outside the source buffer.", nameof(count));
        }

        byte[] packet = data.AsSpan(offset, count).ToArray();
        bytesPerSecondSent = checked(bytesPerSecondSent + count);
        if (target is LocalNetworkGamer localTarget)
        {
            localTarget.Enqueue(packet, sender);
            return;
        }

        foreach (LocalNetworkGamer gamer in localGamers)
        {
            gamer.Enqueue((byte[])packet.Clone(), sender);
        }
    }

    internal void Remove(NetworkGamer gamer)
    {
        if (!allGamers.Remove(gamer))
        {
            return;
        }

        if (gamer is LocalNetworkGamer local)
        {
            localGamers.Remove(local);
        }
        else
        {
            remoteGamers.Remove(gamer);
        }
        gamer.HasLeftSession = true;
        GamerLeft?.Invoke(this, new GamerLeftEventArgs(gamer));
    }

    private static NetworkSession CreateLocal(
        NetworkSessionType requestedType,
        IEnumerable<SignedInGamer> signedInGamers,
        int maxGamers,
        NetworkSessionProperties? properties)
    {
        ArgumentNullException.ThrowIfNull(signedInGamers);
        List<SignedInGamer> gamers = signedInGamers.ToList();
        if (gamers.Count == 0)
        {
            throw new InvalidOperationException("At least one local gamer must be signed in.");
        }
        if (gamers.Count > 4)
        {
            throw new ArgumentException("At most four local gamers are supported.", nameof(signedInGamers));
        }
        if (maxGamers < gamers.Count || maxGamers > 31)
        {
            throw new ArgumentOutOfRangeException(nameof(maxGamers));
        }

        // Until a transport is implemented, every requested mode is isolated
        // to this process while preserving the requested type for game logic.
        return new NetworkSession(requestedType, gamers, maxGamers, properties);
    }

    private static IEnumerable<SignedInGamer> CurrentSignedInGamers()
    {
        GamerServicesRuntime.EnsureSignedIn(1);
        return Gamer.SignedInGamers;
    }

    private void ThrowIfDisposed()
    {
        ObjectDisposedException.ThrowIf(disposed, this);
    }
}

public sealed class GamerJoinedEventArgs : EventArgs
{
    internal GamerJoinedEventArgs(NetworkGamer gamer)
    {
        Gamer = gamer;
    }

    public NetworkGamer Gamer { get; }
}

public sealed class GamerLeftEventArgs : EventArgs
{
    internal GamerLeftEventArgs(NetworkGamer gamer)
    {
        Gamer = gamer;
    }

    public NetworkGamer Gamer { get; }
}

public sealed class HostChangedEventArgs : EventArgs
{
    internal HostChangedEventArgs(NetworkGamer oldHost, NetworkGamer newHost)
    {
        OldHost = oldHost;
        NewHost = newHost;
    }

    public NetworkGamer OldHost { get; }

    public NetworkGamer NewHost { get; }
}

public sealed class NetworkSessionEndedEventArgs : EventArgs
{
    internal NetworkSessionEndedEventArgs(NetworkSessionEndReason endReason)
    {
        EndReason = endReason;
    }

    public NetworkSessionEndReason EndReason { get; }
}

public sealed class GameStartedEventArgs : EventArgs
{
}

public sealed class GameEndedEventArgs : EventArgs
{
}
