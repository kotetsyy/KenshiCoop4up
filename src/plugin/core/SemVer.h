// SemVer.h - ordering for KenshiCoop build ids (COOP_BUILD_VERSION).
//
// Header-only and dependency-free so the wire/unit test layer can lock the
// behaviour without pulling in the updater or the game. Lives here rather than
// inside Updater.cpp because a mistake in this comparison is not a local bug:
// it decides whether every player's client accepts a build. Getting ">" backwards
// would push the whole player base onto an old DLL, and getting parsing wrong
// would wedge them on the current one with no way to publish a fix.
//
// Format: MAJOR.MINOR.PATCH, decimal, missing components read as 0 - so "0.1"
// and "0.1.0" are the same version. Anything else (letters, spaces, a fourth
// component, an empty string) is UNORDERABLE and reports ok=false; callers are
// expected to fall back to plain inequality there rather than guess, so a
// malformed version string stays recoverable by publishing a good one.

#ifndef KENSHICOOP_SEMVER_H
#define KENSHICOOP_SEMVER_H

#include <string>

namespace coop {

struct Ver {
    unsigned a, b, c;
    bool     ok; // false = not orderable; a/b/c are meaningless
};

inline Ver parseVer(const std::string& s) {
    Ver v; v.a = 0; v.b = 0; v.c = 0; v.ok = false;
    unsigned part[3] = { 0, 0, 0 };
    int idx = 0;
    bool anyDigit = false;
    for (size_t i = 0; i < s.size(); ++i) {
        char ch = s[i];
        if (ch >= '0' && ch <= '9') {
            part[idx] = part[idx] * 10u + (unsigned)(ch - '0');
            if (part[idx] > 100000u) return v; // junk, or an overflow attempt
            anyDigit = true;
        } else if (ch == '.') {
            if (++idx > 2) return v;           // more than three components
        } else {
            return v;                          // letters, spaces, '-', anything
        }
    }
    if (!anyDigit) return v;                   // "", ".", "..'
    v.a = part[0]; v.b = part[1]; v.c = part[2]; v.ok = true;
    return v;
}

// -1 / 0 / +1 for l<r, l==r, l>r. Both sides must be .ok - comparing an
// unorderable version is a caller error, and answering 0 here would read as
// "same version" and silently take the same-version branch.
inline int cmpVer(const Ver& l, const Ver& r) {
    if (l.a != r.a) return l.a < r.a ? -1 : 1;
    if (l.b != r.b) return l.b < r.b ? -1 : 1;
    if (l.c != r.c) return l.c < r.c ? -1 : 1;
    return 0;
}

} // namespace coop

#endif // KENSHICOOP_SEMVER_H
