#ifndef COOP_ENGINE_ENVIRONMENT_ITEMS_H
#define COOP_ENGINE_ENVIRONMENT_ITEMS_H
#include "Engine.h"
#include "../../netproto/EnvironmentItems.h"
#include <vector>
namespace coop { namespace procedural { class NativeCensusFence; } namespace engine {
typedef bool (*EnvironmentEncodeActorFn)(const unsigned int localHand[5],u32* streamOwner,unsigned int streamKey[5]);
typedef bool (*EnvironmentResolveActorFn)(u32 streamOwner,const unsigned int streamKey[5],unsigned int localHand[5]);
void setEnvironmentActorResolvers(EnvironmentEncodeActorFn,EnvironmentResolveActorFn);
struct EnvironmentNativeRow {
    EnvironmentItemObject object;
    unsigned int hand[5];
    bool replica;
};
// Installed before native world loading. Generation and queued creation remain native.
bool installEnvironmentItemHooks();
bool environmentItemHooksReady();
// Enqueue/deferred/birth/exclusion/removal/reset revision. Readiness consumers
// retain the capture certificate and compare against this without rescanning.
u64 environmentOriginRevision();
void resetEnvironmentItemRecords();
void environmentSavedLoadEnter();
void environmentSavedLoadLeave();
void environmentSavedItemLoaded(void* object,bool inputCaptured,bool firstTime);
void environmentFactoryQueued(void* work,bool proceduralGeneration);
bool environmentItemExcluded(const unsigned int hand[5]);
u32 environmentFactoryWorkBegin(void* work);
void environmentFactoryProcessed(void* result,u32 generation);
void environmentExcludeItem(const unsigned int hand[5]);
// -1 pending/managed, 0 unmanaged, 1 managed region acknowledged.
int environmentItemInteraction(const unsigned int hand[5]);
// A null certificate clears readiness; stale certificates cannot republish it.
void setEnvironmentRegionReady(i32 x,i32 z,const procedural::NativeCensusFence*);
void clearEnvironmentRegionReadiness();
void setEnvironmentJoin(bool join);
bool captureEnvironmentItems(GameWorld*,i32 x,i32 z,std::vector<EnvironmentNativeRow>&,
    procedural::NativeCensusFence* census=0);
bool environmentPreflight(GameWorld*,const EnvironmentItemObject&,unsigned int parent[5],unsigned int owner[5],unsigned int persistent[5]);
bool environmentSafeToRemove(GameWorld*,const unsigned int hand[5]);
bool removeEnvironmentItem(GameWorld*,const unsigned int hand[5]);
bool spawnEnvironmentItem(GameWorld*,const EnvironmentItemObject&,unsigned int outHand[5]);
bool environmentMerchantRoom(GameWorld*,EnvironmentAnchor*,i32* x,i32* z);
// Existing exact native Inventory recipe; parent adds thin wrapper, not a second implementation.
Item* fabricateEnvironmentItem(GameWorld*,const InvItemEntry&);
} }
#endif
