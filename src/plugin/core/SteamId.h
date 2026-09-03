// SteamId.h - Steam64 ID parsing + display masking (pure, zero game/Win32 deps).
//
// The F2 panel lets a player paste a friend's Steam ID from the clipboard
// instead of editing coop_config.json. Clipboard text is noisy (surrounding
// whitespace, a trailing newline, or a "Steam ID: 7656..." wrapper the friend
// copied), so the digits are extracted and validated before use. This logic is
// shared by:
//   * EngineEntity.cpp - the "Paste friend's Steam ID" button
//   * prototest        - the no-game unit layer that guards the parse
//
// A Steam community ID (SteamID64) is a 17-digit decimal that begins with the
// individual-account prefix 76561 (base 0x0110000100000000). We require exactly
// 17 digits and that prefix so arbitrary clipboard junk is rejected.

#ifndef COOP_STEAM_ID_H
#define COOP_STEAM_ID_H

#include <string>

namespace coop {

// Parse a SteamID64 out of arbitrary text: keep only decimal digits, then accept
// it iff it is exactly 17 digits and starts with "76561" (the community-ID
// prefix). On success writes the value to out and returns true; otherwise leaves
// out untouched and returns false. Pure - safe to unit-test without the game.
inline bool parseSteamId64(const std::string& text, unsigned long long& out) {
    std::string digits;
    digits.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        char ch = text[i];
        if (ch >= '0' && ch <= '9') digits += ch;
    }
    if (digits.size() != 17) return false;
    if (digits.compare(0, 5, "76561") != 0) return false;
    unsigned long long v = 0;
    for (size_t i = 0; i < digits.size(); ++i) {
        v = v * 10ull + (unsigned long long)(digits[i] - '0');
    }
    out = v;
    return true;
}

// Extract every 17-digit SteamID64 in `text` (comma/space/newline separated).
// Unlike parseSteamId64 this does NOT concatenate all digits, so two ids on
// the clipboard stay two ids. Writes up to maxOut unique values; returns count.
inline int parseSteamId64List(const std::string& text, unsigned long long* out, int maxOut) {
    if (!out || maxOut <= 0) return 0;
    int n = 0;
    std::string digits;
    for (size_t i = 0; i <= text.size(); ++i) {
        char ch = (i < text.size()) ? text[i] : 0;
        if (ch >= '0' && ch <= '9') digits += ch;
        else {
            if (digits.size() == 17 && digits.compare(0, 5, "76561") == 0 && n < maxOut) {
                unsigned long long v = 0;
                for (size_t k = 0; k < digits.size(); ++k)
                    v = v * 10ull + (unsigned long long)(digits[k] - '0');
                bool dup = false;
                for (int j = 0; j < n; ++j) if (out[j] == v) dup = true;
                if (!dup) out[n++] = v;
            }
            digits.clear();
        }
    }
    return n;
}

// Render an id for on-screen display with all but the last 4 digits hidden
// ("76561198012345678" -> "****5678"), so a player streaming or screen-sharing
// the F2 panel does not expose their (or their friend's) account. The digits are
// built here instead of with _snprintf("%llu") to keep this header pure.
// A pasted id is always 17 digits, but the config fallback (steamPeer) is not
// length-checked, so shorter values are tolerated: fewer than 4 digits yields
// "****" plus whatever exists.
inline std::string maskSteamId64(unsigned long long id) {
    char digits[24]; // decimal digits, least-significant first
    int n = 0;
    if (id == 0) digits[n++] = '0';
    while (id > 0 && n < 20) {
        digits[n++] = (char)('0' + (int)(id % 10ull));
        id /= 10ull;
    }
    std::string out("****");
    int take = n < 4 ? n : 4;
    for (int i = take - 1; i >= 0; --i) out += digits[i];
    return out;
}

} // namespace coop

#endif // COOP_STEAM_ID_H
