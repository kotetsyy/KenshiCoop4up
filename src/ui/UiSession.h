#ifndef KENSHICOOP_UI_SESSION_H
#define KENSHICOOP_UI_SESSION_H
#include "CoopUiApi.h"
#include <string>
#include "../plugin/core/SteamId.h"

namespace coopui {
inline bool hasGuests(const CoopUiSnapshot& s) {
    if (!s.isHost) return false;
    if (s.playerCount > 1) return true;
    const int count = s.playerCount < 0 ? 0 : s.playerCount > 4 ? 4 : s.playerCount;
    for (int i = 0; i < count; ++i)
        if (!s.players[i].local && s.players[i].id != 0) return true;
    return false;
}

inline bool gameReady(const CoopUiSnapshot& s) {
    if (!(s.running || s.busy) || s.worldPhase != COOP_WORLD_READY) return false;
    if (!s.isHost) return s.phase == COOP_CONNECTED;
    if (s.phase != COOP_HOSTING) return false;
    const int count = s.playerCount < 0 ? 0 : s.playerCount > 4 ? 4 : s.playerCount;
    bool guest = false;
    for (int i = 0; i < count; ++i) {
        const CoopUiPlayer& p = s.players[i];
        if (p.id == 0 || p.local) continue;
        guest = true;
        if (!p.worldReadyKnown || !p.worldReady) return false;
    }
    return guest;
}

// Session generation invalidates confirmation even across unobserved restarts.
class StopConfirmation {
public:
    enum Decision { NONE, PROMPT, DISCONNECT };
    StopConfirmation() : seen_(false), active_(false), host_(false), guests_(false),
        phase_(COOP_OFFLINE), requested_(-1), actual_(-1), port_(0), self_(0), peer_(0), generation_(0),
        open_(false), stopping_(false) {}
    void observe(const CoopUiSnapshot& s) {
        const bool active = s.running || s.busy;
        const char* address = s.udpIp ? s.udpIp : "";
        const bool changed = seen_ && (active != active_ || (s.isHost != 0) != host_ ||
            s.sessionGeneration != generation_ || s.transportSel != requested_ || s.activeTransport != actual_ ||
            s.selfSteamId != self_ || (!s.isHost && s.peerSteamId != peer_) || s.udpPort != port_ ||
            address_ != address || (s.phase == COOP_STARTING && phase_ != COOP_STARTING));
        if (changed || !active) { open_ = false; stopping_ = false; }
        if (!s.isHost || s.phase != COOP_HOSTING) open_ = false;
        seen_ = true; active_ = active; host_ = s.isHost != 0; guests_ = hasGuests(s);
        phase_ = s.phase; requested_ = s.transportSel; actual_ = s.activeTransport;
        port_ = s.udpPort; self_ = s.selfSteamId; peer_ = s.peerSteamId;
        generation_ = s.sessionGeneration;
        if (address_ != address) address_ = address;
    }
    Decision request() {
        if (!active_ || open_ || stopping_) return NONE;
        if (host_ && guests_) {
            if (phase_ != COOP_HOSTING) return NONE;
            open_ = true;
            return PROMPT;
        }
        if (host_) stopping_ = true;
        return DISCONNECT;
    }
    bool confirm() {
        const bool valid = open_ && active_ && host_ && phase_ == COOP_HOSTING && !stopping_;
        open_ = false;
        if (valid) stopping_ = true;
        return valid;
    }
    void cancel() { open_ = false; }
    bool open() const { return open_; }
    bool stopping() const { return stopping_; }
private:
    bool seen_, active_, host_, guests_;
    int phase_, requested_, actual_, port_;
    unsigned long long self_, peer_, generation_;
    std::string address_;
    bool open_, stopping_;
};
}
#endif
