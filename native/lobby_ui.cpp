// The Heroes Post entry in HotA's multiplayer screen and the lobby window it opens.
// Dialogs are built from the engine's own controls through H3API (MIT, RoseKavalier):
// HotA's executable keeps the SoD 3.2 dialog code these addresses come from.
// All art is drawn at runtime from the player's own HotA and HD+ files.
#undef UNICODE
#undef _UNICODE
#pragma warning(push, 0)
#include "vendor/H3API.hpp"
#pragma warning(pop)
#include "lobby_ui.hpp"
#include "lobby_client.hpp"
#include "dplay_relay.hpp"
#include <algorithm>
#include <atomic>
#include <deque>
#include <string>
#include <thread>
#include <vector>

using namespace h3;

namespace hp_lobby {
namespace {
// SoD's multiplayer dialog (Hot Seat / IPX / TCP/IP / Modem / Direct, ids 102-106, Cancel 124).
constexpr DWORD MultiplayerVtable = 0x6400B0;
constexpr DWORD ProcSlot = MultiplayerVtable + 0x0C;
constexpr UINT16 AddressLabelId = 126, TcpIpId = 104, HostId = 107, JoinId = 108, FirstSessionRowId = 110, BackId = 124, StatusBarId = 123, NameEditId = 125;
// Free slot in the button column, between Direct (y 306..354) and Cancel (y 424).
constexpr UINT16 ButtonId = 130;
constexpr INT32 ButtonX = 373, ButtonY = 365, ButtonW = 64, ButtonH = 48;
// Engine message kinds (H3Msg::command) and subtypes. HotA/HD report the raw mouse buttons as
// 0x30A with the Windows message in the subtype, in game coordinates, before items see them.
constexpr int MouseMove = 0x4, RawMouse = 0x30A, ItemCommand = 0x200, ItemClick = 0xD;
bool IsLeftDown(const H3Msg& m) { return m.command == RawMouse && m.subtype == WM_LBUTTONDOWN; }
bool IsLeftUp(const H3Msg& m) { return m.command == RawMouse && m.subtype == WM_LBUTTONUP; }

using Proc = INT(__thiscall*)(H3BaseDlg*, H3Msg*);
Proc previous = nullptr;
std::atomic<bool> clickRequested{false};

HWND GameWindow() { return *reinterpret_cast<HWND*>(0x699650); }
// A mouse move at the cursor: the engine turns it into a message for the top dialog.
void Wake(DWORD delay = 0) {
    std::thread([delay] {
        if (delay) Sleep(delay);
        const HWND window = GameWindow();
        POINT p{}; GetCursorPos(&p); ScreenToClient(window, &p);
        PostMessageW(window, WM_MOUSEMOVE, 0, MAKELPARAM(p.x, p.y));
    }).detach();
}

// ----------------------------------------------------------------------- art
// The multiplayer button: the frame of HotA's connection buttons, the plain gold of the
// Cancel button around its icon, and the spell scroll artifact on top.
void PaintPostButton(H3LoadedPcx16* pcx, int frame) {
    H3DefLoader border("muBhot.def"), gold("muBcanc.def"), icons("artifact.def");
    if (!border.Get() || !gold.Get() || !icons.Get()) { pcx->FillRectangle(0, 0, ButtonW, ButtonH, 120, 96, 40); return; }
    border->DrawToPcx16(0, frame, 0, 0, ButtonW, ButtonH, pcx, 0, 0);
    // Cancel's gold is free of its icon in columns 3..15 and 48..60, rows 3..28.
    for (int y = 3; y < 45; y += 26)
        for (int x = 3; x < 61;) {
            const int w = x >= 48 ? 61 - x : std::min(13, 48 - x), h = std::min(26, 45 - y);
            gold->DrawToPcx16(0, frame, x >= 48 ? x : 3, 3, w, h, pcx, x, y);
            x += w;
        }
    const int shift = frame == 1 ? 1 : 0;
    icons->DrawToPcx16(0, 1, 0, 0, 44, 43, pcx, 10 + shift, 2 + shift); // artifact 1: the spell scroll
}

// Art cut to any size without a seam inside: the source's own top-left part as one
// piece, then its far edges (the last `edge` columns and rows) for the border.
// Transparent pixels (rounded corners) keep what the canvas already holds.
void PaintCut(H3LoadedPcx* src, H3LoadedPcx16* pcx, int w, int h, int edge) {
    const int sw = src->width, sh = src->height, iw = std::min(w - edge, sw - edge), ih = std::min(h - edge, sh - edge);
    for (int y = 0; y < h - edge; y += ih)
        for (int x = 0; x < w - edge; x += iw)
            src->DrawToPcx16(0, 0, std::min(iw, w - edge - x), std::min(ih, h - edge - y), pcx, x, y, TRUE);
    for (int x = 0; x < w - edge; x += iw) src->DrawToPcx16(0, sh - edge, std::min(iw, w - edge - x), edge, pcx, x, h - edge, TRUE);
    for (int y = 0; y < h - edge; y += ih) src->DrawToPcx16(sw - edge, 0, edge, std::min(ih, h - edge - y), pcx, w - edge, y, TRUE);
    src->DrawToPcx16(sw - edge, sh - edge, edge, edge, pcx, w - edge, h - edge, TRUE);
}
// HD+'s gold bar (the online lobby's button).
void PaintGold(H3LoadedPcx16* pcx, int w, int h) {
    H3PcxLoader bar("GldBtn2.pcx");
    if (!bar.Get()) { pcx->FillRectangle(0, 0, w, h, 176, 144, 64); pcx->DrawFrame(0, 0, w, h); return; }
    PaintCut(bar.Get(), pcx, w, h, 4);
}
// HD+'s sunken blue panel.
void PaintPanel(H3LoadedPcx16* pcx, int w, int h) {
    H3PcxLoader panel("Bl3DCcav.pcx");
    if (!panel.Get()) { pcx->FillRectangle(0, 0, w, h, 16, 32, 72); pcx->SinkArea(0, 0, w, h); return; }
    PaintCut(panel.Get(), pcx, w, h, 3);
}
// HD+'s gold-ornamented blue border: corners lu, ru, ld, rd, then edges l, r, u, d, each 42x42.
void PaintBlueFrame(H3LoadedPcx16* pcx, int w, int h) {
    H3DefLoader box("DlgBluBo.def");
    if (!box.Get()) { pcx->DrawThickFrame(0, 0, w, h, 3, 232, 212, 120); return; }
    constexpr int T = 42;
    for (int x = T; x < w - T; x += T) {
        const int n = std::min(T, w - T - x);
        box->DrawToPcx16(0, 6, 0, 0, n, T, pcx, x, 0);
        box->DrawToPcx16(0, 7, 0, 0, n, T, pcx, x, h - T);
    }
    for (int y = T; y < h - T; y += T) {
        const int n = std::min(T, h - T - y);
        box->DrawToPcx16(0, 4, 0, 0, T, n, pcx, 0, y);
        box->DrawToPcx16(0, 5, 0, 0, T, n, pcx, w - T, y);
    }
    box->DrawToPcx16(0, 0, 0, 0, T, T, pcx, 0, 0);
    box->DrawToPcx16(0, 1, 0, 0, T, T, pcx, w - T, 0);
    box->DrawToPcx16(0, 2, 0, 0, T, T, pcx, 0, h - T);
    box->DrawToPcx16(0, 3, 0, 0, T, T, pcx, w - T, h - T);
}
void PaintIcon(H3LoadedPcx16* pcx, LPCSTR name, int x, int y) {
    H3PcxLoader icon(name);
    if (icon.Get()) icon->DrawToPcx16(0, 0, 16, 16, pcx, x, y, TRUE);
}

// A picture item that owns its own canvas: the engine frees it with the dialog.
H3DlgPcx16* Canvas(H3BaseDlg* dialog, int x, int y, int w, int h, UINT16 id) {
    auto* item = H3DlgPcx16::Create(x, y, w, h, id, nullptr);
    auto* pcx = H3LoadedPcx16::Create(w, h);
    if (!item || !pcx) return nullptr;
    item->SetPcx(pcx);
    dialog->AddItem(item);
    return item;
}
void Show(H3DlgItem* item) { item->Draw(); item->Refresh(); }
// The dialog background under a canvas, so art with transparent pixels sits on it.
void TakeBackground(H3DlgPcx16* item, H3LoadedPcx16* back) {
    if (item && back) back->DrawToPcx16(0, 0, FALSE, item->GetPcx(), item->GetX(), item->GetY());
}
// Text on a canvas that first takes back the dialog background under it, so new text never overlaps old.
void PaintLabel(H3DlgPcx16* item, H3LoadedPcx16* back, const std::string& text, H3Font* font, eTextColor color, eTextAlignment align) {
    if (!item) return;
    auto* pcx = item->GetPcx();
    TakeBackground(item, back);
    pcx->TextDraw(font, text.c_str(), 0, 0, item->GetWidth(), item->GetHeight(), color, align);
}
std::atomic<int> devPress{-1};

std::string NoBraces(std::string s) {
    // Braces switch HotA's text colour; players' text must not.
    std::replace(s.begin(), s.end(), '{', '(');
    std::replace(s.begin(), s.end(), '}', ')');
    return s;
}
std::string Duration(int seconds) {
    if (seconds <= 0) return "-";
    if (seconds % 86400 == 0) return std::to_string(seconds / 86400) + (seconds == 86400 ? " day" : " days");
    if (seconds % 3600 == 0) return std::to_string(seconds / 3600) + " h";
    if (seconds % 60 == 0) return std::to_string(seconds / 60) + " min";
    return std::to_string(seconds) + " s";
}

// ------------------------------------------------------------- lobby window
enum class Action { None, Create, Join };
struct Choice { Action action = Action::None; std::string game; };

class LobbyDlg : public H3Dlg {
public:
    static constexpr int W = 640, H = 480;
    explicit LobbyDlg(const std::string& player) : H3Dlg(W, H, -1, -1, FALSE, FALSE) {
        // H3API's blue frame expects 64-pixel tiles with status-bar pieces; HD's has neither.
        AddBackground(0, 0, W, H, FALSE, FALSE, 0, TRUE);
        if (auto* back = GetBackgroundPcx()) {
            PaintBlueFrame(back, W, H);
            // A dark hint strip like the multiplayer screen's, sunk into the background.
            back->FillRectangle(HintX, HintY, W - 2 * HintX, 20, 0, 0, 0);
            back->DrawFrame(HintX - 1, HintY - 1, W - 2 * HintX + 2, 22, 148, 116, 56);
        }
        CreateText(0, 14, W, 32, "Heroes Post", NH3Dlg::Text::BIG, eTextColor::GOLD, 0);
        status = Canvas(this, 20, 46, W - 40, 18, 0);
        hint = Canvas(this, HintX, HintY, W - 2 * HintX, 20, 0);
        CreateText(Left, 70, ListW, 18, "Your games", NH3Dlg::Text::MEDIUM, eTextColor::GOLD, 0, eTextAlignment::MIDDLE_LEFT);
        CreateText(Left, 206, ListW, 18, "Games on this server", NH3Dlg::Text::MEDIUM, eTextColor::GOLD, 0, eTextAlignment::MIDDLE_LEFT);
        CreateText(ChatX, 70, ChatW, 18, "Chat", NH3Dlg::Text::MEDIUM, eTextColor::GOLD, 0, eTextAlignment::MIDDLE_LEFT);
        mineList = Canvas(this, Left, MineY, ListW, MineRows * RowH + 8, 0);
        gameList = Canvas(this, Left, GamesY, ListW, GameRows * RowH + 8, 0);
        chatBox = Canvas(this, ChatX, MineY, ChatW, ChatH, 0);
        auto* field = Canvas(this, ChatX, EditY, ChatW, 22, 0);
        if (field) { TakeBackground(field, GetBackgroundPcx()); PaintPanel(field->GetPcx(), ChatW, 22); }
        edit = CreateEdit(ChatX + 6, EditY + 2, ChatW - 12, 18, 120, "", NH3Dlg::Text::SMALL, eTextColor::REGULAR,
            eTextAlignment::MIDDLE_LEFT, "PassEdBk.pcx", EditId, FALSE, 0, 0);
        // Created this way the field takes keys but would not repaint them.
        if (edit) edit->SetAutoredraw(TRUE);
        const char* labels[] = {"Create", "Join", "Continue", "Turn limit", "Close"};
        for (int i = 0; i < ButtonCount; ++i) {
            buttons[i].label = labels[i];
            buttons[i].item = Canvas(this, Left + i * (ButtonW2 + 10), ButtonsY, ButtonW2, 32, 0);
        }
        snapshot = hp_lobby_client::Get();
        hp_lobby_client::Open(player);
        Refresh(true);
    }
    ~LobbyDlg() { hp_lobby_client::Close(); }
    Choice choice;

protected:
    BOOL OnCreate() override { if (edit) edit->SetFocus(TRUE); return TRUE; }
    BOOL OnMouseHover(INT32 x, INT32 y) override {
        Refresh(false);
        const int dev = devPress.exchange(-1);
        if (dev >= 0) {
            if (dev == Join && SelectedGame() == nullptr && !snapshot.games.empty()) { game = 0; UpdateButtons(); }
            if (buttons[dev].enabled && Press(dev)) { Stop(); return FALSE; }
        }
        ShowHint(Hint(x - GetX(), y - GetY()));
        return TRUE;
    }
    BOOL OnKeyPress(eVKey key, eMsgFlag) override {
        if (key == NH3VKey::H3VK_ENTER) { Send(); return TRUE; }
        if (key == NH3VKey::H3VK_ESCAPE) { Stop(); return FALSE; }
        return TRUE;
    }
    BOOL DialogProc(H3Msg& msg) override {
        const int x = msg.GetX() - GetX(), y = msg.GetY() - GetY();
        if (IsLeftDown(msg)) {
            pressed = ButtonAt(x, y);
            if (pressed >= 0 && buttons[pressed].enabled) { PaintButton(pressed, true); return TRUE; }
            pressed = -1;
            if (edit && x >= ChatX && x < ChatX + ChatW && y >= EditY && y < EditY + 22) edit->SetFocus(TRUE);
            Select(x, y);
        } else if (IsLeftUp(msg) && pressed >= 0) {
            const int which = pressed; pressed = -1;
            PaintButton(which, false);
            if (ButtonAt(x, y) == which && Press(which)) { Stop(); return FALSE; }
        }
        return TRUE;
    }

private:
    static constexpr int Left = 22, ListW = 370, RowH = 22, MineY = 90, MineRows = 4, GamesY = 226, GameRows = 6;
    static constexpr int ChatX = 410, ChatW = 208, ChatH = 252, EditY = MineY + ChatH + 6, EditId = 40;
    static constexpr int HintX = 12, HintY = H - 33, ButtonCount = 5, ButtonW2 = 110, ButtonsY = HintY - 10 - 32;
    enum { Create, Join, Continue, TurnLimit, Close };
    struct GoldButton { H3DlgPcx16* item = nullptr; std::string label; bool enabled = true; };

