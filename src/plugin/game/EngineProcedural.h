#ifndef KENSHICOOP_ENGINE_PROCEDURAL_H
#define KENSHICOOP_ENGINE_PROCEDURAL_H
#include "Engine.h"
#include "../../netproto/Wire.h"
namespace coop { namespace engine {
struct ProceduralRegionRead { i32 x,z; bool complete; };
struct ProceduralBedRead { ProceduralObject object; unsigned int hand[5]; bool procedural,replica,saved,safeToRemove; };
bool installProceduralWorldHooks();
bool proceduralWorldHooksReady();
bool proceduralRegionGenerationIdle(GameWorld*,i32 x,i32 z);
// Enqueue/birth/adoption/reset revision; complete-region certification must
// remain fenced against this value after native capture, not only during it.
u64 proceduralOriginRevision();
void resetProceduralWorldRecords();
unsigned int enumerateProceduralRegions(GameWorld*,ProceduralRegionRead*,unsigned int,bool*);
unsigned int captureProceduralBeds(GameWorld*,i32,i32,ProceduralBedRead*,unsigned int,bool*);
bool createProceduralBed(GameWorld*,const ProceduralObject&,u64,u32,unsigned int[5]);
// Attach a fully matched saved bed to the procedural set without changing or
// deleting its native object, layout links, or saved-origin removal protection.
bool adoptSavedProceduralBed(GameWorld*,const unsigned int[5],const ProceduralObject&);
bool removeProceduralBed(GameWorld*,const unsigned int[5]);
void setProceduralWorldJoin(bool);
void setProceduralRegionReady(i32,i32,bool);
bool proceduralBedTemplate(const char* sid);
// 1 positive generation/created replica, 0 saved/player (even adopted), -1 unknown.
int proceduralBedOrigin(const unsigned int hand[5]);
void clearProceduralRegionReadiness();
// Single existing RootObjectFactory::process detour owns these scopes. Leave
// must execute in __finally, including native exceptions and nested consumers.
void proceduralFactoryWorkEnter(void* work);
void proceduralFactoryWorkLeave();
void proceduralFactoryProcessSeamReady(bool);
// Private bridges keep ZoneManager isolated from EngineInternal's SDK prelude.
bool readProceduralBedNative(GameWorld*,void*,ProceduralBedRead*);
struct ProceduralBedBirth {
    unsigned int generation;
    int origin;
#ifdef KENSHICOOP_HARNESS
    unsigned int saveFlags; // present / firstTime / named dataSource / instanceID
#endif
};
// Observe input state before the native factory can consume or mutate it.
ProceduralBedBirth captureProceduralBedBirth(void* saveState);
void recordProceduralBedBirth(void*,const ProceduralBedBirth&,bool unsafeCreationReferences);
bool proceduralNativeHooksReady();
bool proceduralRegionAtObject(void*,i32*,i32*);
bool proceduralLoadedReferencesClear(GameWorld*,void*);
bool proceduralCharacterReferencesClear(void*,const unsigned int[5]);
} }
#endif
