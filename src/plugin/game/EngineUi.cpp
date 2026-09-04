// EngineUi.cpp - the in-game co-op UI plane: the debug marker HUD labels, the
// F2 co-op session panel (native DatapanelGUI), and the persistent status
// overlay. Split out of EngineEntity.cpp (Phase 5e code motion, 2026-07-19) so
// the entity capture/resolve/apply TU stays focused on sync and the UI/render
// surface lives with its public header (EngineUi.h).
//
// Owner state: section-private statics / anon-namespace SEH shims only (marker
// colour + create/update/destroy shims, panel widget pointers, overlay label).
// Must NOT: define g_* engine pointers (EngineInternal.cpp owns them), install
// hooks, or change any log string - "[coop-ui] ..." phrasing is API consumed by
// the harness. The public marker* declarations stay in Engine.h (the Replicator
// uses them for KENSHICOOP_DEBUG_MARKERS); only their definitions moved here.

#include "EngineInternal.h"

// In-game co-op session panel: the native DatapanelGUI window + its interactive
// rows and Win32 key capture. EngineInternal.h already pulls Globals.h (::gui),
// ForgottenGUI.h and MyGUI_Button.h; these add the panel row types, the
// free-function delegate factory, and GetAsyncKeyState/VK_* for the F2 toggle +
// digit entry.
#include <kenshi/gui/DatapanelGUI.h>
#include <kenshi/gui/DataPanelLine.h>
#include <kenshi/OptionsHolder.h> // options->damageFloaters
#include <mygui/MyGUI_Delegate.h> // MyGUI::newDelegate + CDelegate* (free-fn callbacks)
#include <mygui/MyGUI_Align.h>
#include <mygui/MyGUI_EditBox.h>
#include <windows.h>

#include "../core/SteamId.h" // parseSteamId64 (paste button) + maskSteamId64 (id rows)
#include "../core/UdpEndpoint.h" // parseUdpEndpoint (join UDP paste)
#include "../core/PlayerNick.h" // parsePlayerNick (F2 nick paste)