    hp_lobby_client::Snapshot snapshot;
    unsigned shown = ~0u;
    H3DlgPcx16 *status = nullptr, *hint = nullptr;
    std::string hintShown;
    H3DlgPcx16 *mineList = nullptr, *gameList = nullptr, *chatBox = nullptr;
    H3DlgEdit* edit = nullptr;
    GoldButton buttons[ButtonCount];
    int pressed = -1, mine = -1, game = -1, lastRow = -1;
    DWORD lastClick = 0;

    const hp_lobby_client::MyGame* SelectedMine() const { return mine >= 0 && mine < static_cast<int>(snapshot.mine.size()) ? &snapshot.mine[mine] : nullptr; }
    const hp_lobby_client::OpenGame* SelectedGame() const { return game >= 0 && game < static_cast<int>(snapshot.games.size()) ? &snapshot.games[game] : nullptr; }

    void Refresh(bool force) {
        auto fresh = hp_lobby_client::Get();
        if (!force && fresh.version == shown) return;
        // Keep the selection on the same game when the lists change order.
        const std::string keepMine = SelectedMine() ? SelectedMine()->id : "", keepGame = SelectedGame() ? SelectedGame()->id : "";
        snapshot = std::move(fresh); shown = snapshot.version;
        mine = game = -1;
        for (size_t i = 0; i < snapshot.mine.size(); ++i) if (snapshot.mine[i].id == keepMine) mine = static_cast<int>(i);
        for (size_t i = 0; i < snapshot.games.size(); ++i) if (snapshot.games[i].id == keepGame) game = static_cast<int>(i);
        PaintLabel(status, GetBackgroundPcx(), snapshot.status, H3SmallFont::Get(), eTextColor::REGULAR, eTextAlignment::MIDDLE_CENTER);
        if (!force && status) Show(status);
        PaintMine(); PaintGames(); PaintChat(); UpdateButtons();
        if (!force) { Show(mineList); Show(gameList); Show(chatBox); for (auto& b : buttons) if (b.item) Show(b.item); }
    }

