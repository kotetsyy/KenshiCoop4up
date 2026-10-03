#ifndef KENSHICOOP_UI_NATIVE_EDIT_H
#define KENSHICOOP_UI_NATIVE_EDIT_H

// Native text/focus/game-input seam for the F2 panel's editable fields.
//
// ABI ownership rules (v100 /MD against the game's MyGUIEngine_x64.dll):
// - Text is read through the engine's own exported EditBox::getCaption() and
//   UString::asUTF8_c_str(); we copy the engine-owned UTF-8 bytes and never copy
//   a UString or assume its layout. getOnlyText() is avoided: it returns a
//   UString by value into our frame.
// - Text is written through a UString built by the engine's exported
//   constructor (engine allocator owns its buffer); the engine only gets a ref.
// - Every engine access sits in a POD-only __try helper (no unwinding objects,
//   C2712); objects with destructors stay outside SEH.

#include <cstddef>
#include <excpt.h>

#include <kenshi/Globals.h>       // ::key (address of the game's static InputHandler)
#include <kenshi/InputHandler.h>  // InputHandler::controlEnabled (0xD0)
#include <mygui/MyGUI_EditBox.h>
#include <mygui/MyGUI_InputManager.h>
#include <mygui/MyGUI_UString.h>

namespace coopui {
namespace native {
namespace detail {

// One EditBox caption character sequence, in UString code units (UTF-16):
// a 4-byte UTF-8 sequence is a surrogate pair, i.e. two units.
inline const char* skipCodeUnits(const char* s, int units) {
    while (units > 0 && *s) {
        const unsigned char lead = static_cast<unsigned char>(*s++);
        while ((static_cast<unsigned char>(*s) & 0xC0) == 0x80) ++s;
        units -= lead >= 0xF0 ? 2 : 1;
    }
    return s;
}

// 1 = copied, 0 = engine fault, -1 = does not fit. The caption holds tagged
// text (typed '#' is stored as "##", "#RRGGBB" is a colour tag); this yields
// exactly what EditBox::getOnlyText() would (MyGUI TextIterator::getOnlyText).
inline int readSeh(MyGUI::EditBox* edit, char* out, size_t capacity) {
    __try {
        const char* s = edit->MyGUI::EditBox::getCaption().asUTF8_c_str();
        if (!s) return 0;
        size_t n = 0;
        while (*s) {
            const char c = *s++;
            if (c == '#') {
                if (!*s) break;                       // lone trailing '#': dropped
                if (*s != '#') {                      // colour tag: '#' + 6 units
                    s = skipCodeUnits(s, 6);
                    continue;
                }
                ++s;                                  // "##" -> '#'
            }
            if (n + 1 >= capacity) return -1;
            out[n++] = c;
        }
        out[n] = 0;
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

inline bool setOnlyTextSeh(MyGUI::EditBox* edit, const MyGUI::UString* text) {
    __try {
        edit->MyGUI::EditBox::setOnlyText(*text);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

inline MyGUI::Widget* focusWidgetSeh() {
    __try {
        MyGUI::InputManager* input = MyGUI::InputManager::getInstancePtr();
        return input ? input->getKeyFocusWidget() : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

inline bool focusSeh(MyGUI::Widget* widget) {
    __try {
        MyGUI::InputManager* input = MyGUI::InputManager::getInstancePtr();
        if (!input) return false;
        // Unchanged focus: no event round-trip through the engine.
        if (input->getKeyFocusWidget() != widget) input->setKeyFocusWidget(widget);
        return input->getKeyFocusWidget() == widget;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

inline void releaseFocusSeh(MyGUI::Widget* root) {
    __try {
        MyGUI::InputManager* input = MyGUI::InputManager::getInstancePtr();
        if (!input) return;
        MyGUI::Widget* focused = input->getKeyFocusWidget();
        // Bounded walk: a corrupt parent cycle must not hang the frame.
        MyGUI::Widget* w = focused;
        for (int depth = 0; w && depth < 64; ++depth, w = w->getParent()) {
            if (w == root) {
                input->resetKeyFocusWidget(focused);
                return;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// On focus transfer, another native EditBox owns the game's input suppression.
inline bool editHasFocusSeh() {
    __try {
        MyGUI::InputManager* input = MyGUI::InputManager::getInstancePtr();
        MyGUI::Widget* focused = input ? input->getKeyFocusWidget() : 0;
        return focused && focused->isType<MyGUI::EditBox>();
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Returns the prior controlEnabled (0/1), or -1 on fault. set < 0 only reads.
inline int controlEnabledSeh(InputHandler* handler, int set) {
    __try {
        const int prior = handler->controlEnabled ? 1 : 0;
        if (set >= 0 && prior != set) handler->controlEnabled = set != 0;
        return prior;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

// Game-input lease. Kenshi gates its bound commands (camera, hotkeys, Esc menu)
// on InputHandler::controlEnabled while MyGUI still receives keys; its own
// text fields (faction name) clear it on key focus. We record the owned
// handler and its prior value so release restores exactly what we replaced.
struct GameInputLease {
    InputHandler* owner;  // 0 = not held
    bool prior;
};

inline GameInputLease& gameInputLease() {
    static GameInputLease lease = { 0, true };  // constant-initialised POD
    return lease;
}

}  // namespace detail

// Copies the field's user text (tags removed, like getOnlyText) as UTF-8 into
// out. False on null/invalid widget, engine fault or when the text plus NUL
// does not fit: never truncates; out is "" on failure when capacity > 0.
inline bool read(MyGUI::EditBox* edit, char* out, size_t capacity) {
    if (!out || capacity == 0) return false;
    out[0] = 0;
    if (!edit) return false;
    if (detail::readSeh(edit, out, capacity) == 1) return true;
    out[0] = 0;
    return false;
}

// Replaces the field's text with plain UTF-8 (a '#' stays literal). False on
// null input, ill-formed UTF-8 (the engine's UString throws) or engine fault.
// The engine still applies the field's setMaxTextLength cap.
inline bool write(MyGUI::EditBox* edit, const char* utf8) {
    if (!edit || !utf8) return false;
    try {
        const MyGUI::UString text(utf8);  // engine-exported ctor/dtor own the buffer
        return detail::setOnlyTextSeh(edit, &text);
    } catch (...) {
        return false;
    }
}

// MyGUI key-focus widget, 0 when none or MyGUI is not up.
inline MyGUI::Widget* focusWidget() { return detail::focusWidgetSeh(); }

// Gives widget key focus; no engine call when it already has it.
inline bool focus(MyGUI::Widget* widget) {
    return widget ? detail::focusSeh(widget) : false;
}

// Drops key focus only when it sits on root or one of its descendants;
// focus held by any unrelated widget is left alone.
inline void releaseFocus(MyGUI::Widget* root) {
    if (root) detail::releaseFocusSeh(root);
}

// Main thread only. active = one of the panel's visible fields has key focus:
// takes the lease (clears controlEnabled) and keeps it cleared while held.
// Inactive: restores the prior value on the owned handler, but only if our
// cleared value is still in effect. A changed ::key drops the lease without
// touching the old object.
inline void guardGameInput(bool active) {
    detail::GameInputLease& lease = detail::gameInputLease();
    InputHandler* live = ::key;
    if (lease.owner && lease.owner != live) lease.owner = 0;
    if (active) {
        if (!live) return;
        const int prior = detail::controlEnabledSeh(live, 0);
        if (prior < 0 || lease.owner) return;  // fault, or re-assert while held
        lease.owner = live;
        lease.prior = prior != 0;
        return;
    }
    if (!lease.owner) return;
    if (!detail::editHasFocusSeh() && detail::controlEnabledSeh(live, -1) == 0)
        detail::controlEnabledSeh(live, lease.prior ? 1 : 0);
    lease.owner = 0;
}

}  // namespace native
}  // namespace coopui

#endif  // KENSHICOOP_UI_NATIVE_EDIT_H