namespace coop {
namespace engine {

// ---- Debug marker HUD labels (KENSHICOOP_DEBUG_MARKERS, spike-47 substrate) --
// ForgottenGUI::createScreenLabel + ScreenLabel::setTracking pin a colored text
// label to a character; the engine's own per-frame projection keeps it on the
// body (spike 47 render proof). The Replicator uses these to make join-side
// authority states self-explaining on screen: who is host-driven, who is
// hidden, who is a local-only ghost. C2712 split: the outer fns build the
// std::string/Colour/Vector3 (unwindable), POD-only inner fns hold the SEH.

namespace {

void markerColour(int colorId, MyGUI::Colour* col) {
    switch (colorId) {
    case 0:  *col = MyGUI::Colour(0.30f, 1.00f, 0.30f, 1.0f); break; // driven
    case 1:  *col = MyGUI::Colour(1.00f, 0.25f, 0.25f, 1.0f); break; // hidden
    case 2:  *col = MyGUI::Colour(1.00f, 0.90f, 0.25f, 1.0f); break; // local-only
    default: *col = MyGUI::Colour(0.80f, 0.80f, 0.80f, 1.0f); break;
    }
}

ScreenLabel* markerCreateSeh(ForgottenGUI* g, Character* c,
                             const std::string* text, const MyGUI::Colour* col,
                             const Ogre::Vector3* off) {
    __try {
        ScreenLabel* l = g->createScreenLabel(*text, *col, ScreenLabel::LS_SMALL,
                                              ScreenLabel::RS_STOPPED);
        if (l) {
            l->_NV_setRisingSpeed(ScreenLabel::RS_STOPPED);
            l->_NV_setTracking(c->handle, *off);
        }
        return l;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

bool markerUpdateSeh(ScreenLabel* l, const std::string* text,
                     const MyGUI::Colour* col) {
    __try {
        l->_NV_setCaption(*text);
        l->_NV_setColor(*col);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Is this label one the GUI still owns? ForgottenGUI keeps the authoritative
// registry of live ScreenLabels (plus deferred add/remove queues), and that
// registry - not our cached pointer - is the only ground truth available: the
// GUI destroys labels on its own schedule and notifies nobody, so a handle held
// across ticks dangles silently. Measured 2026-08-03: the join minted three
// proxies in one burst, debugMark found stale map entries sitting at RECYCLED
// Character* addresses, and setCaption on the previous occupants' dead labels
// faulted twice inside ScreenLabel::setCaption (kenshi+0x6e451e) moments before
// the process died. Same lesson as the world-item proxy hands - never
// dereference a cached engine pointer without re-asking the engine.
//
// Deliberately unlocked. guiScreenLabelsMutex guards these lektors, but taking
// an engine shared_mutex from inside a detour is its own class of hazard, and a
// torn read here is harmless BECAUSE we test for an exact pointer match: garbage
// answers "not present", which costs one discarded marker, whereas a false
// "alive" would need the torn word to equal the very pointer we are asking
// about. The safe direction is the likely one.
bool labelListHas(const lektor<ScreenLabelInterface*>* v, const void* l) {
    __try {
        ScreenLabelInterface* const* p = v->stuff;
        unsigned int n = v->count;
        if (!p || n > 8192u) return false;   // bound a torn/garbage count
        for (unsigned int i = 0; i < n; ++i)
            if ((const void*)p[i] == l) return true;
        return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool markerDestroySeh(ForgottenGUI* g, ScreenLabel* l) {
    __try {
        g->destroy(l);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

} // namespace

void* markerCreate(Character* c, const char* text, int colorId) {
    if (!c || !text) return 0;
    ForgottenGUI* g = ::gui; // KenshiLib data export (spike 46)
    if (!g) return 0;
    std::string t(text);
    MyGUI::Colour col;
    markerColour(colorId, &col);
    Ogre::Vector3 off(0.0f, 2.2f, 0.0f); // head height (spike 47)
    return markerCreateSeh(g, c, &t, &col, &off);
}

bool markerAlive(void* label) {
    if (!label) return false;
    ForgottenGUI* g = ::gui; // KenshiLib data export (spike 46)
    if (!g) return false;
    // Queued for removal counts as dead: the GUI has already decided, and we
    // would only be racing its next flush.
    if (labelListHas(&g->guiScreenLabelsToRemove, label)) return false;
    // A freshly created label sits in the add queue until the GUI flushes it, so
    // both lists are "alive" or markerCreate's own handle would look dead.
    return labelListHas(&g->guiScreenLabels, label) ||
           labelListHas(&g->guiScreenLabelsToAdd, label);
}

bool markerUpdate(void* label, const char* text, int colorId) {
    if (!label || !text || !markerAlive(label)) return false;
    std::string t(text);
    MyGUI::Colour col;
    markerColour(colorId, &col);
    return markerUpdateSeh((ScreenLabel*)label, &t, &col);
}

void markerDestroy(void* label) {
    if (!label) return;
    ForgottenGUI* g = ::gui; // KenshiLib data export (spike 46)
    if (!g) return;
    // Handing the GUI a label it has already destroyed is the same
    // use-after-free as updating one; prune paths reach here with handles whose
    // Character went away, which is exactly when the GUI has cleaned up too.
    if (!markerAlive(label)) return;
    markerDestroySeh(g, (ScreenLabel*)label);
}

namespace {
ScreenLabel* floaterCreateSeh(ForgottenGUI* g, Character* c,
                              const std::string* text, const MyGUI::Colour* col,
                              const Ogre::Vector3* off) {
    __try {
        ScreenLabel* l = g->createScreenLabel(*text, *col, ScreenLabel::LS_MEDIUM,
                                              ScreenLabel::RS_NORMAL);
        if (l) l->_NV_setTracking(c->handle, *off);
        return l;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
} // namespace

void spawnDamageFloater(Character* c, float amount) {
    const char* skip = 0;
    if (!c) skip = "no-char";
    else if (amount < 0.05f) skip = "amt-low";
    else if (::options && ::options->damageFloaters == 0) skip = "opt-off";
    ForgottenGUI* g = skip ? 0 : ::gui;
    if (!skip && !g) skip = "no-gui";
    if (skip) {
        char b[128];
        _snprintf(b, sizeof(b) - 1, "[dmg] FLOATER skip=%s amt=%.2f", skip, amount);
        b[sizeof(b) - 1] = '\0';
        coop::logLine(b);
        return;
    }
    int n = (int)(amount + 0.5f);
    if (n < 1) n = 1;
    char cap[24];
    _snprintf(cap, sizeof(cap) - 1, "%d", n);
    cap[sizeof(cap) - 1] = '\0';
    std::string text(cap);
    MyGUI::Colour col(1.00f, 0.28f, 0.12f, 1.0f);
    Ogre::Vector3 off(0.0f, 1.9f, 0.0f);
    ScreenLabel* l = floaterCreateSeh(g, c, &text, &col, &off);
    char b[96];
    _snprintf(b, sizeof(b) - 1, "[dmg] FLOATER n=%d ok=%d", n, l ? 1 : 0);
    b[sizeof(b) - 1] = '\0';
    coop::logLine(b);
}

// ---- In-game co-op session panel (config-driven, spike-50 DatapanelGUI stack) -
// A native DatapanelGUI window toggled with F2. Role/transport/connection are
// toggle BUTTONS (callable RVA callback). Steam IDs stay clipboard-paste. The
// display nick is setLineTextEditable (the same row type Kenshi uses for typed
// numbers on sliders). Status-line changes update the debug row in place so a
// rebuild does not wipe the caret while the player is typing.
// The GUI layer is session-agnostic: live status arrives via *st; the user's
// actions leave via the onConnect/onDisconnect callbacks (the plugin root owns the
// net/session/config wiring).
//
// SEH discipline (spike 47/48): the mutation calls take std::string by const-ref
// or PODs, so they all sit inside one __try, provided NO std::string temporary is
// constructed in that frame. The one exception is createDatapanel's BY-VALUE
// std::string 'layer' arg (an unwindable temporary => C2712), so the window is
// created in the outer, non-SEH function; ::gui is verified non-null first and the
// createScreenLabel/createFloatingLabel factory family is render-proven (46-48).

namespace {

// Write a UTF-8/ANSI string to the Windows clipboard (CF_TEXT). Mirror of the
// paste-read: OpenClipboard -> EmptyClipboard -> GlobalAlloc+copy -> SetClipboardData
// -> CloseClipboard. Used by the "Copy my Steam ID" button. Win32 only (no MyGUI).
bool clipboardSetText(const char* text) {
    if (!text) return false;
    size_t n = strlen(text);
    if (!OpenClipboard(0)) return false;
    bool ok = false;
    if (EmptyClipboard()) {
        HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, n + 1);
        if (h) {
            char* dst = (char*)GlobalLock(h);
            if (dst) {
                memcpy(dst, text, n);
                dst[n] = '\0';
                GlobalUnlock(h);
                if (SetClipboardData(CF_TEXT, h)) ok = true; // clipboard now owns h
            }
            if (!ok) GlobalFree(h); // ownership not transferred on failure
        }
    }
    CloseClipboard();
    return ok;
}

// Read text from the Windows clipboard into out. Prefers CF_UNICODETEXT (what the
// Steam overlay / browsers usually publish) and falls back to CF_TEXT, converting
// either to a narrow std::string (the SteamID parse keeps only ASCII digits, so a
// lossy WideCharToMultiByte is fine here). Used by the "Paste friend's Steam ID"
// button. Win32 only (no MyGUI). Returns true iff some text was retrieved.
bool clipboardGetText(std::string& out) {
    if (!OpenClipboard(0)) return false;
    bool ok = false;
    HANDLE hw = GetClipboardData(CF_UNICODETEXT);
    if (hw) {
        const wchar_t* src = (const wchar_t*)GlobalLock(hw);
        if (src) {
            int need = WideCharToMultiByte(CP_UTF8, 0, src, -1, 0, 0, 0, 0);
            if (need > 0) {
                std::string tmp((size_t)need, '\0');
                if (WideCharToMultiByte(CP_UTF8, 0, src, -1, &tmp[0], need, 0, 0) > 0) {
                    if (!tmp.empty() && tmp[tmp.size() - 1] == '\0') tmp.resize(tmp.size() - 1);
                    out = tmp;
                    ok = true;
                }
            }
            GlobalUnlock(hw);
        }
    }
    if (!ok) {
        HANDLE ha = GetClipboardData(CF_TEXT);
        if (ha) {
            const char* src = (const char*)GlobalLock(ha);
            if (src) { out = src; ok = true; GlobalUnlock(ha); }
        }
    }
    CloseClipboard();
    return ok;
}

struct CoopPanelUi {
    DatapanelGUI* panel;
    bool          open, built;
    bool          hostFlag;      // true = HOST role armed
    bool          steamFlag;     // true = Steam transport armed (else UDP)
    bool          connectedFlag; // desired connection state (Online/Offline toggle)
    bool          lastConnected; // last observed st->running (external-change sync)
    bool          lastChkVal;    // last toggle value (connect/disconnect edge)
    bool          needsRebuild;
    bool          f2Down;        // F2 held last tick (rising-edge toggle)
    std::string   lastStatus;    // last status text shown (refresh gate)
    std::string   lastTransfer;  // last save-transfer line shown (refresh gate)
    std::string   lastUpdate;    // last self-update line shown (refresh gate)
    CoopPanelUi()
        : panel(0), open(false), built(false), hostFlag(true), steamFlag(true),
          connectedFlag(false), lastConnected(false), lastChkVal(false),
          needsRebuild(false), f2Down(false) {}
};

CoopPanelUi             g_panel;
DataPanelLine_Button*   g_roleBtn      = 0;
DataPanelLine_Button*   g_transBtn     = 0;
DataPanelLine_Button*   g_connBtn      = 0; // Online/Offline toggle (replaces the checkbox)
DataPanelLine_Button*   g_copyIdBtn    = 0;
DataPanelLine_Button*   g_applyNickBtn = 0; // explicit "apply the typed nick"
DataPanelLine_TextEditable* g_nickLine = 0;
DataPanelLine_TextEditable* g_udpLine  = 0;
DataPanelLine*          g_nickHint     = 0;
DataPanelLine*          g_udpHint      = 0;
DataPanelLine_Button*   g_pasteBtns[3] = {0, 0, 0}; // one paste slot per friend
DataPanelLine*          g_debugLine    = 0; // white connection-status debug row
DataPanelLine*          g_selfLine     = 0; // white "Your Steam ID" row
std::string             g_selfIdStr;   // self SteamID as digits (set each tick; "" = none)

// Up to 3 friend SteamIDs pasted this session (slot 0 = first friend / join's host).
unsigned long long      g_pastedPeers[3] = {0, 0, 0};
int                     g_pasteFailedSlot = -1; // which slot last failed, or -1
std::string             g_udpIp;         // join UDP host (pasted or seeded)
int                     g_udpPort = 0;   // 0 = not set this session
bool                    g_udpPasteFailed = false;
std::string             g_playerNick;    // display nick (typed / seeded)
DWORD                   g_nickDirtyTick = 0; // 0 = clean; else last edit tick
bool                    g_editHoldHarvest = false; // skip one tick after rebuild
bool                    g_memorySeeded = false; // config fallback applied once
CoopConnectFn           g_onConnectCb  = 0;
CoopRememberFn          g_onRememberCb = 0;

void fireRemember() {
    if (g_onRememberCb)
        g_onRememberCb(g_panel.hostFlag, g_panel.steamFlag);
}

// Button callbacks (free functions - MyGUI::newDelegate wraps them without any
// raw-MyGUI link). A press flips the armed flag and requests a rebuild so the
// caption reflects the new choice on the next tick.
void onRoleBtn(DataPanelLine*) {
    g_panel.hostFlag = !g_panel.hostFlag;
    g_panel.needsRebuild = true;
    coop::logLine(g_panel.hostFlag ? "[coop-ui] role -> Host" : "[coop-ui] role -> Join");
    fireRemember();
}
void onTransBtn(DataPanelLine*) {
    g_panel.steamFlag = !g_panel.steamFlag;
    g_panel.needsRebuild = true;
    coop::logLine(g_panel.steamFlag ? "[coop-ui] transport -> Steam" : "[coop-ui] transport -> UDP");
    fireRemember();
}
// Online/Offline toggle: flip the desired connection state. The connect/disconnect
// edge (connectedFlag vs lastChkVal) is handled in coopPanelTick, same as before.
void onConnBtn(DataPanelLine*) {
    g_panel.connectedFlag = !g_panel.connectedFlag;
    g_panel.needsRebuild = true;
    coop::logLine(g_panel.connectedFlag ? "[coop-ui] connection -> ONLINE"
                                        : "[coop-ui] connection -> OFFLINE");
}
// Copy the player's own SteamID to the clipboard so they can paste it to a friend
// (who pastes it into their panel via "Paste friend's Steam ID").
void onCopyIdBtn(DataPanelLine*) {
    if (g_selfIdStr.empty()) {
        coop::logLine("[coop-ui] copy Steam ID: none (Steam not running)");
        return;
    }
    bool ok = clipboardSetText(g_selfIdStr.c_str());
    char b[64];
    _snprintf(b, sizeof(b) - 1, "[coop-ui] copied Steam ID to clipboard: %s",
              ok ? "ok" : "FAILED");
    b[sizeof(b) - 1] = '\0';
    coop::logLine(b);
}
void pasteIntoSlot(int slot) {
    if (slot < 0 || slot > 2) return;
    std::string clip;
    unsigned long long id = 0;
    bool ok = clipboardGetText(clip) && coop::parseSteamId64(clip, id);
    if (!ok) {
        unsigned long long ids[4];
        int n = coop::parseSteamId64List(clip, ids, 4);
        if (n > 0) { id = ids[0]; ok = true; }
    }
    if (ok && id != 0) {
        g_pastedPeers[slot] = id;
        g_pasteFailedSlot = -1;
        char b[80];
        _snprintf(b, sizeof(b) - 1, "[coop-ui] paste slot %d id=%llu", slot + 1, id);
        b[sizeof(b) - 1] = '\0';
        coop::logLine(b);
        fireRemember();
        // Host already ONLINE: add this tunnel peer without restarting.
        if (g_panel.connectedFlag && g_panel.hostFlag && g_panel.steamFlag && g_onConnectCb)
            g_onConnectCb(true, true, id);
    } else {
        g_pasteFailedSlot = slot;
        coop::logLine("[coop-ui] paste failed (clipboard not a Steam ID)");
    }
    g_panel.needsRebuild = true;
}
void pasteUdpEndpoint() {
    std::string clip;
    std::string ip;
    int port = g_udpPort > 0 ? g_udpPort : 27800;
    bool ok = clipboardGetText(clip) && coop::parseUdpEndpoint(clip, ip, port);
    if (ok && !ip.empty()) {
        g_udpIp = ip;
        g_udpPort = port;
        g_udpPasteFailed = false;
        char b[96];
        _snprintf(b, sizeof(b) - 1, "[coop-ui] paste udp %s:%d",
                  g_udpIp.c_str(), g_udpPort);
        b[sizeof(b) - 1] = '\0';
        coop::logLine(b);
        fireRemember();
    } else {
        g_udpPasteFailed = true;
        coop::logLine("[coop-ui] paste failed (clipboard not an IP:port)");
    }
    g_panel.needsRebuild = true;
}
void onPasteSlot0(DataPanelLine*) { pasteIntoSlot(0); }
void onPasteSlot1(DataPanelLine*) { pasteIntoSlot(1); }
void onPasteSlot2(DataPanelLine*) { pasteIntoSlot(2); }

// Read the typed text from the engine's DataPanelLine strings only. Calling
// MyGUI::EditBox::getCaption / UString::asUTF8 from this DLL walks the GAME's
// UString with OUR MyGUI copy and AV'd (join log 2026-09-04, READ of
// 0x000056C500000000 while F2 was open). textChanged writes s2.
void harvestLineRaw(DataPanelLine_TextEditable* line, char* raw, unsigned cap) {
    raw[0] = '\0';
    if (!line || cap == 0) return;
    // Make the engine refresh its own s2 from the live EditBox first. Without
    // this, s2 still holds the string the row was SEEDED with, so typing changed
    // what the player saw and nothing else: session 15:33 had "moogg" visible in
    // the field while the harvest kept reading back "nickedit", found no change,
    // and therefore never applied it, never sent it and never wrote it to
    // coop_config.json. Calling the ENGINE's handler keeps MyGUI entirely on its
    // own side of the DLL boundary - reading the caption with our copy of MyGUI
    // is what faulted in v0.57.
    if (g_lineTextChangedFn && line->editBox) {
        __try { g_lineTextChangedFn(line, line->editBox); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    // s2 ONLY. keyValue was tried as a fallback and is wrong: it holds the row's
    // KEY ("nickedit" / "udpedit"), never anything the player typed, so an empty
    // field harvested the key and used it as the value. Session 12:29:09 shows
    // the result - "[nick] applied id=1 'nickedit'" - and the same fallback
    // would have fed "udpedit" to the UDP endpoint parser. An unreadable field
    // must report NOTHING and let the caller keep what it already had.
    if (line->s2.empty()) return;
    size_t n = line->s2.size();
    if (n >= cap) n = cap - 1;
    memcpy(raw, line->s2.c_str(), n);
    raw[n] = '\0';
}

void harvestNick() {
    if (!g_nickLine) return;
    char raw[128];
    harvestLineRaw(g_nickLine, raw, sizeof(raw));
    // Empty read = the field told us nothing this tick, which is NOT the same as
    // the player clearing their nick. Keep what we have (usually the one seeded
    // from coop_config.json), the way harvestUdp already does. Clearing here
    // would silently drop a remembered nick every time the row read back blank.
    if (raw[0] == '\0') return;
    std::string nick;
    if (!coop::parsePlayerNick(raw, nick)) return;
    if (nick == g_playerNick) return;
    g_playerNick = nick;
    g_nickDirtyTick = GetTickCount();
}

void harvestUdp() {
    if (!g_udpLine) return;
    char raw[128];
    harvestLineRaw(g_udpLine, raw, sizeof(raw));
    if (raw[0] == '\0') return;
    std::string ip;
    int port = g_udpPort > 0 ? g_udpPort : 27800;
    if (!coop::parseUdpEndpoint(raw, ip, port) || ip.empty()) return;
    if (ip == g_udpIp && port == g_udpPort) return;
    g_udpIp = ip;
    g_udpPort = port;
    g_udpPasteFailed = false;
    g_nickDirtyTick = GetTickCount();
}

void harvestEdits() {
    harvestNick();
    harvestUdp();
}

// Explicit "apply my nick" click. Two jobs.
//
// One: give the harvest a moment the player controls. Typing alone kept losing
// the text - a status change rebuilds the panel, the row is recreated seeded
// from the REMEMBERED nick, and anything typed but not yet harvested dies with
// the old widget (screenshot: "qwegwegwe" typed, field blank after toggling
// Connection).
//
// Two: say what it actually found. Two attempts at reading the typed text have
// failed and the logs could not distinguish "the lever did not resolve" from
// "the text lives in a different field" from "keystrokes never reach the box" -
// so this prints all three row strings and the lever state. One run settles it
// instead of a third guess.
void onApplyNickBtn(DataPanelLine*) {
    if (!g_nickLine) return;
    const char* ks = "";
    const char* s1 = "";
    const char* s2 = "";
    __try {
        ks = g_nickLine->keyValue.c_str();
        s1 = g_nickLine->s1.c_str();
        s2 = g_nickLine->s2.c_str();
    } __except (EXCEPTION_EXECUTE_HANDLER) { ks = s1 = s2 = "<fault>"; }
    char pb[300]; _snprintf(pb, sizeof(pb) - 1,
        "[coop-ui] nick-probe lever=%d box=%d key='%s' s1='%s' s2='%s'",
        g_lineTextChangedFn ? 1 : 0, g_nickLine->editBox ? 1 : 0, ks, s1, s2);
    pb[sizeof(pb) - 1] = '\0'; coop::logLine(pb);

    harvestNick();
    // Persist + broadcast even when harvestNick saw no change: the player asked
    // for this explicitly, and a no-op that logs nothing is what made the last
    // two rounds unreadable.
    g_nickDirtyTick = 0;
    fireRemember();
    char b[160]; _snprintf(b, sizeof(b) - 1, "[coop-ui] nick applied '%s'",
                           g_playerNick.empty() ? "-" : g_playerNick.c_str());
    b[sizeof(b) - 1] = '\0'; coop::logLine(b);
}

void formatUdpText(std::string& out) {
    int p = g_udpPort > 0 ? g_udpPort : 27800;
    char b[96];
    if (!g_udpIp.empty())
        _snprintf(b, sizeof(b) - 1, "%s:%d", g_udpIp.c_str(), p);
    else
        _snprintf(b, sizeof(b) - 1, "127.0.0.1:%d", p);
    b[sizeof(b) - 1] = '\0';
    out = b;
}

void flushNickRemember() {
    harvestEdits();
    if (g_nickDirtyTick != 0) {
        g_nickDirtyTick = 0;
        char b[128];
        _snprintf(b, sizeof(b) - 1, "[coop-ui] nick '%s' udp=%s:%d",
                  g_playerNick.empty() ? "-" : g_playerNick.c_str(),
                  g_udpIp.empty() ? "-" : g_udpIp.c_str(),
                  g_udpPort > 0 ? g_udpPort : 27800);
        b[sizeof(b) - 1] = '\0';
        coop::logLine(b);
        fireRemember();
    }
}

void fillDbgLine(const CoopPanelState* st, bool hostFlag, bool steamFlag,
                 const std::string& transfer, const std::string& update,
                 std::string& dbgKey, std::string& dbgVal) {
    std::string transStr = (st->transportSel == 0) ? "Steam" : "UDP";
    dbgKey = "Connection status";
    if (st->running) {
        if (st->peerPresent)
            dbgVal = (st->isHost ? std::string("Hosting") : std::string("Joining")) +
                     " over " + transStr + " - player(s) connected";
        else if (st->isHost)
            dbgVal = std::string("Hosting over ") + transStr + " - waiting for players (max 4)...";
        else
            dbgVal = std::string("Joining over ") + transStr + " - connecting to host...";
    } else {
        dbgVal = std::string("Offline - will ") + (hostFlag ? "host" : "join") +
                 " over " + (steamFlag ? "Steam" : "UDP") + " on Connect";
    }
    if (!transfer.empty()) { dbgVal = transfer; dbgKey = "World transfer"; }
    if (!update.empty()) { dbgVal = update; dbgKey = "Update"; }
}

void debugLineSet(DataPanelLine* line, const std::string& key, const std::string& val) {
    if (!line) return;
    line->s1 = key;
    line->s2 = val;
    __try {
        line->refresh();
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// POD-only pointer bundle so the row-build SEH frame constructs no std::string.
struct PanelStrings {
    const std::string *title, *roleKey, *roleCap, *transKey, *transCap;
    const std::string *connKey, *connCap;
    const std::string *dbgKey, *dbgVal;
    const std::string *pasteKey[3], *pasteCap[3];
    int nSlots;
    const std::string *selfKey, *selfVal, *copyKey, *copyCap;
    const std::string *applyNickKey, *applyNickCap;
    const std::string *nickLblKey, *nickLbl, *nickKey, *nickText;
    const std::string *udpLblKey, *udpLbl, *udpKey, *udpText;
    const MyGUI::Align *editAlign;
    float editWidth;
    int showSteam;  // 1 = Steam ID rows, 0 = hide (UDP)
    int showSelfId; // 1 = own Steam ID + Copy button (HOST only)
    int showUdp;   // 1 = Host IP:port edit, 0 = hide (Steam)
    const std::string *empty;
};

void panelBuildSeh(DatapanelGUI* p, const PanelStrings* s) {
    __try {
        p->_NV_clear();
        p->setCaption(*s->title);
        g_roleBtn  = p->setLineButton(*s->roleKey,  *s->roleCap,  0);
        g_transBtn = p->setLineButton(*s->transKey, *s->transCap, 0);
        g_connBtn  = p->setLineButton(*s->connKey,  *s->connCap,  0);
        p->addSpace(0, 0.25f);
        g_nickHint = p->setLine(*s->nickLblKey, *s->nickLbl, *s->empty, 0, false, true);
        g_nickLine = p->setLineTextEditable(*s->nickKey, *s->nickText, 0, false,
                                            false, *s->editAlign, s->editWidth);
        g_applyNickBtn = p->setLineButton(*s->applyNickKey, *s->applyNickCap, 0);
        p->addSpace(0, 0.45f);
        g_udpHint = 0;
        g_udpLine = 0;
        if (s->showUdp) {
            g_udpHint = p->setLine(*s->udpLblKey, *s->udpLbl, *s->empty, 0, false, true);
            g_udpLine = p->setLineTextEditable(*s->udpKey, *s->udpText, 0, false,
                                               false, *s->editAlign, s->editWidth);
            p->addSpace(0, 0.45f);
        }
        g_debugLine = p->setLine(*s->dbgKey, *s->dbgVal, *s->empty, 0, false, true);
        p->addSpace(0, 0.25f);
        int i;
        for (i = 0; i < 3; ++i) g_pasteBtns[i] = 0;
        g_selfLine = 0;
        g_copyIdBtn = 0;
        if (s->showSteam) {
            for (i = 0; i < s->nSlots && i < 3; ++i)
                g_pasteBtns[i] = p->setLineButton(*s->pasteKey[i], *s->pasteCap[i], 0);
            // Own ID + its copy button are HOST-only. Nobody needs the join's id:
            // the host runs setAllowAny and takes whoever dials in, so a join
            // handing its id around achieves nothing. On the join the row was
            // just one more number next to the one that does matter (the host's),
            // which is exactly the confusion to avoid when both are masked.
            if (s->showSelfId) {
                p->addSpace(0, 0.25f);
                g_selfLine = p->setLine(*s->selfKey, *s->selfVal, *s->empty, 0, false, true);
                g_copyIdBtn = p->setLineButton(*s->copyKey, *s->copyCap, 0);
            }
        }
        p->_NV_update();
        // NO fit-to-content resize here. It was tried and reverted: resizing the
        // window per rebuild clipped the panel - a mode switch left the three
        // toggle buttons drawn off the bottom edge, and only closing/reopening
        // F2 (which destroys and recreates the panel) restored it.
        // getContentHeight() does not return window pixels the way resize()
        // consumes them, so the two cannot be chained. The panel is a fixed
        // fraction of the screen instead, sized for its tallest layout; a little
        // empty rust beats content that is silently cut off, and this widget has
        // scrolling=false so nothing clipped can be reached.
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Colour a line's key + value TextBoxes for readability. Runs AFTER
// panelBuildSeh's _NV_update (the w1/w2 widgets exist by then). MyGUI::Colour is a
// trivial 4-float struct (no destructor), so it may live in the SEH frame.
// yellow=true tints the value column amber - used for the join's live
// "Streaming host world..." transfer line so it reads as in-progress activity.
void dbgColourSeh(DataPanelLine* line, bool yellow) {
    if (!line) return;
    __try {
        MyGUI::Colour white(1.0f, 1.0f, 1.0f, 1.0f);
        MyGUI::Colour amber(1.0f, 0.82f, 0.20f, 1.0f);
        if (line->w1) line->w1->setTextColour(white);
        if (line->w2) line->w2->setTextColour(yellow ? amber : white);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Arm a freshly-minted panel: register it for ForgottenGUI's per-frame refresh
// AND make it visible. createDatapanel returns a built-but-hidden window; without
// this pair the F2 toggle logs open/close yet nothing ever draws (the render bug
// in the reconstruction). PODs only, so the whole thing sits in one SEH frame.
bool uiPanelArmSeh(ForgottenGUI* g, DatapanelGUI* p) {
    if (!g || !p) return false;
    __try {
        g->addDatapanelToUpdateList(p);
        p->_NV_show(true);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void panelDestroySeh(ForgottenGUI* g, DatapanelGUI* p) {
    if (!g || !p) return;
    // Pull it off the refresh list BEFORE destroying so ForgottenGUI never
    // dereferences the freed panel on the next frame.
    __try {
        g->removeDatapanelFromUpdateList(p);
        g->destroy(p);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

} // namespace

int coopPanelPastedCount() { return 3; }
unsigned long long coopPanelPastedId(int i) {
    if (i < 0 || i > 2) return 0;
    return g_pastedPeers[i];
}
const char* coopPanelUdpIp() { return g_udpIp.c_str(); }
int coopPanelUdpPort() { return g_udpPort; }
const char* coopPanelPlayerName() { return g_playerNick.c_str(); }

void coopPanelTick(const CoopPanelState* st, CoopConnectFn onConnect,
                   CoopDisconnectFn onDisconnect, CoopRememberFn onRemember) {
    if (!st) return;
    g_onConnectCb = onConnect;
    g_onRememberCb = onRemember;
    if (!g_memorySeeded) {
        if (g_pastedPeers[0] == 0) g_pastedPeers[0] = st->peerSteamId;
        if (g_pastedPeers[1] == 0) g_pastedPeers[1] = st->peerSteamId2;
        if (g_pastedPeers[2] == 0) g_pastedPeers[2] = st->peerSteamId3;
        if (g_udpIp.empty() && st->udpIp && st->udpIp[0]) g_udpIp = st->udpIp;
        if (g_udpPort <= 0 && st->udpPort > 0) g_udpPort = st->udpPort;
        if (g_playerNick.empty() && st->playerName && st->playerName[0])
            g_playerNick = st->playerName;
        g_memorySeeded = true;
    }
    ForgottenGUI* g = ::gui; // KenshiLib data export (spike 46)
    { static void* s_last = (void*)-1;
      if ((void*)g != s_last) { s_last = (void*)g;
          char b[64]; _snprintf(b, sizeof(b) - 1, "[coop-ui] gui ptr=%p", (void*)g);
          b[sizeof(b) - 1] = '\0'; coop::logLine(b); } }
    if (!g) return;

    // Cache the self id as a string for the Copy button (used by onCopyIdBtn).
    if (st->selfSteamId) {
        char b[32];
        _snprintf(b, sizeof(b) - 1, "%llu", (unsigned long long)st->selfSteamId);
        b[sizeof(b) - 1] = '\0';
        g_selfIdStr = b;
    } else {
        g_selfIdStr.clear();
    }

    // F2 rising edge toggles the panel open/closed.
    bool f2 = (GetAsyncKeyState(VK_F2) & 0x8000) != 0;
    if (f2 && !g_panel.f2Down) {
        if (!g_panel.open) {
            g_panel.hostFlag      = st->isHost;
            g_panel.steamFlag     = (st->transportSel == 0);
            g_panel.connectedFlag = st->running;
            g_panel.lastConnected = st->running;
            g_panel.lastChkVal    = st->running;
            g_panel.open = true;
            g_panel.needsRebuild = true;
            coop::logLine("[coop-ui] panel opened");
        } else {
            g_editHoldHarvest = false;
            flushNickRemember();
            panelDestroySeh(g, g_panel.panel);
            g_panel.panel = 0; g_panel.built = false;
            g_roleBtn = 0; g_transBtn = 0; g_connBtn = 0; g_copyIdBtn = 0;
            g_applyNickBtn = 0;
            g_nickLine = 0; g_udpLine = 0; g_nickHint = 0; g_udpHint = 0;
            g_pasteBtns[0] = g_pasteBtns[1] = g_pasteBtns[2] = 0;
            g_debugLine = 0; g_selfLine = 0;
            g_panel.open = false;
            coop::logLine("[coop-ui] panel closed");
        }
    }
    g_panel.f2Down = f2;

    if (!g_panel.open) return;

    if (g_editHoldHarvest) g_editHoldHarvest = false;
    else harvestEdits();
    if (g_nickDirtyTick != 0 && (GetTickCount() - g_nickDirtyTick) >= 800ul) {
        g_nickDirtyTick = 0;
        fireRemember();
    }

    // Keep the Online/Offline toggle honest when the session state changes
    // underneath us (a peer-driven connect, a failed connect that stopped, etc):
    // resync the desired flag to the real state and rebuild so the button caption
    // + debug line reflect it.
    if (st->running != g_panel.lastConnected) {
        g_panel.lastConnected = st->running;
        g_panel.connectedFlag = st->running;
        g_panel.lastChkVal    = st->running;
        g_panel.needsRebuild = true;
    }

    std::string detail = st->detail ? std::string(st->detail) : std::string();
    std::string transfer = st->transferDetail ? std::string(st->transferDetail)
                                               : std::string();
    std::string update = st->updateDetail ? std::string(st->updateDetail) : std::string();
    const bool statusChanged = (detail != g_panel.lastStatus) ||
                               (transfer != g_panel.lastTransfer) ||
                               (update != g_panel.lastUpdate);

    // Create the window once (outside SEH - see the header note on C2712).
    // Layer MUST be "Info": spike 48 proved createFloatingLabel renders non-null
    // there. "Windows" is not a visible MyGUI layer here - the panel is minted
    // and armed but attaches to nothing, so F2 logs open/close yet nothing draws.
    if (!g_panel.panel) {
        std::string layer = "Info";
        // Height: sized for the TALLEST layout (join + Steam is the row count to
        // beat - three toggles, nick label + field, status, host-ID row, own-ID
        // row, copy button, plus the spacers between groups). 0.72 was two thirds
        // empty; this is trimmed but still has headroom, because the panel does
        // not scroll and anything that does not fit is simply lost.
        g_panel.panel = g->createDatapanel(0.20f, 0.14f, 0.36f, 0.40f, false, layer, true);
        g_panel.built = false;
        if (!g_panel.panel) {
            coop::logErrLine("[coop-ui] createDatapanel FAILED");
        } else if (!uiPanelArmSeh(g, g_panel.panel)) {
            coop::logErrLine("[coop-ui] panel arm (update-list/show) FAILED");
        }
    }

    std::string dbgKey, dbgVal;
    fillDbgLine(st, g_panel.hostFlag, g_panel.steamFlag, transfer, update, dbgKey, dbgVal);
    if (g_panel.panel && g_panel.built && statusChanged && !g_panel.needsRebuild) {
        debugLineSet(g_debugLine, dbgKey, dbgVal);
        dbgColourSeh(g_debugLine, !transfer.empty() && update.empty());
        g_panel.lastStatus = detail;
        g_panel.lastTransfer = transfer;
        g_panel.lastUpdate = update;
    }

    // (Re)populate the rows when role/transport/connection/paste slots change.
    // Status text alone must NOT rebuild: that destroys the nick EditBox.
    if (g_panel.panel && (g_panel.needsRebuild || !g_panel.built)) {
        g_editHoldHarvest = false;
        harvestEdits();
        std::string title = "Co-op Session";
        if (st->versionText && st->versionText[0]) {
            title += "   ";
            title += st->versionText;
        }
        title += "    -    F2 to close";
        std::string roleKey  = "role";
        std::string roleCap  = std::string("Role: ") + (g_panel.hostFlag ? "HOST" : "JOIN") + "    (switch)";
        std::string transKey = "trans";
        std::string transCap = std::string("Transport: ") + (g_panel.steamFlag ? "STEAM" : "UDP") + "    (switch)";
        std::string connKey  = "conn";
        std::string connCap  = std::string("Connection: ") + (g_panel.connectedFlag ? "ONLINE" : "OFFLINE") + "    (switch)";

        // Steam paste slots: the JOIN needs the host's ID; the HOST needs nothing.
        // Plugin.cpp arms steamp2p::setAllowAny(isHost), so an inbound tunnel is
        // accepted and given a slot on its first packet - the host never had to
        // know an ID in advance. The three "Friend N" rows only ever looked
        // mandatory, and made a 2-player setup read like it needed four steps.
        // coop_config.json "steamPeer" still pre-registers peers for anyone who
        // wants it; this is the panel dropping a prompt, not the tunnel losing one.
        const int nSlots = (g_panel.steamFlag && !g_panel.hostFlag) ? 1 : 0;
        std::string pasteKey[3], pasteCap[3];
        int si;
        for (si = 0; si < nSlots; ++si) {
            char k[16];
            _snprintf(k, sizeof(k) - 1, "paste%d", si + 1);
            k[sizeof(k) - 1] = '\0';
            pasteKey[si] = k;
            std::string label;
            unsigned long long shown = g_pastedPeers[si];
            if (shown == 0 && si == 0) shown = (unsigned long long)st->peerSteamId;
            if (g_panel.hostFlag) {
                char nbuf[16];
                _snprintf(nbuf, sizeof(nbuf) - 1, "Friend %d: ", si + 1);
                nbuf[sizeof(nbuf) - 1] = '\0';
                label = nbuf;
            } else {
                label = "Host ID: ";
            }
            if (shown != 0) {
                label += coop::maskSteamId64(shown);
                label += "    (click to re-paste)";
            } else if (g_pasteFailedSlot == si) {
                label += "(not a Steam ID - copy theirs and retry)";
            } else {
                label += "(click to paste Steam ID)";
            }
            pasteCap[si] = label;
        }

        // The key column of a DataPanelLine does not render in this skin - the
        // status line and this row both came out as a bare value with nothing
        // saying what it was ("****1843" floating under the friend rows). Put
        // the label in the VALUE so the row explains itself.
        std::string selfKey  = "selfid";
        std::string selfVal  = st->selfSteamId
                                   ? std::string("Your Steam ID:  ") +
                                     coop::maskSteamId64((unsigned long long)st->selfSteamId) +
                                     std::string("   (send it to your friends)")
                                   : std::string("Your Steam ID:  (Steam not running)");
        std::string applyNickKey = "applynick";
        std::string applyNickCap = "Apply nick";
        std::string copyKey  = "copyid";
        std::string copyCap  = "Copy my Steam ID";
        std::string nickLblKey = "name";
        std::string nickLbl    = "Your nick  (type your name below)";
        std::string nickKey    = "nickedit";
        std::string nickText   = g_playerNick;
        std::string udpLblKey  = "udp";
        std::string udpLbl     = g_panel.hostFlag
            ? "UDP IP:port  (your address, type or paste)"
            : "Host IP:port  (type or paste the host address)";
        std::string udpKey     = "udpedit";
        std::string udpText;
        formatUdpText(udpText);
        MyGUI::Align editAlign(MyGUI::Align::Left);
        std::string empty    = "";

        PanelStrings ps;
        ps.title = &title; ps.roleKey = &roleKey; ps.roleCap = &roleCap;
        ps.transKey = &transKey; ps.transCap = &transCap;
        ps.connKey = &connKey; ps.connCap = &connCap;
        ps.dbgKey = &dbgKey; ps.dbgVal = &dbgVal;
        ps.nSlots = nSlots;
        for (si = 0; si < 3; ++si) {
            ps.pasteKey[si] = (si < nSlots) ? &pasteKey[si] : &empty;
            ps.pasteCap[si] = (si < nSlots) ? &pasteCap[si] : &empty;
        }
        ps.selfKey = &selfKey; ps.selfVal = &selfVal;
        ps.copyKey = &copyKey; ps.copyCap = &copyCap;
        ps.applyNickKey = &applyNickKey; ps.applyNickCap = &applyNickCap;
        ps.nickLblKey = &nickLblKey; ps.nickLbl = &nickLbl;
        ps.nickKey = &nickKey; ps.nickText = &nickText;
        ps.udpLblKey = &udpLblKey; ps.udpLbl = &udpLbl;
        ps.udpKey = &udpKey; ps.udpText = &udpText;
        ps.editAlign = &editAlign; ps.editWidth = 420.0f;
        ps.showSteam = g_panel.steamFlag ? 1 : 0;
        ps.showSelfId = (g_panel.steamFlag && g_panel.hostFlag) ? 1 : 0;
        ps.showUdp = g_panel.steamFlag ? 0 : 1;
        ps.empty = &empty;
        panelBuildSeh(g_panel.panel, &ps);
        g_editHoldHarvest = true;

        // Delegate assignment + white-colouring live OUTSIDE the SEH frame (pointer)
        // targets are valid post-build; assignment can't fault) so no delegate
        // temporary lands in it.
        if (g_roleBtn)    g_roleBtn->callback    = MyGUI::newDelegate(&onRoleBtn);
        if (g_transBtn)   g_transBtn->callback   = MyGUI::newDelegate(&onTransBtn);
        if (g_connBtn)    g_connBtn->callback    = MyGUI::newDelegate(&onConnBtn);
        if (g_copyIdBtn)  g_copyIdBtn->callback  = MyGUI::newDelegate(&onCopyIdBtn);
        if (g_applyNickBtn) g_applyNickBtn->callback = MyGUI::newDelegate(&onApplyNickBtn);
        if (g_pasteBtns[0]) g_pasteBtns[0]->callback = MyGUI::newDelegate(&onPasteSlot0);
        if (g_pasteBtns[1]) g_pasteBtns[1]->callback = MyGUI::newDelegate(&onPasteSlot1);
        if (g_pasteBtns[2]) g_pasteBtns[2]->callback = MyGUI::newDelegate(&onPasteSlot2);
        dbgColourSeh(g_nickHint, false);
        dbgColourSeh(g_udpHint, false);
        dbgColourSeh(g_debugLine, !transfer.empty()); // amber while streaming
        dbgColourSeh(g_selfLine, false);

        g_panel.built = true;
        g_panel.needsRebuild = false;
        g_panel.lastStatus = detail;
        g_panel.lastTransfer = transfer;
        g_panel.lastUpdate = update;
    }

    // Connect / disconnect on the Online/Offline toggle edge (edge, not level, so
    // a connect that hasn't reported running yet is not re-fired every tick). The
    // pasted friend id (0 if none) is handed to the plugin, which lets a non-zero
    // value override the config steamPeer; UDP ip/port still come from the config.
    if (g_panel.connectedFlag != g_panel.lastChkVal) {
        g_panel.lastChkVal = g_panel.connectedFlag;
        if (g_panel.connectedFlag && !st->running) {
            g_editHoldHarvest = false;
            flushNickRemember();
            char b[80];
            _snprintf(b, sizeof(b) - 1, "[coop-ui] CONNECT role=%s transport=%s",
                      g_panel.hostFlag ? "HOST" : "JOIN",
                      g_panel.steamFlag ? "steam" : "udp");
            b[sizeof(b) - 1] = '\0';
            coop::logLine(b);
            unsigned long long first = 0;
            for (int pi = 0; pi < 3; ++pi)
                if (g_pastedPeers[pi] != 0) { first = g_pastedPeers[pi]; break; }
            if (onConnect) onConnect(g_panel.hostFlag, g_panel.steamFlag, first);
        } else if (!g_panel.connectedFlag && st->running) {
            coop::logLine("[coop-ui] DISCONNECT requested");
            if (onDisconnect) onDisconnect();
        }
    }
}

// ---- Persistent co-op status overlay ----------------------------------------
// A fixed banner in the top-left corner of the screen showing live session status
// colored by state (0 = offline/red, 1 = waiting/yellow, 2 = connected/green).
// Unlike the character-tracked ScreenLabel this replaces, it needs no player
// character and holds its place while the camera moves - which also makes it
// visible at the title screen, where a join has no leader while it streams the
// host's world. Removed when show=false.
//
// Two widgets, because neither factory alone does the job. Measured 2026-08-04:
// createFloatingLabel hands back a bare MyGUI::Window that IS visible and
// layer-attached at the coords we ask for, but its skin carries no text region at
// all - setCaption is silently dropped (getCaption().size() stays 0) and it has no
// children, so it draws nothing. It is still the only way to get a widget parented
// to a screen layer instead of to another window, so we keep it as an invisible
// container and put Kenshi's own label factory inside it (createLabelAbs ->
// MyGUI::TextBox with a text-bearing skin, the one the datapanel rows use).
// Caption + colour go to the child; the container is only geometry.

namespace {
// Banner box in pixels: 10 px in from the top-left corner. Applied with the
// absolute setCoord instead of createFloatingLabel's normalized coords, since a
// corner inset is a pixel quantity and the reconstructed header's top/left
// argument order is ambiguous (the normalized values are overwritten either way).
const int kOverlayX = 10;
const int kOverlayY = 10;
const int kOverlayW = 520;
const int kOverlayH = 26;

MyGUI::Window*  g_overlayBox   = 0; // container: geometry + layer attachment
MyGUI::TextBox* g_overlay      = 0; // the label that actually draws the text
int             g_overlayState = -1;
std::string     g_overlayText;

int overlayColorId(int state) { return state == 2 ? 0 : (state == 1 ? 2 : 1); }

// Put the freshly-minted container in its pixel box and mint the label inside it.
// createLabelAbs takes its text by const-ref and MyGUI::Align is a trivial int
// wrapper (no destructor), so this whole frame is SEH-safe - the same rule
// dbgColourSeh follows for MyGUI::Colour.
MyGUI::TextBox* overlayBuildSeh(MyGUI::Window* box, const std::string* text) {
    __try {
        box->setCoord(kOverlayX, kOverlayY, kOverlayW, kOverlayH);
        box->setVisible(true);
        MyGUI::TextBox* l = ::gui->createLabelAbs(box, 0, 0, kOverlayW, kOverlayH,
                                                  *text, MyGUI::Align::Left);
        if (l) {
            l->setTextAlign(MyGUI::Align::Left);
            l->setVisible(true);
        }
        return l;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// Caption + colour in place. MyGUI::UString owns a buffer (destructor => C2712),
// so the caller builds it outside this frame and passes a pointer. false = the
// widget faulted; the caller then treats the pointer as dead.
bool overlayUpdateSeh(MyGUI::TextBox* l, const MyGUI::UString* text,
                      const MyGUI::Colour* col) {
    __try {
        l->setCaption(*text);
        l->setTextColour(*col);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Destroying the container takes its label child with it (MyGUI owns the subtree).
void overlayDestroySeh(ForgottenGUI* g, MyGUI::Window* box) {
    __try { g->destroyWidget(box); } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
} // namespace

void coopOverlayTick(const char* text, int state, bool show) {
    ForgottenGUI* g = ::gui;
    if (!g) return;

    if (!show) {
        if (g_overlayBox) {
            overlayDestroySeh(g, g_overlayBox);
            g_overlayBox = 0; g_overlay = 0;
            g_overlayState = -1; g_overlayText.clear();
        }
        return;
    }

    std::string t = text ? std::string(text) : std::string();
    if (!g_overlay) {
        // createFloatingLabel takes the layer BY VALUE (an unwindable temporary
        // => C2712), so the container mint stays outside SEH, exactly like
        // createDatapanel above; ::gui was verified non-null. Layer MUST be "Info"
        // for the same reason the panel uses it - nothing draws on "Windows".
        if (g_overlayBox) { overlayDestroySeh(g, g_overlayBox); g_overlayBox = 0; }
        std::string layer = "Info";
        std::string empty;
        g_overlayBox = g->createFloatingLabel(0.01f, 0.01f, 0.30f, 0.03f, empty,
                                              MyGUI::Align::Default, layer);
        if (!g_overlayBox) {
            coop::logErrLine("[coop-ui] banner container FAILED");
            return;
        }
        g_overlay = overlayBuildSeh(g_overlayBox, &t);
        char b[96];
        _snprintf(b, sizeof(b) - 1, "[coop-ui] banner box=%p label=%p",
                  (void*)g_overlayBox, (void*)g_overlay);
        b[sizeof(b) - 1] = '\0';
        coop::logLine(b);
        if (!g_overlay) {
            coop::logErrLine("[coop-ui] banner label FAILED");
            return;
        }
        g_overlayState = -1;   // no caller state is -1: forces the caption pass
        g_overlayText.clear();
    }

    if (t != g_overlayText || state != g_overlayState) {
        MyGUI::Colour col; markerColour(overlayColorId(state), &col);
        MyGUI::UString u(t.c_str());
        if (overlayUpdateSeh(g_overlay, &u, &col)) {
            g_overlayText = t; g_overlayState = state;
        } else {
            // The GUI destroyed the widgets under us - clearGUI() on a world load
            // empties the layer and notifies nobody, so a pointer held across
            // ticks dangles silently (same lesson as the ScreenLabel registry
            // note above). Forget them and re-mint on the next tick.
            g_overlayBox = 0; g_overlay = 0;
            g_overlayState = -1; g_overlayText.clear();
        }
    }
}

} // namespace engine
} // namespace coop
