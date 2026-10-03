// CoopUi.cpp - KenshiCoopUI.dll: the F2 co-op session panel (native Kenshi
// window with native edit fields), its built-in Cyrillic fonts, and the
// persistent connection banner. Moved out of KenshiCoop.dll so the UI can be
// rebuilt and updated on its own while the game is closed. It is never
// hot-unloaded: initialize pins the module until process exit, because MyGUI
// delegates, the widget unlinker and cached widget state point into it.
//
// Boundary (CoopUiApi.h, plain C ABI): each main-thread tick the core hands in
// a POD snapshot and reads back at most one POD command after tick returns.
// This module owns no network or config state, installs no hooks and starts
// no threads. Borrowed snapshot strings are copied before they are kept past
// the tick. Log lines go to the host callback; the "[coop-ui] ..." phrasing is
// consumed by the harness, so keep it stable.

#include "CoopUiApi.h"
#include "NativeEdit.h"

#include <kenshi/Globals.h>          // ::gui (ForgottenGUI*, KenshiLib data export)
#include <kenshi/gui/ForgottenGUI.h>
#include <kenshi/gui/DatapanelGUI.h>
#include <mygui/MyGUI_Button.h>
#include <mygui/MyGUI_EditBox.h>
#include <mygui/MyGUI_Delegate.h>    // MyGUI::newDelegate + CDelegate* (free-fn callbacks)
#include <mygui/MyGUI_Align.h>
#include <mygui/MyGUI_IUnlinkWidget.h>
#include <mygui/MyGUI_InputManager.h>
#include <mygui/MyGUI_ResourceManager.h>
#include <mygui/MyGUI_WidgetManager.h>
#include <mygui/MyGUI_Window.h>
#include <mygui/MyGUI_XmlDocument.h>
#include <sstream>
#include <windows.h>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../plugin/core/SteamId.h"     // parseSteamId64 (host id field) + maskSteamId64 (own id)
#include "../plugin/core/UdpEndpoint.h" // parseUdpEndpoint (host address field)
#include "../plugin/core/PlayerNick.h"  // parsePlayerNick (nick field)

// ---- In-game co-op session panel (F2) ----------------------------------------
// One native Kenshi window (createDatapanel: rust frame, caption, close button)
// toggled with F2. Its client area holds widgets minted with ForgottenGUI's
// own factories and Kenshi's installed skins, laid out once per build in two
// columns above a footer:
//   left   = ACTIONS: role and transport selectors (gold = selected), the nick
//            field, the one endpoint field the chosen flow needs - each a real
//            native EditBox with a small Paste button (own Steam ID: Copy) -
//            and one stateful primary action (create / connect / cancel /
//            stop / disconnect);
//   right  = RESULT: one primary status, role / link, real milestones derived
//            from the snapshot's network and world phases, transfer progress,
//            the real roster, and the mod-updater line as its own section.
//            Folded diagnostics replace milestones + roster while open;
//   footer = version + hide hint, Diagnostics, Copy report, Hide.
// Widgets persist while the window lives: a tick only rewrites the captions,
// colours and visibility that changed, at most 10 times a second.
//
// Text is Russian and written as hex-escaped UTF-8 (the v100 toolchain has no
// source-charset switch; raw Cyrillic would be re-encoded through the ANSI code
// page). Kenshi's own fonts carry no Cyrillic glyphs, so every text widget of
// the panel and banner uses a built-in KenshiCoop_* font (see loadPanelFont).
//
// Fields are normal MyGUI EditBoxes: native caret, selection, typing and
// Ctrl+A/C/V/X. Their text is read and written only through NativeEdit.h
// (engine-exported accessors inside POD-only SEH). A field's text is its
// DRAFT; the remembered (canonical) values change only when a draft parses
// valid and the player is not editing it, or on a start, and go to the core
// as REMEMBER / CONNECT. Start needs every visible field valid, so a cleared
// or malformed field never falls back to an older value. While one of the
// panel's fields has key focus, game hotkeys are held off with
// native::guardGameInput; hide, GUI reset and shutdown release focus and lease.
// Enter in a field only ever starts (idle + valid), Tab / Shift+Tab cycle the
// visible editable fields.
//
// Role / transport / endpoint are locked while the network is busy (launch
// accepted or worker alive); the displayed choice then mirrors the armed
// config. F2, Esc, the window close button and "Hide" only hide the window -
// never disconnect. Widget callbacks only change panel state and queue work;
// the next tick turns it into the single CoopUiCommand the core executes after
// tick returns, so no engine window is created or destroyed from inside its own
// MyGUI event and no network call is made from here.
//
// Lifetime: a world load may destroy the GUI layer under the panel without
// telling anyone. A MyGUI unlinker reports the destruction of the frame or of
// any panel widget; the panel then forgets every pointer without touching it,
// releases focus and game input, and is rebuilt (drafts kept) on the next F2.
//
// SEH discipline (spike 47/48): the mutation calls take std::string by const-ref
// or PODs, so they sit inside __try frames that construct no std::string
// temporaries (C2712). createDatapanel(top, left, ...) takes its layer BY VALUE,
// so the frame is created outside SEH; ::gui is verified non-null first.

