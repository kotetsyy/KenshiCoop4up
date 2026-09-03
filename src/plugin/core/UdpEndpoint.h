// UdpEndpoint.h - paste-parse a host:port from clipboard text (pure, no Win32).
//
// The F2 panel has no text fields, so a join on UDP pastes an address the same
// way they paste a Steam ID. Clipboard text is noisy (whitespace, "ip:port",
// "host port", a trailing newline). This keeps only the first host token and an
// optional port so junk is rejected before it is written to coop_config.json.

#ifndef COOP_UDP_ENDPOINT_H
#define COOP_UDP_ENDPOINT_H

#include <string>

namespace coop {

// Parse an IPv4 / hostname endpoint. On success writes ip and, if a port was
// present, port (1..65535). If the text has a host but no port, ip is written
// and port is left untouched. Returns false on empty / invalid input (out
// params unchanged).
inline bool parseUdpEndpoint(const std::string& text, std::string& ip, int& port) {
    std::string t;
    t.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        char ch = text[i];
        if (ch == '\r' || ch == '\n' || ch == '\t') ch = ' ';
        t += ch;
    }
    size_t a = 0;
    while (a < t.size() && t[a] == ' ') ++a;
    size_t b = t.size();
    while (b > a && t[b - 1] == ' ') --b;
    if (a >= b) return false;
    t = t.substr(a, b - a);

    std::string host;
    std::string portStr;
    size_t colon = t.rfind(':');
    size_t space = t.find(' ');
    if (colon != std::string::npos && (space == std::string::npos || colon < space)) {
        host = t.substr(0, colon);
        size_t pe = colon + 1;
        while (pe < t.size() && t[pe] != ' ') ++pe;
        portStr = t.substr(colon + 1, pe - (colon + 1));
    } else if (space != std::string::npos) {
        host = t.substr(0, space);
        size_t ps = space + 1;
        while (ps < t.size() && t[ps] == ' ') ++ps;
        size_t pe = ps;
        while (pe < t.size() && t[pe] != ' ') ++pe;
        portStr = t.substr(ps, pe - ps);
    } else {
        host = t;
    }
    while (!host.empty() && host[host.size() - 1] == ' ') host.erase(host.size() - 1);
    if (host.empty() || host.size() > 253) return false;
    for (size_t i = 0; i < host.size(); ++i) {
        char ch = host[i];
        bool ok = (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') ||
                  (ch >= 'a' && ch <= 'z') || ch == '.' || ch == '-';
        if (!ok) return false;
    }
    if (host[0] == '.' || host[0] == '-') return false;

    int p = -1;
    if (!portStr.empty()) {
        if (portStr.size() > 5) return false;
        p = 0;
        for (size_t i = 0; i < portStr.size(); ++i) {
            if (portStr[i] < '0' || portStr[i] > '9') return false;
            p = p * 10 + (portStr[i] - '0');
        }
        if (p < 1 || p > 65535) return false;
    }
    ip = host;
    if (p >= 1) port = p;
    return true;
}

} // namespace coop

#endif // COOP_UDP_ENDPOINT_H
