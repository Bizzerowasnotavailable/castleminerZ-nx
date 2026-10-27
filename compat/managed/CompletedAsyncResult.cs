using System.Runtime.ExceptionServices;

namespace CastleMinerZ.XnaCompat;

internal sealed class CompletedAsyncResult<T> : IAsyncResult, IDisposable
{
    private readonly object owner;
    private readonly ExceptionDispatchInfo? failure;
    private ManualResetEvent? waitHandle;
    private int ended;

    private CompletedAsyncResult(
        object owner,
        T? value,
        ExceptionDispatchInfo? failure,
        AsyncCallback? callback,
        object? state)
    {
        this.owner = owner;
        Value = value;
        this.failure = failure;
        AsyncState = state;
        callback?.Invoke(this);
    }

    public object? AsyncState { get; }

    public WaitHandle AsyncWaitHandle =>
        waitHandle ??= new ManualResetEvent(initialState: true);

    public bool CompletedSynchronously => true;

    public bool IsCompleted => true;

    private T? Value { get; }

    public static IAsyncResult Run(
        object owner,
        Func<T> operation,
        AsyncCallback? callback,
        object? state)
    {
        try
        {
            return new CompletedAsyncResult<T>(owner, operation(), null, callback, state);
        }
        catch (Exception exception)
        {
            return new CompletedAsyncResult<T>(
                owner,
                default,
                ExceptionDispatchInfo.Capture(exception),
                callback,
                state);
        }
    }

    public static T End(IAsyncResult result, object owner)
    {
        if (result is not CompletedAsyncResult<T> completed ||
            !ReferenceEquals(completed.owner, owner))
        {
            throw new ArgumentException("The asynchronous result belongs to another operation.", nameof(result));
        }

        if (Interlocked.Exchange(ref completed.ended, 1) != 0)
        {
            throw new InvalidOperationException("The asynchronous operation has already ended.");
        }

        completed.failure?.Throw();
        return completed.Value!;
    }

    public void Dispose()
    {
        waitHandle?.Dispose();
    }
}
