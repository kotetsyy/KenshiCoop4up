// EngineUi.cpp - the in-game marker HUD plane: the debug marker ScreenLabels
// (KENSHICOOP_DEBUG_MARKERS) and the damage floaters. Split out of
// EngineEntity.cpp (Phase 5e code motion, 2026-07-19) so the entity
// capture/resolve/apply TU stays focused on sync. The F2 co-op panel, its
// clipboard/font plumbing and the connection banner live in KenshiCoopUI.dll
// (src/ui/CoopUi.cpp) behind the C ABI in src/ui/CoopUiApi.h.
//
// Owner state: anon-namespace SEH shims only (marker colour +
// create/update/destroy shims). Must NOT: define g_* engine pointers
// (EngineInternal.cpp owns them), install hooks, or change any log string. The
// public marker* declarations stay in Engine.h (the Replicator uses them for
// KENSHICOOP_DEBUG_MARKERS); only their definitions live here.

#include "EngineInternal.h"

#include <kenshi/OptionsHolder.h> // options->damageFloaters
#include <windows.h>
#include <string>

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

} // namespace engine
} // namespace coop
