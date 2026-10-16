// UiModule.h - core-side host for the optional KenshiCoopUI.dll provider.
//
// The F2 panel, connection banner and private diagnostic overlay live in
// KenshiCoopUI.dll beside KenshiCoop.dll. The provider is loaded on the first
// tick, initialized once and pinned until process exit (never unloaded or
// reloaded). A missing, ABI-incompatible or faulting provider is logged once and
// disables the UI only; networking and the game loop keep running.

#ifndef KENSHICOOP_UI_MODULE_H
#define KENSHICOOP_UI_MODULE_H

#include "../../ui/CoopUiApi.h"
#include <cstring>
#include "SteamId.h"

namespace coop {
namespace uimodule {
inline bool validSnapshot(const CoopUiSnapshot& s) {
    if (s.structSize != sizeof(s) || s.phase < COOP_OFFLINE || s.phase > COOP_FAILED ||
        s.worldPhase < COOP_WORLD_NONE || s.worldPhase > COOP_WORLD_FAILED ||
        s.transportSel < 0 || s.transportSel > 1 || s.activeTransport < -1 || s.activeTransport > 1 ||
        s.playerCount < 0 || s.playerCount > 4 ||
        s.inviteResult < -1 || s.inviteResult > 6 ||
        (s.isHost != 0 && s.isHost != 1) || (s.running != 0 && s.running != 1) ||
        (s.busy != 0 && s.busy != 1) || (s.peerPresent != 0 && s.peerPresent != 1)) return false;
    for (int i = 0; i < s.playerCount; ++i) {
        const CoopUiPlayer& p = s.players[i];
        if (!std::memchr(p.name, 0, sizeof(p.name)) || (p.local != 0 && p.local != 1) ||
            (p.worldReadyKnown != 0 && p.worldReadyKnown != 1) || (p.worldReady != 0 && p.worldReady != 1)) return false;
    }
    return true;
}
inline bool validCommand(const CoopUiCommand& c) {
    if (c.structSize != sizeof(c) || c.kind < COOP_UI_NONE || c.kind > COOP_UI_MAKE_UDP_INVITE ||
        !std::memchr(c.settings.udpIp, 0, sizeof(c.settings.udpIp)) ||
        !std::memchr(c.settings.playerName, 0, sizeof(c.settings.playerName)) ||
        !std::memchr(c.udpJoinInvite, 0, sizeof(c.udpJoinInvite))) return false;
    if (std::strstr(c.settings.udpIp, "KCU1") || std::strstr(c.settings.playerName, "KCU1")) return false;
    if (c.kind == COOP_UI_NONE) return !c.udpJoinInvite[0];
    if (c.kind == COOP_UI_MAKE_UDP_INVITE) return c.sessionGeneration && c.inviteRequestId && !c.udpJoinInvite[0];
    if (c.kind == COOP_UI_DISCONNECT) return !c.udpJoinInvite[0];
    const CoopUiSettings& s = c.settings;
    if ((s.isHost != 0 && s.isHost != 1) || (s.useSteam != 0 && s.useSteam != 1) ||
        s.udpPort < 1 || s.udpPort > 65535) return false;
    if (c.kind == COOP_UI_REMEMBER || s.isHost || s.useSteam) return !c.udpJoinInvite[0];
    return c.udpJoinInvite[0] != 0;
}

// Game main thread only. Hands *state to the provider and fills *command with
// the one action it queued (kind COOP_UI_NONE when there is none, or when the UI
// is unavailable). Strings in command->settings are NUL-terminated on return.
void tick(const CoopUiSnapshot* state, CoopUiCommand* command);

} // namespace uimodule
} // namespace coop

#endif // KENSHICOOP_UI_MODULE_H
