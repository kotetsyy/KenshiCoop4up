// Test-only snapshot of the published pre-native Updater.cpp parser and its
// pre-download required-field gate. Intentionally frozen, never runtime code.
#ifndef KENSHICOOP_LEGACY_UPDATER_PARSER_H
#define KENSHICOOP_LEGACY_UPDATER_PARSER_H
#include <map>
#include <string>
namespace legacyUpdaterSnapshot {
inline std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
    return s.substr(a, b - a);
}
inline std::map<std::string, std::string> parseManifest(const std::string& text) {
    std::map<std::string, std::string> m;
    size_t i = 0;
    while (i < text.size()) {
        size_t nl = text.find('\n', i);
        std::string line = text.substr(i, nl == std::string::npos ? std::string::npos : nl - i);
        i = (nl == std::string::npos) ? text.size() : nl + 1;
        size_t h = line.find('#');
        if (h != std::string::npos) line = line.substr(0, h);
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trim(line.substr(0, eq));
        std::string v = trim(line.substr(eq + 1));
        if (!k.empty()) m[k] = v;
    }
    return m;
}
inline bool reachesDownload(const std::string& body) {
    std::map<std::string,std::string> m=parseManifest(body);
    std::string ver=m["version"],sha=m["sha256"],url=m["url"],uiSha=m["uiSha256"],uiUrl=m["uiUrl"];
    // Exact original required-field condition; case normalization cannot change size.
    if (ver.empty() || sha.size() != 64 || url.empty() || uiSha.size() != 64 || uiUrl.empty()) return false;
    return true;
}
}
#endif
