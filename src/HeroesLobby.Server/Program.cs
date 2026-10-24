using System.Threading.RateLimiting;
using HeroesLobby.Contracts;
using HeroesLobby.Server;
using Microsoft.AspNetCore.RateLimiting;
using Microsoft.AspNetCore.SignalR;

var builder = WebApplication.CreateBuilder(args);
builder.WebHost.ConfigureKestrel(options => options.Limits.MaxRequestBodySize = 16 * 1024);
builder.Services.AddSingleton<LobbyStore>();
builder.Services.AddSingleton<CheckpointStore>();
builder.Services.AddSignalR(options => options.MaximumReceiveMessageSize = 4096);
builder.Services.AddRateLimiter(options =>
{
    options.RejectionStatusCode = 429;
    options.AddPolicy("writes", context => RateLimitPartition.GetFixedWindowLimiter(
        context.Connection.RemoteIpAddress?.ToString() ?? "local",
        _ => new FixedWindowRateLimiterOptions { PermitLimit = 60, Window = TimeSpan.FromMinutes(1), QueueLimit = 0 }));
});
var app = builder.Build();
_ = app.Services.GetRequiredService<LobbyStore>();
app.Use(async (context, next) =>
{
    try { await next(context); }
    catch (LobbyError error)
    {
        await Results.Problem(statusCode: error.Status, title: "Lobby request failed", detail: error.Message).ExecuteAsync(context);
    }
});
app.UseRateLimiter();
app.MapGet("/health", () => Results.Ok(new { status = "ok" }));
app.MapGet("/api/info", (IConfiguration config) => new ServerInfo(config["Lobby:Name"] ?? "Heroes Post · Community server", Protocol.Version, false, true));
app.MapPost("/api/players", (RegisterPlayer request, LobbyStore store) => store.Register(request)).RequireRateLimiting("writes");
app.MapGet("/api/me", (HttpContext context, LobbyStore store) => Identity(context, store));
app.MapGet("/api/games", (LobbyStore store) => store.List());
app.MapGet("/api/games/{id}", (string id, LobbyStore store) => store.Get(id));

app.MapPost("/api/games", async (CreateGame request, HttpContext context, LobbyStore store, IHubContext<LobbyHub> hub) =>
    await Announce(await store.Create(Identity(context, store), request), hub)).RequireRateLimiting("writes");
app.MapPost("/api/games/{id}/join", async (string id, JoinGame request, HttpContext context, LobbyStore store, IHubContext<LobbyHub> hub) =>
    await Announce(await store.Join(id, Identity(context, store), request), hub)).RequireRateLimiting("writes");
app.MapPut("/api/games/{id}/settings", async (string id, PlayerSettings request, HttpContext context, LobbyStore store, IHubContext<LobbyHub> hub) =>
    await Announce(await store.Settings(id, Identity(context, store), request), hub)).RequireRateLimiting("writes");
app.MapPut("/api/games/{id}/ready", async (string id, ReadyRequest request, HttpContext context, LobbyStore store, IHubContext<LobbyHub> hub) =>
    await Announce(await store.Ready(id, Identity(context, store), request), hub)).RequireRateLimiting("writes");
app.MapPost("/api/games/{id}/lock", async (string id, RevisionRequest request, HttpContext context, LobbyStore store, IHubContext<LobbyHub> hub) =>
    await Announce(await store.Lock(id, Identity(context, store), request, true), hub)).RequireRateLimiting("writes");
app.MapPost("/api/games/{id}/reopen", async (string id, RevisionRequest request, HttpContext context, LobbyStore store, IHubContext<LobbyHub> hub) =>
    await Announce(await store.Lock(id, Identity(context, store), request, false), hub)).RequireRateLimiting("writes");
app.MapPost("/api/games/{id}/leave", async (string id, RevisionRequest request, HttpContext context, LobbyStore store, IHubContext<LobbyHub> hub) =>
    await Announce(await store.Leave(id, Identity(context, store), request), hub)).RequireRateLimiting("writes");
app.MapHub<LobbyHub>("/hubs/lobby");
app.MapGet("/api/games/{id}/native", (string id, HttpContext context, LobbyStore store) => Results.Json(store.GetNativeLaunch(id, Identity(context, store))));
app.MapPost("/api/games/{id}/native", async (string id, BeginNativeLaunch request, HttpContext context, LobbyStore store, IHubContext<LobbyHub> hub) =>
{
    var run = await store.BeginNative(id, Identity(context, store), request); await hub.Clients.All.SendAsync("Changed", id); return run;
}).RequireRateLimiting("writes");
app.MapPut("/api/games/{id}/native", async (string id, NativeLaunchReport request, HttpContext context, LobbyStore store, IHubContext<LobbyHub> hub) =>
{
    var run = await store.ReportNative(id, Identity(context, store), request); await hub.Clients.All.SendAsync("Changed", id); return run;
}).RequireRateLimiting("writes");
app.MapCheckpointEndpoints();
app.MapNativeBranchEndpoints();
app.Run();

static PlayerInfo Identity(HttpContext context, LobbyStore store) => store.Authenticate(context.Request.Headers.Authorization.ToString());
static async Task<GameInfo> Announce(GameInfo game, IHubContext<LobbyHub> hub)
{
    // Database commit precedes the hint. Clients always fetch authoritative state and also poll.
    await hub.Clients.All.SendAsync("Changed", game.Id);
    return game;
}

public sealed class LobbyHub : Hub;
public partial class Program;
