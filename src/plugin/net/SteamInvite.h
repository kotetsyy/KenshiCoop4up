// Main-thread Steam lobby metadata only. Gameplay is exclusively NetLink's native provider.
#ifndef KENSHICOOP_STEAMINVITE_H
#define KENSHICOOP_STEAMINVITE_H
namespace coop { namespace steaminvite {
typedef unsigned long long SteamId;
typedef void (*ConnectFn)(SteamId hostId);
// Borrow exact legacy interfaces from the game's already loaded Steam context.
bool init(ConnectFn onConnect);
SteamId selfId();
// Session generation fences callback intents; guests need no manual allowlist.
void synchronize(bool host, bool running, SteamId sessionGeneration);
void pumpCallbacks();
// Called only AFTER metadata and native callback pumps return. Never connects in a callback.
void processIntents();
// Leaves lobby, unregisters callback/call-results, wipes queued intents. No global Steam shutdown.
void reset();
} }
#endif
