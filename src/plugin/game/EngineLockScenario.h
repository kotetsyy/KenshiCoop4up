#ifndef KENSHICOOP_ENGINE_LOCK_SCENARIO_H
#define KENSHICOOP_ENGINE_LOCK_SCENARIO_H
class Character;
namespace coop { namespace engine {
// Harness-only action: submit the actual native PICK_LOCK order, never unlock.
// Returns false on missing target/lock/order API. Issued is not task acceptance.
bool orderNativeLockpick(Character* actor, const unsigned int target[5]);
} }
#endif
