#ifndef KENSHICOOP_UI_PRIVACY_H
#define KENSHICOOP_UI_PRIVACY_H
#include "CoopUiApi.h"
#include "../plugin/core/SteamId.h"
#include "../plugin/core/UdpEndpoint.h"
#include <string>

namespace coopui {
namespace privacy {
inline bool digit(char c) { return c >= '0' && c <= '9'; }
inline bool word(char c) {
    const unsigned char u = static_cast<unsigned char>(c);
    return digit(c) || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || u >= 128;
}
inline std::string alias(unsigned int id) {
    char b[12]; int n = 0;
    do { b[n++] = static_cast<char>('0' + id % 10); id /= 10; } while (id);
    std::string s("Player ");
    while (n) s += b[--n];
    return s;
}
inline bool boundary(const std::string& s, size_t pos, size_t len) {
    return (pos == 0 || !word(s[pos - 1])) && (pos + len == s.size() || !word(s[pos + len]));
}
inline size_t ipv4Length(const std::string& s, size_t at) {
    size_t p = at;
    if (at && (word(s[at - 1]) || s[at - 1] == '.')) return 0;
    for (int part = 0; part < 4; ++part) {
        int value = 0, count = 0;
        while (p < s.size() && digit(s[p])) { value = value * 10 + s[p++] - '0'; if (++count > 3) return 0; }
        if (!count || value > 255) return 0;
        if (part != 3) { if (p == s.size() || s[p++] != '.') return 0; }
    }
    if (p < s.size() && (word(s[p]) || s[p] == '.')) return 0;
    return p - at;
}
inline std::string steam(unsigned long long id, bool report) {
    return report ? std::string("[Steam ID]") : coop::maskSteamId64(id);
}
inline std::string line(std::string s, const CoopUiSnapshot& st, bool report) {
    // Defense in depth: native credentials are opaque and consume their token.
    size_t secret = 0;
    while ((secret = s.find("KCU1", secret)) != std::string::npos) {
        size_t end = s.find_first_of(" \t\r\n'\"", secret);
        s.replace(secret, end == std::string::npos ? s.size() - secret : end - secret, "[invitation]");
        secret += 12;
    }
    const size_t colon = s.find(": ");
    const std::string key = colon == std::string::npos ? std::string() : s.substr(0, colon);
    // Diagnostic payloads can lag an edited endpoint. Mask the address field
    // by grammar, independently of the current snapshot, retaining its port.
    if (key == "Host" || key == "Host address" ||
        key == "\xD0\x90\xD0\xB4\xD1\x80\xD0\xB5\xD1\x81 \xD1\x85\xD0\xBE\xD1\x81\xD1\x82\xD0\xB0") {
        const size_t start = colon + 2;
        const size_t close = s.find(']', start);
        const size_t port = start < s.size() && s[start] == '['
            ? (close == std::string::npos ? std::string::npos : s.find(':', close))
            : s.find(':', start);
        if (start < s.size() && s[start] != '-')
            s.replace(start, (port == std::string::npos ? s.size() : port) - start, "[address]");
    }
    if (colon != std::string::npos && (key.find("Steam ID") != std::string::npos || key.find("SteamID") != std::string::npos)) {
        const size_t start = colon + 2; size_t end = start; unsigned long long id = 0;
        while (end < s.size() && digit(s[end])) id = id * 10 + s[end++] - '0';
        if (end > start) s.replace(start, end - start, steam(id, report));
    }
    // Roster diagnostics have stable slot IDs. Quote delimiters belong to the
    // format, not to the nickname (a nickname may itself contain an apostrophe).
    if (report) {
        const size_t idAt = s.find("id ");
        if (idAt != std::string::npos) {
            size_t p = idAt + 3; unsigned int id = 0;
            while (p < s.size() && digit(s[p])) id = id * 10 + s[p++] - '0';
            const size_t quote = s.find(": '", p), end = s.rfind("', ");
            if (quote != std::string::npos && end != std::string::npos && end >= quote + 3) {
                s.replace(quote + 3, end - quote - 3, alias(id));
            }
        }
    }
    std::string out; out.reserve(s.size() + 16);
    const char* address = st.udpIp ? st.udpIp : "";
    const size_t addressLen = std::char_traits<char>::length(address);
    bool numericAddress = addressLen != 0;
    for (size_t i = 0; i < addressLen; ++i)
        if (!digit(address[i])) { numericAddress = false; break; }
    const bool versionLine = s.compare(0, 10, "KenshiCoop ") == 0 || key == "Version" ||
                             key == "version" || key == "Build";
    for (size_t p = 0; p < s.size();) {
        if ((s[p] == '[' || s[p] == ':' || digit(s[p]) ||
            (s[p] >= 'a' && s[p] <= 'f') || (s[p] >= 'A' && s[p] <= 'F')) &&
            (!p || !word(s[p - 1]))) {
            size_t end = s.find_first_of(" \t\r\n,()'\"", p);
            if (end == std::string::npos) end = s.size();
            const size_t firstColon = s.find(':', p);
            if (firstColon < end && s.find(':', firstColon + 1) < end) {
                const std::string token = s.substr(p, end - p);
                std::string ip; int port = -1;
                if (coop::parseUdpEndpoint(token, ip, port)) {
                    out += "[address]";
                    if (token[0] == '[') {
                        const size_t close = token.find(']');
                        if (close != std::string::npos && close + 1 < token.size()) out += token.substr(close + 1);
                    }
                    p = end; continue;
                }
            }
        }
        if (!numericAddress && addressLen > 1 && s.compare(p, addressLen, address) == 0 && boundary(s, p, addressLen)) {
            out += "[address]"; p += addressLen; continue;
        }
        if (digit(s[p]) && (!p || !word(s[p - 1]))) {
            size_t end = p; unsigned long long id = 0;
            while (end < s.size() && digit(s[end])) { if (end - p < 20) id = id * 10 + s[end] - '0'; ++end; }
            if (end - p == 17 && s.compare(p, 5, "76561") == 0 && (end == s.size() || !word(s[end]))) {
                out += steam(id, report); p = end; continue;
            }
            const size_t ip = versionLine ? 0 : ipv4Length(s, p);
            if (ip) { out += "[address]"; p += ip; continue; }
        }
        // Participant text is anonymized above by the roster grammar. Do not
        // replace nicknames in arbitrary values: a player may be named READY,
        // UDP or failed, which are also essential technical report facts.
        out += s[p++];
    }
    return out;
}
}
// A single presentation boundary for messages, diagnostics and report export.
// Real endpoint settings and explicit clipboard sharing never pass through it.
inline std::string redact(const std::string& text, const CoopUiSnapshot& state, bool forReport) {
    std::string out; out.reserve(text.size());
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        out += privacy::line(text.substr(at, end - at), state, forReport);
        if (end < text.size()) out += '\n';
        at = end + 1;
    }
    return out;
}
}
#endif