    void PaintRowBackground(H3LoadedPcx16* pcx, int row, bool selected) {
        if (selected) { pcx->LightenArea(4, 4 + row * RowH, ListW - 8, RowH, 40); pcx->DrawFrame(4, 4 + row * RowH, ListW - 8, RowH); }
    }
    void Empty(H3LoadedPcx16* pcx, int h, LPCSTR text) {
        pcx->TextDraw(H3SmallFont::Get(), text, 0, 0, ListW, h, eTextColor::GRAY, eTextAlignment::MIDDLE_CENTER);
    }
    void PaintMine() {
        if (!mineList) return;
        auto* pcx = mineList->GetPcx(); const int h = MineRows * RowH + 8;
        TakeBackground(mineList, GetBackgroundPcx());
        PaintPanel(pcx, ListW, h);
        if (!snapshot.online) return;
        if (snapshot.mine.empty()) { Empty(pcx, h, "You have no games on this server yet."); return; }
        for (int i = 0; i < MineRows && i < static_cast<int>(snapshot.mine.size()); ++i) {
            const auto& g = snapshot.mine[i]; const int y = 4 + i * RowH;
            PaintRowBackground(pcx, i, i == mine);
            const char* icon = g.state == "Playing" ? "playin16.pcx" : g.state == "Away" ? "saved16.pcx" : g.state == "StandIn" ? "frend16.pcx" : "new16.pcx";
            PaintIcon(pcx, icon, 10, y + 3);
            pcx->TextDraw(H3SmallFont::Get(), NoBraces(g.name).c_str(), 32, y, 170, RowH, i == mine ? eTextColor::GOLD : eTextColor::REGULAR, eTextAlignment::MIDDLE_LEFT);
            std::string what = g.state == "Lobby" ? "setting up" : g.state == "Playing" ? "you are in" : g.state == "Away" ? "away, seat kept" : "server plays your turn";
            if (!g.date.empty()) what = "day " + g.date + ", " + what;
            pcx->TextDraw(H3SmallFont::Get(), what.c_str(), 200, y, ListW - 212, RowH, eTextColor::GRAY, eTextAlignment::MIDDLE_RIGHT);
        }
    }
    void PaintGames() {
        if (!gameList) return;
        auto* pcx = gameList->GetPcx(); const int h = GameRows * RowH + 8;
        TakeBackground(gameList, GetBackgroundPcx());
        PaintPanel(pcx, ListW, h);
        if (!snapshot.online) return;
        if (snapshot.games.empty()) { Empty(pcx, h, "No games yet: press Create to host one."); return; }
        for (int i = 0; i < GameRows && i < static_cast<int>(snapshot.games.size()); ++i) {
            const auto& g = snapshot.games[i]; const int y = 4 + i * RowH;
            PaintRowBackground(pcx, i, i == game);
            PaintIcon(pcx, g.started ? "playin16.pcx" : g.password ? "lock16.pcx" : "person16.pcx", 10, y + 3);
            const auto color = g.started ? eTextColor::GRAY : i == game ? eTextColor::GOLD : eTextColor::REGULAR;
            pcx->TextDraw(H3SmallFont::Get(), NoBraces(g.name).c_str(), 32, y, 150, RowH, color, eTextAlignment::MIDDLE_LEFT);
            pcx->TextDraw(H3SmallFont::Get(), NoBraces(g.host).c_str(), 186, y, 100, RowH, eTextColor::GRAY, eTextAlignment::MIDDLE_LEFT);
            const std::string seats = g.started ? "playing" : std::to_string(g.players) + (g.max_players ? "/" + std::to_string(g.max_players) : "");
            pcx->TextDraw(H3SmallFont::Get(), seats.c_str(), 290, y, ListW - 302, RowH, eTextColor::GRAY, eTextAlignment::MIDDLE_RIGHT);
        }
    }
    void PaintChat() {
        if (!chatBox) return;
        auto* pcx = chatBox->GetPcx();
        TakeBackground(chatBox, GetBackgroundPcx());
        PaintPanel(pcx, ChatW, ChatH);
        auto* font = H3SmallFont::Get();
        const int width = ChatW - 16, line = font->height + 1;
        int bottom = ChatH - 6;
        // Newest at the bottom; older lines scroll off the top.
        for (auto it = snapshot.chat.rbegin(); it != snapshot.chat.rend(); ++it) {
            const std::string text = "{" + NoBraces(it->name) + "}: " + NoBraces(it->text);
            const int h = std::max(1, font->GetLinesCountInText(text.c_str(), width)) * line;
            if (bottom - h < 6) break;
            bottom -= h;
            pcx->TextDraw(font, text.c_str(), 8, bottom, width, h, eTextColor::REGULAR, eTextAlignment::TOP_LEFT);
        }
        if (snapshot.chat.empty() && snapshot.online)
            pcx->TextDraw(font, "Say hello: type below and press Enter.", 8, 0, width, ChatH, eTextColor::GRAY, eTextAlignment::MIDDLE_CENTER);
    }
    void UpdateButtons() {
        const auto* m = SelectedMine(); const auto* g = SelectedGame();
        buttons[Create].enabled = snapshot.online;
        buttons[Join].enabled = snapshot.online && g && !g->started;
        buttons[Continue].enabled = false; // resuming an away seat from here comes next
        buttons[TurnLimit].enabled = snapshot.online && m;
        buttons[TurnLimit].label = m ? "Turn: " + Duration(m->turn_seconds) : "Turn limit";
        for (int i = 0; i < ButtonCount; ++i) PaintButton(i, false, false);
    }
    void PaintButton(int i, bool down, bool show = true) {
        auto& b = buttons[i]; if (!b.item) return;
        auto* pcx = b.item->GetPcx();
        TakeBackground(b.item, GetBackgroundPcx());
        PaintGold(pcx, ButtonW2, 32);
        if (down) pcx->DarkenArea(0, 0, ButtonW2, 32, 50);
        if (!b.enabled) { pcx->GrayScaleArea(0, 0, ButtonW2, 32); pcx->DarkenArea(0, 0, ButtonW2, 32, 40); }
        const int shift = down ? 1 : 0;
        pcx->TextDraw(H3MediumFont::Get(), b.label.c_str(), shift, shift, ButtonW2, 32, b.enabled ? eTextColor::BLACK : eTextColor::GRAY, eTextAlignment::MIDDLE_CENTER);
        if (show) Show(b.item);
    }
    int ButtonAt(int x, int y) const {
        for (int i = 0; i < ButtonCount; ++i) {
            const int bx = Left + i * (ButtonW2 + 10);
            if (x >= bx && x < bx + ButtonW2 && y >= ButtonsY && y < ButtonsY + 32) return i;
        }
        return -1;
    }
    // Row under the point: list 0 (yours) or 1 (server), or -1.
    int RowAt(int x, int y, int& list) const {
        if (x < Left + 4 || x >= Left + ListW - 4) return -1;
        if (y >= MineY + 4 && y < MineY + 4 + MineRows * RowH) { list = 0; return (y - MineY - 4) / RowH; }
        if (y >= GamesY + 4 && y < GamesY + 4 + GameRows * RowH) { list = 1; return (y - GamesY - 4) / RowH; }
        return -1;
    }
    void Select(int x, int y) {
        int list = -1; const int row = RowAt(x, y, list);
        if (row < 0) return;
        const bool twice = GetTickCount() - lastClick < 400 && lastRow == list * 100 + row;
        lastClick = GetTickCount(); lastRow = list * 100 + row;
        if (list == 0 && row < static_cast<int>(snapshot.mine.size())) mine = row;
        if (list == 1 && row < static_cast<int>(snapshot.games.size())) {
            game = row;
            if (twice && buttons[Join].enabled && Press(Join)) { Stop(); return; }
        }
        PaintMine(); PaintGames(); UpdateButtons();
        Show(mineList); Show(gameList);
        for (auto& b : buttons) if (b.item) Show(b.item);
    }
    // True when the window should close.
    bool Press(int which) {
        switch (which) {
        case Create: choice = {Action::Create, ""}; return true;
        case Join: if (const auto* g = SelectedGame()) { choice = {Action::Join, g->id}; return true; } return false;
        case TurnLimit:
            if (const auto* m = SelectedMine()) {
                static const int presets[] = {120, 300, 600, 1800, 3600, 4 * 3600, 86400};
                int next = presets[0];
                for (const int p : presets) if (p > m->turn_seconds) { next = p; break; }
                hp_lobby_client::SetTurnLimit(m->id, next);
                snapshot.mine[mine].turn_seconds = next;
                UpdateButtons();
            }
            return false;
        case Close: return true;
        default: return false;
        }
    }
    void Send() {
        if (!edit) return;
        const std::string text = edit->GetText();
        if (text.find_first_not_of(' ') == std::string::npos) return;
        hp_lobby_client::SendChat(text);
        edit->SetText("");
        Show(edit);
    }
    void ShowHint(const std::string& text) {
        if (!hint || text == hintShown) return;
        hintShown = text;
        PaintLabel(hint, GetBackgroundPcx(), text, H3SmallFont::Get(), eTextColor::REGULAR, eTextAlignment::MIDDLE_CENTER);
        Show(hint);
    }
    std::string Hint(int x, int y) const {
        switch (ButtonAt(x, y)) {
        case Create: return "Host a new game on the server: HotA's own setup, with the server carrying it.";
        case Join: return SelectedGame() ? "Join the selected game." : "Select a game below to join it.";
        case Continue: return "Coming next: return to a game you left. Until then the server keeps playing your seat.";
        case TurnLimit: return SelectedMine() ? "How long the server waits for a player who left before ending their turn. Click to change."
                                             : "Select one of your games to change its turn limit.";
        case Close: return "Back to the multiplayer menu.";
        default: break;
        }
        int list = -1; const int row = RowAt(x, y, list);
        if (list == 1 && row >= 0 && row < static_cast<int>(snapshot.games.size()))
            return snapshot.games[row].started ? "This game is under way." : "Double-click to join.";
        return "Heroes Post: simultaneous turns, chat, and leave or return at any time.";
    }
};

// ----------------------------------------------------- multiplayer screen
// Clicks the lobby performs in the multiplayer dialog after it closes, one per engine message.
struct Step { enum Kind { Click, WaitForSessions } kind; UINT16 id; };
std::deque<Step> steps;
ULONGLONG waitDeadline = 0;
bool relabel = false; // after the lobby's TCP/IP click: HotA's "Your IP Address" means nothing for a relay game
bool buttonDown = false;

int SessionCount(H3BaseDlg* dialog) {
    const auto* list = *reinterpret_cast<int* const*>(reinterpret_cast<const char*>(dialog) + 0x60);
    return list ? list[4] : 0;
}
bool ConnectionChosen(H3BaseDlg* dialog) { return *(reinterpret_cast<const char*>(dialog) + 0x54) != 0; }

bool Activate(const std::string& only) {
    const auto s = hp_lobby_client::Get();
    if (!s.online || s.relay_host.empty() || !s.relay_port) return false;
    hp_relay::Config config;
    config.host = s.relay_host; config.port = static_cast<std::uint16_t>(s.relay_port);
    config.ticket = s.ticket; config.name = s.player; config.only_instance = only;
    hp_relay::Activate(config);
    return true;
}

void Begin(H3BaseDlg* dialog, const Choice& choice) {
    if (choice.action == Action::None) return;
    if (!Activate(choice.action == Action::Join ? choice.game : "")) { H3Messagebox("Heroes Post is not connected to its server."); return; }
    steps.clear();
    if (ConnectionChosen(dialog)) steps.push_back({Step::Click, BackId});
    steps.push_back({Step::Click, TcpIpId});
    if (choice.action == Action::Create) steps.push_back({Step::Click, HostId});
    else {
        steps.push_back({Step::WaitForSessions, 0});
        steps.push_back({Step::Click, FirstSessionRowId});
        steps.push_back({Step::Click, JoinId});
    }
    Wake(50);
}

// Turns this engine message into the next queued click; false leaves it alone.
bool RunStep(H3BaseDlg* dialog, H3Msg& msg) {
    if (steps.empty() || msg.command != MouseMove) return false;
    const Step step = steps.front();
    if (step.kind == Step::WaitForSessions) {
        if (!waitDeadline) waitDeadline = GetTickCount64() + 10000;
        if (SessionCount(dialog) > 0) { steps.pop_front(); waitDeadline = 0; Wake(50); }
        else if (GetTickCount64() > waitDeadline) {
            steps.clear(); waitDeadline = 0;
            H3Messagebox("That game is no longer open on the server.");
        } else Wake(250);
        return false;
    }
    steps.pop_front();
    relabel = step.id == TcpIpId;
    msg.command = static_cast<eMsgCommand>(ItemCommand);
    msg.subtype = static_cast<eMsgSubtype>(ItemClick);
    msg.itemId = step.id;
    msg.flags = static_cast<eMsgFlag>(0);
    if (!steps.empty()) Wake(100);
    return true;
}

void EnsureButton(H3BaseDlg* dialog) {
    if (dialog->GetH3DlgItem(ButtonId)) return;
    if (auto* item = Canvas(dialog, ButtonX, ButtonY, ButtonW, ButtonH, ButtonId)) { PaintPostButton(item->GetPcx(), 0); dialog->Redraw(); }
}
bool OnButton(H3BaseDlg* dialog, const H3Msg& msg) {
    const int x = msg.GetX() - dialog->GetX(), y = msg.GetY() - dialog->GetY();
    return x >= ButtonX && x < ButtonX + ButtonW && y >= ButtonY && y < ButtonY + ButtonH;
}
void PaintButtonState(H3BaseDlg* dialog, int frame) {
    if (auto* item = dialog->GetPcx16(ButtonId)) { PaintPostButton(item->GetPcx(), frame); Show(item); }
}

void OpenLobby(H3BaseDlg* dialog) {
    std::string player = "Player";
    if (auto* name = dialog->GetH3DlgItem(NameEditId)) {
        const auto& text = reinterpret_cast<H3DlgText*>(name)->GetH3String();
        if (text.Length()) player = text.String();
    }
    hp_relay::Deactivate();
    Choice choice;
    {
        LobbyDlg lobby(player);
        lobby.Start();
        choice = lobby.choice;
    }
    dialog->Redraw();
    Begin(dialog, choice);
}

// Replaces the dialog's message procedure; everything else goes to the previous one (HD/HotA's).
INT __fastcall MultiplayerProc(H3BaseDlg* dialog, void*, H3Msg* msg) {
    EnsureButton(dialog);
    if (!msg) return previous(dialog, msg);
    if (RunStep(dialog, *msg)) return previous(dialog, msg);
    if (clickRequested.exchange(false)) { OpenLobby(dialog); return 1; }
    if (IsLeftDown(*msg) && OnButton(dialog, *msg)) { buttonDown = true; PaintButtonState(dialog, 1); return 1; }
    if (IsLeftUp(*msg) && buttonDown) {
        buttonDown = false; PaintButtonState(dialog, 0);
        if (OnButton(dialog, *msg)) { OpenLobby(dialog); return 1; }
    }
    // The player chose a connection by hand: that is HotA's own DirectPlay again.
    if (msg->command == ItemCommand && msg->subtype == ItemClick && msg->itemId >= 102 && msg->itemId <= 106) {
        steps.clear(); hp_relay::Deactivate();
    }
    const INT result = previous(dialog, msg);
    if (relabel) {
        relabel = false;
        if (auto* label = dialog->GetText(AddressLabelId)) {
            label->SetText(("Heroes Post\n" + hp_lobby_client::Get().server).c_str());
            dialog->Redraw();
        }
    }
    if (msg->command == MouseMove && OnButton(dialog, *msg))
        if (auto* bar = dialog->GetTextPcx(StatusBarId)) {
            bar->SetText("Heroes Post: online games with simultaneous turns that you can leave and rejoin.");
            Show(bar);
        }
    return result;
}
}

void RequestClick() noexcept { clickRequested.store(true); Wake(); }
void RequestPress(int button) noexcept { devPress.store(button); Wake(); }

bool Install(std::string* error) {
    if (previous) return true;
    wchar_t path[MAX_PATH]{}; GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring folder(path); folder = folder.substr(0, folder.find_last_of(L"\\/"));
    hp_lobby_client::Start(folder, GameWindow());
    auto* slot = reinterpret_cast<Proc*>(ProcSlot);
    DWORD protection = 0;
    if (!VirtualProtect(slot, sizeof(Proc), PAGE_EXECUTE_READWRITE, &protection)) { if (error) *error = "lobby_slot_protect_failed"; return false; }
    previous = *slot;
    *slot = reinterpret_cast<Proc>(&MultiplayerProc);
    VirtualProtect(slot, sizeof(Proc), protection, &protection);
    FlushInstructionCache(GetCurrentProcess(), slot, sizeof(Proc));
    return true;
}
}
