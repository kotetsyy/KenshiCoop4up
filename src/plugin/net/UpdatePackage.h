#ifndef KENSHICOOP_UPDATE_PACKAGE_H
#define KENSHICOOP_UPDATE_PACKAGE_H
#include <string>
#include <stddef.h>
namespace coop { namespace updater {
extern const char* const kComponentNames[4];
struct PackageManifest {
    std::string version, proto, notes, hash[4], url[4];
    bool allowDowngrade;
};
// Fixed repository/release/asset paths; schema 2 has no legacy sha256/url fields.
bool parsePackageManifest(const std::string&, PackageManifest*, std::string* error);
std::string packageHash(const void*, size_t);
std::string packageFileHash(const std::string&);
// directory includes its trailing separator. All four installed images required.
bool installPackage(const std::string& directory, const PackageManifest&,
                    const std::string payloads[4], std::string* error);
// Filesystem access errors count as pending, never as safe absence.
bool packageRecoveryPending(const std::string& directory);
// Called before loading companions. Never discards an incomplete rollback's backup.
bool recoverPackage(const std::string& directory, std::string* error);
// Shared production startup barrier: a restored core cannot run companions in
// the process that still maps the promoted image. loadedCoreHash is captured
// before recovery; pending quarantine evidence remains until the next launch.
bool recoverPackageForStartup(const std::string& directory,
                              const std::string& loadedCoreHash,
                              bool* restartRequired, std::string* error);
#ifdef KENSHICOOP_UPDATER_TEST
void packageTestFailure(int operation, int rollbackComponent);
void packageTestCrash(int operation);
#endif
} }
#endif
