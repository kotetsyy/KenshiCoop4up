// Updater - in-DLL cloud update of KenshiCoop.dll itself.
//
// WHY THIS EXISTS: every player must run the SAME DLL. PROTOCOL_VERSION is a
// hard gate in the handshake with no back-compat, so one stale copy shows up as
// "it just won't connect" with nothing in the UI explaining why. Hand-shipping
// the file after every fix does not scale past a couple of sessions.
//
// SHAPE: a background Win32 thread (the game thread must never block on the
// network - that is the whole reason NetLink is threaded too) fetches a small
// text manifest over HTTPS, compares it against what this build is, and when it
// differs downloads the new DLL, verifies its SHA-256, and swaps it into place
// on disk. Windows lets you RENAME a mapped image even though it will not let
// you delete or overwrite one, which is what makes the swap possible from
// inside the very DLL being replaced. The running session keeps executing the
// old code - only the file on disk changes - so the update takes effect on the
// NEXT launch. The panel says so rather than pretending it is live.
//
// TRUST: the manifest and the payload come from a pinned owner/repo over TLS,
// and the payload must match the SHA-256 the manifest names or it is discarded.
// That makes whoever controls the repo able to run code on every player's
// machine - which is the deal with any auto-updater, and worth being explicit
// about: keep the release account on 2FA. A stronger version would ship a
// public key in the DLL and sign the manifest; the hash check is the floor, not
// the ceiling.
//
// MANIFEST FORMAT (dist/UPDATE.txt on the release branch), one key=value per
// line, '#' comments, unknown keys ignored so the format can grow:
//
//   version=0.1.0         # release id, MAJOR.MINOR.PATCH; a missing component
//                         # reads as 0. Installed only when it is NEWER than the
//                         # built-in build tag. A same-version SHA change (an
//                         # in-place GitHub asset replace for a tiny fix) also
//                         # counts as an update. An OLDER version is ignored
//                         # unless the manifest also carries allowDowngrade=1.
//   allowDowngrade=1      # optional; force a deliberate rollback
//   proto=59              # PROTOCOL_VERSION that build speaks (display only)
//   sha256=<64 hex>       # of the DLL at `url`
//   url=https://github.com/<owner>/<repo>/releases/download/v0.52/KenshiCoop.dll
//   notes=loot GUI crash  # optional one-liner for the panel
//
// Plain key=value rather than the Releases API's JSON: this path is security
// sensitive and hand-rolling a JSON parser in C++03 to read it would be the
// largest, least-reviewable part of the feature. A flat manifest also lets a
// release be pointed anywhere without teaching the client a new API shape.

#ifndef KENSHICOOP_UPDATER_H
#define KENSHICOOP_UPDATER_H

#include <string>

namespace coop {
namespace updater {

struct Settings {
    bool        enabled;   // update.enabled (default false until owner/repo set)
    std::string owner;     // GitHub account that publishes releases
    std::string repo;      // repository name
    std::string branch;    // branch the manifest lives on (default "main")
    std::string path;      // manifest path in the repo (default "dist/UPDATE.txt")
    bool        autoApply; // false = report only, never touch the file on disk
};

// Defaults + the coop_config.json "update.*" overrides. Never throws.
Settings settingsFromConfig();

// Spawn the check. `buildVersion` is what this DLL calls itself (compared
// against the manifest's version=); `protoVersion` is only displayed. Safe to
// call once per process; a second call is ignored. Returns false when disabled
// or already running.
bool start(const Settings& s, const char* buildVersion, unsigned int protoVersion);

// (No stop/join: the check is one-shot, every WinHTTP call carries a timeout,
// and the plugin has no process-teardown hook to call one from - the thread is
// reaped with the process. Add one here if that ever changes.)

// One-line status for the F2 panel. Always a valid, stable C string; the
// pointer stays good for the life of the process.
const char* status();

// True once a new DLL has been staged into place: the player is still running
// the OLD code and must restart for the update to take effect.
bool restartRequired();

// Remove the previous DLL left behind by an earlier swap. Must be called EARLY
// at startup, before anything maps it: at that point the old image from the
// previous session is no longer loaded, so it can finally be deleted. No-op
// when there is nothing to clean up.
void sweepOldImage();

} // namespace updater
} // namespace coop

#endif
