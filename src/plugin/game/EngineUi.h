// EngineUi.h - narrow PUBLIC engine surface: the in-game co-op session panel +
// status overlay. Carved out of Engine.h (Phase 5a domain split, 2026-07-19) so
// the UI root (Plugin.cpp) includes only what it needs and the sync/replication
// consumers stop transitively seeing the panel API.
//
// Like Engine.h this is a PUBLIC header: it declares only the SEH-guarded engine
// facade and must NEVER pull in a <kenshi/...> internal header - those live in
// the adapter (EngineInternal.h). Forward declarations only.

#ifndef KENSHICOOP_ENGINE_UI_H
#define KENSHICOOP_ENGINE_UI_H

namespace coop {
namespace engine {

// ---- In-game co-op session panel ---------------------------------------------
// A native DatapanelGUI opened with F2 that lets the player pick role + transport
// (toggle buttons) and Connect/Disconnect. Steam IDs are clipboard-paste
// (17 digits). The display nick and the UDP host IP:port are type-in
// DatapanelGUI rows (setLineTextEditable). Steam ID copy/paste rows are hidden
// on UDP. A paste / edit / Connect is remembered in coop_config.json. The GUI
// layer stays
// session-agnostic: live status is passed IN via *st and the user's actions
// are handed BACK through the callbacks (the plugin root owns the
// session/config wiring). Main-thread only; SEH-guarded.
struct CoopPanelState {
    unsigned long long selfSteamId; // steamp2p::selfId (0 = Steam not up)
    unsigned long long peerSteamId; // config steamPeer fallback (0 = unset; pasted id wins)
    unsigned long long peerSteamId2;// extra host friends from config (slot 1)
    unsigned long long peerSteamId3;// extra host friends from config (slot 2)
    bool               running;     // net thread up
    bool               peerPresent; // peer connected
    bool               isHost;      // current armed role (seeds the Host toggle)
    int                transportSel;// current armed transport (0 steam, 1 udp)
    const char*        udpIp;       // last remembered UDP host (join); may be empty
    int                udpPort;     // last remembered UDP port (0 = default)
    const char*        detail;      // one-line status string for the panel/overlay
    // Join-side save-transfer status (null when not streaming): byte-level
    // progress the one-line detail above has no room for, shown on the F2 panel
    // while a join receives the host's world (e.g. "Streaming host world... 42%
    // (3.1/7.4 MB)"). Set by coopPanelDrive, rendered in dbgVal.
    const char*        transferDetail;
    // Self-update status, and only when it is ACTIONABLE (an update installed and
    // waiting on a restart, or a check that failed) - null the rest of the time.
    // Takes the debug line ahead of everything else: a player whose DLL is stale
    // cannot connect at all, so "restart to finish updating" outranks any
    // connection detail that line would otherwise carry.
    const char*        updateDetail;
    // Build identity for the panel title, e.g. "v0.52 - proto 58". Null = omit.
    // Deliberately in the TITLE and not a row of its own: it answers "are we on
    // the same build?" - the first question when a connection refuses - without
    // spending one of the panel's few visible lines on something that never
    // changes during a session.
    const char*        versionText;
    // Last-remembered display nick (coop_config.json playerName). Empty/null =
    // none yet; the panel's Nick row is a type-in field seeded from this.
    const char*        playerName;
};
// The panel's role/transport selections at the moment Connect is hit. peerId is the
// Steam ID pasted in-panel this session (0 if none), and overrides the config
// steamPeer in coopUiConnect; the UDP endpoint is re-read from the config there.
typedef void (*CoopConnectFn)(bool isHost, bool useSteam, unsigned long long peerId);
typedef void (*CoopDisconnectFn)();
// Fired after a paste (Steam ID or UDP endpoint) so the plugin can write
// coop_config.json. isHost/useSteam are the panel's armed toggles at paste time.
typedef void (*CoopRememberFn)(bool isHost, bool useSteam);
void coopPanelTick(const CoopPanelState* st, CoopConnectFn onConnect,
                   CoopDisconnectFn onDisconnect, CoopRememberFn onRemember);

// Steam IDs pasted in the F2 panel this session (host may paste several).
int                coopPanelPastedCount();
unsigned long long coopPanelPastedId(int i);
// UDP endpoint pasted (or seeded from config) this session. Empty ip / 0 port
// means the join has not set one in-panel; Connect then uses coop_config.json.
const char*        coopPanelUdpIp();
int                coopPanelUdpPort();
// Display nick pasted (or seeded from config) this session. Empty = unset.
const char*        coopPanelPlayerName();

// Persistent co-op connection-status banner: a single screen-space label fixed 10
// px in from the top-left corner (a createFloatingLabel MyGUI::Window on the
// spike-48 screenshot-proven "Info" layer) whose caption shows the live session
// status, colored by state (0 = offline/red, 1 = waiting/yellow, 2 =
// connected/green). Needs no player character, so it also shows at the title
// screen; updated in place when the text/state changes and re-minted if the GUI
// destroyed the widget (world load). Pass show=false to remove it. Main-thread
// only; SEH-guarded.
void coopOverlayTick(const char* text, int state, bool show);

} // namespace engine
} // namespace coop

#endif // KENSHICOOP_ENGINE_UI_H
