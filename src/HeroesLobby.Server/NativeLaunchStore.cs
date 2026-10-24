using System.Net;
using System.Net.Sockets;
using HeroesLobby.Contracts;

namespace HeroesLobby.Server;

public sealed partial class LobbyStore
{
    public NativeLaunch? GetNativeLaunch(string id, PlayerInfo player)
    {
        using var db = Open(); var stored = Read(db, null, id); RequireMember(stored.Game, player); return stored.Native;
    }

    public async Task<NativeLaunch> BeginNative(string id, PlayerInfo player, BeginNativeLaunch request)
    {
        await writeGate.WaitAsync();
        try
        {
            using var db = Open(); using var tx = db.BeginTransaction(); var stored = Read(db, tx, id); var game = stored.Game;
            if (game.HostId != player.Id) throw new LobbyError(403, "Only the host can start this game.");
            if (stored.Native is not null) return stored.Native; // Lost response/repeated clicks never create another attempt.
            RequireRevision(game, request.Revision);
            if (game.Status is not ("Lobby" or "Prepared") || game.Members.Count != 2 || game.Members.Any(m => !m.Ready))
                throw new LobbyError(409, "Two players must join and both must be ready before starting HotA.");
            if (game.Members.Any(m => !m.NativeReady)) throw new LobbyError(409, "Both players must Set up HotA in the updated app, then mark themselves ready again.");
            if (!IPAddress.TryParse(request.HostAddress, out var address) || address.AddressFamily != AddressFamily.InterNetwork)
                throw new LobbyError(400, "Enter the host PC's IPv4 address. Use 127.0.0.1 only when both players are on this PC.");
            var map = game.MapName.EndsWith(".h3m", StringComparison.OrdinalIgnoreCase) ? game.MapName : game.MapName + ".h3m";
            if (!map.Equals("Judgement Day.h3m", StringComparison.OrdinalIgnoreCase))
                throw new LobbyError(400, "This playable startup build supports Judgement Day. Create a two-player game with that map.");
            if (game.Members.Any(m => m.Faction != "Random" || m.Hero != "Random" || m.Bonus != "Random") ||
                game.Members.Single(m => m.PlayerId == game.HostId).Color != 0 || game.Members.Single(m => m.PlayerId != game.HostId).Color != 1)
                throw new LobbyError(400, "For this startup build use Red for the host, Blue for the guest, and Random town, hero and bonus. HotA applies the map defaults.");
            var attempt = Guid.NewGuid().ToString("N");
            var launch = new NativeLaunch(attempt, id, game.HostId, "HP" + attempt[..24], address.ToString(), "Judgement Day.h3m", 211,
                "Launching", game.Members.Select(m => new NativeLaunchPlayer(m.PlayerId, m.Color, "Pending", 0, null)).ToList());
            Save(db, tx, stored with { Game = game with { Status = "Launching", Revision = game.Revision + 1 }, Native = launch });
            tx.Commit(); return launch;
        }
        finally { writeGate.Release(); }
    }

    public async Task<NativeLaunch> ReportNative(string id, PlayerInfo player, NativeLaunchReport report)
    {
        await writeGate.WaitAsync();
        try
        {
            using var db = Open(); using var tx = db.BeginTransaction(); var stored = Read(db, tx, id); RequireMember(stored.Game, player);
            var run = stored.Native ?? throw new LobbyError(409, "The host has not started this game.");
            if (run.Id != report.AttemptId) throw new LobbyError(409, "This report belongs to a different launch attempt.");
            var own = run.Players.Single(p => p.PlayerId == player.Id);
            if (own.Phase == report.Phase && own.NativePlayerId == report.NativePlayerId) return run;
            if (run.Status == "Failed") throw new LobbyError(409, "This launch has failed. Review the reported error.");
            var host = player.Id == run.HostId;
            var allowed = report.Phase == "Failed" || (own.Phase, report.Phase, host) is
                ("Pending", "Hosted", true) or ("Hosted", "Started", true) or ("Pending", "Joined", false) or ("Joined", "Armed", false) or ("Armed", "Started", false);
            if (!allowed || own.Phase == "Started") throw new LobbyError(409, "Invalid native startup transition.");
            if (report.Phase != "Failed" && (report.NativePlayerId == 0 ||
                (own.NativePlayerId != 0 && own.NativePlayerId != report.NativePlayerId) ||
                run.Players.Any(p => p.PlayerId != player.Id && p.NativePlayerId == report.NativePlayerId)))
                throw new LobbyError(400, "Native player IDs must be nonzero, distinct, and stable within the launch.");
            if (report.Phase == "Joined" && run.Players.Single(p => p.PlayerId == run.HostId).Phase != "Hosted")
                throw new LobbyError(409, "The native host is not ready.");
            if (report.Phase == "Started" && host && run.Players.Single(p => p.PlayerId != player.Id).Phase is not ("Armed" or "Started"))
                throw new LobbyError(409, "The guest must accept the native startup request first.");
            var error = report.Phase == "Failed" ? Clean(report.Error, "Launch error", 800) : null;
            var players = run.Players.Select(p => p.PlayerId == player.Id ? p with { Phase = report.Phase, NativePlayerId = report.NativePlayerId, Error = error } : p).ToList();
            run = run with { Players = players, Status = report.Phase == "Failed" ? "Failed" : players.All(p => p.Phase == "Started") ? "Playing" : "Launching" };
            Save(db, tx, stored with { Native = run, Game = stored.Game with { Status = run.Status == "Failed" ? "LaunchFailed" : run.Status, Revision = stored.Game.Revision + 1 } });
            tx.Commit(); return run;
        }
        finally { writeGate.Release(); }
    }
}