namespace {

void (COOP_UI_CALL* g_hostLog)(const char* utf8, int error) = 0;

void logLine(const char* s) {
    if (g_hostLog && s) g_hostLog(s, 0);
}
void logErrLine(const char* s) {
    if (g_hostLog && s) g_hostLog(s, 1);
}

// ---- Fonts ---------------------------------------------------------------------
// Banner font: unchanged since the banner shipped.
const std::string kPanelFont = "KenshiCoop_Interface";
const int         kFontHeight = 16;

// Panel fonts, registered at their pixel size (Resolution 72 => Size == px) so
// setFontHeight never rescales glyphs.
enum FontId { F_BODY, F_SMALL, F_HEAD, F_BIG, F_COUNT };
struct FontSpec { const char* name; const char* source; int px; };
const FontSpec kFonts[F_COUNT] = {
    { "KenshiCoop_Body",  "Exo2-SemiBold.ttf", 19 },
    { "KenshiCoop_Small", "Exo2-Medium.ttf",   17 },
    { "KenshiCoop_Head",  "Exo2-Bold.ttf",     19 },
    { "KenshiCoop_Big",   "Exo2-Bold.ttf",     23 },
};

const std::string& fontName(int f) {
    static std::string names[F_COUNT];
    if (names[f].empty()) names[f] = kFonts[f].name;
    return names[f];
}

bool loadPanelFontSeh(MyGUI::xml::Document* document, std::istream* stream,
                      const std::string* source) {
    __try {
        if (!document->open(*stream)) return false;
        MyGUI::ResourceManager::getInstance().loadFromXmlNode(
            document->getRoot(), *source, MyGUI::Version(1, 1));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void appendFontXml(std::string& xml, const char* name, const char* source, int size,
                   int resolution) {
    char b[768];
    _snprintf(b, sizeof(b) - 1,
              "<Resource type=\"ResourceTrueTypeFont\" name=\"%s\">"
              "<Property key=\"Source\" value=\"%s\"/>"
              "<Property key=\"Size\" value=\"%d\"/>"
              "<Property key=\"Hinting\" value=\"use_native\"/>"
              "<Property key=\"Resolution\" value=\"%d\"/>"
              "<Property key=\"Antialias\" value=\"false\"/>"
              "<Property key=\"TabWidth\" value=\"4\"/>"
              "<Property key=\"SubstituteCode\" value=\"63\"/>"
              "<Property key=\"Distance\" value=\"2\"/>"
              "<Codes><Code range=\"32 126\"/><Code range=\"1024 1279\"/></Codes>"
              "</Resource>",
              name, source, size, resolution);
    b[sizeof(b) - 1] = '\0';
    xml += b;
}

bool loadPanelFont() {
    // Keep the font definitions in the DLL: DLL-only updates and remote test
    // installs must not depend on a separately deployed UI asset. The TTF
    // sources are the game's own data/gui/fonts.
    std::string xml = "<MyGUI type=\"Resource\" version=\"1.1\">";
    appendFontXml(xml, kPanelFont.c_str(), "Exo2-SemiBold.ttf", 24, 50);
    for (int i = 0; i < F_COUNT; ++i)
        appendFontXml(xml, kFonts[i].name, kFonts[i].source, kFonts[i].px, 72);
    xml += "</MyGUI>";
    MyGUI::xml::Document document;
    std::istringstream stream(xml);
    const std::string source = "KenshiCoop built-in font";
    return loadPanelFontSeh(&document, &stream, &source);
}

bool g_fontReady = false;

// Registers the built-in fonts on first success; retried by later callers while
// it keeps failing (the panel refuses to open without them).
bool ensurePanelFont() {
    if (g_fontReady) return true;
    g_fontReady = loadPanelFont();
    if (!g_fontReady) logErrLine("[coop-ui] built-in Cyrillic font registration failed");
    return g_fontReady;
}

// Switch one text widget's font through the VIRTUAL setters: an EditBox keeps
// its text in the Client child's text layer and overrides setFontName /
// setFontHeight to reach it; an explicitly qualified TextBox:: call would only
// touch the EditBox's own, absent, text layer. false = not a text widget or
// it faulted.
bool textFontSeh(MyGUI::Widget* w, const std::string* font, int px) {
    if (!w) return false;
    __try {
        if (!w->isType<MyGUI::TextBox>()) return false;
        MyGUI::TextBox* t = static_cast<MyGUI::TextBox*>(w);
        t->setFontName(*font);
        t->setFontHeight(px);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

size_t childCountSeh(MyGUI::Widget* w) {
    __try { return w->getChildCount(); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

MyGUI::Widget* childAtSeh(MyGUI::Widget* w, size_t i) {
    __try { return w->getChildAt(i); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

MyGUI::Widget* frameCaptionWidgetSeh(DatapanelGUI* panel) {
    __try {
        MyGUI::Window* window = static_cast<MyGUI::Window*>(panel->win);
        return window ? window->getCaptionWidget() : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// The caption widget itself plus its direct children: the window skin may carry
// the visible caption text on a child TextBox.
void frameFont(DatapanelGUI* panel) {
    if (!panel || !g_fontReady) return;
    MyGUI::Widget* w = frameCaptionWidgetSeh(panel);
    if (!w) return;
    textFontSeh(w, &fontName(F_HEAD), kFonts[F_HEAD].px);
    size_t kids = childCountSeh(w);
    if (kids > 8) kids = 8;
    for (size_t i = 0; i < kids; ++i)
        textFontSeh(childAtSeh(w, i), &fontName(F_HEAD), kFonts[F_HEAD].px);
}

void panelShowSeh(DatapanelGUI* panel, bool visible) {
    if (!panel) return;
    __try { panel->_NV_show(visible); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Write UTF-8 text to the Windows clipboard as CF_UNICODETEXT (Windows then
// synthesizes CF_TEXT for ANSI readers). UTF-8 through CF_TEXT would turn the
// Russian report into mojibake. Win32 only (no MyGUI).
bool clipboardSetText(const char* text) {
    if (!text) return false;
    int wn = MultiByteToWideChar(CP_UTF8, 0, text, -1, 0, 0);
    if (wn <= 0) return false;
    if (!OpenClipboard(0)) return false;
    bool ok = false;
    if (EmptyClipboard()) {
        HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)wn * sizeof(wchar_t));
        if (h) {
            wchar_t* dst = (wchar_t*)GlobalLock(h);
            if (dst) {
                MultiByteToWideChar(CP_UTF8, 0, text, -1, dst, wn);
                GlobalUnlock(h);
                if (SetClipboardData(CF_UNICODETEXT, h)) ok = true; // clipboard now owns h
            }
            if (!ok) GlobalFree(h); // ownership not transferred on failure
        }
    }
    CloseClipboard();
    return ok;
}

// Read text from the Windows clipboard into out (the small Paste buttons; the
// fields' own Ctrl+V is native). Prefers CF_UNICODETEXT (what the Steam overlay /
// browsers usually publish) and falls back to CF_TEXT, converting to UTF-8.
// Win32 only (no MyGUI). Returns true iff some text was retrieved.
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

// Panel text (UTF-8). The comment after each constant is the literal it encodes.
namespace ru {
const char kTitle[] =
    "\xD0\xA1\xD0\x9E\xD0\x92\xD0\x9C\xD0\x95\xD0\xA1\xD0\xA2\xD0\x9D\xD0\x90\xD0\xAF \xD0\x98\xD0\x93\xD0\xA0\xD0\x90"; // СОВМЕСТНАЯ ИГРА
const char kTitleHint[] =
    "F2 \xD0\xB8\xD0\xBB\xD0\xB8 Esc - \xD1\x81\xD0\xBA\xD1\x80\xD1\x8B\xD1\x82\xD1\x8C"; // F2 или Esc - скрыть
const char kHdrMode[] =
    "\xD0\xA0\xD0\x95\xD0\x96\xD0\x98\xD0\x9C"; // РЕЖИМ
const char kRoleHost[] =
    "\xD0\xA5\xD0\x9E\xD0\xA1\xD0\xA2"; // ХОСТ
const char kRoleJoin[] =
    "\xD0\x9A\xD0\x9B\xD0\x98\xD0\x95\xD0\x9D\xD0\xA2"; // КЛИЕНТ
const char kHdrNick[] =
    "\xD0\x92\xD0\x90\xD0\xA8 \xD0\x9D\xD0\x98\xD0\x9A"; // ВАШ НИК
const char kNickHint[] =
    "\xD0\x9D\xD0\xB8\xD0\xBA \xD0\xB8\xD0\xB3\xD1\x80\xD0\xBE\xD0\xBA\xD0\xB0, \xD0\xB2\xD0\xB8\xD0\xB4\xD0\xB8\xD0\xBC\xD1\x8B\xD0\xB9 \xD0\xB4\xD1\x80\xD1\x83\xD0\xB3\xD0\xB8\xD0\xBC \xD0\xB2 \xD1\x81\xD0\xB5\xD1\x81\xD1\x81\xD0\xB8\xD0\xB8."; // Ник игрока, видимый другим в сессии.
const char kHdrTransport[] =
    "\xD0\xA1\xD0\x9F\xD0\x9E\xD0\xA1\xD0\x9E\xD0\x91 \xD0\x9F\xD0\x9E\xD0\x94\xD0\x9A\xD0\x9B\xD0\xAE\xD0\xA7\xD0\x95\xD0\x9D\xD0\x98\xD0\xAF"; // СПОСОБ ПОДКЛЮЧЕНИЯ
const char kTrSteam[] =
    "STEAM"; // STEAM
const char kTrUdp[] =
    "\xD0\x9F\xD0\xA0\xD0\xAF\xD0\x9C\xD0\x9E\xD0\x99 IP"; // ПРЯМОЙ IP
const char kHdrSelfId[] =
    "\xD0\x92\xD0\x90\xD0\xA8 STEAM ID"; // ВАШ STEAM ID
const char kHdrHostPort[] =
    "\xD0\x9F\xD0\x9E\xD0\xA0\xD0\xA2 UDP"; // ПОРТ UDP
const char kHdrHostId[] =
    "STEAM ID \xD0\xA5\xD0\x9E\xD0\xA1\xD0\xA2\xD0\x90"; // STEAM ID ХОСТА
const char kHdrHostAddr[] =
    "\xD0\x90\xD0\x94\xD0\xA0\xD0\x95\xD0\xA1 \xD0\xA5\xD0\x9E\xD0\xA1\xD0\xA2\xD0\x90 (IP:\xD0\x9F\xD0\x9E\xD0\xA0\xD0\xA2)"; // АДРЕС ХОСТА (IP:ПОРТ)
const char kPaste[] =
    "\xD0\x92\xD0\xA1\xD0\xA2\xD0\x90\xD0\x92\xD0\x98\xD0\xA2\xD0\xAC"; // ВСТАВИТЬ
const char kCopy[] =
    "\xD0\x9A\xD0\x9E\xD0\x9F\xD0\x98\xD0\xA0\xD0\x9E\xD0\x92\xD0\x90\xD0\xA2\xD0\xAC"; // КОПИРОВАТЬ
const char kSteamMissing[] =
    "Steam \xD0\xBD\xD0\xB5 \xD0\xBD\xD0\xB0\xD0\xB9\xD0\xB4\xD0\xB5\xD0\xBD"; // Steam не найден
const char kHostHintSteam[] =
    "\xD0\x9E\xD1\x82\xD0\xBF\xD1\x80\xD0\xB0\xD0\xB2\xD1\x8C\xD1\x82\xD0\xB5 \xD1\x8D\xD1\x82\xD0\xBE\xD1\x82 Steam ID \xD0\xB4\xD1\x80\xD1\x83\xD0\xB7\xD1\x8C\xD1\x8F\xD0\xBC. \xD0\x9A \xD1\x85\xD0\xBE\xD1\x81\xD1\x82\xD1\x83 \xD0\xBC\xD0\xBE\xD0\xB3\xD1\x83\xD1\x82 \xD0\xBF\xD0\xBE\xD0\xB4\xD0\xBA\xD0\xBB\xD1\x8E\xD1\x87\xD0\xB8\xD1\x82\xD1\x8C\xD1\x81\xD1\x8F \xD0\xB4\xD0\xBE 3 \xD0\xB8\xD0\xB3\xD1\x80\xD0\xBE\xD0\xBA\xD0\xBE\xD0\xB2."; // Отправьте этот Steam ID друзьям. К хосту могут подключиться до 3 игроков.
const char kHostHintUdp[] =
    "\xD0\x94\xD1\x80\xD1\x83\xD0\xB7\xD1\x8C\xD1\x8F \xD0\xBF\xD0\xBE\xD0\xB4\xD0\xBA\xD0\xBB\xD1\x8E\xD1\x87\xD0\xB0\xD1\x8E\xD1\x82\xD1\x81\xD1\x8F \xD0\xBA \xD0\xB2\xD0\xB0\xD1\x88\xD0\xB5\xD0\xBC\xD1\x83 IP \xD0\xB8 \xD1\x8D\xD1\x82\xD0\xBE\xD0\xBC\xD1\x83 \xD0\xBF\xD0\xBE\xD1\x80\xD1\x82\xD1\x83. \xD0\xA7\xD0\xB5\xD1\x80\xD0\xB5\xD0\xB7 \xD0\xB8\xD0\xBD\xD1\x82\xD0\xB5\xD1\x80\xD0\xBD\xD0\xB5\xD1\x82 \xD0\xBD\xD1\x83\xD0\xB6\xD0\xB5\xD0\xBD \xD0\xBF\xD1\x80\xD0\xBE\xD0\xB1\xD1\x80\xD0\xBE\xD1\x81 UDP-\xD0\xBF\xD0\xBE\xD1\x80\xD1\x82\xD0\xB0."; // Друзья подключаются к вашему IP и этому порту. Через интернет нужен проброс UDP-порта.
const char kJoinHintSteam[] =
    "\xD0\x9D\xD1\x83\xD0\xB6\xD0\xB5\xD0\xBD SteamID64 \xD1\x85\xD0\xBE\xD1\x81\xD1\x82\xD0\xB0 - 17 \xD1\x86\xD0\xB8\xD1\x84\xD1\x80, \xD0\xB0 \xD0\xBD\xD0\xB5 \xD0\xB8\xD0\xBC\xD1\x8F \xD0\xB0\xD0\xBA\xD0\xBA\xD0\xB0\xD1\x83\xD0\xBD\xD1\x82\xD0\xB0 \xD0\xB8 \xD0\xBD\xD0\xB5 Lobby ID."; // Нужен SteamID64 хоста - 17 цифр, а не имя аккаунта и не Lobby ID.
const char kJoinHintUdp[] =
    "\xD0\x9D\xD0\xB0\xD0\xBF\xD1\x80\xD0\xB8\xD0\xBC\xD0\xB5\xD1\x80 192.168.1.50:27800. \xD0\x91\xD0\xB5\xD0\xB7 \xD0\xBF\xD0\xBE\xD1\x80\xD1\x82\xD0\xB0 \xD0\xB1\xD1\x83\xD0\xB4\xD0\xB5\xD1\x82 27800."; // Например 192.168.1.50:27800. Без порта будет 27800.
const char kErrNickEmpty[] =
    "\xD0\x92\xD0\xB2\xD0\xB5\xD0\xB4\xD0\xB8\xD1\x82\xD0\xB5 \xD0\xBD\xD0\xB8\xD0\xBA."; // Введите ник.
const char kErrNickLong[] =
    "\xD0\x9D\xD0\xB8\xD0\xBA \xD1\x81\xD0\xBB\xD0\xB8\xD1\x88\xD0\xBA\xD0\xBE\xD0\xBC \xD0\xB4\xD0\xBB\xD0\xB8\xD0\xBD\xD0\xBD\xD1\x8B\xD0\xB9."; // Ник слишком длинный.
const char kErrNickBad[] =
    "\xD0\xAD\xD1\x82\xD0\xBE\xD1\x82 \xD0\xBD\xD0\xB8\xD0\xBA \xD0\xB7\xD0\xB0\xD0\xBD\xD1\x8F\xD1\x82 \xD1\x81\xD0\xBB\xD1\x83\xD0\xB6\xD0\xB5\xD0\xB1\xD0\xBD\xD1\x8B\xD0\xBC \xD1\x81\xD0\xBB\xD0\xBE\xD0\xB2\xD0\xBE\xD0\xBC, \xD0\xB2\xD1\x8B\xD0\xB1\xD0\xB5\xD1\x80\xD0\xB8\xD1\x82\xD0\xB5 \xD0\xB4\xD1\x80\xD1\x83\xD0\xB3\xD0\xBE\xD0\xB9."; // Этот ник занят служебным словом, выберите другой.
const char kErrIdEmpty[] =
    "\xD0\x92\xD0\xB2\xD0\xB5\xD0\xB4\xD0\xB8\xD1\x82\xD0\xB5 Steam ID \xD1\x85\xD0\xBE\xD1\x81\xD1\x82\xD0\xB0."; // Введите Steam ID хоста.
const char kErrIdBad[] =
    "\xD0\xAD\xD1\x82\xD0\xBE \xD0\xBD\xD0\xB5 SteamID64: \xD0\xBD\xD1\x83\xD0\xB6\xD0\xBD\xD0\xBE 17 \xD1\x86\xD0\xB8\xD1\x84\xD1\x80, \xD0\xBD\xD0\xB0\xD1\x87\xD0\xB8\xD0\xBD\xD0\xB0\xD1\x8F \xD1\x81 76561."; // Это не SteamID64: нужно 17 цифр, начиная с 76561.
const char kErrAddrEmpty[] =
    "\xD0\x92\xD0\xB2\xD0\xB5\xD0\xB4\xD0\xB8\xD1\x82\xD0\xB5 \xD0\xB0\xD0\xB4\xD1\x80\xD0\xB5\xD1\x81 \xD1\x85\xD0\xBE\xD1\x81\xD1\x82\xD0\xB0."; // Введите адрес хоста.
const char kErrAddrBad[] =
    "\xD0\x9D\xD0\xB5\xD0\xB2\xD0\xB5\xD1\x80\xD0\xBD\xD1\x8B\xD0\xB9 \xD0\xB0\xD0\xB4\xD1\x80\xD0\xB5\xD1\x81. \xD0\xA4\xD0\xBE\xD1\x80\xD0\xBC\xD0\xB0\xD1\x82 IP:\xD0\xBF\xD0\xBE\xD1\x80\xD1\x82, \xD0\xBD\xD0\xB0\xD0\xBF\xD1\x80\xD0\xB8\xD0\xBC\xD0\xB5\xD1\x80 192.168.1.50:27800."; // Неверный адрес. Формат IP:порт, например 192.168.1.50:27800.
const char kErrPortEmpty[] =
    "\xD0\x92\xD0\xB2\xD0\xB5\xD0\xB4\xD0\xB8\xD1\x82\xD0\xB5 \xD0\xBF\xD0\xBE\xD1\x80\xD1\x82 UDP."; // Введите порт UDP.
const char kErrPortBad[] =
    "\xD0\x9F\xD0\xBE\xD1\x80\xD1\x82 - \xD1\x87\xD0\xB8\xD1\x81\xD0\xBB\xD0\xBE \xD0\xBE\xD1\x82 1 \xD0\xB4\xD0\xBE 65535."; // Порт - число от 1 до 65535.
const char kErrUnreadable[] =
    "\xD0\x9F\xD0\xBE\xD0\xBB\xD0\xB5 \xD0\xBD\xD0\xB5 \xD1\x87\xD0\xB8\xD1\x82\xD0\xB0\xD0\xB5\xD1\x82\xD1\x81\xD1\x8F - \xD0\xBE\xD1\x82\xD0\xBA\xD1\x80\xD0\xBE\xD0\xB9\xD1\x82\xD0\xB5 \xD0\xBE\xD0\xBA\xD0\xBD\xD0\xBE \xD0\xB7\xD0\xB0\xD0\xBD\xD0\xBE\xD0\xB2\xD0\xBE (F2)."; // Поле не читается - откройте окно заново (F2).
const char kErrClipEmpty[] =
    "\xD0\x91\xD1\x83\xD1\x84\xD0\xB5\xD1\x80 \xD0\xBE\xD0\xB1\xD0\xBC\xD0\xB5\xD0\xBD\xD0\xB0 \xD0\xBF\xD1\x83\xD1\x81\xD1\x82 \xD0\xB8\xD0\xBB\xD0\xB8 \xD0\xBD\xD0\xB5\xD0\xB4\xD0\xBE\xD1\x81\xD1\x82\xD1\x83\xD0\xBF\xD0\xB5\xD0\xBD."; // Буфер обмена пуст или недоступен.
const char kErrClipSteamId[] =
    "\xD0\x92 \xD0\xB1\xD1\x83\xD1\x84\xD0\xB5\xD1\x80\xD0\xB5 \xD0\xBE\xD0\xB1\xD0\xBC\xD0\xB5\xD0\xBD\xD0\xB0 \xD0\xBD\xD0\xB5\xD1\x82 Steam ID (17 \xD1\x86\xD0\xB8\xD1\x84\xD1\x80)."; // В буфере обмена нет Steam ID (17 цифр).
const char kErrClipAddr[] =
    "\xD0\x92 \xD0\xB1\xD1\x83\xD1\x84\xD0\xB5\xD1\x80\xD0\xB5 \xD0\xBE\xD0\xB1\xD0\xBC\xD0\xB5\xD0\xBD\xD0\xB0 \xD0\xBD\xD0\xB5\xD1\x82 \xD0\xB0\xD0\xB4\xD1\x80\xD0\xB5\xD1\x81\xD0\xB0 \xD0\xB2\xD0\xB8\xD0\xB4\xD0\xB0 IP:\xD0\xBF\xD0\xBE\xD1\x80\xD1\x82."; // В буфере обмена нет адреса вида IP:порт.
const char kErrClipPort[] =
    "\xD0\x92 \xD0\xB1\xD1\x83\xD1\x84\xD0\xB5\xD1\x80\xD0\xB5 \xD0\xBE\xD0\xB1\xD0\xBC\xD0\xB5\xD0\xBD\xD0\xB0 \xD0\xBD\xD0\xB5\xD1\x82 \xD0\xBD\xD0\xBE\xD0\xBC\xD0\xB5\xD1\x80\xD0\xB0 \xD0\xBF\xD0\xBE\xD1\x80\xD1\x82\xD0\xB0 (1-65535)."; // В буфере обмена нет номера порта (1-65535).
const char kErrClipNick[] =
    "\xD0\x92 \xD0\xB1\xD1\x83\xD1\x84\xD0\xB5\xD1\x80\xD0\xB5 \xD0\xBE\xD0\xB1\xD0\xBC\xD0\xB5\xD0\xBD\xD0\xB0 \xD0\xBD\xD0\xB5\xD1\x82 \xD0\xBF\xD0\xBE\xD0\xB4\xD1\x85\xD0\xBE\xD0\xB4\xD1\x8F\xD1\x89\xD0\xB5\xD0\xB3\xD0\xBE \xD0\xB8\xD0\xBC\xD0\xB5\xD0\xBD\xD0\xB8."; // В буфере обмена нет подходящего имени.
const char kOkCopyId[] =
    "Steam ID \xD1\x81\xD0\xBA\xD0\xBE\xD0\xBF\xD0\xB8\xD1\x80\xD0\xBE\xD0\xB2\xD0\xB0\xD0\xBD \xD0\xB2 \xD0\xB1\xD1\x83\xD1\x84\xD0\xB5\xD1\x80 \xD0\xBE\xD0\xB1\xD0\xBC\xD0\xB5\xD0\xBD\xD0\xB0."; // Steam ID скопирован в буфер обмена.
const char kOkReport[] =
    "\xD0\x9E\xD1\x82\xD1\x87\xD1\x91\xD1\x82 \xD1\x81\xD0\xBA\xD0\xBE\xD0\xBF\xD0\xB8\xD1\x80\xD0\xBE\xD0\xB2\xD0\xB0\xD0\xBD \xD0\xB2 \xD0\xB1\xD1\x83\xD1\x84\xD0\xB5\xD1\x80 \xD0\xBE\xD0\xB1\xD0\xBC\xD0\xB5\xD0\xBD\xD0\xB0."; // Отчёт скопирован в буфер обмена.
const char kErrCopy[] =
    "\xD0\x9D\xD0\xB5 \xD1\x83\xD0\xB4\xD0\xB0\xD0\xBB\xD0\xBE\xD1\x81\xD1\x8C \xD0\xB7\xD0\xB0\xD0\xBF\xD0\xB8\xD1\x81\xD0\xB0\xD1\x82\xD1\x8C \xD0\xB2 \xD0\xB1\xD1\x83\xD1\x84\xD0\xB5\xD1\x80 \xD0\xBE\xD0\xB1\xD0\xBC\xD0\xB5\xD0\xBD\xD0\xB0."; // Не удалось записать в буфер обмена.
const char kActCreate[] =
    "\xD0\xA1\xD0\x9E\xD0\x97\xD0\x94\xD0\x90\xD0\xA2\xD0\xAC \xD0\xA1\xD0\x95\xD0\xA1\xD0\xA1\xD0\x98\xD0\xAE"; // СОЗДАТЬ СЕССИЮ
const char kActConnect[] =
    "\xD0\x9F\xD0\x9E\xD0\x94\xD0\x9A\xD0\x9B\xD0\xAE\xD0\xA7\xD0\x98\xD0\xA2\xD0\xAC\xD0\xA1\xD0\xAF"; // ПОДКЛЮЧИТЬСЯ
const char kActCancelStart[] =
    "\xD0\x9E\xD0\xA2\xD0\x9C\xD0\x95\xD0\x9D\xD0\x98\xD0\xA2\xD0\xAC \xD0\x97\xD0\x90\xD0\x9F\xD0\xA3\xD0\xA1\xD0\x9A"; // ОТМЕНИТЬ ЗАПУСК
const char kActCancel[] =
    "\xD0\x9E\xD0\xA2\xD0\x9C\xD0\x95\xD0\x9D\xD0\x98\xD0\xA2\xD0\xAC \xD0\x9F\xD0\x9E\xD0\x94\xD0\x9A\xD0\x9B\xD0\xAE\xD0\xA7\xD0\x95\xD0\x9D\xD0\x98\xD0\x95"; // ОТМЕНИТЬ ПОДКЛЮЧЕНИЕ
const char kActStopHost[] =
    "\xD0\x9E\xD0\xA1\xD0\xA2\xD0\x90\xD0\x9D\xD0\x9E\xD0\x92\xD0\x98\xD0\xA2\xD0\xAC \xD0\xA1\xD0\x95\xD0\xA1\xD0\xA1\xD0\x98\xD0\xAE"; // ОСТАНОВИТЬ СЕССИЮ
const char kActDisconnect[] =
    "\xD0\x9E\xD0\xA2\xD0\x9A\xD0\x9B\xD0\xAE\xD0\xA7\xD0\x98\xD0\xA2\xD0\xAC\xD0\xA1\xD0\xAF"; // ОТКЛЮЧИТЬСЯ
const char kLockedNote[] =
    "\xD0\x9F\xD0\xBE\xD0\xBA\xD0\xB0 \xD1\x81\xD0\xB5\xD1\x81\xD1\x81\xD0\xB8\xD1\x8F \xD0\xB8\xD0\xB4\xD1\x91\xD1\x82, \xD1\x80\xD0\xBE\xD0\xBB\xD1\x8C, \xD1\x81\xD0\xBF\xD0\xBE\xD1\x81\xD0\xBE\xD0\xB1 \xD0\xB8 \xD0\xB0\xD0\xB4\xD1\x80\xD0\xB5\xD1\x81 \xD0\xBD\xD0\xB5 \xD0\xBC\xD0\xB5\xD0\xBD\xD1\x8F\xD1\x8E\xD1\x82\xD1\x81\xD1\x8F."; // Пока сессия идёт, роль, способ и адрес не меняются.
const char kEnterHint[] =
    "Enter \xD0\xB2 \xD0\xBF\xD0\xBE\xD0\xBB\xD0\xB5 - \xD1\x82\xD0\xBE\xD0\xB6\xD0\xB5 \xD0\xB7\xD0\xB0\xD0\xBF\xD1\x83\xD1\x81\xD0\xBA."; // Enter в поле - тоже запуск.
const char kHide[] =
    "\xD0\xA1\xD0\xBA\xD1\x80\xD1\x8B\xD1\x82\xD1\x8C (F2)"; // Скрыть (F2)
const char kHdrState[] =
    "\xD0\xA1\xD0\x9E\xD0\xA1\xD0\xA2\xD0\x9E\xD0\xAF\xD0\x9D\xD0\x98\xD0\x95"; // СОСТОЯНИЕ
const char kRowRole[] =
    "\xD0\xA0\xD0\xBE\xD0\xBB\xD1\x8C"; // Роль
const char kRowLink[] =
    "\xD0\xA1\xD0\xBE\xD0\xB5\xD0\xB4\xD0\xB8\xD0\xBD\xD0\xB5\xD0\xBD\xD0\xB8\xD0\xB5"; // Соединение
const char kValHost[] =
    "\xD0\xA5\xD0\xBE\xD1\x81\xD1\x82"; // Хост
const char kValJoin[] =
    "\xD0\x9A\xD0\xBB\xD0\xB8\xD0\xB5\xD0\xBD\xD1\x82"; // Клиент
const char kValSteam[] =
    "Steam"; // Steam
const char kValUdp[] =
    "\xD0\x9F\xD1\x80\xD1\x8F\xD0\xBC\xD0\xBE\xD0\xB9 IP (UDP)"; // Прямой IP (UDP)
const char kChanPending[] =
    "\xD0\xB2\xD1\x8B\xD0\xB1\xD0\xB8\xD1\x80\xD0\xB0\xD0\xB5\xD1\x82\xD1\x81\xD1\x8F..."; // выбирается...
const char kStOffline[] =
    "\xD0\x9D\xD0\xB5 \xD0\xB2 \xD1\x81\xD0\xB5\xD1\x82\xD0\xB8"; // Не в сети
const char kStStartingHost[] =
    "\xD0\x97\xD0\xB0\xD0\xBF\xD1\x83\xD1\x81\xD0\xBA \xD1\x81\xD0\xB5\xD1\x81\xD1\x81\xD0\xB8\xD0\xB8..."; // Запуск сессии...
const char kStStartingJoin[] =
    "\xD0\x97\xD0\xB0\xD0\xBF\xD1\x83\xD1\x81\xD0\xBA \xD0\xBF\xD0\xBE\xD0\xB4\xD0\xBA\xD0\xBB\xD1\x8E\xD1\x87\xD0\xB5\xD0\xBD\xD0\xB8\xD1\x8F..."; // Запуск подключения...
const char kStHostingWait[] =
    "\xD0\xA1\xD0\xB5\xD1\x81\xD1\x81\xD0\xB8\xD1\x8F \xD0\xB7\xD0\xB0\xD0\xBF\xD1\x83\xD1\x89\xD0\xB5\xD0\xBD\xD0\xB0, \xD0\xB6\xD0\xB4\xD1\x91\xD0\xBC \xD0\xB8\xD0\xB3\xD1\x80\xD0\xBE\xD0\xBA\xD0\xBE\xD0\xB2"; // Сессия запущена, ждём игроков
const char kStHostingPlayers[] =
    "\xD0\xA1\xD0\xB5\xD1\x81\xD1\x81\xD0\xB8\xD1\x8F \xD0\xB7\xD0\xB0\xD0\xBF\xD1\x83\xD1\x89\xD0\xB5\xD0\xBD\xD0\xB0"; // Сессия запущена
const char kStHostNoWorld[] =
    "\xD0\xA1\xD0\xB5\xD1\x81\xD1\x81\xD0\xB8\xD1\x8F \xD0\xB7\xD0\xB0\xD0\xBF\xD1\x83\xD1\x89\xD0\xB5\xD0\xBD\xD0\xB0: \xD0\xB7\xD0\xB0\xD0\xB3\xD1\x80\xD1\x83\xD0\xB7\xD0\xB8\xD1\x82\xD0\xB5 \xD0\xB8\xD0\xBB\xD0\xB8 \xD0\xBD\xD0\xB0\xD1\x87\xD0\xBD\xD0\xB8\xD1\x82\xD0\xB5 \xD0\xB8\xD0\xB3\xD1\x80\xD1\x83"; // Сессия запущена: загрузите или начните игру
const char kStHostSending[] =
    "\xD0\xA1\xD0\xB5\xD1\x81\xD1\x81\xD0\xB8\xD1\x8F \xD0\xB7\xD0\xB0\xD0\xBF\xD1\x83\xD1\x89\xD0\xB5\xD0\xBD\xD0\xB0: \xD0\xBC\xD0\xB8\xD1\x80 \xD0\xBE\xD1\x82\xD0\xBF\xD1\x80\xD0\xB0\xD0\xB2\xD0\xBB\xD1\x8F\xD0\xB5\xD1\x82\xD1\x81\xD1\x8F \xD0\xB8\xD0\xB3\xD1\x80\xD0\xBE\xD0\xBA\xD1\x83"; // Сессия запущена: мир отправляется игроку
const char kStHostLoading[] =
    "\xD0\xA1\xD0\xB5\xD1\x81\xD1\x81\xD0\xB8\xD1\x8F \xD0\xB7\xD0\xB0\xD0\xBF\xD1\x83\xD1\x89\xD0\xB5\xD0\xBD\xD0\xB0: \xD0\xB7\xD0\xB0\xD0\xB3\xD1\x80\xD1\x83\xD0\xB7\xD0\xBA\xD0\xB0 \xD0\xBC\xD0\xB8\xD1\x80\xD0\xB0"; // Сессия запущена: загрузка мира
const char kStConnecting[] =
    "\xD0\x9F\xD0\xBE\xD0\xB4\xD0\xBA\xD0\xBB\xD1\x8E\xD1\x87\xD0\xB5\xD0\xBD\xD0\xB8\xD0\xB5 \xD0\xBA \xD1\x85\xD0\xBE\xD1\x81\xD1\x82\xD1\x83..."; // Подключение к хосту...
const char kStHandshake[] =
    "\xD0\xA1\xD0\xB2\xD1\x8F\xD0\xB7\xD1\x8C \xD1\x81 \xD1\x85\xD0\xBE\xD1\x81\xD1\x82\xD0\xBE\xD0\xBC \xD0\xB5\xD1\x81\xD1\x82\xD1\x8C, \xD1\x81\xD0\xBE\xD0\xB3\xD0\xBB\xD0\xB0\xD1\x81\xD0\xBE\xD0\xB2\xD0\xB0\xD0\xBD\xD0\xB8\xD0\xB5..."; // Связь с хостом есть, согласование...
const char kStConnected[] =
    "\xD0\x9F\xD0\xBE\xD0\xB4\xD0\xBA\xD0\xBB\xD1\x8E\xD1\x87\xD0\xB5\xD0\xBD\xD0\xBE \xD0\xBA \xD1\x85\xD0\xBE\xD1\x81\xD1\x82\xD1\x83"; // Подключено к хосту
const char kStConnWaiting[] =
    "\xD0\x9F\xD0\xBE\xD0\xB4\xD0\xBA\xD0\xBB\xD1\x8E\xD1\x87\xD0\xB5\xD0\xBD\xD0\xBE, \xD0\xB6\xD0\xB4\xD1\x91\xD0\xBC \xD0\xBC\xD0\xB8\xD1\x80 \xD1\x85\xD0\xBE\xD1\x81\xD1\x82\xD0\xB0"; // Подключено, ждём мир хоста
const char kStConnPreparing[] =
    "\xD0\x9F\xD0\xBE\xD0\xB4\xD0\xBA\xD0\xBB\xD1\x8E\xD1\x87\xD0\xB5\xD0\xBD\xD0\xBE, \xD1\x85\xD0\xBE\xD1\x81\xD1\x82 \xD0\xB3\xD0\xBE\xD1\x82\xD0\xBE\xD0\xB2\xD0\xB8\xD1\x82 \xD0\xBC\xD0\xB8\xD1\x80"; // Подключено, хост готовит мир
const char kStConnReceiving[] =
    "\xD0\x9F\xD0\xBE\xD0\xB4\xD0\xBA\xD0\xBB\xD1\x8E\xD1\x87\xD0\xB5\xD0\xBD\xD0\xBE, \xD0\xBF\xD0\xBE\xD0\xBB\xD1\x83\xD1\x87\xD0\xB5\xD0\xBD\xD0\xB8\xD0\xB5 \xD0\xBC\xD0\xB8\xD1\x80\xD0\xB0 \xD1\x85\xD0\xBE\xD1\x81\xD1\x82\xD0\xB0"; // Подключено, получение мира хоста
const char kStConnLoading[] =
    "\xD0\x9F\xD0\xBE\xD0\xB4\xD0\xBA\xD0\xBB\xD1\x8E\xD1\x87\xD0\xB5\xD0\xBD\xD0\xBE, \xD0\xB7\xD0\xB0\xD0\xB3\xD1\x80\xD1\x83\xD0\xB7\xD0\xBA\xD0\xB0 \xD0\xBC\xD0\xB8\xD1\x80\xD0\xB0"; // Подключено, загрузка мира
const char kStConnReady[] =
    "\xD0\x92 \xD0\xB8\xD0\xB3\xD1\x80\xD0\xB5: \xD0\xBC\xD0\xB8\xD1\x80 \xD0\xB7\xD0\xB0\xD0\xB3\xD1\x80\xD1\x83\xD0\xB6\xD0\xB5\xD0\xBD"; // В игре: мир загружен
const char kStConnWorldFailed[] =
    "\xD0\x9F\xD0\xBE\xD0\xB4\xD0\xBA\xD0\xBB\xD1\x8E\xD1\x87\xD0\xB5\xD0\xBD\xD0\xBE, \xD0\xBD\xD0\xBE \xD0\xBC\xD0\xB8\xD1\x80 \xD0\xBD\xD0\xB5 \xD0\xB7\xD0\xB0\xD0\xB3\xD1\x80\xD1\x83\xD0\xB7\xD0\xB8\xD0\xBB\xD1\x81\xD1\x8F"; // Подключено, но мир не загрузился
const char kStReconnecting[] =
    "\xD0\xA1\xD0\xB2\xD1\x8F\xD0\xB7\xD1\x8C \xD0\xBF\xD0\xBE\xD1\x82\xD0\xB5\xD1\x80\xD1\x8F\xD0\xBD\xD0\xB0, \xD0\xBF\xD0\xB5\xD1\x80\xD0\xB5\xD0\xBF\xD0\xBE\xD0\xB4\xD0\xBA\xD0\xBB\xD1\x8E\xD1\x87\xD0\xB5\xD0\xBD\xD0\xB8\xD0\xB5..."; // Связь потеряна, переподключение...
const char kStFailedHost[] =
    "\xD0\x9D\xD0\xB5 \xD1\x83\xD0\xB4\xD0\xB0\xD0\xBB\xD0\xBE\xD1\x81\xD1\x8C \xD0\xB7\xD0\xB0\xD0\xBF\xD1\x83\xD1\x81\xD1\x82\xD0\xB8\xD1\x82\xD1\x8C \xD1\x81\xD0\xB5\xD1\x81\xD1\x81\xD0\xB8\xD1\x8E"; // Не удалось запустить сессию
const char kStFailedJoin[] =
    "\xD0\x9D\xD0\xB5 \xD1\x83\xD0\xB4\xD0\xB0\xD0\xBB\xD0\xBE\xD1\x81\xD1\x8C \xD0\xBF\xD0\xBE\xD0\xB4\xD0\xBA\xD0\xBB\xD1\x8E\xD1\x87\xD0\xB8\xD1\x82\xD1\x8C\xD1\x81\xD1\x8F"; // Не удалось подключиться
const char kLinkNone[] =
    "\xD0\xBD\xD0\xB5\xD1\x82"; // нет
const char kLinkConnecting[] =
    "\xD0\xBF\xD0\xBE\xD0\xB4\xD0\xBA\xD0\xBB\xD1\x8E\xD1\x87\xD0\xB5\xD0\xBD\xD0\xB8\xD0\xB5..."; // подключение...
const char kLinkHandshake[] =
    "\xD1\x81\xD0\xBE\xD0\xB3\xD0\xBB\xD0\xB0\xD1\x81\xD0\xBE\xD0\xB2\xD0\xB0\xD0\xBD\xD0\xB8\xD0\xB5..."; // согласование...
const char kLinkUp[] =
    "\xD1\x83\xD1\x81\xD1\x82\xD0\xB0\xD0\xBD\xD0\xBE\xD0\xB2\xD0\xBB\xD0\xB5\xD0\xBD\xD0\xBE"; // установлено
const char kLinkRetry[] =
    "\xD0\xBF\xD0\xB5\xD1\x80\xD0\xB5\xD0\xBF\xD0\xBE\xD0\xB4\xD0\xBA\xD0\xBB\xD1\x8E\xD1\x87\xD0\xB5\xD0\xBD\xD0\xB8\xD0\xB5..."; // переподключение...
const char kLinkListening[] =
    "\xD0\xBE\xD1\x82\xD0\xBA\xD1\x80\xD1\x8B\xD1\x82\xD0\xBE \xD0\xB4\xD0\xBB\xD1\x8F \xD0\xB8\xD0\xB3\xD1\x80\xD0\xBE\xD0\xBA\xD0\xBE\xD0\xB2"; // открыто для игроков
const char kLinkFailed[] =
    "\xD0\xBE\xD1\x88\xD0\xB8\xD0\xB1\xD0\xBA\xD0\xB0"; // ошибка
const char kHdrStepsJoin[] =
    "\xD0\xAD\xD0\xA2\xD0\x90\xD0\x9F\xD0\xAB \xD0\x9F\xD0\x9E\xD0\x94\xD0\x9A\xD0\x9B\xD0\xAE\xD0\xA7\xD0\x95\xD0\x9D\xD0\x98\xD0\xAF"; // ЭТАПЫ ПОДКЛЮЧЕНИЯ
const char kHdrStepsHost[] =
    "\xD0\xAD\xD0\xA2\xD0\x90\xD0\x9F\xD0\xAB \xD0\xA1\xD0\x95\xD0\xA1\xD0\xA1\xD0\x98\xD0\x98"; // ЭТАПЫ СЕССИИ
const char kStepLink[] =
    "\xD0\xA1\xD0\xB2\xD1\x8F\xD0\xB7\xD1\x8C \xD1\x81 \xD1\x85\xD0\xBE\xD1\x81\xD1\x82\xD0\xBE\xD0\xBC"; // Связь с хостом
const char kStepWorldGet[] =
    "\xD0\x9C\xD0\xB8\xD1\x80 \xD1\x85\xD0\xBE\xD1\x81\xD1\x82\xD0\xB0"; // Мир хоста
const char kStepWorldLoad[] =
    "\xD0\x97\xD0\xB0\xD0\xB3\xD1\x80\xD1\x83\xD0\xB7\xD0\xBA\xD0\xB0 \xD0\xBC\xD0\xB8\xD1\x80\xD0\xB0"; // Загрузка мира
const char kStepSession[] =
    "\xD0\xA1\xD0\xB5\xD1\x81\xD1\x81\xD0\xB8\xD1\x8F \xD0\xBE\xD1\x82\xD0\xBA\xD1\x80\xD1\x8B\xD1\x82\xD0\xB0"; // Сессия открыта
const char kStepOwnWorld[] =
    "\xD0\x92\xD0\xB0\xD1\x88 \xD0\xBC\xD0\xB8\xD1\x80 \xD0\xB7\xD0\xB0\xD0\xB3\xD1\x80\xD1\x83\xD0\xB6\xD0\xB5\xD0\xBD"; // Ваш мир загружен
const char kStepPlayers[] =
    "\xD0\x98\xD0\xB3\xD1\x80\xD0\xBE\xD0\xBA\xD0\xB8 \xD0\xB2 \xD1\x81\xD0\xB5\xD1\x81\xD1\x81\xD0\xB8\xD0\xB8"; // Игроки в сессии
const char kSWait[] =
    "\xD0\xBE\xD0\xB6\xD0\xB8\xD0\xB4\xD0\xB0\xD0\xBD\xD0\xB8\xD0\xB5"; // ожидание
const char kSDone[] =
    "\xD0\xB3\xD0\xBE\xD1\x82\xD0\xBE\xD0\xB2\xD0\xBE"; // готово
const char kSStarting[] =
    "\xD0\xB7\xD0\xB0\xD0\xBF\xD1\x83\xD1\x81\xD0\xBA..."; // запуск...
const char kSFailed[] =
    "\xD0\xBE\xD1\x88\xD0\xB8\xD0\xB1\xD0\xBA\xD0\xB0"; // ошибка
const char kSWaitHost[] =
    "\xD0\xB6\xD0\xB4\xD1\x91\xD0\xBC \xD1\x85\xD0\xBE\xD1\x81\xD1\x82\xD0\xB0"; // ждём хоста
const char kSHostPreparing[] =
    "\xD1\x85\xD0\xBE\xD1\x81\xD1\x82 \xD0\xB3\xD0\xBE\xD1\x82\xD0\xBE\xD0\xB2\xD0\xB8\xD1\x82 \xD0\xBC\xD0\xB8\xD1\x80"; // хост готовит мир
const char kSReceiving[] =
    "\xD0\xBF\xD0\xBE\xD0\xBB\xD1\x83\xD1\x87\xD0\xB5\xD0\xBD\xD0\xB8\xD0\xB5..."; // получение...
const char kSReceived[] =
    "\xD0\xBF\xD0\xBE\xD0\xBB\xD1\x83\xD1\x87\xD0\xB5\xD0\xBD"; // получен
const char kSLoading[] =
    "\xD0\xB7\xD0\xB0\xD0\xB3\xD1\x80\xD1\x83\xD0\xB7\xD0\xBA\xD0\xB0..."; // загрузка...
const char kSInGame[] =
    "\xD0\xB2 \xD0\xB8\xD0\xB3\xD1\x80\xD0\xB5"; // в игре
const char kSLoadOrStart[] =
    "\xD0\xB7\xD0\xB0\xD0\xB3\xD1\x80\xD1\x83\xD0\xB7\xD0\xB8\xD1\x82\xD0\xB5 \xD0\xB8\xD0\xBB\xD0\xB8 \xD0\xBD\xD0\xB0\xD1\x87\xD0\xBD\xD0\xB8\xD1\x82\xD0\xB5 \xD0\xB8\xD0\xB3\xD1\x80\xD1\x83"; // загрузите или начните игру
const char kSWaitPlayers[] =
    "\xD0\xB6\xD0\xB4\xD1\x91\xD0\xBC \xD0\xB8\xD0\xB3\xD1\x80\xD0\xBE\xD0\xBA\xD0\xBE\xD0\xB2"; // ждём игроков
const char kSSending[] =
    "\xD0\xBE\xD1\x82\xD0\xBF\xD1\x80\xD0\xB0\xD0\xB2\xD0\xBA\xD0\xB0 \xD0\xBC\xD0\xB8\xD1\x80\xD0\xB0 \xD0\xB8\xD0\xB3\xD1\x80\xD0\xBE\xD0\xBA\xD1\x83"; // отправка мира игроку
const char kSPlayersFmt[] =
    "\xD0\xBF\xD0\xBE\xD0\xB4\xD0\xBA\xD0\xBB\xD1\x8E\xD1\x87\xD0\xB5\xD0\xBD\xD0\xBE: %d"; // подключено: %d
const char kSNotTracked[] =
    "\xD0\xBD\xD0\xB5 \xD0\xBE\xD1\x82\xD1\x81\xD0\xBB\xD0\xB5\xD0\xB6\xD0\xB8\xD0\xB2\xD0\xB0\xD0\xB5\xD1\x82\xD1\x81\xD1\x8F"; // не отслеживается
const char kProgressFmt[] =
    "%.1f / %.1f \xD0\x9C\xD0\x91 (%d%%)"; // %.1f / %.1f МБ (%d%%)
const char kHdrPlayers[] =
    "\xD0\x98\xD0\x93\xD0\xA0\xD0\x9E\xD0\x9A\xD0\x98"; // ИГРОКИ
const char kYou[] =
    "\xD0\xB2\xD1\x8B"; // вы
const char kHostTag[] =
    "\xD1\x85\xD0\xBE\xD1\x81\xD1\x82"; // хост
const char kPlayerFallback[] =
    "\xD0\x98\xD0\xB3\xD1\x80\xD0\xBE\xD0\xBA %u"; // Игрок %u
const char kPInGame[] =
    "\xD0\xB2 \xD0\xB8\xD0\xB3\xD1\x80\xD0\xB5"; // в игре
const char kPNotReady[] =
    "\xD0\xBC\xD0\xB8\xD1\x80 \xD0\xBD\xD0\xB5 \xD0\xB3\xD0\xBE\xD1\x82\xD0\xBE\xD0\xB2"; // мир не готов
const char kPConnected[] =
    "\xD0\xBF\xD0\xBE\xD0\xB4\xD0\xBA\xD0\xBB\xD1\x8E\xD1\x87\xD1\x91\xD0\xBD"; // подключён
const char kNoSession[] =
    "\xD0\x9D\xD0\xB5\xD1\x82 \xD0\xB0\xD0\xBA\xD1\x82\xD0\xB8\xD0\xB2\xD0\xBD\xD0\xBE\xD0\xB9 \xD1\x81\xD0\xB5\xD1\x81\xD1\x81\xD0\xB8\xD0\xB8."; // Нет активной сессии.
const char kNobodyYet[] =
    "\xD0\x9F\xD0\xBE\xD0\xBA\xD0\xB0 \xD0\xBD\xD0\xB8\xD0\xBA\xD1\x82\xD0\xBE \xD0\xBD\xD0\xB5 \xD0\xBF\xD0\xBE\xD0\xB4\xD0\xBA\xD0\xBB\xD1\x8E\xD1\x87\xD0\xB8\xD0\xBB\xD1\x81\xD1\x8F. \xD0\x9F\xD0\xB5\xD1\x80\xD0\xB5\xD0\xB4\xD0\xB0\xD0\xB9\xD1\x82\xD0\xB5 \xD0\xB4\xD1\x80\xD1\x83\xD0\xB3\xD1\x83 \xD0\xB4\xD0\xB0\xD0\xBD\xD0\xBD\xD1\x8B\xD0\xB5 \xD0\xB4\xD0\xBB\xD1\x8F \xD0\xBF\xD0\xBE\xD0\xB4\xD0\xBA\xD0\xBB\xD1\x8E\xD1\x87\xD0\xB5\xD0\xBD\xD0\xB8\xD1\x8F \xD0\xB8\xD0\xB7 \xD0\xBB\xD0\xB5\xD0\xB2\xD0\xBE\xD0\xB9 \xD0\xBA\xD0\xBE\xD0\xBB\xD0\xBE\xD0\xBD\xD0\xBA\xD0\xB8."; // Пока никто не подключился. Передайте другу данные для подключения из левой колонки.
const char kHdrUpdate[] =
    "\xD0\x9E\xD0\x91\xD0\x9D\xD0\x9E\xD0\x92\xD0\x9B\xD0\x95\xD0\x9D\xD0\x98\xD0\x95 \xD0\x9C\xD0\x9E\xD0\x94\xD0\x90"; // ОБНОВЛЕНИЕ МОДА
const char kUpdate[] =
    "\xD0\x9E\xD0\xB1\xD0\xBD\xD0\xBE\xD0\xB2\xD0\xBB\xD0\xB5\xD0\xBD\xD0\xB8\xD0\xB5: "; // Обновление: 
const char kDiagShow[] =
    "\xD0\x94\xD0\xB8\xD0\xB0\xD0\xB3\xD0\xBD\xD0\xBE\xD1\x81\xD1\x82\xD0\xB8\xD0\xBA\xD0\xB0 [+]"; // Диагностика [+]
const char kDiagHide[] =
    "\xD0\x94\xD0\xB8\xD0\xB0\xD0\xB3\xD0\xBD\xD0\xBE\xD1\x81\xD1\x82\xD0\xB8\xD0\xBA\xD0\xB0 [-]"; // Диагностика [-]
const char kHdrDiag[] =
    "\xD0\x94\xD0\x98\xD0\x90\xD0\x93\xD0\x9D\xD0\x9E\xD0\xA1\xD0\xA2\xD0\x98\xD0\x9A\xD0\x90"; // ДИАГНОСТИКА
const char kCopyReport[] =
    "\xD0\x9A\xD0\xBE\xD0\xBF\xD0\xB8\xD1\x80\xD0\xBE\xD0\xB2\xD0\xB0\xD1\x82\xD1\x8C \xD0\xBE\xD1\x82\xD1\x87\xD1\x91\xD1\x82"; // Копировать отчёт
const char kDiagNone[] =
    "\xD0\x94\xD0\xB8\xD0\xB0\xD0\xB3\xD0\xBD\xD0\xBE\xD1\x81\xD1\x82\xD0\xB8\xD0\xBA\xD0\xB0 \xD0\xBD\xD0\xB5\xD0\xB4\xD0\xBE\xD1\x81\xD1\x82\xD1\x83\xD0\xBF\xD0\xBD\xD0\xB0"; // Диагностика недоступна
const char kRepError[] =
    "\xD0\x9E\xD1\x88\xD0\xB8\xD0\xB1\xD0\xBA\xD0\xB0"; // Ошибка
const char kRepDetail[] =
    "\xD0\x9E\xD0\xBF\xD0\xB8\xD1\x81\xD0\xB0\xD0\xBD\xD0\xB8\xD0\xB5"; // Описание
const char kRepPlayers[] =
    "\xD0\x98\xD0\xB3\xD1\x80\xD0\xBE\xD0\xBA\xD0\xB8"; // Игроки
const char kRepDiag[] =
    "\xD0\x94\xD0\xB8\xD0\xB0\xD0\xB3\xD0\xBD\xD0\xBE\xD1\x81\xD1\x82\xD0\xB8\xD0\xBA\xD0\xB0"; // Диагностика
const char kRepSteps[] =
    "\xD0\xAD\xD1\x82\xD0\xB0\xD0\xBF\xD1\x8B"; // Этапы
const char kRepState[] =
    "\xD0\xA1\xD0\xBE\xD1\x81\xD1\x82\xD0\xBE\xD1\x8F\xD0\xBD\xD0\xB8\xD0\xB5"; // Состояние
const char kRepWorldOwn[] =
    "\xD0\x92\xD0\xB0\xD1\x88 \xD0\xBC\xD0\xB8\xD1\x80"; // Ваш мир
const char kRepWorldHost[] =
    "\xD0\x9C\xD0\xB8\xD1\x80 \xD1\x85\xD0\xBE\xD1\x81\xD1\x82\xD0\xB0"; // Мир хоста
} // namespace ru

// ---- Palette ---------------------------------------------------------------------
struct Rgb { float r, g, b; };
const Rgb C_TEXT       = { 0.93f, 0.91f, 0.86f };
const Rgb C_SOFT       = { 0.80f, 0.77f, 0.70f }; // unselected / secondary button text
const Rgb C_MUTED      = { 0.70f, 0.68f, 0.63f };
const Rgb C_DIM        = { 0.55f, 0.53f, 0.50f }; // disabled, still readable
const Rgb C_GOLD       = { 0.91f, 0.73f, 0.42f }; // headings
const Rgb C_SEL        = { 1.00f, 0.88f, 0.60f }; // selected / primary caption
const Rgb C_HOVER      = { 1.00f, 0.95f, 0.84f };
const Rgb C_GOOD       = { 0.50f, 0.88f, 0.45f };
const Rgb C_BUSY       = { 1.00f, 0.80f, 0.30f };
const Rgb C_BAD        = { 1.00f, 0.47f, 0.40f };
const Rgb C_FRAME_GOLD = { 0.84f, 0.62f, 0.27f };
const Rgb C_VEIL_GOLD  = { 0.86f, 0.60f, 0.22f };
const Rgb C_FRAME_RUST = { 0.80f, 0.33f, 0.20f };
const Rgb C_VEIL_RUST  = { 0.72f, 0.26f, 0.16f };
const Rgb C_BLACK      = { 0.00f, 0.00f, 0.00f };
const Rgb C_RULE       = { 0.55f, 0.45f, 0.30f };
const Rgb C_FIELD_TINT = { 0.34f, 0.32f, 0.30f }; // multiplies the input skin to a dark field
const Rgb C_TRACK      = { 0.10f, 0.09f, 0.08f };
const Rgb C_DOT_IDLE   = { 0.42f, 0.40f, 0.37f };

enum Tone { TONE_NORMAL, TONE_TITLE, TONE_MUTED, TONE_GOOD, TONE_BUSY, TONE_BAD };

const Rgb& toneRgb(int tone) {
    switch (tone) {
    case TONE_TITLE: return C_GOLD;
    case TONE_MUTED: return C_MUTED;
    case TONE_GOOD:  return C_GOOD;
    case TONE_BUSY:  return C_BUSY;
    case TONE_BAD:   return C_BAD;
    default:         return C_TEXT;
    }
}

bool sameRgb(const Rgb& a, const Rgb& b) { return a.r == b.r && a.g == b.g && a.b == b.b; }

// ---- UTF-8 text helpers -----------------------------------------------------------
bool utf8Cont(unsigned char c) { return (c & 0xC0) == 0x80; }
size_t utf8Count(const std::string& s) {
    size_t n = 0;
    for (size_t i = 0; i < s.size(); ++i) if (!utf8Cont((unsigned char)s[i])) ++n;
    return n;
}
// Byte length of the first `cols` code points of s.
size_t utf8Prefix(const std::string& s, size_t cols) {
    size_t n = 0, i = 0;
    for (; i < s.size(); ++i) {
        if (!utf8Cont((unsigned char)s[i])) {
            if (n == cols) break;
            ++n;
        }
    }
    return i;
}

// Greedy word wrap on code points; '\n' starts a new line, '\r' is dropped.
void wrapUtf8(const std::string& text, size_t cols, std::vector<std::string>& out) {
    size_t start = 0;
    for (;;) {
        size_t nl = text.find('\n', start);
        if (nl == std::string::npos) nl = text.size();
        std::string line;
        size_t lineCols = 0;
        size_t i = start;
        while (i < nl) {
            size_t j = text.find(' ', i);
            if (j == std::string::npos || j > nl) j = nl;
            std::string word = text.substr(i, j - i);
            i = j + 1;
            size_t cr = word.find('\r');
            if (cr != std::string::npos) word.erase(cr, 1);
            if (word.empty()) continue;
            size_t wc = utf8Count(word);
            if (!line.empty() && lineCols + 1 + wc > cols) {
                out.push_back(line);
                line.clear();
                lineCols = 0;
            }
            while (wc > cols) { // one word longer than a line: hard split
                size_t cut = utf8Prefix(word, cols);
                out.push_back(word.substr(0, cut));
                word.erase(0, cut);
                wc = utf8Count(word);
            }
            if (!line.empty()) { line += ' '; ++lineCols; }
            line += word;
            lineCols += wc;
        }
        if (!line.empty()) out.push_back(line);
        if (nl >= text.size()) break;
        start = nl + 1;
    }
}

// Code points that fit a label of widthPx in font f. Labels are single-line
// TextBoxes, so long sentences are wrapped here instead of being clipped.
size_t colsFor(int widthPx, int font) {
    const int px = kFonts[font].px;
    int c = (widthPx * 100) / (px * 58); // Exo2 averages ~0.55 em per glyph
    return c < 8 ? 8 : (size_t)c;
}

// MyGUI reads '#' in a caption as a colour tag; "##" is a literal '#'.
std::string escapeTags(const std::string& s) {
    if (s.find('#') == std::string::npos) return s;
    std::string out;
    out.reserve(s.size() + 4);
    for (size_t i = 0; i < s.size(); ++i) {
        out += s[i];
        if (s[i] == '#') out += '#';
    }
    return out;
}

std::string decimal(unsigned long long v) {
    char b[32];
    _snprintf(b, sizeof(b) - 1, "%llu", v);
    b[sizeof(b) - 1] = '\0';
    return b;
}

int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// ---- Native widget primitives (POD-only SEH) ------------------------------------------
bool captionSeh(MyGUI::Widget* w, const MyGUI::UString* u) {
    __try { static_cast<MyGUI::TextBox*>(w)->setCaption(*u); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// Manual text colour: MyGUI 3.2 keeps it across Button state changes
// (EditText::mManualColour), so a disabled button stays readable.
bool textColourSeh(MyGUI::Widget* w, const MyGUI::Colour* c) {
    __try { static_cast<MyGUI::TextBox*>(w)->setTextColour(*c); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// Skin colour multiplies the skin's texels (not the text); alpha fades the widget.
bool fillSeh(MyGUI::Widget* w, const MyGUI::Colour* c, float alpha) {
    __try { w->setColour(*c); w->setAlpha(alpha); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool visibleSeh(MyGUI::Widget* w, bool v) {
    __try { w->setVisible(v); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool enabledSeh(MyGUI::Widget* w, bool e) {
    __try { w->setEnabled(e); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool readOnlySeh(MyGUI::Widget* w, bool ro) {
    __try { static_cast<MyGUI::EditBox*>(w)->setEditReadOnly(ro); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool coordSeh(MyGUI::Widget* w, int x, int y, int cw, int ch) {
    __try { w->setCoord(x, y, cw, ch); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
MyGUI::Widget* mouseFocusSeh() {
    __try {
        MyGUI::InputManager* im = MyGUI::InputManager::getInstancePtr();
        return im ? im->getMouseFocusWidget() : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
bool shiftDownSeh() {
    __try {
        MyGUI::InputManager* im = MyGUI::InputManager::getInstancePtr();
        return im ? im->isShiftPressed() : false;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

enum LabelAlign { AL_LEFT, AL_RIGHT };

// The factories take (parent, top, left, ...) per the reconstructed header, whose
// top/left order is not trusted: setCoord then pins the exact client-pixel box.
// Every widget is minted hidden; the first refresh shows what the state needs.
MyGUI::Widget* labelCreateSeh(ForgottenGUI* g, MyGUI::Widget* parent, const std::string* empty,
                              int x, int y, int w, int h, int align) {
    __try {
        MyGUI::TextBox* t = g->createLabelAbs(parent, y, x, w, h, *empty, MyGUI::Align::Left);
        if (!t) return 0;
        t->setCoord(x, y, w, h);
        t->setTextAlign(align == AL_RIGHT ? (MyGUI::Align::Right | MyGUI::Align::VCenter)
                                          : (MyGUI::Align::Left | MyGUI::Align::VCenter));
        t->setNeedMouseFocus(false);
        t->setInheritsPick(false);
        t->setVisible(false);
        return t;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// Buttons, and plain rectangles (WhiteSkin + skin colour) that never take the
// mouse: card backgrounds, selection frames and veils, dots, rules, the bar.
MyGUI::Widget* buttonCreateSeh(ForgottenGUI* g, MyGUI::Widget* parent, const std::string* name,
                               const std::string* empty, const std::string* skin,
                               int x, int y, int w, int h, bool pickable) {
    __try {
        MyGUI::Button* b = g->createButtonAbs(parent, y, x, w, h, *name, *empty, *skin);
        if (!b) return 0;
        b->setCoord(x, y, w, h);
        if (pickable) {
            b->setTextAlign(MyGUI::Align::Center);
        } else {
            b->setNeedMouseFocus(false);
            b->setInheritsPick(false);
        }
        b->setVisible(false);
        return b;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// Same configuration the isolated-game probe proved: single-line, editable,
// not static; native typing, selection and Ctrl+V then just work.
MyGUI::Widget* editCreateSeh(ForgottenGUI* g, MyGUI::Widget* parent, const std::string* name,
                             int x, int y, int w, int h, int maxLen) {
    __try {
        MyGUI::EditBox* e = g->createEditBoxAbs(parent, y, x, w, h, *name, false);
        if (!e) return 0;
        e->setCoord(x, y, w, h);
        e->setEditMultiLine(false);
        e->setEditReadOnly(false);
        e->setEditStatic(false);
        // Inverting light text makes a selection unreadable on the dark input skin.
        e->setInvertSelected(false);
        e->setMaxTextLength(maxLen);
        e->setTextAlign(MyGUI::Align::Left | MyGUI::Align::VCenter);
        e->setVisible(false);
        return e;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// ---- Cached control ----------------------------------------------------------------
// One native widget plus what was last written to it, so a refresh only calls
// into the engine for what changed (a rewrite would reset caret / hover state).
enum CtlKind { K_LABEL, K_BUTTON, K_EDIT, K_RECT };

struct Ctl {
    MyGUI::Widget* w;
    int            kind;
    int            x, y, cw, ch;
    std::string    text;    bool textSet;
    Rgb            col;     bool colSet;  // text colour (rect: skin colour)
    float          alpha;
    int            visible, enabled, readOnly; // -1 unknown
    Rgb            base, hover;           // buttons: text colour at rest / under the mouse
    Ctl() : w(0), kind(K_LABEL), x(0), y(0), cw(0), ch(0), textSet(false), colSet(false),
            alpha(-1.0f), visible(-1), enabled(-1), readOnly(-1) {
        col = base = hover = C_TEXT;
    }
};

void ctlText(Ctl& c, const std::string& s) {
    if (!c.w || (c.textSet && c.text == s)) return;
    try {
        const MyGUI::UString u(escapeTags(s)); // engine-exported ctor owns the buffer
        if (captionSeh(c.w, &u)) { c.text = s; c.textSet = true; }
    } catch (...) {
        // Ill-formed UTF-8 from a snapshot string: keep the old caption.
    }
}
void ctlColour(Ctl& c, const Rgb& col) {
    if (!c.w || (c.colSet && sameRgb(c.col, col))) return;
    const MyGUI::Colour mc(col.r, col.g, col.b, 1.0f);
    if (textColourSeh(c.w, &mc)) { c.col = col; c.colSet = true; }
}
void ctlFill(Ctl& c, const Rgb& col, float alpha) {
    if (!c.w || (c.colSet && sameRgb(c.col, col) && c.alpha == alpha)) return;
    const MyGUI::Colour mc(col.r, col.g, col.b, 1.0f);
    if (fillSeh(c.w, &mc, alpha)) { c.col = col; c.alpha = alpha; c.colSet = true; }
}
void ctlVisible(Ctl& c, bool v) {
    if (!c.w || c.visible == (v ? 1 : 0)) return;
    if (visibleSeh(c.w, v)) c.visible = v ? 1 : 0;
}
void ctlEnabled(Ctl& c, bool e) {
    if (!c.w || c.enabled == (e ? 1 : 0)) return;
    if (enabledSeh(c.w, e)) c.enabled = e ? 1 : 0;
}
void ctlReadOnly(Ctl& c, bool ro) {
    if (!c.w || c.kind != K_EDIT || c.readOnly == (ro ? 1 : 0)) return;
    if (readOnlySeh(c.w, ro)) c.readOnly = ro ? 1 : 0;
}
void ctlCoord(Ctl& c, int x, int y, int cw, int ch) {
    if (!c.w || (c.x == x && c.y == y && c.cw == cw && c.ch == ch)) return;
    if (coordSeh(c.w, x, y, cw, ch)) { c.x = x; c.y = y; c.cw = cw; c.ch = ch; }
}

// A label line: text + colour + shown.
void ctlLine(Ctl& c, const std::string& s, const Rgb& col) {
    ctlText(c, s);
    ctlColour(c, col);
    ctlVisible(c, !s.empty());
}

MyGUI::Widget* g_mouseFocus = 0; // MyGUI mouse focus as of this tick

// Text colour of a button by its rest / hover colours.
void ctlButtonColour(Ctl& c, const Rgb& base, const Rgb& hover) {
    c.base = base;
    c.hover = hover;
    ctlColour(c, (c.w && c.w == g_mouseFocus && c.enabled == 1) ? hover : base);
}

// Fill `n` line labels with wrapped text; overflow ends in "...". Returns lines used.
int fillLines(Ctl* lines, int n, const std::string& text, int font, const Rgb& col) {
    std::vector<std::string> wrapped;
    if (!text.empty() && n > 0 && lines[0].cw > 0)
        wrapUtf8(text, colsFor(lines[0].cw, font), wrapped);
    if ((int)wrapped.size() > n && n > 0) {
        std::string& last = wrapped[n - 1];
        const size_t cols = colsFor(lines[0].cw, font);
        if (utf8Count(last) + 3 > cols) last.erase(utf8Prefix(last, cols > 3 ? cols - 3 : 0));
        last += "...";
    }
    int used = 0;
    for (int i = 0; i < n; ++i) {
        if (i < (int)wrapped.size()) { ctlLine(lines[i], wrapped[i], col); ++used; }
        else ctlVisible(lines[i], false);
    }
    return used;
}

// ---- Panel model ------------------------------------------------------------------
struct Selector { Ctl frame, btn, veil; }; // gold frame behind, veil on top when selected
struct Field    { Ctl ring, edit, button; }; // focus / error ring behind the EditBox

enum { kKvRows = 2, kMsgRows = 3, kStepRows = 3, kPlayerRows = 4, kDiagRowsMax = 24 };

struct PanelWidgets {
    // left
    Ctl      hdrMode;   Selector selHost, selJoin;
    Ctl      hdrNick;   Field nick; Ctl nickNote;
    Ctl      hdrTrans;  Selector selSteam, selUdp;
    Ctl      hdrEndpoint;
    Field    hostId, addr, port;
    Ctl      selfBg, selfText, copyId;
    Ctl      epNote[2];
    Selector primary;   Ctl primaryNote;
    // right
    Ctl      hdrState, stateCard, stateDot, stateText;
    Ctl      kvKey[kKvRows], kvVal[kKvRows];
    Ctl      msg[kMsgRows];
    int      msgRows;
    Ctl      hdrSteps, stepsCard, stepDot[kStepRows], stepName[kStepRows], stepVal[kStepRows];
    Ctl      barTrack, barFill, barText, xferLine;
    Ctl      hdrPlayers, playersCount, playersCard, pName[kPlayerRows], pState[kPlayerRows];
    Ctl      updHdr, upd[2];
    Ctl      diagCard, hdrDiag, diagKey[kDiagRowsMax], diagVal[kDiagRowsMax];
    int      diagRows;
    // chrome
    Ctl      colRule, footRule, footText, btnDiag, btnReport, btnHide;
    PanelWidgets() : msgRows(0), diagRows(0) {}
};

enum PanelAction { ACT_NONE, ACT_START, ACT_STOP };

struct CoopPanelUi {
    DatapanelGUI*  frame;         // the one native outer window
    ForgottenGUI*  gui;           // the GUI that minted it
    MyGUI::Widget* win;           // frame window: root of every panel widget
    bool           built;         // widgets minted
    bool           open;
    bool           seenVisible;   // frame reported visible at least once since open
    bool           hostFlag;      // selected role (mirrors the armed role while busy)
    bool           steamFlag;     // selected transport (mirrors the armed one while busy)
    bool           diagOpen;      // diagnostics view unfolded
    bool           f2Down, escDown;
    bool           closeRequested;
    int            pendingAction;
    CoopPanelUi()
        : frame(0), gui(0), win(0), built(false), open(false), seenVisible(false),
          hostFlag(true), steamFlag(true), diagOpen(false), f2Down(false), escDown(false),
          closeRequested(false), pendingAction(ACT_NONE) {}
};

CoopPanelUi  g_panel;
PanelWidgets g_ui;
bool         g_locked = false;   // last tick: st->busy || st->running
bool         g_dirty = true;     // refresh on this tick, not at the next 10 Hz slot
std::string  g_selfIdStr;        // own SteamID digits ("" = Steam not up)
std::string  g_reportText;       // "Copy report" payload, rebuilt each refresh

// Every widget the panel owns, frame window first. POD on purpose: the unlinker
// may still be called during process teardown, after C++ statics are gone.
const int      kOwnedMax = 200;
MyGUI::Widget* g_owned[kOwnedMax];
int            g_ownedN = 0;
volatile bool  g_panelLost = false; // the GUI destroyed one of g_owned

// MyGUI calls every registered unlinker for each widget it destroys, before the
// memory goes. Only a flag is set here; the next tick forgets the pointers.
class PanelUnlinker : public MyGUI::IUnlinkWidget {
public:
    virtual void _unlinkWidget(MyGUI::Widget* w) {
        if (!w || g_panelLost) return;
        for (int i = 0; i < g_ownedN; ++i)
            if (g_owned[i] == w) { g_panelLost = true; return; }
    }
};
PanelUnlinker* g_unlinker = 0; // allocated once, never freed (module is pinned)

bool registerUnlinkerSeh(MyGUI::IUnlinkWidget* u) {
    __try {
        MyGUI::WidgetManager* wm = MyGUI::WidgetManager::getInstancePtr();
        if (!wm) return false;
        wm->registerUnlinker(u);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// ---- Fields: drafts vs remembered values -----------------------------------------
enum FieldId { FLD_NICK, FLD_HOSTID, FLD_ADDR, FLD_PORT, FLD_COUNT };
enum Verdict { V_OK, V_EMPTY, V_BAD, V_LONG, V_POISON, V_UNREAD };

struct Draft {
    std::string text;    // field text as last read (UTF-8)
    int         verdict;
    Draft() : verdict(V_EMPTY) {}
};
Draft g_draft[FLD_COUNT];

// Parsed value of each draft (meaningful only while its verdict is V_OK).
std::string        g_pNick;
unsigned long long g_pHostId = 0;
std::string        g_pIp;
int                g_pJoinPort = 0;
int                g_pHostPort = 0;

// Remembered (canonical) values: seeded once from the snapshot's config, then
// changed only by committed drafts. These are what REMEMBER persists.
const int          kDefaultPort = 27800;
std::string        g_nick;
unsigned long long g_hostSteamId = 0; // the host a client dials over Steam
std::string        g_udpIp;           // the host a client dials over UDP
int                g_udpPort = kDefaultPort; // one port in the C ABI and saved config
const size_t       kUdpIpCap = sizeof(((CoopUiSettings*)0)->udpIp);
bool               g_memorySeeded = false;
bool               g_rememberPending = false; // settings changed: queue REMEMBER

const int kNickMaxChars = coop::PLAYER_NICK_MAX + 1; // allow overflow to be rejected, not silently accepted
const int kIdMaxChars   = 40;
const int kAddrMaxChars = 255;
const int kPortMaxChars = 8;

Ctl& fieldEdit(int f) {
    switch (f) {
    case FLD_HOSTID: return g_ui.hostId.edit;
    case FLD_ADDR:   return g_ui.addr.edit;
    case FLD_PORT:   return g_ui.port.edit;
    default:         return g_ui.nick.edit;
    }
}

// Trimmed, first line, controls stripped - the bytes parsePlayerNick would keep
// before its 63-byte cut. Longer means the nick would be cut, maybe mid-letter.
bool nickTooLong(const std::string& text) {
    std::string t;
    for (size_t i = 0; i < text.size(); ++i) {
        unsigned char ch = (unsigned char)text[i];
        if (ch == '\r' || ch == '\n') break;
        if (ch < 32 || ch == 127) continue;
        t += (char)ch;
    }
    size_t a = t.find_first_not_of(' ');
    size_t z = t.find_last_not_of(' ');
    return a != std::string::npos && (z - a + 1) > coop::PLAYER_NICK_MAX;
}

bool blank(const std::string& s) { return s.find_first_not_of(" \t\r\n") == std::string::npos; }

// Port draft: a bare 1..65535, nothing else.
bool parsePort(const std::string& text, int& port) {
    size_t a = text.find_first_not_of(" \t");
    size_t z = text.find_last_not_of(" \t");
    if (a == std::string::npos) return false;
    std::string t = text.substr(a, z - a + 1);
    if (t.size() > 5 || t.find_first_not_of("0123456789") != std::string::npos) return false;
    int p = atoi(t.c_str());
    if (p < 1 || p > 65535) return false;
    port = p;
    return true;
}

void validate(int f) {
    Draft& d = g_draft[f];
    if (d.verdict == V_UNREAD) return;
    if (blank(d.text)) { d.verdict = V_EMPTY; return; }
    switch (f) {
    case FLD_NICK: {
        std::string nick;
        if (nickTooLong(d.text))                     d.verdict = V_LONG;
        else if (!coop::parsePlayerNick(d.text, nick)) d.verdict = V_EMPTY;
        else if (coop::isPoisonedNick(nick))         d.verdict = V_POISON;
        else { g_pNick = nick; d.verdict = V_OK; }
        return;
    }
    case FLD_HOSTID: {
        unsigned long long id = 0;
        if (coop::parseSteamId64(d.text, id) && id != 0) { g_pHostId = id; d.verdict = V_OK; }
        else d.verdict = V_BAD;
        return;
    }
    case FLD_ADDR: {
        std::string ip;
        int port = kDefaultPort; // no ":port" given: the default port
        if (coop::parseUdpEndpoint(d.text, ip, port) && !ip.empty() && ip.size() < kUdpIpCap) {
            g_pIp = ip; g_pJoinPort = port; d.verdict = V_OK;
        } else {
            d.verdict = V_BAD;
        }
        return;
    }
    case FLD_PORT: {
        int port = 0;
        if (parsePort(d.text, port)) { g_pHostPort = port; d.verdict = V_OK; }
        else d.verdict = V_BAD;
        return;
    }
    }
}

// Pull every live field's text (10 Hz while open, and right before a start).
void readDrafts() {
    if (!g_panel.built) return;
    char buf[1024];
    for (int f = 0; f < FLD_COUNT; ++f) {
        Ctl& e = fieldEdit(f);
        if (!e.w) continue;
        if (coopui::native::read(static_cast<MyGUI::EditBox*>(e.w), buf, sizeof(buf))) {
            if (g_draft[f].verdict == V_UNREAD || g_draft[f].text != buf) {
                g_draft[f].text = buf;
                g_draft[f].verdict = V_EMPTY;
                validate(f);
            }
        } else if (g_draft[f].verdict != V_UNREAD) {
            g_draft[f].verdict = V_UNREAD;
            logErrLine("[coop-ui] field read FAILED");
        }
    }
}

// Which field holds MyGUI key focus (-1 = none of ours).
int focusedField() {
    if (!g_panel.built) return -1;
    MyGUI::Widget* f = coopui::native::focusWidget();
    if (!f) return -1;
    for (int i = 0; i < FLD_COUNT; ++i)
        if (fieldEdit(i).w == f) return i;
    return -1;
}

// The endpoint field the selected flow needs (-1 = host over Steam: own id).
int activeEndpoint() {
    if (g_panel.hostFlag) return g_panel.steamFlag ? -1 : FLD_PORT;
    return g_panel.steamFlag ? FLD_HOSTID : FLD_ADDR;
}

// Can the selected flow start with the CURRENT drafts?
int firstInvalidField() {
    if (g_draft[FLD_NICK].verdict != V_OK) return FLD_NICK;
    const int ep = activeEndpoint();
    if (ep >= 0 && g_draft[ep].verdict != V_OK) return ep;
    return -1;
}
bool startReady() { return firstInvalidField() < 0; }

// Persisting is the core's job: the next tick hands it the current settings.
void fireRemember() { g_rememberPending = true; }

std::string addrText(const std::string& ip, int port);
void writeField(int f, const std::string& text);

// Valid drafts become the remembered values. A field being edited is skipped
// (`all` = start / hide). Endpoints are not committed while a session holds
// them: the fields then only mirror the armed config.
void commitDrafts(bool all) {
    const int focused = all ? -1 : focusedField();
    if (g_draft[FLD_NICK].verdict == V_OK && focused != FLD_NICK && g_pNick != g_nick) {
        g_nick = g_pNick;
        fireRemember();
        char b[160];
        _snprintf(b, sizeof(b) - 1, "[coop-ui] nick applied '%s'", g_nick.c_str());
        b[sizeof(b) - 1] = '\0';
        logLine(b);
    }
    if (g_locked) return;
    if (g_draft[FLD_HOSTID].verdict == V_OK && focused != FLD_HOSTID && g_pHostId != g_hostSteamId) {
        g_hostSteamId = g_pHostId;
        fireRemember();
        char b[80];
        _snprintf(b, sizeof(b) - 1, "[coop-ui] host steam id set %s",
                  coop::maskSteamId64(g_hostSteamId).c_str());
        b[sizeof(b) - 1] = '\0';
        logLine(b);
    }
    if (g_draft[FLD_ADDR].verdict == V_OK && focused != FLD_ADDR &&
        (g_pIp != g_udpIp || g_pJoinPort != g_udpPort)) {
        g_udpIp = g_pIp;
        g_udpPort = g_pJoinPort;
        writeField(FLD_PORT, decimal((unsigned long long)g_udpPort));
        fireRemember();
        char b[320];
        _snprintf(b, sizeof(b) - 1, "[coop-ui] udp host set %s:%d", g_udpIp.c_str(), g_udpPort);
        b[sizeof(b) - 1] = '\0';
        logLine(b);
    }
    if (g_draft[FLD_PORT].verdict == V_OK && focused != FLD_PORT && g_pHostPort != g_udpPort) {
        g_udpPort = g_pHostPort;
        writeField(FLD_ADDR, addrText(g_udpIp, g_udpPort));
        fireRemember();
        char b[64];
        _snprintf(b, sizeof(b) - 1, "[coop-ui] udp port set %d", g_udpPort);
        b[sizeof(b) - 1] = '\0';
        logLine(b);
    }
}

void seedDraft(int f, const std::string& text) {
    g_draft[f].text = text;
    g_draft[f].verdict = V_EMPTY;
    validate(f);
}

std::string addrText(const std::string& ip, int port) {
    if (ip.empty()) return std::string();
    char b[300];
    _snprintf(b, sizeof(b) - 1, "%s:%d", ip.c_str(), port);
    b[sizeof(b) - 1] = '\0';
    return b;
}

// Once per process: remembered values and the drafts from the core's config.
void seedMemory(const CoopUiSnapshot* st) {
    if (g_memorySeeded) return;
    g_hostSteamId = st->peerSteamId;
    if (st->udpIp && st->udpIp[0] && std::strlen(st->udpIp) < kUdpIpCap) g_udpIp = st->udpIp;
    if (st->udpPort >= 1 && st->udpPort <= 65535) g_udpPort = st->udpPort;
    if (st->playerName && st->playerName[0]) {
        std::string nick;
        if (coop::parsePlayerNick(st->playerName, nick) && !coop::isPoisonedNick(nick))
            g_nick = nick;
    }
    seedDraft(FLD_NICK, g_nick);
    seedDraft(FLD_HOSTID, g_hostSteamId ? decimal(g_hostSteamId) : std::string());
    seedDraft(FLD_ADDR, addrText(g_udpIp, g_udpPort));
    seedDraft(FLD_PORT, decimal((unsigned long long)g_udpPort));
    g_memorySeeded = true;
}

// Rewrite a field (Paste, armed-config mirror, rebuild). Caret / selection of
// that field reset, so never called for the player's own typing.
void writeField(int f, const std::string& text) {
    Ctl& e = fieldEdit(f);
    g_draft[f].text = text;
    g_draft[f].verdict = V_EMPTY;
    if (e.w && !coopui::native::write(static_cast<MyGUI::EditBox*>(e.w), text.c_str())) {
        g_draft[f].verdict = V_UNREAD;
        logErrLine("[coop-ui] field write FAILED");
        return;
    }
    validate(f);
}

// ---- Notes (short-lived feedback) ----------------------------------------------------
enum NoteSlot { NOTE_NICK, NOTE_EP, NOTE_FOOT, NOTE_COUNT };
struct PanelNote {
    std::string text;
    int         tone;
    DWORD       at;
    PanelNote() : tone(TONE_NORMAL), at(0) {}
};
PanelNote g_note[NOTE_COUNT];
const DWORD kNoteMs = 8000;

void setNote(int slot, const char* text, int tone) {
    g_note[slot].text = text;
    g_note[slot].tone = tone;
    g_note[slot].at = GetTickCount();
    g_dirty = true;
}
bool noteLive(int slot) {
    return !g_note[slot].text.empty() && (GetTickCount() - g_note[slot].at) < kNoteMs;
}

// ---- Widget callbacks ---------------------------------------------------------------
// Free functions (MyGUI::newDelegate wraps them). They only change panel state;
// paste / focus work is deferred to the tick, network work to the command.
int  g_pendingPaste = -1; // field whose Paste button was pressed
int  g_tabDir = 0;        // +1 Tab, -1 Shift+Tab, from a field
bool g_enterPressed = false;

void selectRole(bool host) {
    if (g_locked || g_panel.hostFlag == host) return;
    g_panel.hostFlag = host;
    logLine(host ? "[coop-ui] role -> Host" : "[coop-ui] role -> Join");
    fireRemember();
}
void selectTransport(bool steam) {
    if (g_locked || g_panel.steamFlag == steam) return;
    g_panel.steamFlag = steam;
    logLine(steam ? "[coop-ui] transport -> Steam" : "[coop-ui] transport -> UDP");
    fireRemember();
}

void copySelfId() {
    if (g_selfIdStr.empty()) {
        logLine("[coop-ui] copy Steam ID: none (Steam not running)");
        return;
    }
    bool ok = clipboardSetText(g_selfIdStr.c_str());
    setNote(NOTE_EP, ok ? ru::kOkCopyId : ru::kErrCopy, ok ? TONE_GOOD : TONE_BAD);
    logLine(ok ? "[coop-ui] copied Steam ID to clipboard: ok"
               : "[coop-ui] copied Steam ID to clipboard: FAILED");
}

void copyReport() {
    bool ok = clipboardSetText(g_reportText.c_str());
    setNote(NOTE_FOOT, ok ? ru::kOkReport : ru::kErrCopy, ok ? TONE_GOOD : TONE_BAD);
    logLine(ok ? "[coop-ui] copied report to clipboard: ok"
               : "[coop-ui] copied report to clipboard: FAILED");
}

// The one stateful action. Its meaning follows the state the button showed: a
// locked (busy) panel can only stop / cancel, an idle one can only start.
void onPrimary() {
    if (g_locked) {
        g_panel.pendingAction = ACT_STOP;
        logLine("[coop-ui] connection -> OFFLINE");
    } else {
        g_panel.pendingAction = ACT_START;
        logLine("[coop-ui] connection -> ONLINE");
    }
}

void onPanelClick(MyGUI::Widget* w) {
    PanelWidgets& u = g_ui;
    if (!w) return;
    if (w == u.selHost.btn.w)        selectRole(true);
    else if (w == u.selJoin.btn.w)   selectRole(false);
    else if (w == u.selSteam.btn.w)  selectTransport(true);
    else if (w == u.selUdp.btn.w)    selectTransport(false);
    else if (w == u.nick.button.w)   g_pendingPaste = FLD_NICK;
    else if (w == u.hostId.button.w) g_pendingPaste = FLD_HOSTID;
    else if (w == u.addr.button.w)   g_pendingPaste = FLD_ADDR;
    else if (w == u.port.button.w)   g_pendingPaste = FLD_PORT;
    else if (w == u.copyId.w)        copySelfId();
    else if (w == u.primary.btn.w)   onPrimary();
    else if (w == u.btnDiag.w)       g_panel.diagOpen = !g_panel.diagOpen;
    else if (w == u.btnReport.w)     copyReport();
    else if (w == u.btnHide.w)       g_panel.closeRequested = true;
    g_dirty = true;
}

// Enter in a single-line field. Only ever a start, checked in the tick.
void onEditAccept(MyGUI::EditBox*) { g_enterPressed = true; }

// The EditBox ignores Tab itself (no tab printing); it still raises the key.
void onEditKey(MyGUI::Widget*, MyGUI::KeyCode key, MyGUI::Char) {
    if (key == MyGUI::KeyCode::Tab) g_tabDir = shiftDownSeh() ? -1 : 1;
}

void onFrameClose(MyGUI::Window*, const std::string&) { g_panel.closeRequested = true; }

// Paste button: clipboard -> the field, normalised by the field's own parser.
void runPaste(int f) {
    if (f != FLD_NICK && g_locked) return;
    Ctl& e = fieldEdit(f);
    if (!e.w) return;
    const int slot = f == FLD_NICK ? NOTE_NICK : NOTE_EP;
    std::string clip;
    if (!clipboardGetText(clip) || blank(clip)) {
        setNote(slot, ru::kErrClipEmpty, TONE_BAD);
        logLine("[coop-ui] paste failed (clipboard empty)");
        return;
    }
    std::string value;
    const char* err = 0;
    switch (f) {
    case FLD_NICK: {
        if (nickTooLong(clip)) err = ru::kErrNickLong;
        else if (!coop::parsePlayerNick(clip, value) || coop::isPoisonedNick(value)) err = ru::kErrClipNick;
        break;
    }
    case FLD_HOSTID: {
        unsigned long long id = 0;
        bool ok = coop::parseSteamId64(clip, id);
        if (!ok) { // "id1, id2": the first one
            unsigned long long ids[4];
            if (coop::parseSteamId64List(clip, ids, 4) > 0) { id = ids[0]; ok = true; }
        }
        if (ok && id) value = decimal(id); else err = ru::kErrClipSteamId;
        break;
    }
    case FLD_ADDR: {
        std::string ip;
        int port = -1;
        if (coop::parseUdpEndpoint(clip, ip, port) && !ip.empty() && ip.size() < kUdpIpCap)
            value = port > 0 ? addrText(ip, port) : ip;
        else
            err = ru::kErrClipAddr;
        break;
    }
    case FLD_PORT: {
        int port = 0;
        std::string ip;
        int p = 0;
        if (parsePort(clip, port)) value = decimal((unsigned long long)port);
        else if (coop::parseUdpEndpoint(clip, ip, p) && p > 0) value = decimal((unsigned long long)p);
        else err = ru::kErrClipPort;
        break;
    }
    }
    if (err) {
        setNote(slot, err, TONE_BAD);
        logLine("[coop-ui] paste failed (clipboard not usable for the field)");
        return;
    }
    writeField(f, value);
    if (g_draft[f].verdict == V_UNREAD) {
        setNote(slot, ru::kErrUnreadable, TONE_BAD);
        return;
    }
    coopui::native::focus(e.w);
    g_note[slot].text.clear();
    logLine("[coop-ui] paste into field: ok");
}

// ---- Status wording ------------------------------------------------------------
bool sessionRoleHost(const CoopUiSnapshot* st) {
    return g_locked ? st->isHost != 0 : g_panel.hostFlag;
}

void primaryStatus(const CoopUiSnapshot* st, std::string& text, int& tone) {
    const bool host = sessionRoleHost(st);
    switch (st->phase) {
    case COOP_STARTING:
        text = host ? ru::kStStartingHost : ru::kStStartingJoin; tone = TONE_BUSY; return;
    case COOP_HOSTING:
        if (st->worldPhase == COOP_WORLD_WAITING) { text = ru::kStHostNoWorld; tone = TONE_BUSY; }
        else if (st->worldPhase == COOP_WORLD_PREPARING) { text = ru::kStHostSending; tone = TONE_BUSY; }
        else if (st->worldPhase == COOP_WORLD_LOADING) { text = ru::kStHostLoading; tone = TONE_BUSY; }
        else if (st->playerCount > 1) { text = ru::kStHostingPlayers; tone = TONE_GOOD; }
        else { text = ru::kStHostingWait; tone = TONE_BUSY; }
        return;
    case COOP_CONNECTING:
        text = ru::kStConnecting; tone = TONE_BUSY; return;
    case COOP_HANDSHAKING:
        text = ru::kStHandshake; tone = TONE_BUSY; return;
    case COOP_CONNECTED:
        switch (st->worldPhase) {
        case COOP_WORLD_WAITING:   text = ru::kStConnWaiting;     tone = TONE_BUSY; return;
        case COOP_WORLD_PREPARING: text = ru::kStConnPreparing;   tone = TONE_BUSY; return;
        case COOP_WORLD_RECEIVING: text = ru::kStConnReceiving;   tone = TONE_BUSY; return;
        case COOP_WORLD_LOADING:   text = ru::kStConnLoading;     tone = TONE_BUSY; return;
        case COOP_WORLD_READY:     text = ru::kStConnReady;       tone = TONE_GOOD; return;
        case COOP_WORLD_FAILED:    text = ru::kStConnWorldFailed; tone = TONE_BAD;  return;
        default:                   text = ru::kStConnected;       tone = TONE_GOOD; return;
        }
    case COOP_RECONNECTING:
        text = ru::kStReconnecting; tone = TONE_BUSY; return;
    case COOP_FAILED:
        text = host ? ru::kStFailedHost : ru::kStFailedJoin; tone = TONE_BAD; return;
    default:
        text = ru::kStOffline; tone = TONE_MUTED; return;
    }
}

void linkStatus(const CoopUiSnapshot* st, std::string& text, int& tone) {
    switch (st->phase) {
    case COOP_STARTING:
    case COOP_CONNECTING:    text = ru::kLinkConnecting; tone = TONE_BUSY; return;
    case COOP_HANDSHAKING:   text = ru::kLinkHandshake;  tone = TONE_BUSY; return;
    case COOP_CONNECTED:     text = ru::kLinkUp;         tone = TONE_GOOD; return;
    case COOP_HOSTING:       text = ru::kLinkListening;  tone = TONE_GOOD; return;
    case COOP_RECONNECTING:  text = ru::kLinkRetry;      tone = TONE_BUSY; return;
    case COOP_FAILED:        text = ru::kLinkFailed;     tone = TONE_BAD;  return;
    default:                 text = ru::kLinkNone;       tone = TONE_MUTED; return;
    }
}

// The channel actually carrying the session (requested transport while idle).
std::string channelText(const CoopUiSnapshot* st) {
    if (!g_locked) return g_panel.steamFlag ? ru::kValSteam : ru::kValUdp;
    if (st->activeTransport == 0) return ru::kValSteam;
    if (st->activeTransport == 1) return ru::kValUdp;
    return ru::kChanPending;
}

// Milestones: only what the snapshot's real network / world phases state.
enum StepState { S_PENDING, S_ACTIVE, S_DONE, S_FAILED, S_MUTED };
struct StepView {
    const char* name;
    int         state;
    std::string value;
};

void setStep(StepView& s, const char* name, int state, const char* value) {
    s.name = name; s.state = state; s.value = value;
}

void computeSteps(const CoopUiSnapshot* st, bool host, StepView* s) {
    const int ph = st->phase, wp = st->worldPhase;
    if (host) {
        switch (ph) {
        case COOP_STARTING:     setStep(s[0], ru::kStepSession, S_ACTIVE, ru::kSStarting); break;
        case COOP_HOSTING:      setStep(s[0], ru::kStepSession, S_DONE, ru::kLinkListening); break;
        case COOP_RECONNECTING: setStep(s[0], ru::kStepSession, S_ACTIVE, ru::kLinkRetry); break;
        case COOP_FAILED:       setStep(s[0], ru::kStepSession, S_FAILED, ru::kSFailed); break;
        default:                setStep(s[0], ru::kStepSession, S_PENDING, ru::kSWait); break;
        }
        switch (wp) {
        case COOP_WORLD_WAITING:
            setStep(s[1], ru::kStepOwnWorld, g_locked ? S_ACTIVE : S_PENDING, ru::kSLoadOrStart); break;
        case COOP_WORLD_LOADING: setStep(s[1], ru::kStepOwnWorld, S_ACTIVE, ru::kSLoading); break;
        case COOP_WORLD_FAILED:  setStep(s[1], ru::kStepOwnWorld, S_FAILED, ru::kSFailed); break;
        case COOP_WORLD_NONE:    setStep(s[1], ru::kStepOwnWorld, S_PENDING, ru::kSWait); break;
        default:                 setStep(s[1], ru::kStepOwnWorld, S_DONE, ru::kSDone); break;
        }
        if (ph == COOP_HOSTING) {
            int others = st->playerCount - 1;
            if (others < 0) others = 0;
            if (wp == COOP_WORLD_PREPARING) {
                setStep(s[2], ru::kStepPlayers, S_ACTIVE, ru::kSSending);
            } else if (others > 0) {
                char b[64];
                _snprintf(b, sizeof(b) - 1, ru::kSPlayersFmt, others);
                b[sizeof(b) - 1] = '\0';
                setStep(s[2], ru::kStepPlayers, S_DONE, b);
            } else {
                setStep(s[2], ru::kStepPlayers, S_ACTIVE, ru::kSWaitPlayers);
            }
        } else {
            setStep(s[2], ru::kStepPlayers, S_PENDING, ru::kSWait);
        }
        return;
    }
    switch (ph) {
    case COOP_STARTING:
    case COOP_CONNECTING:   setStep(s[0], ru::kStepLink, S_ACTIVE, ru::kLinkConnecting); break;
    case COOP_HANDSHAKING:  setStep(s[0], ru::kStepLink, S_ACTIVE, ru::kLinkHandshake); break;
    case COOP_CONNECTED:    setStep(s[0], ru::kStepLink, S_DONE, ru::kLinkUp); break;
    case COOP_RECONNECTING: setStep(s[0], ru::kStepLink, S_ACTIVE, ru::kLinkRetry); break;
    case COOP_FAILED:       setStep(s[0], ru::kStepLink, S_FAILED, ru::kLinkFailed); break;
    default:                setStep(s[0], ru::kStepLink, S_PENDING, ru::kSWait); break;
    }
    const bool linked = ph == COOP_CONNECTED;
    switch (wp) {
    case COOP_WORLD_WAITING:   setStep(s[1], ru::kStepWorldGet, S_ACTIVE, ru::kSWaitHost); break;
    case COOP_WORLD_PREPARING: setStep(s[1], ru::kStepWorldGet, S_ACTIVE, ru::kSHostPreparing); break;
    case COOP_WORLD_RECEIVING: setStep(s[1], ru::kStepWorldGet, S_ACTIVE, ru::kSReceiving); break;
    case COOP_WORLD_LOADING:
    case COOP_WORLD_READY:     setStep(s[1], ru::kStepWorldGet, S_DONE, ru::kSReceived); break;
    case COOP_WORLD_FAILED:    setStep(s[1], ru::kStepWorldGet, S_FAILED, ru::kSFailed); break;
    default:
        setStep(s[1], ru::kStepWorldGet, linked ? S_MUTED : S_PENDING,
                linked ? ru::kSNotTracked : ru::kSWait);
        break;
    }
    switch (wp) {
    case COOP_WORLD_LOADING: setStep(s[2], ru::kStepWorldLoad, S_ACTIVE, ru::kSLoading); break;
    case COOP_WORLD_READY:   setStep(s[2], ru::kStepWorldLoad, S_DONE, ru::kSInGame); break;
    case COOP_WORLD_NONE:
        setStep(s[2], ru::kStepWorldLoad, linked ? S_MUTED : S_PENDING,
                linked ? ru::kSNotTracked : ru::kSWait);
        break;
    default:                 setStep(s[2], ru::kStepWorldLoad, S_PENDING, ru::kSWait); break;
    }
}

const Rgb& stepDotRgb(int state) {
    switch (state) {
    case S_ACTIVE: return C_BUSY;
    case S_DONE:   return C_GOOD;
    case S_FAILED: return C_BAD;
    default:       return C_DOT_IDLE;
    }
}
const Rgb& stepTextRgb(int state) {
    switch (state) {
    case S_ACTIVE: return C_BUSY;
    case S_DONE:   return C_GOOD;
    case S_FAILED: return C_BAD;
    default:       return C_MUTED;
    }
}

void worldStatus(const CoopUiSnapshot* st, bool host, std::string& text) {
    switch (st->worldPhase) {
    case COOP_WORLD_WAITING:   text = host ? ru::kSLoadOrStart : ru::kSWaitHost; return;
    case COOP_WORLD_PREPARING: text = host ? ru::kSSending : ru::kSHostPreparing; return;
    case COOP_WORLD_RECEIVING: text = ru::kSReceiving; return;
    case COOP_WORLD_LOADING:   text = ru::kSLoading; return;
    case COOP_WORLD_READY:     text = ru::kSDone; return;
    case COOP_WORLD_FAILED:    text = ru::kSFailed; return;
    default:                   text = "-"; return;
    }
}

std::string boundedName(const char* s, size_t cap) {
    size_t n = 0;
    while (n < cap && s[n]) ++n;
    return std::string(s, n);
}

// "name (you, host)" + readiness for roster row i. Readiness is only claimed
// when the plugin knows it; otherwise a member is merely "connected".
void playerRow(const CoopUiPlayer& p, std::string& who, std::string& state, int& tone) {
    who = boundedName(p.name, sizeof(p.name));
    if (who.empty()) {
        char b[48];
        _snprintf(b, sizeof(b) - 1, ru::kPlayerFallback, p.id);
        b[sizeof(b) - 1] = '\0';
        who = b;
    }
    const bool isHostId = (p.id == 0); // NetLink: the host is always player 0
    if (p.local || isHostId) {
        who += " (";
        if (p.local) who += ru::kYou;
        if (p.local && isHostId) who += ", ";
        if (isHostId) who += ru::kHostTag;
        who += ")";
    }
    if (p.worldReadyKnown) {
        state = p.worldReady ? ru::kPInGame : ru::kPNotReady;
        tone = p.worldReady ? TONE_GOOD : TONE_BUSY;
    } else {
        state = ru::kPConnected;
        tone = TONE_NORMAL;
    }
}

const char* verdictText(int f, int v) {
    if (v == V_UNREAD) return ru::kErrUnreadable;
    switch (f) {
    case FLD_NICK:
        return v == V_LONG ? ru::kErrNickLong : v == V_POISON ? ru::kErrNickBad : ru::kErrNickEmpty;
    case FLD_HOSTID: return v == V_EMPTY ? ru::kErrIdEmpty : ru::kErrIdBad;
    case FLD_ADDR:   return v == V_EMPTY ? ru::kErrAddrEmpty : ru::kErrAddrBad;
    default:         return v == V_EMPTY ? ru::kErrPortEmpty : ru::kErrPortBad;
    }
}

// ---- Window lifetime -------------------------------------------------------------
// createDatapanel geometry, as screen fractions. The reconstructed header is
// ambiguous about top/left order, so both keep the window on screen either way;
// the window is then re-pinned to an exact centred pixel box (frameMaxW/H,
// clamped to the screen) so the layout below fits 1280x720 and 1920x1080.
const float kFrameA = 0.11f;
const float kFrameB = 0.18f;
const float kFrameW = 0.64f;
const float kFrameH = 0.78f;
const int   kFrameMaxW = 1240;
const int   kFrameMaxH = 820;

// Layout metrics (client pixels).
const int kPad       = 18;  // outer padding; the column gap is twice this
const int kFooterH   = 50;
const int kHeadH     = 24;  // section heading
const int kHeadGap   = 6;
const int kSelH      = 44;  // role / transport selector
const int kFieldH    = 40;  // edit field + its small button
const int kSmallBtnW = 140;
const int kPrimaryH  = 50;
const int kNoteH     = 21;  // one small-text line
const int kRowH      = 24;

// createDatapanel already registers the panel in guiDatapanels. Registering it
// again makes ForgottenGUI::shutDown delete the same object twice.
bool uiPanelArmSeh(DatapanelGUI* p) {
    if (!p) return false;
    __try {
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


bool frameCloseHookSeh(DatapanelGUI* f,
                       MyGUI::delegates::IDelegate2<MyGUI::Window*, const std::string&>* d) {
    __try {
        f->setCloseCallback(d);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool frameCaptionSeh(DatapanelGUI* f, const std::string* caption) {
    __try {
        f->setCaption(*caption);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// 1 visible, 0 hidden, -1 unreadable.
int frameVisibleSeh(DatapanelGUI* f) {
    __try {
        return f->_NV_isVisible() ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

MyGUI::Widget* frameWinSeh(DatapanelGUI* f) {
    __try { return f->win; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// Screen (layer) size the root window lives in.
bool parentSizeSeh(MyGUI::Widget* w, int* vw, int* vh) {
    __try {
        MyGUI::IntSize s = w->getParentSize();
        *vw = s.width;
        *vh = s.height;
        return s.width > 0 && s.height > 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// The frame's client area in pixels (window minus rust border and title bar).
// Children of the window are placed in this client area.
bool frameClientSeh(DatapanelGUI* f, MyGUI::Widget** parent, int* w, int* h) {
    __try {
        MyGUI::Widget* win = f->win;
        if (!win) return false;
        MyGUI::IntCoord c = win->getClientCoord();
        *parent = win;
        *w = c.width;
        *h = c.height;
        return c.width > 0 && c.height > 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void widgetHideSeh(MyGUI::Widget* w) {
    if (!w) return;
    __try { w->setVisible(false); } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// ---- Widget build ------------------------------------------------------------------
struct Builder {
    ForgottenGUI*  g;
    MyGUI::Widget* parent;
    int            failed;
};
int g_widgetSerial = 0;

void track(MyGUI::Widget* w) {
    if (w && g_ownedN < kOwnedMax) g_owned[g_ownedN++] = w;
}

std::string nextName() {
    char b[48];
    _snprintf(b, sizeof(b) - 1, "KenshiCoopUi_%d", ++g_widgetSerial);
    b[sizeof(b) - 1] = '\0';
    return b;
}

void place(Builder& b, Ctl& c, MyGUI::Widget* w, int kind, int font, int x, int y, int cw, int ch) {
    c = Ctl();
    c.w = w; c.kind = kind; c.x = x; c.y = y; c.cw = cw; c.ch = ch; c.visible = 0;
    if (!w) { ++b.failed; return; }
    track(w);
    if (font >= 0) textFontSeh(w, &fontName(font), kFonts[font].px);
}

void mkLabel(Builder& b, Ctl& c, int font, int x, int y, int w, int h, int align = AL_LEFT) {
    const std::string empty;
    place(b, c, labelCreateSeh(b.g, b.parent, &empty, x, y, w, h, align), K_LABEL, font,
          x, y, w, h);
}

void mkRect(Builder& b, Ctl& c, int x, int y, int w, int h, const Rgb& fill, float alpha) {
    static const std::string skin = "WhiteSkin"; // Kenshi_UI.png white texel, tinted
    const std::string empty;
    const std::string name = nextName();
    place(b, c, buttonCreateSeh(b.g, b.parent, &name, &empty, &skin, x, y, w, h, false),
          K_RECT, -1, x, y, w, h);
    ctlFill(c, fill, alpha);
}

void mkButton(Builder& b, Ctl& c, int font, int x, int y, int w, int h) {
    static const std::string skin = "Kenshi_Button1Skin"; // the game's own button
    const std::string empty;
    const std::string name = nextName();
    place(b, c, buttonCreateSeh(b.g, b.parent, &name, &empty, &skin, x, y, w, h, true),
          K_BUTTON, font, x, y, w, h);
    // Delegate assignment stays OUT of SEH (it allocates); the widget is alive.
    if (c.w) c.w->eventMouseButtonClick += MyGUI::newDelegate(&onPanelClick);
}

// Gold frame BEHIND the button, translucent veil ON it: skins draw in creation
// order, while all text of the window draws after all skins, so the caption
// stays crisp above the veil. Neither rectangle takes the mouse.
void mkSelector(Builder& b, Selector& s, int font, int x, int y, int w, int h) {
    mkRect(b, s.frame, x - 2, y - 2, w + 4, h + 4, C_FRAME_GOLD, 1.0f);
    mkButton(b, s.btn, font, x, y, w, h);
    mkRect(b, s.veil, x + 3, y + 3, w - 6, h - 6, C_VEIL_GOLD, 0.30f);
}

void mkField(Builder& b, Field& f, int x, int y, int w, int h, int maxLen) {
    const int ew = w - kSmallBtnW - 10;
    mkRect(b, f.ring, x - 2, y - 2, ew + 4, h + 4, C_FRAME_GOLD, 1.0f);
    const std::string name = nextName();
    place(b, f.edit, editCreateSeh(b.g, b.parent, &name, x, y, ew, h, maxLen), K_EDIT, F_BODY,
          x, y, ew, h);
    if (f.edit.w) {
        // Darken the input skin once (not cached: the edit's colour cache is its text).
        const MyGUI::Colour tint(C_FIELD_TINT.r, C_FIELD_TINT.g, C_FIELD_TINT.b, 1.0f);
        fillSeh(f.edit.w, &tint, 1.0f);
        MyGUI::EditBox* e = static_cast<MyGUI::EditBox*>(f.edit.w);
        e->eventEditSelectAccept += MyGUI::newDelegate(&onEditAccept);
        e->eventKeyButtonPressed += MyGUI::newDelegate(&onEditKey);
    }
    mkButton(b, f.button, F_SMALL, x + ew + 10, y, kSmallBtnW, h);
}

// Every widget of the panel, laid out once for the client size cw x ch.
bool buildPanelWidgets(ForgottenGUI* g, MyGUI::Widget* parent, int cw, int ch) {
    PanelWidgets& u = g_ui;
    u = PanelWidgets();
    Builder b = { g, parent, 0 };
    const int x0 = kPad;
    const int colGap = 2 * kPad;
    const int lw = (cw - 2 * kPad - colGap) * 47 / 100;
    const int rx = x0 + lw + colGap;
    const int rw = cw - kPad - rx;
    const int top = 12;
    const int bottom = ch - kFooterH - 8;
    const int avail = bottom - top;
    const int head = kHeadH + kHeadGap;

    mkRect(b, u.colRule, x0 + lw + colGap / 2, top, 1, bottom - top, C_RULE, 0.45f);
    mkRect(b, u.footRule, kPad, ch - kFooterH, cw - 2 * kPad, 1, C_RULE, 0.45f);

    // Left column: fixed blocks; the slack is spread over the four section gaps.
    const int leftFixed = 4 * head + 2 * kSelH + 2 * (kFieldH + 4) + 3 * kNoteH +
                          kPrimaryH + 6 + kNoteH;
    const int gapL = clampi((avail - leftFixed) / 4, 8, 30);
    int y = top;
    mkLabel(b, u.hdrMode, F_HEAD, x0, y, lw, kHeadH);
    y += head;
    const int selW = (lw - 12) / 2;
    mkSelector(b, u.selHost, F_HEAD, x0, y, selW, kSelH);
    mkSelector(b, u.selJoin, F_HEAD, x0 + selW + 12, y, lw - selW - 12, kSelH);
    y += kSelH + gapL;
    mkLabel(b, u.hdrNick, F_HEAD, x0, y, lw, kHeadH);
    y += head;
    mkField(b, u.nick, x0, y, lw, kFieldH, kNickMaxChars);
    y += kFieldH + 4;
    mkLabel(b, u.nickNote, F_SMALL, x0, y, lw, kNoteH);
    y += kNoteH + gapL;
    mkLabel(b, u.hdrTrans, F_HEAD, x0, y, lw, kHeadH);
    y += head;
    mkSelector(b, u.selSteam, F_HEAD, x0, y, selW, kSelH);
    mkSelector(b, u.selUdp, F_HEAD, x0 + selW + 12, y, lw - selW - 12, kSelH);
    y += kSelH + gapL;
    mkLabel(b, u.hdrEndpoint, F_HEAD, x0, y, lw, kHeadH);
    y += head;
    // One slot, four occupants: only the selected flow's is shown.
    mkField(b, u.hostId, x0, y, lw, kFieldH, kIdMaxChars);
    mkField(b, u.addr, x0, y, lw, kFieldH, kAddrMaxChars);
    mkField(b, u.port, x0, y, lw, kFieldH, kPortMaxChars);
    const int sw = lw - kSmallBtnW - 10;
    mkRect(b, u.selfBg, x0, y, sw, kFieldH, C_BLACK, 0.45f);
    mkLabel(b, u.selfText, F_BODY, x0 + 12, y, sw - 24, kFieldH);
    mkButton(b, u.copyId, F_SMALL, x0 + sw + 10, y, kSmallBtnW, kFieldH);
    y += kFieldH + 4;
    mkLabel(b, u.epNote[0], F_SMALL, x0, y, lw, kNoteH);
    mkLabel(b, u.epNote[1], F_SMALL, x0, y + kNoteH, lw, kNoteH);
    y += 2 * kNoteH + gapL;
    mkSelector(b, u.primary, F_BIG, x0, y, lw, kPrimaryH);
    y += kPrimaryH + 6;
    mkLabel(b, u.primaryNote, F_SMALL, x0, y, lw, kNoteH);

    // Right column: status card, milestones, roster (stretches), updater line.
    const int stepsH = 8 + kStepRows * 26 + kRowH + 8;
    const int playersMin = 6 + kPlayerRows * 23 + 6;
    const int updH = 3 * kNoteH;
    int msgRows = kMsgRows;
    int stateH = 10 + 30 + 6 + kKvRows * kRowH + 4 + msgRows * kNoteH + 8;
    int rightFixed = 3 * head + stateH + stepsH + playersMin + updH;
    if (avail - rightFixed < 3 * 6) { // short screen: one message line less
        msgRows = kMsgRows - 1;
        stateH -= kNoteH;
        rightFixed -= kNoteH;
    }
    const int gapR = clampi((avail - rightFixed) / 3, 6, 16);
    int yr = top;
    mkLabel(b, u.hdrState, F_HEAD, rx, yr, rw, kHeadH);
    yr += head;
    mkRect(b, u.stateCard, rx, yr, rw, stateH, C_BLACK, 0.30f);
    mkRect(b, u.stateDot, rx + 14, yr + 10 + 9, 12, 12, C_DOT_IDLE, 1.0f);
    mkLabel(b, u.stateText, F_BIG, rx + 36, yr + 10, rw - 50, 30);
    const int ky = yr + 10 + 30 + 6;
    for (int i = 0; i < kKvRows; ++i) {
        mkLabel(b, u.kvKey[i], F_SMALL, rx + 14, ky + i * kRowH, 150, kRowH);
        mkLabel(b, u.kvVal[i], F_BODY, rx + 170, ky + i * kRowH, rw - 184, kRowH);
    }
    const int my = ky + kKvRows * kRowH + 4;
    for (int i = 0; i < msgRows; ++i)
        mkLabel(b, u.msg[i], F_SMALL, rx + 14, my + i * kNoteH, rw - 28, kNoteH);
    u.msgRows = msgRows;
    yr += stateH + gapR;

    const int diagTop = yr;
    const int half = rw / 2;
    mkLabel(b, u.hdrSteps, F_HEAD, rx, yr, rw, kHeadH);
    yr += head;
    mkRect(b, u.stepsCard, rx, yr, rw, stepsH, C_BLACK, 0.30f);
    for (int i = 0; i < kStepRows; ++i) {
        const int sy = yr + 8 + i * 26;
        mkRect(b, u.stepDot[i], rx + 14, sy + 8, 10, 10, C_DOT_IDLE, 1.0f);
        mkLabel(b, u.stepName[i], F_BODY, rx + 34, sy, half - 34, 26);
        mkLabel(b, u.stepVal[i], F_BODY, rx + half + 6, sy, rw - half - 20, 26);
    }
    const int by = yr + 8 + kStepRows * 26;
    mkRect(b, u.barTrack, rx + 34, by + 7, half - 44, 10, C_TRACK, 1.0f);
    mkRect(b, u.barFill, rx + 34, by + 7, 2, 10, C_FRAME_GOLD, 1.0f);
    mkLabel(b, u.barText, F_SMALL, rx + half + 6, by, rw - half - 20, kRowH);
    mkLabel(b, u.xferLine, F_SMALL, rx + 34, by, rw - 48, kRowH);
    yr += stepsH + gapR;

    const int updTop = bottom - updH;
    mkLabel(b, u.hdrPlayers, F_HEAD, rx, yr, half, kHeadH);
    mkLabel(b, u.playersCount, F_HEAD, rx + half, yr, rw - half - 14, kHeadH, AL_RIGHT);
    yr += head;
    int pcH = updTop - gapR - yr;
    if (pcH < playersMin) pcH = playersMin;
    mkRect(b, u.playersCard, rx, yr, rw, pcH, C_BLACK, 0.30f);
    const int nameW = rw * 58 / 100;
    for (int i = 0; i < kPlayerRows; ++i) {
        const int py = yr + 6 + i * 23;
        mkLabel(b, u.pName[i], F_BODY, rx + 14, py, nameW - 14, 23);
        mkLabel(b, u.pState[i], F_SMALL, rx + nameW, py, rw - nameW - 14, 23);
    }
    const int diagBottom = yr + pcH;
    mkLabel(b, u.updHdr, F_SMALL, rx, updTop, rw, kNoteH);
    mkLabel(b, u.upd[0], F_SMALL, rx, updTop + kNoteH, rw, kNoteH);
    mkLabel(b, u.upd[1], F_SMALL, rx, updTop + 2 * kNoteH, rw, kNoteH);

    // Folded diagnostics: the milestones + roster region, while unfolded.
    mkLabel(b, u.hdrDiag, F_HEAD, rx, diagTop, rw, kHeadH);
    const int dcTop = diagTop + head;
    const int dcH = diagBottom - dcTop;
    mkRect(b, u.diagCard, rx, dcTop, rw, dcH, C_BLACK, 0.30f);
    const int rows = clampi((dcH - 12) / kNoteH, 1, kDiagRowsMax);
    const int valX = rw * 45 / 100;
    for (int i = 0; i < rows; ++i) {
        const int dy = dcTop + 6 + i * kNoteH;
        mkLabel(b, u.diagKey[i], F_SMALL, rx + 14, dy, rw - 28, kNoteH);
        mkLabel(b, u.diagVal[i], F_SMALL, rx + valX, dy, rw - valX - 14, kNoteH);
    }
    u.diagRows = rows;

    // Footer.
    const int fbH = 36;
    const int fy = ch - kFooterH + (kFooterH - fbH) / 2;
    int fx = cw - kPad - 160;
    mkButton(b, u.btnHide, F_SMALL, fx, fy, 160, fbH);
    fx -= 10 + 200;
    mkButton(b, u.btnReport, F_SMALL, fx, fy, 200, fbH);
    fx -= 10 + 190;
    mkButton(b, u.btnDiag, F_SMALL, fx, fy, 190, fbH);
    mkLabel(b, u.footText, F_SMALL, kPad, fy, fx - 10 - kPad, fbH);

    char m[160];
    _snprintf(m, sizeof(m) - 1,
              "[coop-ui] panel layout client=%dx%d left=%d right=%d gaps=%d/%d widgets=%d failed=%d",
              cw, ch, lw, rw, gapL, gapR, g_ownedN, b.failed);
    m[sizeof(m) - 1] = '\0';
    if (b.failed) logErrLine(m); else logLine(m);
    return b.failed == 0;
}

bool openPanelWindows(ForgottenGUI* g) {
    if (!ensurePanelFont()) return false;
    // Layer MUST be "Info": spike 48 proved createFloatingLabel renders non-null
    // there. "Windows" is not a visible MyGUI layer here - the panel is minted
    // and armed but attaches to nothing, so F2 logs open/close yet nothing draws.
    // Created OUTSIDE SEH: the layer argument is a by-value std::string (C2712).
    std::string layer = "Info";
    DatapanelGUI* frame = g->createDatapanel(kFrameA, kFrameB, kFrameW, kFrameH, false, layer, true);
    if (!frame) {
        logErrLine("[coop-ui] createDatapanel FAILED");
        return false;
    }
    if (!uiPanelArmSeh(frame))
        logErrLine("[coop-ui] panel show FAILED");
    const std::string caption = ru::kTitle;
    if (frameCaptionSeh(frame, &caption)) frameFont(frame);
    // The native close button only hides the panel, through the same path as F2.
    if (!frameCloseHookSeh(frame, MyGUI::newDelegate(&onFrameClose)))
        logErrLine("[coop-ui] close button hook FAILED");

    MyGUI::Widget* win = frameWinSeh(frame);
    int vw = 0, vh = 0;
    if (win && parentSizeSeh(win, &vw, &vh)) {
        const int fw = clampi(vw - 40, 640, kFrameMaxW);
        const int fh = clampi(vh - 24, 480, kFrameMaxH);
        coordSeh(win, (vw - fw) / 2, (vh - fh) / 2, fw, fh);
    }
    MyGUI::Widget* parent = 0;
    int cw = 0, ch = 0;
    if (!win || !frameClientSeh(frame, &parent, &cw, &ch) || cw < 600 || ch < 420) {
        char b[96];
        _snprintf(b, sizeof(b) - 1, "[coop-ui] frame client unusable %dx%d (screen %dx%d)",
                  cw, ch, vw, vh);
        b[sizeof(b) - 1] = '\0';
        logErrLine(b);
        panelDestroySeh(g, frame);
        return false;
    }
    g_panel.frame = frame;
    g_panel.gui = g;
    g_panel.win = win;
    g_ownedN = 0;
    track(win);
    if (!g_unlinker) {
        g_unlinker = new PanelUnlinker(); // never freed: MyGUI may call it until exit
        if (!registerUnlinkerSeh(g_unlinker))
            logErrLine("[coop-ui] widget unlinker registration FAILED");
    }
    if (!buildPanelWidgets(g, parent, cw, ch)) {
        // Forget first, so the unlinker does not flag our own teardown.
        g_ownedN = 0;
        g_ui = PanelWidgets();
        panelDestroySeh(g, frame);
        g_panel.frame = 0;
        g_panel.gui = 0;
        g_panel.win = 0;
        return false;
    }
    g_panel.built = true;
    // Drafts survive a rebuild (GUI reset): the new fields start where the old ended.
    for (int f = 0; f < FLD_COUNT; ++f) writeField(f, g_draft[f].text);
    return true;
}

// The GUI destroyed the window (or was replaced): drop every pointer unseen.
void forgetPanel(const char* why) {
    // The GUI owns the panel list; the old frame may already have been freed.
    // Only drop our focus, without dereferencing the obsolete root pointer.
    coopui::native::releaseFocus(g_panel.win);
    coopui::native::guardGameInput(false);
    g_ownedN = 0;
    g_ui = PanelWidgets();
    g_panel.frame = 0;
    g_panel.gui = 0;
    g_panel.win = 0;
    g_panel.built = false;
    g_panel.open = false;
    g_panel.seenVisible = false;
    g_panel.closeRequested = false;
    g_panel.pendingAction = ACT_NONE;
    g_pendingPaste = -1;
    g_tabDir = 0;
    g_enterPressed = false;
    g_panelLost = false;
    logErrLine(why);
}

// Hide (F2 / Esc / close / Hide / engine): keep the hierarchy, commit what is
// valid, and give the keyboard back to the game.
void hidePanel() {
    if (g_panel.built) {
        readDrafts();
        coopui::native::releaseFocus(g_panel.win);
        commitDrafts(true);
    }
    coopui::native::guardGameInput(false);
    g_pendingPaste = -1;
    g_tabDir = 0;
    g_enterPressed = false;
    panelShowSeh(g_panel.frame, false);
    g_panel.open = false;
    g_panel.seenVisible = false;
    logLine("[coop-ui] panel closed");
}

// ---- Refresh (state -> widgets) ------------------------------------------------------
void heading(Ctl& c, const char* text, bool show) {
    ctlText(c, text);
    ctlColour(c, C_GOLD);
    ctlVisible(c, show);
}

void lineIf(Ctl& c, const std::string& s, const Rgb& col, bool show) {
    ctlText(c, s);
    ctlColour(c, col);
    ctlVisible(c, show && !s.empty());
}

void hideLines(Ctl* lines, int n) {
    for (int i = 0; i < n; ++i) ctlVisible(lines[i], false);
}

void styleSelector(Selector& s, const std::string& text, bool lit, bool enabled, bool rust,
                   float veilAlpha) {
    ctlText(s.btn, text);
    ctlVisible(s.btn, true);
    ctlEnabled(s.btn, enabled);
    ctlFill(s.frame, rust ? C_FRAME_RUST : C_FRAME_GOLD, enabled ? 1.0f : 0.55f);
    ctlVisible(s.frame, lit);
    ctlFill(s.veil, rust ? C_VEIL_RUST : C_VEIL_GOLD, enabled ? veilAlpha : veilAlpha * 0.55f);
    ctlVisible(s.veil, lit);
    const Rgb& base = lit ? (enabled ? C_SEL : C_GOLD) : (enabled ? C_SOFT : C_DIM);
    ctlButtonColour(s.btn, base, C_HOVER);
}

bool badVerdict(int v) { return v == V_BAD || v == V_LONG || v == V_POISON || v == V_UNREAD; }

void styleField(Field& f, int id, bool visible, bool readOnly, bool buttonEnabled, int focused) {
    ctlVisible(f.edit, visible);
    ctlReadOnly(f.edit, readOnly);
    ctlColour(f.edit, readOnly ? C_MUTED : C_TEXT);
    ctlText(f.button, ru::kPaste);
    ctlVisible(f.button, visible);
    ctlEnabled(f.button, buttonEnabled);
    ctlButtonColour(f.button, buttonEnabled ? C_SOFT : C_DIM, C_HOVER);
    const bool editing = focused == id && !readOnly;
    const bool bad = !readOnly && badVerdict(g_draft[id].verdict);
    ctlFill(f.ring, (bad && !editing) ? C_BAD : C_FRAME_GOLD, 1.0f);
    ctlVisible(f.ring, visible && (editing || bad));
}

// A field's note lines: live feedback > what is wrong > the hint.
void fieldNote(Ctl* lines, int n, int id, int slot, const char* hint, bool checkField) {
    if (noteLive(slot)) {
        fillLines(lines, n, g_note[slot].text, F_SMALL, toneRgb(g_note[slot].tone));
        return;
    }
    const int v = g_draft[id].verdict;
    if (checkField && v != V_OK) {
        if (v == V_EMPTY) // nothing typed yet: say what is needed, not "wrong"
            fillLines(lines, n, id == FLD_NICK ? std::string(verdictText(id, v)) : std::string(hint),
                      F_SMALL, C_BUSY);
        else
            fillLines(lines, n, verdictText(id, v), F_SMALL, C_BAD);
        return;
    }
    fillLines(lines, n, hint ? hint : "", F_SMALL, C_MUTED);
}

void refreshLeft(const CoopUiSnapshot* st, int focused) {
    PanelWidgets& u = g_ui;
    const bool host = g_panel.hostFlag;
    const bool steam = g_panel.steamFlag;
    const bool idle = !g_locked;

    heading(u.hdrMode, ru::kHdrMode, true);
    styleSelector(u.selHost, ru::kRoleHost, host, idle, false, 0.30f);
    styleSelector(u.selJoin, ru::kRoleJoin, !host, idle, false, 0.30f);

    heading(u.hdrNick, ru::kHdrNick, true);
    styleField(u.nick, FLD_NICK, true, false, true, focused); // the nick is never locked
    fieldNote(&u.nickNote, 1, FLD_NICK, NOTE_NICK, ru::kNickHint, true);

    heading(u.hdrTrans, ru::kHdrTransport, true);
    styleSelector(u.selSteam, ru::kTrSteam, steam, idle, false, 0.30f);
    styleSelector(u.selUdp, ru::kTrUdp, !steam, idle, false, 0.30f);

    const int ep = activeEndpoint();
    heading(u.hdrEndpoint, ep == FLD_PORT ? ru::kHdrHostPort
                         : ep == FLD_HOSTID ? ru::kHdrHostId
                         : ep == FLD_ADDR ? ru::kHdrHostAddr : ru::kHdrSelfId, true);
    styleField(u.hostId, FLD_HOSTID, ep == FLD_HOSTID, g_locked, idle, focused);
    styleField(u.addr, FLD_ADDR, ep == FLD_ADDR, g_locked, idle, focused);
    styleField(u.port, FLD_PORT, ep == FLD_PORT, g_locked, idle, focused);

    const bool self = ep < 0;
    ctlVisible(u.selfBg, self);
    lineIf(u.selfText, st->selfSteamId ? coop::maskSteamId64(st->selfSteamId)
                                       : std::string(ru::kSteamMissing),
           st->selfSteamId ? C_TEXT : C_BAD, self);
    ctlText(u.copyId, ru::kCopy);
    ctlVisible(u.copyId, self);
    ctlEnabled(u.copyId, st->selfSteamId != 0);
    ctlButtonColour(u.copyId, st->selfSteamId ? C_SOFT : C_DIM, C_HOVER);

    const char* hint = host ? (steam ? ru::kHostHintSteam : ru::kHostHintUdp)
                            : (steam ? ru::kJoinHintSteam : ru::kJoinHintUdp);
    fieldNote(u.epNote, 2, ep < 0 ? FLD_NICK : ep, NOTE_EP, hint, ep >= 0 && idle);

    // Primary action: exactly one, captioned by the state it acts on.
    const char* primary;
    bool enabled = true;
    if (g_locked) {
        switch (st->phase) {
        case COOP_HOSTING:   primary = ru::kActStopHost; break;
        case COOP_CONNECTED: primary = ru::kActDisconnect; break;
        case COOP_STARTING:  primary = st->isHost ? ru::kActCancelStart : ru::kActCancel; break;
        case COOP_CONNECTING:
        case COOP_HANDSHAKING:
        case COOP_RECONNECTING: primary = ru::kActCancel; break;
        default:             primary = st->isHost ? ru::kActStopHost : ru::kActDisconnect; break;
        }
    } else {
        primary = host ? ru::kActCreate : ru::kActConnect;
        enabled = startReady();
    }
    styleSelector(u.primary, primary, enabled, enabled, g_locked, g_locked ? 0.34f : 0.38f);
    if (g_locked) {
        lineIf(u.primaryNote, ru::kLockedNote, C_MUTED, true);
    } else if (!enabled) {
        const int f = firstInvalidField();
        const int v = g_draft[f].verdict;
        lineIf(u.primaryNote, verdictText(f, v), v == V_EMPTY ? C_BUSY : C_BAD, true);
    } else {
        lineIf(u.primaryNote, ru::kEnterHint, C_MUTED, true);
    }
}

void appendReportLine(std::string& rep, const char* key, const std::string& value) {
    rep += key;
    rep += ": ";
    rep += value.empty() ? std::string("-") : value;
    rep += "\r\n";
}

void refreshRight(const CoopUiSnapshot* st, std::string& report) {
    PanelWidgets& u = g_ui;
    const bool host = sessionRoleHost(st);
    const bool diag = g_panel.diagOpen;

    std::string status;
    int statusTone = TONE_NORMAL;
    primaryStatus(st, status, statusTone);
    const std::string detail = st->detail ? std::string(st->detail) : std::string();
    const std::string error = st->errorDetail ? std::string(st->errorDetail) : std::string();
    std::string link;
    int linkTone = TONE_MUTED;
    linkStatus(st, link, linkTone);
    const std::string role = std::string(host ? ru::kValHost : ru::kValJoin) + " - " +
                             channelText(st);

    // Status card: the one primary status, then role / link, then the core's
    // error (actionable) before its description.
    heading(u.hdrState, ru::kHdrState, true);
    ctlVisible(u.stateCard, true);
    ctlFill(u.stateDot, statusTone == TONE_MUTED ? C_DOT_IDLE : toneRgb(statusTone), 1.0f);
    ctlVisible(u.stateDot, true);
    lineIf(u.stateText, status, toneRgb(statusTone), true);
    lineIf(u.kvKey[0], ru::kRowRole, C_MUTED, true);
    lineIf(u.kvVal[0], role, C_TEXT, true);
    lineIf(u.kvKey[1], ru::kRowLink, C_MUTED, true);
    lineIf(u.kvVal[1], link, toneRgb(linkTone), true);
    int used = error.empty() ? 0 : fillLines(u.msg, u.msgRows, error, F_SMALL, C_BAD);
    if (used < u.msgRows) fillLines(u.msg + used, u.msgRows - used, detail, F_SMALL, C_MUTED);

    // Milestones.
    StepView steps[kStepRows];
    computeSteps(st, host, steps);
    heading(u.hdrSteps, host ? ru::kHdrStepsHost : ru::kHdrStepsJoin, !diag);
    ctlVisible(u.stepsCard, !diag);
    for (int i = 0; i < kStepRows; ++i) {
        const int s = steps[i].state;
        ctlFill(u.stepDot[i], stepDotRgb(s), 1.0f);
        ctlVisible(u.stepDot[i], !diag);
        lineIf(u.stepName[i], steps[i].name, (s == S_PENDING || s == S_MUTED) ? C_MUTED : C_TEXT,
               !diag);
        lineIf(u.stepVal[i], steps[i].value, stepTextRgb(s), !diag);
    }
    std::string transfer;
    bool bar = false;
    float frac = 0.0f;
    if (st->totalBytes > 0) {
        unsigned long long got = st->receivedBytes;
        if (got > st->totalBytes) got = st->totalBytes;
        frac = (float)((double)got / (double)st->totalBytes);
        char b[96];
        _snprintf(b, sizeof(b) - 1, ru::kProgressFmt, (double)got / (1024.0 * 1024.0),
                  (double)st->totalBytes / (1024.0 * 1024.0), (int)(frac * 100.0f));
        b[sizeof(b) - 1] = '\0';
        transfer = b;
        bar = true;
    } else if (st->transferDetail && st->transferDetail[0]) {
        transfer = st->transferDetail;
    }
    ctlVisible(u.barTrack, !diag && bar);
    if (bar && u.barTrack.w) {
        int fw = (int)(u.barTrack.cw * frac);
        if (fw < 2) fw = 2;
        ctlCoord(u.barFill, u.barTrack.x, u.barTrack.y, fw, u.barTrack.ch);
    }
    ctlVisible(u.barFill, !diag && bar);
    lineIf(u.barText, bar ? transfer : std::string(), C_BUSY, !diag);
    lineIf(u.xferLine, bar ? std::string() : transfer, C_BUSY, !diag);

    // Roster: accepted members only (plugin truth), never a config id list.
    int count = st->playerCount;
    if (count < 0) count = 0;
    if (count > kPlayerRows) count = kPlayerRows;
    std::string rosterReport;
    heading(u.hdrPlayers, ru::kHdrPlayers, !diag);
    char c[16];
    _snprintf(c, sizeof(c) - 1, "%d / 4", count);
    c[sizeof(c) - 1] = '\0';
    lineIf(u.playersCount, g_locked ? std::string(c) : std::string("-"),
           g_locked ? C_TEXT : C_MUTED, !diag);
    ctlVisible(u.playersCard, !diag);
    int row = 0;
    if (g_locked) {
        for (; row < count; ++row) {
            std::string who, state;
            int tone = TONE_NORMAL;
            playerRow(st->players[row], who, state, tone);
            lineIf(u.pName[row], who, C_TEXT, !diag);
            lineIf(u.pState[row], state, toneRgb(tone), !diag);
            if (!rosterReport.empty()) rosterReport += ", ";
            rosterReport += who + " - " + state;
        }
    }
    const char* rosterNote = !g_locked ? ru::kNoSession
                           : (st->isHost && count <= 1) ? ru::kNobodyYet : 0;
    if (row < kPlayerRows) {
        hideLines(u.pState + row, kPlayerRows - row);
        if (rosterNote && !diag)
            fillLines(u.pName + row, kPlayerRows - row, rosterNote, F_BODY, C_MUTED);
        else
            hideLines(u.pName + row, kPlayerRows - row);
    }

    // The mod updater is not the connection: its own secondary section.
    const std::string update = st->updateDetail ? std::string(st->updateDetail) : std::string();
    heading(u.updHdr, ru::kHdrUpdate, !update.empty());
    if (update.empty()) hideLines(u.upd, 2);
    else fillLines(u.upd, 2, update, F_SMALL, C_BUSY);

    // Diagnostics (rates and internals live only here and in the report).
    const std::string diagText =
        st->diagnosticsDetail ? std::string(st->diagnosticsDetail) : std::string();
    heading(u.hdrDiag, ru::kHdrDiag, diag);
    ctlVisible(u.diagCard, diag);
    int dn = 0;
    if (diag) {
        if (diagText.empty()) {
            lineIf(u.diagKey[0], ru::kDiagNone, C_MUTED, true);
            ctlVisible(u.diagVal[0], false);
            dn = 1;
        } else {
            size_t start = 0;
            while (start < diagText.size() && dn < u.diagRows) {
                size_t nl = diagText.find('\n', start);
                if (nl == std::string::npos) nl = diagText.size();
                std::string line = diagText.substr(start, nl - start);
                start = nl + 1;
                size_t cr = line.find('\r');
                if (cr != std::string::npos) line.erase(cr);
                if (line.empty()) continue;
                const size_t colon = line.find(": ");
                if (colon != std::string::npos) {
                    lineIf(u.diagKey[dn], line.substr(0, colon), C_MUTED, true);
                    lineIf(u.diagVal[dn], line.substr(colon + 2), C_TEXT, true);
                } else {
                    lineIf(u.diagKey[dn], line, C_MUTED, true);
                    ctlVisible(u.diagVal[dn], false);
                }
                ++dn;
            }
            if (start < diagText.size() && dn > 0) {
                lineIf(u.diagKey[dn - 1], "...", C_BUSY, true);
                lineIf(u.diagVal[dn - 1], ru::kCopyReport, C_BUSY, true);
            }
        }
    }
    hideLines(u.diagKey + dn, u.diagRows - dn);
    hideLines(u.diagVal + dn, u.diagRows - dn);

    // Plain-text report for "Copy report": what the panel shows plus the raw
    // diagnostics, so a pasted report needs no screenshot.
    std::string world;
    worldStatus(st, host, world);
    report.clear();
    report += "KenshiCoop ";
    report += (st->versionText && st->versionText[0]) ? st->versionText : "-";
    report += "\r\n";
    appendReportLine(report, ru::kRowRole, role);
    appendReportLine(report, ru::kRepState, status);
    appendReportLine(report, ru::kRepDetail, detail);
    appendReportLine(report, ru::kRepError, error);
    appendReportLine(report, ru::kRowLink, link);
    std::string stepsReport;
    for (int i = 0; i < kStepRows; ++i) {
        if (i) stepsReport += "; ";
        stepsReport += steps[i].name;
        stepsReport += " - ";
        stepsReport += steps[i].value;
    }
    appendReportLine(report, ru::kRepSteps, stepsReport);
    appendReportLine(report, host ? ru::kRepWorldOwn : ru::kRepWorldHost, world);
    if (!transfer.empty()) appendReportLine(report, ru::kSReceiving, transfer);
    appendReportLine(report, ru::kRepPlayers, rosterReport);
    if (!update.empty()) report += std::string(ru::kUpdate) + update + "\r\n";
    report += "--- ";
    report += ru::kRepDiag;
    report += " ---\r\n";
    if (diagText.empty()) {
        report += ru::kDiagNone;
        report += "\r\n";
    } else {
        for (size_t i = 0; i < diagText.size(); ++i) {
            if (diagText[i] == '\n' && (i == 0 || diagText[i - 1] != '\r')) report += '\r';
            report += diagText[i];
        }
        report += "\r\n";
    }
}

void refreshFooter(const CoopUiSnapshot* st) {
    PanelWidgets& u = g_ui;
    ctlVisible(u.colRule, true);
    ctlVisible(u.footRule, true);
    if (noteLive(NOTE_FOOT)) {
        lineIf(u.footText, g_note[NOTE_FOOT].text, toneRgb(g_note[NOTE_FOOT].tone), true);
    } else {
        std::string foot = "KenshiCoop ";
        foot += (st->versionText && st->versionText[0]) ? st->versionText : "-";
        foot += "   |   ";
        foot += ru::kTitleHint;
        lineIf(u.footText, foot, C_MUTED, true);
    }
    Ctl* buttons[3] = { &u.btnDiag, &u.btnReport, &u.btnHide };
    const char* text[3] = { g_panel.diagOpen ? ru::kDiagHide : ru::kDiagShow, ru::kCopyReport,
                            ru::kHide };
    for (int i = 0; i < 3; ++i) {
        ctlText(*buttons[i], text[i]);
        ctlVisible(*buttons[i], true);
        ctlEnabled(*buttons[i], true);
        ctlButtonColour(*buttons[i], C_SOFT, C_HOVER);
    }
}

// While a session holds role / transport / endpoint, the locked field shows
// the armed config, not a stale draft.
void mirrorArmedEndpoint(const CoopUiSnapshot* st) {
    if (!g_locked) return;
    const int ep = activeEndpoint();
    std::string armed;
    if (ep == FLD_PORT) {
        if (st->udpPort > 0) armed = decimal((unsigned long long)st->udpPort);
    } else if (ep == FLD_HOSTID) {
        if (st->peerSteamId) armed = decimal(st->peerSteamId);
    } else if (ep == FLD_ADDR) {
        if (st->udpIp && st->udpIp[0]) armed = addrText(st->udpIp, st->udpPort);
    }
    if (!armed.empty() && armed != g_draft[ep].text) writeField(ep, armed);
}

void refreshPanel(const CoopUiSnapshot* st) {
    readDrafts();
    commitDrafts(false);
    mirrorArmedEndpoint(st);
    const int focused = focusedField();
    refreshLeft(st, focused);
    refreshRight(st, g_reportText);
    refreshFooter(st);
}

// Between refreshes only the hover colour of the buttons follows the mouse.
void hoverPass() {
    PanelWidgets& u = g_ui;
    Ctl* b[] = { &u.selHost.btn, &u.selJoin.btn, &u.selSteam.btn, &u.selUdp.btn,
                 &u.nick.button, &u.hostId.button, &u.addr.button, &u.port.button,
                 &u.copyId, &u.primary.btn, &u.btnDiag, &u.btnReport, &u.btnHide };
    for (size_t i = 0; i < sizeof(b) / sizeof(b[0]); ++i) {
        Ctl& c = *b[i];
        if (c.w && c.colSet)
            ctlColour(c, (c.w == g_mouseFocus && c.enabled == 1) ? c.hover : c.base);
    }
}

// Tab / Shift+Tab: the visible editable fields, in reading order, wrapping.
void cycleFocus(int dir) {
    MyGUI::Widget* order[2];
    int n = 0;
    if (g_ui.nick.edit.w) order[n++] = g_ui.nick.edit.w;
    const int ep = activeEndpoint();
    if (ep >= 0 && !g_locked && fieldEdit(ep).w) order[n++] = fieldEdit(ep).w;
    if (!n) return;
    const int at = focusedField();
    int cur = -1;
    for (int i = 0; i < n; ++i)
        if (at >= 0 && order[i] == fieldEdit(at).w) cur = i;
    const int next = cur < 0 ? (dir > 0 ? 0 : n - 1) : (cur + dir + n) % n;
    coopui::native::focus(order[next]);
    g_dirty = true;
}

// ---- Command hand-off ------------------------------------------------------------
void copyBounded(char* dst, size_t cap, const std::string& s) {
    size_t n = s.size();
    if (n >= cap) n = cap - 1;
    std::memcpy(dst, s.data(), n);
    dst[n] = '\0';
}

void fillSettings(CoopUiSettings* s) {
    std::memset(s, 0, sizeof(*s));
    s->isHost = g_panel.hostFlag ? 1 : 0;
    s->useSteam = g_panel.steamFlag ? 1 : 0;
    s->hostSteamId = g_hostSteamId;
    s->udpPort = g_udpPort;
    copyBounded(s->udpIp, sizeof(s->udpIp), g_udpIp);
    copyBounded(s->playerName, sizeof(s->playerName), g_nick);
}

// Turn the queued primary action (checked against the CURRENT state: a start
// only from idle with every needed field valid right now, a stop only while
// something runs) or pending settings change into the tick's single command.
// CONNECT carries the settings too, so it also settles a pending REMEMBER; a
// REMEMBER that meets a DISCONNECT goes out on the next tick. cmd == 0: no
// command channel this tick - the action is dropped exactly like a missing
// callback was.
void emitCommand(CoopUiCommand* cmd) {
    const int act = g_panel.pendingAction;
    g_panel.pendingAction = ACT_NONE;
    if (act == ACT_START && !g_locked) {
        readDrafts(); // the text typed since the last refresh counts
        if (!startReady()) {
            logLine("[coop-ui] start refused: a needed field is empty or invalid");
            g_dirty = true;
            return;
        }
        commitDrafts(true);
        char b[80];
        _snprintf(b, sizeof(b) - 1, "[coop-ui] CONNECT role=%s transport=%s",
                  g_panel.hostFlag ? "HOST" : "JOIN",
                  g_panel.steamFlag ? "steam" : "udp");
        b[sizeof(b) - 1] = '\0';
        logLine(b);
        if (cmd) {
            cmd->kind = COOP_UI_CONNECT;
            fillSettings(&cmd->settings);
            g_rememberPending = false;
        }
        return;
    }
    if (act == ACT_STOP && g_locked) {
        logLine("[coop-ui] DISCONNECT requested");
        if (cmd) cmd->kind = COOP_UI_DISCONNECT;
        return;
    }
    if (g_rememberPending && cmd) {
        cmd->kind = COOP_UI_REMEMBER;
        fillSettings(&cmd->settings);
        g_rememberPending = false;
    }
}

void panelTick(const CoopUiSnapshot* st, CoopUiCommand* cmd) {
    seedMemory(st);
    ForgottenGUI* g = ::gui; // KenshiLib data export (spike 46)
    { static void* s_last = (void*)-1;
      if ((void*)g != s_last) { s_last = (void*)g;
          char b[64]; _snprintf(b, sizeof(b) - 1, "[coop-ui] gui ptr=%p", (void*)g);
          b[sizeof(b) - 1] = '\0'; logLine(b); } }

    // A replaced GUI object took the panel with it, unlinker or not. A GUI
    // that is only momentarily absent (null) keeps the panel.
    if (g_panel.frame && g && g != g_panel.gui) g_panelLost = true;
    if (g_panelLost)
        forgetPanel("[coop-ui] panel widgets destroyed by a GUI reset - rebuilt on next F2");

    // Busy = a launch was accepted or the worker is alive. While busy the panel
    // shows (and can only act on) the armed role/transport.
    g_locked = st->busy || st->running;
    if (g_locked) {
        g_panel.hostFlag = st->isHost != 0;
        g_panel.steamFlag = (st->transportSel == 0);
    }

    // Enter in a field: a start, and only from idle (never a disconnect).
    if (g_enterPressed) {
        g_enterPressed = false;
        if (!g_locked && g_panel.open && g_panel.built) g_panel.pendingAction = ACT_START;
    }
    // Queued Start/Stop first, so "Hide" pressed in the same frame cannot drop it.
    emitCommand(cmd);
    if (!g) {
        return;
    }

    g_selfIdStr = st->selfSteamId ? decimal(st->selfSteamId) : std::string();

    // F2 toggles; Esc, the close button and "Hide" only close. Never a disconnect.
    // GetAsyncKeyState is desktop-global: only the foreground game handles the press.
    static const DWORD processId = GetCurrentProcessId();
    DWORD foregroundProcess = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &foregroundProcess);
    const bool active = foregroundProcess == processId;
    const bool f2 = (GetAsyncKeyState(VK_F2) & 0x8000) != 0;
    const bool esc = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
    const bool f2Press = active && f2 && !g_panel.f2Down;
    const bool escPress = active && esc && !g_panel.escDown;
    g_panel.f2Down = f2;
    g_panel.escDown = esc;
    bool hiddenByEngine = false;
    if (g_panel.open && g_panel.frame) {
        const int vis = frameVisibleSeh(g_panel.frame);
        if (vis == 1) g_panel.seenVisible = true;
        else if (vis == 0 && g_panel.seenVisible) hiddenByEngine = true;
    }
    if (g_panel.open && (f2Press || escPress || g_panel.closeRequested || hiddenByEngine)) {
        hidePanel();
    } else if (!g_panel.open && f2Press) {
        if (!g_locked) {
            g_panel.hostFlag = st->isHost != 0;
            g_panel.steamFlag = (st->transportSel == 0);
        }
        g_panel.open = true;
        g_dirty = true;
        logLine("[coop-ui] panel opened");
        if (g_panel.frame) panelShowSeh(g_panel.frame, true);
    }
    g_panel.closeRequested = false;
    if (!g_panel.open) {
        coopui::native::guardGameInput(false);
        return;
    }
    if (!g_panel.frame && !openPanelWindows(g)) {
        g_panel.open = false; // retry on the next F2 rather than every frame
        coopui::native::guardGameInput(false);
        return;
    }

    g_mouseFocus = mouseFocusSeh();
    if (g_pendingPaste >= 0) {
        const int f = g_pendingPaste;
        g_pendingPaste = -1;
        runPaste(f);
        g_dirty = true;
    }
    if (g_tabDir) {
        cycleFocus(g_tabDir);
        g_tabDir = 0;
    }

    // Widgets persist; captions / colours / visibility are re-derived at 10 Hz
    // (immediately after a click) and only what changed reaches the engine.
    static DWORD drawnAt = 0;
    const DWORD now = GetTickCount();
    if (g_dirty || (DWORD)(now - drawnAt) >= 100) {
        drawnAt = now;
        g_dirty = false;
        refreshPanel(st);
    } else {
        hoverPass();
    }

    // Game input is held off only while one of the panel's visible, editable
    // fields has the keyboard; focus left on a hidden / locked field is dropped.
    const int focused = focusedField();
    bool editing = false;
    if (focused >= 0) {
        Ctl& e = fieldEdit(focused);
        if (e.visible == 1 && e.readOnly != 1) editing = true;
        else coopui::native::releaseFocus(e.w);
    }
    coopui::native::guardGameInput(editing);
}

// ---- Persistent co-op connection banner ------------------------------------------
// A fixed banner in the top-left corner of the screen showing live session status
// colored by state (0 = offline/red, 1 = waiting/yellow, 2 = connected/green).
// It needs no player character and holds its place while the camera moves -
// which also makes it visible at the title screen, where a join has no leader
// while it streams the host's world. Removed when there is nothing to show.
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

// Banner box in pixels: 10 px in from the top-left corner. Applied with the
// absolute setCoord instead of createFloatingLabel's normalized coords, since a
// corner inset is a pixel quantity and the reconstructed header's top/left
// argument order is ambiguous (the normalized values are overwritten either way).
const int kOverlayX = 10;
const int kOverlayY = 10;
const int kOverlayW = 520;
const int kOverlayH = 26;

MyGUI::Window*  g_overlayBox    = 0; // container: geometry + layer attachment
MyGUI::TextBox* g_overlay       = 0; // the label that actually draws the text
bool            g_overlayFonted = false;
int             g_overlayState  = -1;
std::string     g_overlayText;

void bannerColour(int state, MyGUI::Colour* col) {
    switch (state) {
    case 2:  *col = MyGUI::Colour(0.30f, 1.00f, 0.30f, 1.0f); break; // connected
    case 1:  *col = MyGUI::Colour(1.00f, 0.90f, 0.25f, 1.0f); break; // waiting
    default: *col = MyGUI::Colour(1.00f, 0.25f, 0.25f, 1.0f); break; // offline
    }
}

// Put the freshly-minted container in its pixel box and mint the label inside it.
// createLabelAbs takes its text by const-ref and MyGUI::Align is a trivial int
// wrapper (no destructor), so this whole frame is SEH-safe - the same rule
// rowStyleSeh follows for MyGUI::Colour.
MyGUI::TextBox* overlayBuildSeh(ForgottenGUI* g, MyGUI::Window* box,
                                const std::string* text, int y) {
    __try {
        box->setCoord(kOverlayX, y, kOverlayW, kOverlayH);
        box->setVisible(true);
        MyGUI::TextBox* l = g->createLabelAbs(box, 0, 0, kOverlayW, kOverlayH,
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

void overlayForget() {
    g_overlayBox = 0; g_overlay = 0; g_overlayFonted = false;
    g_overlayState = -1; g_overlayText.clear();
}

void overlayTick(const char* text, int state, bool show) {
    ForgottenGUI* g = ::gui;
    if (!g) return;

    if (!show) {
        if (g_overlayBox) {
            overlayDestroySeh(g, g_overlayBox);
            overlayForget();
        }
        return;
    }

    std::string t = text ? std::string(text) : std::string();
    if (!g_overlay) {
        // createFloatingLabel takes the layer BY VALUE (an unwindable temporary
        // => C2712), so the container mint stays outside SEH, exactly like
        // createDatapanel; ::gui was verified non-null. Layer MUST be "Info"
        // for the same reason the panel uses it - nothing draws on "Windows".
        if (g_overlayBox) { overlayDestroySeh(g, g_overlayBox); overlayForget(); }
        std::string layer = "Info";
        std::string empty;
        g_overlayBox = g->createFloatingLabel(0.01f, 0.01f, 0.30f, 0.03f, empty,
                                              MyGUI::Align::Default, layer);
        if (!g_overlayBox) {
            logErrLine("[coop-ui] banner container FAILED");
            return;
        }
        g_overlay = overlayBuildSeh(g, g_overlayBox, &t, kOverlayY);
        char b[96];
        _snprintf(b, sizeof(b) - 1, "[coop-ui] banner box=%p label=%p",
                  (void*)g_overlayBox, (void*)g_overlay);
        b[sizeof(b) - 1] = '\0';
        logLine(b);
        if (!g_overlay) {
            logErrLine("[coop-ui] banner label FAILED");
            return;
        }
        g_overlayState = -1;   // no caller state is -1: forces the caption pass
        g_overlayText.clear();
    }
    // The status text is Russian: the label needs the Cyrillic-capable font.
    if (!g_overlayFonted && ensurePanelFont())
        g_overlayFonted = textFontSeh(g_overlay, &kPanelFont, kFontHeight);

    if (t != g_overlayText || state != g_overlayState) {
        MyGUI::Colour col;
        bannerColour(state, &col);
        MyGUI::UString u(t.c_str());
        if (overlayUpdateSeh(g_overlay, &u, &col)) {
            g_overlayText = t; g_overlayState = state;
        } else {
            // The GUI destroyed the widgets under us - clearGUI() on a world load
            // empties the layer and notifies nobody, so a pointer held across
            // ticks dangles silently. Forget them and re-mint on the next tick.
            overlayForget();
        }
    }
}

// Banner: red = offline/failed, green = a real session (host with an admitted
// join, or a join past WELCOME), yellow = everything in between. A failed start
// stays visible (red) until the next Connect/Disconnect.
void bannerTick(const CoopUiSnapshot* st) {
    int state = 1;
    if (st->phase == COOP_OFFLINE || st->phase == COOP_FAILED)
        state = 0;
    else if ((st->phase == COOP_HOSTING && st->playerCount > 1) ||
             st->phase == COOP_CONNECTED)
        state = 2;
    overlayTick(st->detail, state, st->running || st->phase == COOP_FAILED);
}

// ---- C ABI -------------------------------------------------------------------------
bool g_initialized = false;
bool g_stopped = false; // shutdown ran: the UI stays hidden for the process

int COOP_UI_CALL apiInitialize(const CoopUiHost* host) {
    if (!host || host->structSize < sizeof(CoopUiHost) ||
        host->apiVersion != COOP_UI_API_VERSION)
        return 0;
    g_hostLog = host->log;
    if (!g_initialized) {
        // Pin: MyGUI delegates and engine-held callbacks point into this module.
        HMODULE self = 0;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_PIN,
                                (LPCWSTR)&apiInitialize, &self))
            logErrLine("[coop-ui] module pin FAILED");
        g_initialized = true;
        logLine("[coop-ui] UI module initialized");
    }
    return 1;
}

void COOP_UI_CALL apiTick(const CoopUiSnapshot* st, CoopUiCommand* cmd) {
    if (!g_initialized || g_stopped || !st || st->structSize < sizeof(CoopUiSnapshot))
        return;
    if (cmd && cmd->structSize != sizeof(CoopUiCommand)) cmd = 0;
    try {
        panelTick(st, cmd);
        bannerTick(st);
    } catch (...) {
        // Never let a C++ exception cross the C boundary.
        logErrLine("[coop-ui] tick exception");
    }
}

// Hide only: never a disconnect and never a widget teardown (the GUI may be the
// reason the core is shutting the UI down).
void COOP_UI_CALL apiShutdown(void) {
    if (!g_initialized || g_stopped) return;
    g_stopped = true;
    // Give the keyboard back first: focus in a panel field, then the game-input
    // lease (both no-ops when not held). A lost panel is not touched.
    if (g_panel.built && !g_panelLost) coopui::native::releaseFocus(g_panel.win);
    coopui::native::guardGameInput(false);
    if (g_panel.frame && !g_panelLost) panelShowSeh(g_panel.frame, false);
    g_panel.open = false;
    g_panel.pendingAction = ACT_NONE;
    widgetHideSeh(g_overlayBox);
    logLine("[coop-ui] UI module stopped (hidden)");
}

} // namespace

extern "C" __declspec(dllexport) int COOP_UI_CALL
KenshiCoopUI_GetApi(unsigned int requestedVersion, unsigned int apiSize, CoopUiApi* api) {
    if (!api || requestedVersion != COOP_UI_API_VERSION || apiSize < sizeof(CoopUiApi))
        return 0;
    std::memset(api, 0, sizeof(CoopUiApi));
    api->structSize = sizeof(CoopUiApi);
    api->apiVersion = COOP_UI_API_VERSION;
    api->initialize = &apiInitialize;
    api->tick = &apiTick;
    api->shutdown = &apiShutdown;
    return 1;
}
