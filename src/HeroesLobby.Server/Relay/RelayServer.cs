using System.Buffers.Binary;
using System.Net;
using System.Net.Sockets;
using System.Threading.Channels;

namespace HeroesLobby.Server.Relay;

/// <summary>TCP endpoint for game clients' replacement DirectPlay transport (Relay:Port, 0 disables).</summary>
public sealed class RelayServer(RelayHub hub, IConfiguration config, ILogger<RelayServer> logger) : BackgroundService
{
    public int Port { get; private set; }
    private readonly TaskCompletionSource started = new(TaskCreationOptions.RunContinuationsAsynchronously);
    public Task Started => started.Task;

    protected override async Task ExecuteAsync(CancellationToken stopping)
    {
        var port = config.GetValue("Relay:Port", 5090);
        if (port < 0) { started.TrySetResult(); return; }
        var address = IPAddress.Parse(config["Relay:Address"] ?? "0.0.0.0");
        var listener = new TcpListener(address, port);
        try { listener.Start(); }
        catch (SocketException error)
        {
            // The lobby stays usable; game clients will report the relay as unreachable.
            logger.LogError(error, "Game relay could not listen on {Address}:{Port}", address, port);
            started.TrySetResult(); return;
        }
        Port = ((IPEndPoint)listener.LocalEndpoint).Port;
        logger.LogInformation("Game relay listening on {Address}:{Port}", address, Port);
        started.TrySetResult();
        try
        {
            while (!stopping.IsCancellationRequested)
            {
                var client = await listener.AcceptTcpClientAsync(stopping);
                _ = Task.Run(() => Serve(client, stopping), stopping);
            }
        }
        catch (OperationCanceledException) { }
        finally { listener.Stop(); }
    }

    private async Task Serve(TcpClient client, CancellationToken stopping)
    {
        client.NoDelay = true;
        var peer = new Peer(client.Client.RemoteEndPoint?.ToString() ?? "?");
        var stream = client.GetStream();
        var writer = Task.Run(async () =>
        {
            try { await foreach (var frame in peer.Outbox.Reader.ReadAllAsync(stopping)) await stream.WriteAsync(frame, stopping); }
            catch (Exception error) when (error is IOException or OperationCanceledException or ObjectDisposedException) { }
        }, stopping);
        try
        {
            var header = new byte[4];
            while (!stopping.IsCancellationRequested)
            {
                await stream.ReadExactlyAsync(header, stopping);
                var length = BinaryPrimitives.ReadUInt32LittleEndian(header);
                if (length < 2 || length > RelayProtocol.MaxFrame) break;
                var frame = new byte[length];
                await stream.ReadExactlyAsync(frame, stopping);
                hub.Handle(peer, frame);
            }
        }
        catch (Exception error) when (error is IOException or EndOfStreamException or OperationCanceledException or ObjectDisposedException) { }
        catch (Exception error) { logger.LogError(error, "Relay client {Peer} failed", peer.Label); }
        finally
        {
            hub.Disconnected(peer);
            peer.Outbox.Writer.TryComplete();
            await writer;
            client.Dispose();
            logger.LogInformation("Relay client {Peer} disconnected", peer.Label);
        }
    }

    private sealed class Peer(string label) : IRelayPeer
    {
        public string Label { get; } = label;
        public Channel<byte[]> Outbox { get; } = Channel.CreateUnbounded<byte[]>(new() { SingleReader = true });
        public void Post(byte[] frame) => Outbox.Writer.TryWrite(frame);
    }
}
