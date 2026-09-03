// PlayerNick.h - paste-parse a co-op display name (pure, no Win32).
//
// The F2 panel has no text fields, so a player pastes a nick from the clipboard
// the same way they paste a Steam ID. Clipboard text is noisy (whitespace, a
// trailing newline, a wrapped line). This keeps the first line, strips ASCII
// controls, and caps at PLAYER_NICK_MAX so the result fits HelloPacket.nameLen
// (0..63) without inventing a packet.

#ifndef COOP_PLAYER_NICK_H
#define COOP_PLAYER_NICK_H

#include <string>

namespace coop {

const unsigned PLAYER_NICK_MAX = 63;

// Parse a display nick from clipboard / config text. On success writes nick
// (trimmed, controls stripped, at most PLAYER_NICK_MAX bytes). Returns false
// on empty / all-junk input (out param unchanged).
inline bool parsePlayerNick(const std::string& text, std::string& nick) {
    std::string t;
    t.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        unsigned char ch = (unsigned char)text[i];
        if (ch == '\r' || ch == '\n') break;
        if (ch < 32 || ch == 127) continue;
        t += (char)ch;
    }
    size_t a = 0;
    while (a < t.size() && t[a] == ' ') ++a;
    size_t b = t.size();
    while (b > a && t[b - 1] == ' ') --b;
    if (a >= b) return false;
    t = t.substr(a, b - a);
    if (t.size() > PLAYER_NICK_MAX) t.resize(PLAYER_NICK_MAX);
    if (t.empty()) return false;
    nick = t;
    return true;
}

} // namespace coop

#endif // COOP_PLAYER_NICK_H
