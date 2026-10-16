// Four-image updater. Initial migration requires the complete six-file kit:
// schema 2 deliberately omits sha256/url, so legacy updaters reject it before
// downloading anything. Subsequent updates require transportAbi=2, version,
// coreSha256/coreUrl, uiSha256/uiUrl, steamSha256/steamUrl, udpSha256/udpUrl.
// URLs name exact assets in kotetsyy/KenshiCoop4up/releases/download/v<version>.
// Required hashes are lowercase SHA-256; duplicate/unknown/legacy fields reject.
// Optional fields: proto, notes, allowDowngrade=0|1. Harness/Debug start is always
// disabled, even with caller-provided Settings. Published stable feed untouched.
//
// All downloads validate before disk mutation; four flushed, rehashed stages
// precede a durable recovery journal, backup/promotion and committed marker.
// Any failure checks rollback and preserves incomplete recovery material.
// Existing config/save/credentials never participate. A shared lifecycle barrier
// waits for the first UI attempt, excludes loaders during replacement, and blocks
// new companion loads/sessions until restart. Complete rollback alone reopens it.
// Already mapped images keep executing; startup recovery verifies the whole set.
#ifndef KENSHICOOP_UPDATER_H
#define KENSHICOOP_UPDATER_H
#include <string>
namespace coop { namespace updater {
struct Settings {
    bool enabled;
    std::string owner, repo, branch, path; // fixed source; overrides must match
    bool autoApply;
};
Settings settingsFromConfig();
bool start(const Settings&, const char* buildVersion, unsigned int protoVersion);
const char* status();
bool restartRequired();
// Startup recovery before companions/hooks; false blocks startup, requires
// restart/inspection. Failed recovery preserves backups, never mixed live modules.
bool sweepOldImage();
} }
#endif
