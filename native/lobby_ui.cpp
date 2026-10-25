// The Heroes Post entry in HotA's multiplayer screen and the lobby window it opens.
// Dialogs are built from the engine's own controls through H3API (MIT, RoseKavalier):
// HotA's executable keeps the SoD 3.2 dialog code these addresses come from.
#undef UNICODE
#undef _UNICODE
#pragma warning(push, 0)
#include "vendor/H3API.hpp"
#pragma warning(pop)
#include "lobby_ui.hpp"
#include <atomic>

using namespace h3;

namespace hp_lobby {
namespace {
// SoD's multiplayer dialog (Hot Seat / IPX / TCP/IP / Modem / Direct, ids 102-106, Cancel 124).
constexpr DWORD MultiplayerVtable = 0x6400B0;
constexpr DWORD ProcSlot = MultiplayerVtable + 0x0C;
// Free slot in the button column, between Direct (y 306..354) and Cancel (y 424).
constexpr INT32 ButtonId = 130, ButtonX = 373, ButtonY = 365;

using Proc = INT(__thiscall*)(H3BaseDlg*, H3Msg*);
Proc previous = nullptr;
std::atomic<bool> clickRequested{false};

void EnsureButton(H3BaseDlg* dialog) {
    if (dialog->GetH3DlgItem(ButtonId)) return;
    auto* button = H3DlgCaptionButton::Create(ButtonX, ButtonY, 64, 48, ButtonId, "GSPBUT2.DEF", "Heroes\nPost",
        NH3Dlg::Text::SMALL, 0, 0, FALSE, 0, eTextColor::GOLD);
    if (!button) return;
    dialog->AddItem(button);
    dialog->Redraw();
}

void OpenLobby() {
    H3Dlg lobby(480, 360);
    lobby.CreateText(20, 20, 440, 40, "Heroes Post", NH3Dlg::Text::BIG, eTextColor::GOLD, 1);
    lobby.CreateText(20, 70, 440, 200, "Your games and the games on this server will appear here.",
        NH3Dlg::Text::MEDIUM, eTextColor::REGULAR, 2);
    lobby.CreateOKButton();
    lobby.Start();
}

// Replaces the dialog's message procedure; everything else goes to the previous one (HD/HotA's).
INT __fastcall MultiplayerProc(H3BaseDlg* dialog, void*, H3Msg* msg) {
    EnsureButton(dialog);
    if (msg && (msg->itemId == ButtonId && msg->IsLeftClick() || clickRequested.exchange(false))) { OpenLobby(); return 1; }
    return previous(dialog, msg);
}
}

void RequestClick() noexcept { clickRequested.store(true); }

bool Install(std::string* error) {
    if (previous) return true;
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
