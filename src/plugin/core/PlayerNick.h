// PlayerNick.h - paste-parse a co-op display name (pure, no Win32).
//
// Sanitize a display nick from the F2 edit row (or clipboard / config). Text is
// noisy (whitespace, a trailing newline). This keeps the first line, strips
// ASCII controls, and caps at PLAYER_NICK_MAX so the result fits
// HelloPacket.nameLen (0..63).

#ifndef COOP_PLAYER_NICK_H
#define COOP_PLAYER_NICK_H

#include <string>

namespace coop {

// Nicks a build before v0.1.2 could WRITE TO DISK by mistake. The F2 text row
// used to fall back to its own row key when the field read back empty, so
// "nickedit" (and "udpedit") got harvested as if the player had typed them, and
// then persisted to coop_config.json - after which they load back as a
// perfectly legitimate remembered nick forever. Session 15:33 shows exactly
// that on a build that already had the fix: "nick=nickedit" came from the file,
// not the field. Rejecting them on read is what actually clears it, since the
// alternative is asking every player to hand-edit JSON.
//
// These are internal row keys and nobody would choose them as a name; the cost
// of the false positive is one player retyping a nick.
inline bool isPoisonedNick(const std::string& s) {
    return s == "nickedit" || s == "udpedit";
}

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
