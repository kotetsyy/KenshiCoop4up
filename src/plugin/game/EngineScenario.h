// EngineScenario.h - narrow PUBLIC engine surface: deterministic test-scene
// setup / re-arm scaffolds (host-side, baked into a save). Carved out of Engine.h
// (Phase 5a domain split, 2026-07-19) so the scenario harness (ScenarioSupport.h)
// and the auto-bake path (Plugin.cpp) include only the scene builders, and the
// pure replication consumers stop transitively seeing them.
//
// PUBLIC header: SEH-guarded facade only, no <kenshi/...> internal headers
// (those live in the adapter, EngineInternal.h). Forward declarations only.
//
// NOTE (Phase 5a is intentionally incremental): the OTHER scenario scaffolds
// (duel / inventory / bed-cage-pole / medical / stats / carry scene helpers)
// remain interleaved with their sync siblings in Engine.h and migrate here as
// those sections are touched in later Phase 5 increments.

#ifndef KENSHICOOP_ENGINE_SCENARIO_H
#define KENSHICOOP_ENGINE_SCENARIO_H

class GameWorld;
class Character;
class RootObject;
class Faction;
class Item;

namespace coop {
struct InvItemEntry;
namespace engine {


// Native physical witnesses: transport ids are read from existing replicator maps.
struct ScenarioItemWitness {
    unsigned int ownerId,eventId,itemHand[5],sourceHand[5],claimOwner;
};
struct ScenarioItemSample {
    int bag,source,ground,location;
    unsigned int itemHand[5],metadataHash;
};
inline bool sameScenarioItemSample(const ScenarioItemSample& a,const ScenarioItemSample& b) {
    if (a.bag!=b.bag || a.source!=b.source || a.ground!=b.ground ||
        a.location!=b.location || a.metadataHash!=b.metadataHash) return false;
    for (int i=0;i<5;++i) if (a.itemHand[i]!=b.itemHand[i]) return false;
    return true;
}
struct ScenarioItemStability {
    ScenarioItemSample sample;
    unsigned long sinceMs,lastMs,stableMs;
    unsigned int sampleCount;
    ScenarioItemStability() : sinceMs(0),lastMs(0),stableMs(0),sampleCount(0) {}
    void observe(bool complete,const ScenarioItemSample& next,unsigned long now) {
        if (!complete) { sampleCount=0; stableMs=0; return; }
        if (!sampleCount || now-lastMs>1000 || !sameScenarioItemSample(sample,next)) {
            sample=next; sinceMs=now; sampleCount=1; stableMs=0;
        } else if (now-lastMs>=500) {
            ++sampleCount; stableMs=now-sinceMs;
        } else return;
        lastMs=now;
    }
    bool stable() const { return sampleCount>=5 && stableMs>=2000; }
};
// Complete simultaneous root-owned quantities and exhaustive loaded-ground units.
bool sampleScenarioItem(GameWorld*,const unsigned int bag[5],
                        const unsigned int source[5],const char* sid,unsigned int type,
                        const unsigned int itemHand[5],ScenarioItemSample* out);
bool pickScenarioPhysicalItem(GameWorld*,const unsigned int owner[5],const char* sid,
                              unsigned int type,unsigned int outHand[5],InvItemEntry* out);
// Exact before/after binding recorded only at a successful native pickup boundary.
bool readScenarioItemAfter(GameWorld*,const unsigned int before[5],unsigned int after[5]);
inline bool scenarioProxyIdentityMatches(const void* mintedWorld,const void* world,
                                         unsigned int generation,unsigned int currentGeneration,
                                         const unsigned int hand[5],const void* minted,const void* resolved) {
    return world && mintedWorld==world && generation==currentGeneration && minted &&
        (hand[3] || hand[4]) && minted==resolved;
}
#ifdef KENSHICOOP_HARNESS
// Native cursor lifetime: a real remove and later add, across separate game ticks.
Item* holdInventoryBag(GameWorld* gw, const unsigned int source[5], const char* sid);
bool putHeldInventoryBag(GameWorld* gw, const unsigned int destination[5], Item* bag);
// Actual native identity of the first carried bag, for native nested-add scenarios.
bool inventoryBagHand(GameWorld* gw, const unsigned int carrier[5], unsigned int out[5]);
#endif


// ---- Deterministic test-scene setup (host-side; baked into a save) ---------
// These spawn objects into the live world so the user can SAVE the result. Once
// saved, the spawned chair/NPC become ordinary save entities with save-stable
// hands that resolve identically on both clients - the only way a controlled
// actor can sync (a runtime spawn alone gets a host-only hand the join can't see).

// SEH-guarded: place a seat (stool/chair/bench) furniture building 'fwd' metres in
// front of the local leader (and 'side' metres to its right), facing the leader.
// Returns true if a seat template was found and createBuilding did not fault.
// 'spawned' (optional) receives the new object for hand logging.
bool spawnSeatInFront(GameWorld* gw, float fwd, float side, RootObject** spawned);

// SEH-guarded: spawn a loose world character (player faction, NOT enlisted in the
// squad, so it travels the host-authoritative NPC path) at 'fwd'/'side' from the
// leader. Returns the new Character* (or 0).
Character* spawnNpcInFront(GameWorld* gw, float fwd, float side);

// SEH-guarded: place an OPERABLE work-fixture building (research bench, training
// dummy, machine...) 'fwd'/'side' from the leader, facing the leader, owned by
// 'owner' (pass a non-player faction so the fixture is a world-owned station, or 0
// for unowned). Used by the 'craft' setup scene to bake a save-stable work station
// both clients resolve. 'spawned' (optional) receives the new object for hand
// logging. Returns true if a machine template was found and createBuilding did not
// fault.
bool spawnMachineInFront(GameWorld* gw, float fwd, float side, Faction* owner,
                         RootObject** spawned);

// SEH-guarded: issue 'c' a player-style work order/job to perform 'task' (e.g.
// OPERATE_MACHINERY / USE_TRAINING_DUMMY) AT 'fixture', at the fixture's position.
// Clears prior goals first. Used by the setup scene to force the host NPC into the
// work pose so the captured task streams to the join. Returns true if issued.
bool orderWorkAt(Character* c, RootObject* fixture, int task);

// Find the BAKED bed (kind=1) or prison cage (kind=2) near the leader by
// scanning loaded BUILDING objects by template name (fixture relocation after
// a bedcage1 reload; same shape as findWorkFixtureNear).
RootObject* findFurnitureNear(GameWorld* gw, int kind);

// Order the subject (by hand) to USE the baked bed via the player-order path
// (USE_BED_ORDER at the bed fixture). Guarded no-op (returns true) when the
// subject is already on a bed task. orderedTask/useBedTask report the numeric
// TaskType ids for scenario logging. Host-side scenario scaffold.
bool orderUseBed(GameWorld* gw, const unsigned int subjHand[5],
                 int* orderedTask, int* useBedTask);

// 'craft' setup scene (host-side): spawn a save-stable work fixture + a world NPC
// and force the NPC into the matching work pose (OPERATE_MACHINERY / training), so
// the host captures the work task and the join reproduces it once the save is
// baked. All task-enum selection is internal. Returns true if a fixture spawned.
bool setupCraftScene(GameWorld* gw);

// Craft RE-ARM (host-side): a worker's addGoal intent does NOT serialise, so a baked
// craft scene reloads with an idle worker. Re-find the baked fixture + nearest
// non-squad worker by search and re-issue the work goal, so the host resumes
// streaming the work task. Idempotent (no-ops when the worker is already on task);
// safe to call once on load and then periodically. Returns true if a goal is active.
bool rearmCraftScene(GameWorld* gw);

// Stage 2 body-state scaffolds. setupDownScene spawns a non-squad world NPC and
// ragdolls it (a controllable "down" subject); rearmDownScene re-applies ragdoll to
// that subject on an interval (a healthy body recovers and stands). Host-only.
// NOTE: the per-body down-drive primitives knockDown()/holdDown() are SYNC-layer
// (the Replicator reproduces down state through them) and stay in Engine.h.
bool setupDownScene(GameWorld* gw);
// Re-knock every non-squad NPC near the leader so down bodies stay down (a healthy
// ragdoll recovers; ragdoll does not persist across save/load). Returns count, or -1.
int  rearmDownScene(GameWorld* gw);

// Phase 3.5 bake: build a SECOND player squad tab (recruit two bodies, separate one
// into its own player platoon). Gives the bidirectional ownership partition two tabs
// to split (host owns tab 0, join owns tab 1). Host-only; user SAVEs the result.
bool setupSquadScene(GameWorld* gw);

// Join-side live claim: if the loaded save has only the host's squad tab (rank 0),
// pull this join's share of player characters (never the leader) into a NEW
// player platoon via separateIntoMyOwnSquad + setFaction. Same faction, different
// tab: the join controls that tab, first-aid / jobs between tabs still work.
// playerId is the WELCOME id (1..3). nPlayers (2..4) sizes the round-robin share
// so two-player sessions split ~half, three-player sessions split thirds.
// Never spawns new characters. Returns true if at least one detach/move was issued.
bool claimJoinSquadTab(GameWorld* gw, unsigned int playerId, unsigned int nPlayers,
                       unsigned int wireHands[][5], unsigned int localHands[][5],
                       unsigned int* outN);
bool seedGearIdentityVariant(GameWorld*, const unsigned int ownerHand[5],
                             const InvItemEntry& identity, void** outItem);
int dropPhysicalItemByHand(GameWorld*, const unsigned int itemHand[5],
                           unsigned int outAfterItemHand[5]);
int pickupPhysicalItemByHand(GameWorld*, const unsigned int ownerHand[5],
                             const unsigned int itemHand[5],
                             unsigned int outAfterItemHand[5]);
bool equipPhysicalItemByHand(GameWorld*, const unsigned int ownerHand[5],
                             const unsigned int itemHand[5]);
bool physicalWeaponIsDrawn(GameWorld*, const unsigned int ownerHand[5],
                            const unsigned int itemHand[5]);

// split_far fixture bake: relocate every player-squad member that is NOT in the
// LEADER's squad tab to (x,y,z) and halt it there, so the baked save opens with
// the two tabs in two different regions. Host-only, single client - doing this
// in a co-op run and capturing afterwards does NOT work, because armConnectPush
// bakes the live world before the squads are positioned.
// The default target is a measured one: the join enumerates ~8 world NPCs there
// while the host enumerates 0 with the zone LOADED, which is the far-apart
// desync this fixture exists to hold still. Returns the number relocated.
unsigned int setupSplitFarScene(GameWorld* gw, float x, float y, float z);

// down_order LIVE-transition helpers (Stage 2). pickDownSubject pins the non-squad
// NPC nearest the leader (its hand, readObjectHand layout); holdSubjectUpright keeps
// it idle/in-range during the baseline; orderDownSubject knocks THAT subject out at
// the order point so the join must transition upright -> down.
bool pickDownSubject(GameWorld* gw, unsigned int subjHand[5]);
bool holdSubjectUpright(GameWorld* gw, const unsigned int subjHand[5]);
bool orderDownSubject(GameWorld* gw, const unsigned int subjHand[5]);
// death_order: KILL the pinned subject (scaffold) so Character::isDead() flips and
// the host emits a reliable EVT_DEATH. Idempotent; re-assertable on a throttle.
bool killSubject(GameWorld* gw, const unsigned int subjHand[5]);
// combat_kill: WEAKEN the subject (lower blood only, never heal/collapse) so an
// ongoing real melee downs it decisively within the window - the down edge comes from
// genuine combat, not a scaffold. Returns true if applied. subjHand readObjectHand layout.
bool woundSubject(GameWorld* gw, const unsigned int subjHand[5], float blood);

} // namespace engine
} // namespace coop

#endif // KENSHICOOP_ENGINE_SCENARIO_H
