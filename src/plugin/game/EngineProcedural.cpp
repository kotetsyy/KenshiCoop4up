#include "EngineInternal.h"
#include "EngineEnvironmentItems.h"
#include <float.h>
#include <stddef.h>
#include <kenshi/GameDataManager.h>
#include <kenshi/AI/AI.h>
#include <kenshi/AI/AITaskSystem.h>
#include "../core/ProceduralWorldState.h"

namespace coop { namespace engine {
extern CreateBuildingFn g_createBldgOrig;
namespace {
const char* const campSid="16845-nodes_otto1.mod";
struct Origin {
    unsigned int generation; bool generated,bedsOnly;
#ifdef KENSHICOOP_HARNESS
    unsigned int source; bool activeLevel; char workSid[48];
#endif
};
__declspec(thread) Origin scopes[64];
__declspec(thread) unsigned int depth=0;
__declspec(thread) unsigned int delayedDepth=0;
unsigned int generation=1;
volatile LONG activeScopes=0;
__declspec(align(8)) volatile LONG64 originRevision=0;
u64 nextJournalId=1;
bool joinWorld=false,processSeam=false,hooksInstalled=false;
typedef lektor<TownBase::DelayedSpawnMsg*> DeferredQueue;
DeferredQueue* delayedQueue=0;
bool resolveDeferredQueue() {
    // KenshiLib declares this static member but does not bind/export it.
    // Resolve its real data through two SDK-translated native consumers, never
    // a guessed data RVA or a replacement container owned by the plugin.
    const unsigned char* checks=reinterpret_cast<const unsigned char*>(KenshiLib::GetRealAddress(&TownBase::delayedSpawningChecks));
    const unsigned char* clear=reinterpret_cast<const unsigned char*>(KenshiLib::GetRealAddress(&TownBase::clearDelayedItemLoadingMessages));
    if (!checks || !clear) return false;
    __try {
        const unsigned char checksPrefix[]={0x48,0x83,0xec,0x28,0x8b,0x05};
        const unsigned char clearPrefix[]={0x40,0x53,0x48,0x83,0xec,0x20,0x33,0xdb,0x39,0x1d};
        if (memcmp(checks,checksPrefix,sizeof(checksPrefix)) ||
            checks[10]!=0x85 || checks[11]!=0xc0 ||
            memcmp(clear,clearPrefix,sizeof(clearPrefix)) ||
            clear[32]!=0x48 || clear[33]!=0x8b || clear[34]!=0x0d) return false;
        const unsigned char* count=checks+10+*reinterpret_cast<const i32*>(checks+6);
        const unsigned char* clearCount=clear+14+*reinterpret_cast<const i32*>(clear+10);
        const unsigned char* stuff=clear+39+*reinterpret_cast<const i32*>(clear+35);
        // Native instructions address the count MEMBER, not the lektor base:
        // Ogre's polymorphic allocator precedes count with its vptr.
        DeferredQueue* queue=reinterpret_cast<DeferredQueue*>(const_cast<unsigned char*>(count)-offsetof(DeferredQueue,count));
        if (clearCount!=count || stuff!=reinterpret_cast<const unsigned char*>(&queue->stuff)) return false;
        MEMORY_BASIC_INFORMATION page;
        if (!VirtualQuery(queue,&page,sizeof(page)) || page.State!=MEM_COMMIT ||
            page.AllocationBase!=GetModuleHandleW(0) ||
            (page.Protect&(PAGE_GUARD|PAGE_NOACCESS)) ||
            !(page.Protect&(PAGE_READWRITE|PAGE_WRITECOPY|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY)) ||
            reinterpret_cast<const unsigned char*>(queue)+sizeof(*queue)>
                reinterpret_cast<const unsigned char*>(page.BaseAddress)+page.RegionSize ||
            queue->size()>queue->capacity() || queue->capacity()>1048576) return false;
        delayedQueue=queue; return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
struct Journal { Building* object; unsigned int hand[5],generation; GameData* data; u64 id; bool replica,generated,unsafeReferences; };
struct WorkToken { unsigned int generation; GameData* data; GameSaveState* save; FactoryCallbackInterface* callback; Ogre::Vector3 position; bool bedsOnly; };
struct DelayedToken { unsigned int generation; TownBase* town; GameData* nest; Ogre::Vector3 position; unsigned int townHand[5],inside[5]; bool active; };
std::map<Building*,Journal> journal;
std::map<void*,WorkToken> workTokens;
std::map<void*,DelayedToken> delayedTokens;
std::map<std::pair<i32,i32>,bool> readyRegions;
CRITICAL_SECTION recordsLock;
bool lockInitialized=false;
void lockRecords() { EnterCriticalSection(&recordsLock); }
void unlockRecords() { LeaveCriticalSection(&recordsLock); }
void pushOrigin(bool generated,bool bedsOnly=false) {
    if (depth<64) {
        scopes[depth].generation=generation; scopes[depth].generated=generated;
        scopes[depth].bedsOnly=bedsOnly;
#ifdef KENSHICOOP_HARNESS
        scopes[depth].source=0; scopes[depth].activeLevel=false; scopes[depth].workSid[0]=0;
#endif
    }
    ++depth;
    InterlockedIncrement(&activeScopes);
}
void popOrigin() { if (depth) { --depth; InterlockedDecrement(&activeScopes); } }
bool originActive() { return depth && depth<=64 && scopes[depth-1].generated && scopes[depth-1].generation==generation; }
void copyHand(const hand& h,unsigned int out[5]) { out[0]=(unsigned int)h.type; out[1]=h.container; out[2]=h.containerSerial; out[3]=h.index; out[4]=h.serial; }
bool text(char* out,const std::string& value) { if (value.empty() || value.size()>=48) return false; strcpy(out,value.c_str()); return true; }
#ifdef KENSHICOOP_HARNESS
void captureWorkScope(RootObjectFactory::CreatelistItem* work) {
        __try {
            if (depth && depth<=64) {
                scopes[depth-1].source=1; scopes[depth-1].activeLevel=work->isFromActiveLevelMod;
                if (work->data) text(scopes[depth-1].workSid,work->data->stringID);
            }
        } __except(EXCEPTION_EXECUTE_HANDLER) {}
}
#endif
bool persistedState(const GameSaveState* state) {
    return state && !state->firstTime && state->dataSource &&
        !state->dataSource->name.empty() && !state->instanceID.empty();
}
bool anchor(RootObject* object,char* sid,float* x,float* y,float* z) {
    if (!object || !object->getGameData() || !text(sid,object->getGameData()->stringID)) return false;
    Ogre::Vector3 p=object->getPosition(); *x=p.x; *y=p.y; *z=p.z;
    return _finite(p.x) && _finite(p.y) && _finite(p.z);
}
bool currentJournal(Building* b,const unsigned int h[5],Journal* out) {
    GameData* data=b->getGameData();
    lockRecords();
    std::map<Building*,Journal>::const_iterator it=journal.find(b);
    bool found=it!=journal.end() && it->second.generation==generation && it->second.data==data && memcmp(it->second.hand,h,20)==0;
    if (found) *out=it->second;
    unlockRecords(); return found;
}
typedef void (__fastcall* NestFn)(TownBase*,GameData*,const Ogre::Vector3*,Building*);
NestFn nestOriginal=0,nestEntryOriginal=0;
typedef void (__fastcall* PopulateFn)(RootObjectFactory*,Building*);
PopulateFn populateOriginal=0;
typedef RootObjectFactory::CreatelistItem* (__fastcall* WorkCtorFn)(RootObjectFactory::CreatelistItem*,GameData*,Ogre::Vector3,bool,Faction*,Ogre::Quaternion,FactoryCallbackInterface*,GameSaveState*,RootObjectContainer*,Building*,float);
WorkCtorFn workCtorOriginal=0;
typedef TownBase::DelayedSpawnMsg* (__fastcall* DelayedCtorFn)(TownBase::DelayedSpawnMsg*,TownBase*,GameData*,const Ogre::Vector3*,const hand*);
DelayedCtorFn delayedCtorOriginal=0;
typedef int (__fastcall* DelayedChecksFn)();
DelayedChecksFn delayedChecksOriginal=0;
typedef bool (__fastcall* OperateFn)(UseableStuff*,const hand*);
OperateFn operateOriginal=0,couldOperateOriginal=0;
SetFurnModeFn bedModeOriginal=0;
bool samePosition(const Ogre::Vector3& a,const Ogre::Vector3& b) { return a.x==b.x && a.y==b.y && a.z==b.z; }
bool delayedOrigin(TownBase* town,GameData* nest,const Ogre::Vector3* p,Building* inside) {
    // Native absent parents use NULL_ITEM, not a zero-filled wire sentinel.
    unsigned int h[5]={(unsigned int)NULL_ITEM,0,0,0,0};
    if (inside && !readObjectHand(inside,h)) return false;
    bool found=false; lockRecords();
    for (std::map<void*,DelayedToken>::iterator it=delayedTokens.begin();it!=delayedTokens.end();++it) {
        const DelayedToken& t=it->second;
        if (t.active && t.generation==generation && t.town==town && t.nest==nest && samePosition(t.position,*p) && memcmp(t.inside,h,20)==0) {
            found=true; pushOrigin(true); delayedTokens.erase(it); break;
        }
    }
    unlockRecords(); return found;
}
#ifdef KENSHICOOP_HARNESS
void nativeBirthCallers(char* out,unsigned int capacity) {
    out[0]=0;
    __try {
        void* frames[12]; const USHORT n=CaptureStackBackTrace(0,12,frames,0);
        const ULONG_PTR base=reinterpret_cast<ULONG_PTR>(GetModuleHandleW(0));
        const IMAGE_DOS_HEADER* dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const IMAGE_NT_HEADERS* nt=reinterpret_cast<const IMAGE_NT_HEADERS*>(base+dos->e_lfanew);
        unsigned int used=0;
        for (USHORT i=0;i<n && used+20<capacity;++i) {
            const ULONG_PTR pc=reinterpret_cast<ULONG_PTR>(frames[i]);
            if (pc<base || pc-base>=nt->OptionalHeader.SizeOfImage) continue;
            const int written=_snprintf(out+used,capacity-used-1,
                "%s%Ix",used?",":"",pc-base);
            if (written>0) used+=(unsigned int)written;
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
}
void traceDeferredMiss(const Ogre::Vector3* p) {
        unsigned int tracked=0,active=0; lockRecords();
        tracked=(unsigned int)delayedTokens.size();
        for (std::map<void*,DelayedToken>::const_iterator it=delayedTokens.begin();it!=delayedTokens.end();++it)
            if (it->second.active) ++active;
        unlockRecords();
        char line[256]; _snprintf(line,sizeof(line)-1,
            "[procworld] DEFERRED_MISS tracked=%u active=%u depth=%u delayedDepth=%u pos=%.2f,%.2f,%.2f",
            tracked,active,depth,delayedDepth,p->x,p->y,p->z);
        line[sizeof(line)-1]=0; coop::logLine(line);
}
#endif
void runNest(NestFn original,TownBase* town,GameData* nest,const Ogre::Vector3* p,Building* inside) {
    const bool inherited=originActive();
    const bool deferred=delayedDepth && !inherited && delayedOrigin(town,nest,p,inside);
#ifdef KENSHICOOP_HARNESS
    if (delayedDepth && !deferred && !inherited) traceDeferredMiss(p);
#endif
    // Consume at the first observed entry, including direct deferred virtual
    // dispatch. Nested implementations inherit the same positive scope.
    if (!deferred) pushOrigin(inherited || !delayedDepth);
    __try { original(town,nest,p,inside); } __finally { popOrigin(); }
}
void __fastcall nestEntryHook(TownBase* town,GameData* nest,const Ogre::Vector3* p,Building* inside) {
    runNest(nestEntryOriginal,town,nest,p,inside);
}
void __fastcall nestHook(TownBase* town,GameData* nest,const Ogre::Vector3* p,Building* inside) {
    runNest(nestOriginal,town,nest,p,inside);
}
void __fastcall populateHook(RootObjectFactory* factory,Building* building) {
    // Native layout population is a distinct, proven birth source. Do not
    // enable unverified ground-item categories through this bed-only scope.
    const bool bedsOnly=!originActive() || scopes[depth-1].bedsOnly;
    pushOrigin(true,bedsOnly);
#ifdef KENSHICOOP_HARNESS
    if (depth<=64) scopes[depth-1].source=2;
#endif
    __try { populateOriginal(factory,building); } __finally { popOrigin(); }
}
RootObjectFactory::CreatelistItem* __fastcall workCtorHook(RootObjectFactory::CreatelistItem* self,GameData* data,Ogre::Vector3 p,bool active,Faction* owner,Ogre::Quaternion rotation,FactoryCallbackInterface* callback,GameSaveState* save,RootObjectContainer* container,Building* home,float age) {
    RootObjectFactory::CreatelistItem* result=workCtorOriginal(self,data,p,active,owner,rotation,callback,save,container,home,age);
    if (result==self && environmentItemHooksReady())
        environmentFactoryQueued(self,originActive() && !scopes[depth-1].bedsOnly);
    lockRecords(); workTokens.erase(self);
    if (originActive() && result==self) {
        WorkToken t; t.generation=generation; t.data=data; t.save=save; t.callback=callback; t.position=p;
        t.bedsOnly=scopes[depth-1].bedsOnly;
        workTokens[self]=t;
        // Queued work can outlive its nest scope. Revoke the previous READY
        // before the first bed birth, and fence a census crossing this enqueue.
        readyRegions.clear(); InterlockedIncrement64(&originRevision);
    }
    unlockRecords(); return result;
}
unsigned int captureDelayedToken(TownBase::DelayedSpawnMsg* msg,TownBase* town,GameData* nest,DelayedToken* out) {
    unsigned int proof=0;
    __try {
        if (!msg || !town || !nest) return proof;
        if (msg->nestData==nest) proof|=1;
        if (!msg->item) proof|=2;
        unsigned int actual[5],parent[5]; copyHand(msg->parentTown,parent);
        if (readObjectHand(town,actual) && !memcmp(actual,parent,sizeof(actual))) proof|=4;
        if (_finite(msg->pos.x) && _finite(msg->pos.y) && _finite(msg->pos.z)) proof|=8;
        if (proof!=15) return proof;
        out->generation=generation; out->town=town; out->nest=nest; out->position=msg->pos;
        memcpy(out->townHand,parent,sizeof(parent)); copyHand(msg->insideBuilding,out->inside);
        out->active=false;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return proof;
}
bool delayedPayloadMatches(const DelayedToken* token,TownBase::DelayedSpawnMsg* msg) {
    __try {
        unsigned int parent[5],actual[5],inside[5];
        copyHand(msg->parentTown,parent); copyHand(msg->insideBuilding,inside);
        return msg->nestData==token->nest && !msg->item && samePosition(msg->pos,token->position) &&
            !memcmp(inside,token->inside,sizeof(inside)) &&
            !memcmp(parent,token->townHand,sizeof(parent)) &&
            readObjectHand(token->town,actual) && !memcmp(actual,parent,sizeof(actual));
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
TownBase::DelayedSpawnMsg* __fastcall delayedCtorHook(TownBase::DelayedSpawnMsg* self,TownBase* town,GameData* nest,const Ogre::Vector3* p,const hand* inside) {
    TownBase::DelayedSpawnMsg* result=delayedCtorOriginal(self,town,nest,p,inside);
    DelayedToken token;
    const unsigned int proof=captureDelayedToken(self,town,nest,&token);
#ifdef KENSHICOOP_HARNESS
    char line[160]; _snprintf(line,sizeof(line)-1,
        "[procworld] DELAYED_BIRTH proof=%u returnSelf=%d generation=%u",
        proof,result==self?1:0,generation);
    line[sizeof(line)-1]=0; coop::logLine(line);
#endif
    lockRecords(); delayedTokens.erase(self);
    // The observed native constructor and its initialized payload establish
    // provenance. A C++ constructor's return register is not a lifetime token.
    if (proof==15) {
        delayedTokens[self]=token;
        readyRegions.clear(); InterlockedIncrement64(&originRevision);
    }
    unlockRecords(); return result;
}
void prepareDelayedTokens() {
    lockRecords();
    for (std::map<void*,DelayedToken>::iterator it=delayedTokens.begin();it!=delayedTokens.end();++it) {
        DelayedToken& token=it->second; token.active=false;
        if (token.generation!=generation) continue;
        for (unsigned int i=0;i<delayedQueue->size();++i) {
            TownBase::DelayedSpawnMsg* msg=(*delayedQueue)[i];
            if (msg!=it->first) continue;
            token.active=delayedPayloadMatches(&token,msg);
        }
    }
    unlockRecords();
}
int __fastcall delayedChecksHook() {
    if (!delayedDepth) prepareDelayedTokens();
    ++delayedDepth; int result=0;
    __try { result=delayedChecksOriginal(); } __finally { --delayedDepth; }
    return result;
}
bool regionReady(i32 x,i32 z) {
    lockRecords(); std::map<std::pair<i32,i32>,bool>::const_iterator it=readyRegions.find(std::make_pair(x,z));
    bool ready=it!=readyRegions.end() && it->second; unlockRecords(); return ready;
}
bool workMatches(const WorkToken* token,RootObjectFactory::CreatelistItem* work) {
    __try { return token->generation==generation && token->data==work->data && token->save==work->saveState && token->callback==work->callbackObject && samePosition(token->position,work->position); }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool interactionAllowed(UseableStuff* bed) {
    if (!joinWorld) return true;
    __try {
        GameData* data=bed->getGameData();
        if (!data || data->stringID!=campSid || bed->specialFunction!=BF_BED) return true;
        i32 x,z; if (!proceduralRegionAtObject(bed,&x,&z)) return false;
        if (!procedural::managedBedRegion(x,z)) return true;
        if (bed->owner && bed->owner->isPlayer) return true;
        unsigned int h[5]; Journal j;
        if (!readObjectHand(bed,h)) return false;
        bool traced=currentJournal(bed,h,&j);
        if (!traced) return false;
        // A saved bed may be adopted by the HOST snapshot; restoration alone
        // cannot authorize use before this region has actually reconciled.
        return regionReady(x,z);
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool __fastcall operateHook(UseableStuff* self,const hand* h) { return interactionAllowed(self) && operateOriginal(self,h); }
bool __fastcall couldOperateHook(UseableStuff* self,const hand* h) { return interactionAllowed(self) && couldOperateOriginal(self,h); }
void __fastcall bedModeHook(Character* character,bool on,UseableStuff* bed) {
    if (!on || (bed && interactionAllowed(bed))) bedModeOriginal(character,on,bed);
}
bool hook(intptr_t address,void* replacement,void** original) { return *original || (address && KenshiLib::AddHook(address,replacement,original)==KenshiLib::SUCCESS && *original); }
}
void proceduralFactoryProcessSeamReady(bool ready) { processSeam=ready; }
bool proceduralBedTemplate(const char* sid) { return sid && strcmp(sid,campSid)==0; }
int proceduralBedOrigin(const unsigned int h[5]) {
    if (!lockInitialized || !h || h[0]!=(unsigned int)BUILDING) return -1;
    __try {
        RootObject* object=resolveObjectByHand(h); unsigned int actual[5];
        if (!object || !readObjectHand(object,actual) || memcmp(actual,h,20)!=0) return -1;
        Building* bed=static_cast<Building*>(object);
        GameData* data=bed->getGameData();
        if (!data) return -1;
        if (data->stringID!=campSid || bed->specialFunction!=BF_BED) return 0;
        // Ownership can be assigned after the native factory returns.
        if (bed->owner && bed->owner->isPlayer) return 0;
        Journal record;
        return currentJournal(bed,h,&record)?(record.generated?1:0):-1;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return -1; }
}
void proceduralFactoryWorkEnter(void* opaque) {
    bool generated=false;
    bool bedsOnly=originActive() && scopes[depth-1].bedsOnly;
    if (lockInitialized) {
        RootObjectFactory::CreatelistItem* work=static_cast<RootObjectFactory::CreatelistItem*>(opaque);
        lockRecords(); std::map<void*,WorkToken>::iterator it=workTokens.find(work);
        if (it!=workTokens.end()) {
            const WorkToken& t=it->second;
            generated=workMatches(&t,work);
            if (generated) bedsOnly=t.bedsOnly;
            workTokens.erase(it);
        }
        pushOrigin(generated || originActive(),bedsOnly);
        unlockRecords();
#ifdef KENSHICOOP_HARNESS
        captureWorkScope(work);
#endif
    } else pushOrigin(false);
}
void proceduralFactoryWorkLeave() { popOrigin(); }
bool installProceduralWorldHooks() {
    if (hooksInstalled) return proceduralWorldHooksReady();
    if (!lockInitialized) { InitializeCriticalSection(&recordsLock); lockInitialized=true; }
    if (!g_createBldgOrig) return false;
    if (!delayedQueue && !resolveDeferredQueue()) {
        coop::logLine("[procworld] unsupported native deferred queue accessors; procedural readiness blocked");
        return false;
    }
    // Resolve typed SDK members; Steam/GOG address translation belongs to KenshiLib.
    const intptr_t entryAddress=KenshiLib::GetRealAddress(&TownBase::_NV_nestUpThisSpot);
    const intptr_t nestedAddress=KenshiLib::GetRealAddress(&TownBase::_NV__nestUpThisSpot);
    bool ok=entryAddress!=nestedAddress &&
        hook(entryAddress,(void*)&nestEntryHook,(void**)&nestEntryOriginal);
    ok=hook(nestedAddress,(void*)&nestHook,(void**)&nestOriginal) && ok;
    ok=hook(KenshiLib::GetRealAddress(&RootObjectFactory::populateBuilding),(void*)&populateHook,(void**)&populateOriginal) && ok;
    ok=hook(KenshiLib::GetRealAddress(&RootObjectFactory::CreatelistItem::_CONSTRUCTOR),(void*)&workCtorHook,(void**)&workCtorOriginal) && ok;
    ok=hook(KenshiLib::GetRealAddress(static_cast<TownBase::DelayedSpawnMsg* (TownBase::DelayedSpawnMsg::*)(TownBase*,GameData*,const Ogre::Vector3&,const hand&)>(&TownBase::DelayedSpawnMsg::_CONSTRUCTOR)),(void*)&delayedCtorHook,(void**)&delayedCtorOriginal) && ok;
    ok=hook(KenshiLib::GetRealAddress(&TownBase::delayedSpawningChecks),(void*)&delayedChecksHook,(void**)&delayedChecksOriginal) && ok;
    ok=hook(KenshiLib::GetRealAddress(&UseableStuff::_NV_tryOperate),(void*)&operateHook,(void**)&operateOriginal) && ok;
    ok=hook(KenshiLib::GetRealAddress(&UseableStuff::_NV_couldIOperate),(void*)&couldOperateHook,(void**)&couldOperateOriginal) && ok;
    ok=hook(KenshiLib::GetRealAddress(&Character::setBedMode),(void*)&bedModeHook,(void**)&bedModeOriginal) && ok;
    hooksInstalled=ok;
    char line[256];
    _snprintf(line,sizeof(line)-1,
        "[procworld] HOOKS nestEntry=%d nest=%d populate=%d workCtor=%d delayedCtor=%d delayedChecks=%d operate=%d couldOperate=%d bedMode=%d process=%d queue=%p ready=%d",
        nestEntryOriginal!=0,nestOriginal!=0,populateOriginal!=0,workCtorOriginal!=0,delayedCtorOriginal!=0,delayedChecksOriginal!=0,
        operateOriginal!=0,couldOperateOriginal!=0,bedModeOriginal!=0,processSeam?1:0,delayedQueue,proceduralWorldHooksReady()?1:0);
    line[sizeof(line)-1]=0; coop::logLine(line);
#ifdef KENSHICOOP_HARNESS
    const ULONG_PTR base=reinterpret_cast<ULONG_PTR>(GetModuleHandleW(0));
    _snprintf(line,sizeof(line)-1,
        "[procworld] NATIVE_ENTRIES nest=%Ix entry=%Ix delayed=%Ix",
        (ULONG_PTR)nestedAddress-base,(ULONG_PTR)entryAddress-base,
        (ULONG_PTR)KenshiLib::GetRealAddress(&TownBase::delayedSpawningChecks)-base);
    line[sizeof(line)-1]=0; coop::logLine(line);
#endif
    return proceduralWorldHooksReady();
}
bool proceduralNativeHooksReady() { return hooksInstalled && processSeam && g_createBldgOrig && delayedQueue; }
bool proceduralWorldHooksReady() { return proceduralNativeHooksReady(); }
bool proceduralRegionGenerationIdle(GameWorld* world,i32 x,i32 z) {
    if (!world || !lockInitialized || !proceduralWorldHooksReady() ||
        InterlockedCompareExchange(&activeScopes,0,0)!=0) return false;
    struct Pending { Ogre::Vector3 position; void* delayed; };
    Pending pending[1024]; unsigned int n=0; bool bounded=true;
    lockRecords();
    for (std::map<void*,WorkToken>::const_iterator it=workTokens.begin();it!=workTokens.end();++it) {
        if (it->second.generation!=generation) continue;
        if (n==1024) { bounded=false; break; }
        pending[n].position=it->second.position; pending[n++].delayed=0;
    }
    if (bounded) for (std::map<void*,DelayedToken>::const_iterator it=delayedTokens.begin();it!=delayedTokens.end();++it) {
        if (it->second.generation!=generation) continue;
        if (n==1024) { bounded=false; break; }
        pending[n].position=it->second.position; pending[n++].delayed=it->first;
    }
    unlockRecords();
    if (!bounded) return false;
    // No native calls while holding the journal lock: native factory code can
    // itself enter these hooks while holding its own registry/queue locks.
    for (unsigned int i=0;i<n;++i) {
        if (pending[i].delayed) {
            bool queued=false;
            for (unsigned int j=0;j<delayedQueue->size();++j)
                if ((*delayedQueue)[j]==pending[i].delayed) { queued=true; break; }
            if (!queued) continue;
        }
        int rx,rz;
        if (!cellAt(world,pending[i].position.x,pending[i].position.z,&rx,&rz) || (rx==x && rz==z)) return false;
    }
    return InterlockedCompareExchange(&activeScopes,0,0)==0;
}
u64 proceduralOriginRevision() { return (u64)InterlockedCompareExchange64(&originRevision,0,0); }
void resetProceduralWorldRecords() {
    if (!lockInitialized) return;
    lockRecords(); ++generation; if (!generation) ++generation;
    journal.clear(); workTokens.clear(); delayedTokens.clear(); readyRegions.clear();
    InterlockedIncrement64(&originRevision);
    unlockRecords();
}
void setProceduralWorldJoin(bool enabled) {
    if (joinWorld!=enabled) { joinWorld=enabled; clearProceduralRegionReadiness(); }
}
void setProceduralRegionReady(i32 x,i32 z,bool ready) { if (!lockInitialized) return; lockRecords(); readyRegions[std::make_pair(x,z)]=ready; unlockRecords(); }
void clearProceduralRegionReadiness() { if (!lockInitialized) return; lockRecords(); readyRegions.clear(); unlockRecords(); }
ProceduralBedBirth captureProceduralBedBirth(void* save) {
    ProceduralBedBirth birth; birth.generation=generation; birth.origin=-1;
#ifdef KENSHICOOP_HARNESS
    birth.saveFlags=0;
#endif
    if (!lockInitialized) return birth;
    __try {
#ifdef KENSHICOOP_HARNESS
        GameSaveState* state=static_cast<GameSaveState*>(save);
        if (state) {
            birth.saveFlags=1|(state->firstTime?2:0)|
                (state->dataSource && !state->dataSource->name.empty()?4:0)|
                (!state->instanceID.empty()?8:0);
        }
#endif
        if (persistedState(static_cast<GameSaveState*>(save))) birth.origin=0;
        else if (originActive()) birth.origin=1;
#ifdef KENSHICOOP_HARNESS
        GameSaveState* input=static_cast<GameSaveState*>(save);
        if (birth.origin<0 && input && input->baseData && input->baseData->stringID==campSid) {
            char line[768];
            _snprintf(line,sizeof(line)-1,
                "[procworld] UNKNOWN_INPUT generation=%u firstTime=%d instance=%d states=%u source='%.160s' sourceBase=%d sourceReadonly=%d instanceId='%.160s'",
                generation,input->firstTime?1:0,input->instance?1:0,(unsigned int)input->states.size(),
                input->dataSource?input->dataSource->name.c_str():"",
                input->dataSource && input->dataSource->isBaseDatafile?1:0,
                input->dataSource && input->dataSource->readOnly?1:0,input->instanceID.c_str());
            line[sizeof(line)-1]=0;coop::logLine(line);
        }
#endif
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
    return birth;
}
void recordProceduralBedBirth(void* opaque,const ProceduralBedBirth& birth,bool unsafeReferences) {
    if (!lockInitialized || !opaque || birth.generation!=generation) return;
    __try {
        Building* b=static_cast<Building*>(opaque); GameData* data=b->getGameData();
        if (!data || data->stringID!=campSid) return;
        if (birth.origin<0 && !(b->owner && b->owner->isPlayer)) {
#ifdef KENSHICOOP_HARNESS
            char line[512],callers[192]; nativeBirthCallers(callers,sizeof(callers));
            const Origin* scope=depth && depth<=64?&scopes[depth-1]:0;
            _snprintf(line,sizeof(line)-1,
                "[procworld] BIRTH_UNKNOWN saveFlags=%u function=%u nest=%d unsafeReferences=%d depth=%u delayedDepth=%u generation=%u scopeGen=%u scopeSource=%u activeLevel=%d workSid='%s' nativeCallers=%s",
                birth.saveFlags,(unsigned)b->specialFunction,b->isNestItem?1:0,
                unsafeReferences?1:0,depth,delayedDepth,generation,scope?scope->generation:0,
                scope?scope->source:0,scope && scope->activeLevel?1:0,
                scope?scope->workSid:"",callers);
            line[sizeof(line)-1]=0; coop::logLine(line);
#endif
            return;
        }
        Journal j; j.object=b; j.data=data; j.generation=generation; j.replica=false;
        j.generated=birth.origin==1 && !(b->owner && b->owner->isPlayer); j.unsafeReferences=unsafeReferences;
        if (!readObjectHand(b,j.hand) || resolveObjectByHand(j.hand)!=b) return;
        lockRecords(); j.id=j.generated?nextJournalId++:0; journal[b]=j; unlockRecords();
        InterlockedIncrement64(&originRevision);
        if (j.generated) { i32 x,z; if (proceduralRegionAtObject(b,&x,&z)) setProceduralRegionReady(x,z,false); else clearProceduralRegionReadiness(); }
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
}
namespace {
bool handEquals(const hand& h,const unsigned int key[5]) { unsigned int got[5]; copyHand(h,got); return memcmp(got,key,20)==0; }
bool taskClear(const Tasker* task,const unsigned int h[5]) { return !task || (!handEquals(task->subject,h) && !handEquals(task->currentSubTarget,h)); }
bool tasksClear(const lektor<Tasker*>& tasks,const unsigned int h[5]) {
    if (tasks.size()>65536) return false;
    for (unsigned int i=0;i<tasks.size();++i) if (!taskClear(tasks[i],h)) return false;
    return true;
}
bool dequeClear(const ActionDeque& tasks,const unsigned int h[5]) {
    if (tasks.list.size()>65536) return false;
    for (std::deque<Tasker*>::const_iterator it=tasks.list.begin();it!=tasks.list.end();++it) if (!taskClear(*it,h)) return false;
    return true;
}
bool characterClear(Character* c,const unsigned int h[5]) {
    if (!c || handEquals(c->inWhat,h) || handEquals(c->_destinationInsideBuilding,h)) return false;
    if (c->body && (handEquals(c->body->target,h) || !taskClear(c->body->currentAction,h))) return false;
    if (!c->ai || !c->ai->taskSystemAI) return false;
    AITaskSytem* t=c->ai->taskSystemAI;
    if (handEquals(t->mainGoalTarget,h) || handEquals(t->currentGoal.subject,h) || handEquals(t->currentGoal.subtarget,h) ||
        !dequeClear(t->orders,h) || !dequeClear(t->actions,h) || !dequeClear(t->actionsTryList,h) ||
        !tasksClear(t->jobs,h) || !tasksClear(t->permajobs,h) || !tasksClear(t->requestsList,h) ||
        !taskClear(t->holdPositionOrders,h) || !taskClear(t->currentlySubTasking,h)) return false;
    if (!taskClear(t->autoSleepTask,h) || !taskClear(t->autoGetupTask,h) || !taskClear(t->autoDitchTask,h) ||
        !taskClear(t->autoSitTask,h) || !taskClear(t->autoEjectEnemiesTask1,h) || !taskClear(t->autoEjectEnemiesTask2,h) ||
        handEquals(c->ai->centerOfMovementTarget,h)) return false;
    for (int i=0;i<5;++i) if (!tasksClear(t->goals[i],h) || !tasksClear(t->squadAIPackage[i],h)) return false;
    for (unsigned int i=0;i<t->slaveJobs.size();++i) if (!t->slaveJobs[i] || handEquals(t->slaveJobs[i]->subject,h)) return false;
    typedef std::map<float,Tasker*,std::less<float>,Ogre::STLAllocator<std::pair<float const,Tasker*>,Ogre::GeneralAllocPolicy> > OrderedTasks;
    for (OrderedTasks::const_iterator it=t->orderedGoals.begin();it!=t->orderedGoals.end();++it)
        if (!taskClear(it->second,h)) return false;
    typedef std::map<TaskMatch,TargetWithScore,std::less<TaskMatch>,Ogre::STLAllocator<std::pair<TaskMatch const,TargetWithScore>,Ogre::GeneralAllocPolicy> > TargetCache;
    for (TargetCache::const_iterator it=c->ai->resultsCache.targetFindersCache.begin();it!=c->ai->resultsCache.targetFindersCache.end();++it)
        if (handEquals(it->first.subject,h) || handEquals(it->first.subtarget,h) || handEquals(it->second.target,h)) return false;
    typedef std::map<GameData*,hand,std::less<GameData*>,std::allocator<std::pair<GameData* const,hand> > > ResourceCache;
    for (ResourceCache::const_iterator it=c->ai->resultsCache.resourceSourceCache.begin();it!=c->ai->resultsCache.resourceSourceCache.end();++it)
        if (handEquals(it->second,h)) return false;
    return true;
}
bool eligibleBed(GameWorld* world,Building* bed,const unsigned int h[5]) {
    if (!proceduralWorldHooksReady() || bed->classType!=BCTYPE_USABLE || bed->getInventory() || bed->myInterior || bed->isFurnitureOf) return false;
    if (!world->player || !g_playerFactionFn || bed->owner==g_playerFactionFn(world->player)) return false;
    UseableStuff* use=static_cast<UseableStuff*>(bed);
    if (!use->currentOperators.empty() || use->getOccupant().index || use->getOccupant().serial) return false;
    i32 x,z; if (!proceduralRegionAtObject(bed,&x,&z)) return false;
    if (!world->theFactory || !world->theFactory->todoList.empty()) return false;
    return world->sysMessageList.empty();
}
bool safeBed(GameWorld* world,Building* bed,const unsigned int h[5]) {
    if (!eligibleBed(world,bed,h)) return false;
    const ogre_unordered_set<Character*>::type& characters=world->charUpdateListMain;
    const size_t before=characters.size();
    for (ogre_unordered_set<Character*>::type::const_iterator it=characters.begin();it!=characters.end();++it)
        if (!characterClear(*it,h)) return false;
    for (ogre_unordered_map<hand,Character*>::type::const_iterator it=world->deathParade.begin();it!=world->deathParade.end();++it)
        if (!characterClear(it->second,h)) return false;
    if (before!=characters.size() || !world->sysMessageList.empty()) return false;
    return proceduralLoadedReferencesClear(world,bed);
}
}
bool proceduralCharacterReferencesClear(void* character,const unsigned int h[5]) {
    __try { return characterClear(static_cast<Character*>(character),h); }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool readProceduralBedNative(GameWorld* world,void* opaque,ProceduralBedRead* out) {
    if (!world || !opaque || !out || !lockInitialized) return false;
    __try {
        Building* b=static_cast<Building*>(opaque); memset(out,0,sizeof(*out));
        if (!readObjectHand(b,out->hand) || resolveObjectByHand(out->hand)!=b) return false;
        if (world->getIsInKillList(b)) return false;
        ProceduralObject& o=out->object; GameData* data=b->getGameData();
        if (!data || !text(o.sid,data->stringID)) return false;
        o.category=PROC_CATEGORY_CAMP_BED;
        Ogre::Vector3 p=b->getPosition(); o.x=p.x; o.y=p.y; o.z=p.z;
        const Ogre::Quaternion rotation=b->getOrientation();
        o.heading=rotation.getYaw().valueRadians();
        o.rotationX=rotation.x; o.rotationY=rotation.y; o.rotationZ=rotation.z; o.rotationW=rotation.w;
        if (!_finite(o.x) || !_finite(o.y) || !_finite(o.z) || !_finite(o.heading) ||
            !_finite(o.rotationX) || !_finite(o.rotationY) || !_finite(o.rotationZ) || !_finite(o.rotationW)) return false;
        o.floor=b->floorNum; o.buildingFunction=(u32)b->specialFunction;
        o.stateFlags=(b->_buildState.isComplete?PROC_COMPLETE:0)|(b->_buildState.isPaused?PROC_PAUSED:0)|(b->_buildState.isDismantled?PROC_DISMANTLED:0)|(b->isNestItem?PROC_NEST:0);
        o.constructionProgress=b->_buildState.constructionProgress;
        if (!_finite(o.constructionProgress)) return false;
        if (b->isOutsideFurniture) o.flags|=PROC_OUTSIDE;
        if (b->owner) { o.flags|=PROC_OWNER; GameData* owner=b->owner->getData(); if (!owner || !text(o.ownerSid,owner->stringID)) return false; }
        if (b->_town.index || b->_town.serial) { o.flags|=PROC_TOWN; if (!anchor(b->getTown(),o.townSid,&o.townX,&o.townY,&o.townZ)) return false; }
        if (b->isInsideBuilding.index || b->isInsideBuilding.serial) { unsigned int h[5]; copyHand(b->isInsideBuilding,h); o.flags|=PROC_INSIDE; if (!anchor(resolveObjectByHand(h),o.insideSid,&o.insideX,&o.insideY,&o.insideZ)) return false; }
        Journal j;
        if (!(b->owner && b->owner->isPlayer) && data->stringID==campSid && b->specialFunction==BF_BED && currentJournal(b,out->hand,&j)) {
            o.id=j.id; out->procedural=j.generated; out->replica=j.replica;
            out->saved=!j.generated && !j.replica;
        }
        out->safeToRemove=out->procedural && !j.unsafeReferences && eligibleBed(world,b,out->hand);
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool adoptSavedProceduralBed(GameWorld* world,const unsigned int h[5],const ProceduralObject& wanted) {
    if (!world || !h || !lockInitialized || !procedural::validObject(wanted)) return false;
    RootObject* object=resolveObjectByHand(h);
    ProceduralBedRead row;
    if (!readProceduralBedNative(world,object,&row) || row.procedural ||
        (!row.saved && !row.replica) || !procedural::sameObject(row.object,wanted)) return false;
    lockRecords();
    std::map<Building*,Journal>::iterator it=journal.find(static_cast<Building*>(object));
    const bool current=it!=journal.end() && it->second.generation==generation &&
        memcmp(it->second.hand,h,sizeof(it->second.hand))==0 && !it->second.generated;
    const bool changed=current && !it->second.replica;
    if (current) it->second.replica=true;
    unlockRecords();
    if (changed) InterlockedIncrement64(&originRevision);
    // This changes only bookkeeping; generated stays false, so even an adopted
    // saved bed cannot pass the native destruction preflight.
    return current;
}
bool removeProceduralBed(GameWorld* world,const unsigned int h[5]) {
    if (!world || !h || h[0]!=(unsigned int)BUILDING) return false;
    ProceduralBedRead row; RootObject* object=resolveObjectByHand(h);
    if (!readProceduralBedNative(world,object,&row) || !row.procedural || !row.safeToRemove) return false;
    i32 x,z;
    if (!proceduralRegionAtObject(object,&x,&z) || !procedural::managedBedRegion(x,z)) return false;
    __try { if (!safeBed(world,static_cast<Building*>(object),h)) return false; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
    return destroyBuildingByHand(world,h);
}
static Faction* proceduralOwner(GameWorld* world,const char* sid) {
    std::string value(sid);
    Faction* owner=factionBySidGuarded(world,&value);
    if (owner && owner->getData() && owner->getData()->stringID==value)
        return owner->isPlayer?0:owner;
    return 0; // Never substitute the native "nofac" lookup fallback.
}
bool createProceduralBed(GameWorld* world,const ProceduralObject& object,u64 token,u32 epoch,unsigned int out[5]) {
    if (out) memset(out,0,20);
    if (!world || !out || !token || !epoch || !object.id || !proceduralWorldHooksReady() || !procedural::validObject(object) || strcmp(object.sid,campSid)!=0 || object.category!=PROC_CATEGORY_CAMP_BED || object.buildingFunction!=(u32)BF_BED) return false;
    if (!world->theFactory || !g_createBldgFn || !g_terrainHeightFn) return false;
    __try {
        TownBase* town=0; Building* inside=0;
        const char* sid[2]={object.townSid,object.insideSid};
        const float x[2]={object.townX,object.insideX},y[2]={object.townY,object.insideY},z[2]={object.townZ,object.insideZ};
        for (int parent=0;parent<2;++parent) {
            if (!(object.flags&(parent?PROC_INSIDE:PROC_TOWN))) continue;
            unsigned int hands[256][5]; bool complete=false;
            unsigned int n=listLoadedObjectHands(world,parent?BUILDING:TOWN,x[parent],y[parent],z[parent],5.0f,hands,256,&complete);
            if (!complete) return false;
            RootObject* found=0;
            for (unsigned int i=0;i<n;++i) {
                RootObject* candidate=resolveObjectByHand(hands[i]);
                if (!candidate || !candidate->getGameData()) return false;
                Ogre::Vector3 p=candidate->getPosition();
                if (candidate->getGameData()->stringID!=sid[parent] || fabs(p.x-x[parent])>0.005f || fabs(p.y-y[parent])>0.005f || fabs(p.z-z[parent])>0.005f) continue;
                if (found) return false; found=candidate;
            }
            if (!found) return false;
            if (parent) inside=static_cast<Building*>(found); else town=static_cast<TownBase*>(found);
        }
        Faction* owner=0;
        if (object.flags&PROC_OWNER) { owner=proceduralOwner(world,object.ownerSid); if (!owner) return false; }
        GameData* data=findItemTemplateImpl(world,object.sid,BUILDING); if (!data) return false;
        int rx,rz;
        if (!cellAt(world,object.x,object.z,&rx,&rz) || !procedural::managedBedRegion(rx,rz)) return false;
        ProceduralBedRead rows[PROCEDURAL_OBJECTS_MAX]; bool complete=false;
        unsigned int n=captureProceduralBeds(world,rx,rz,rows,PROCEDURAL_OBJECTS_MAX,&complete);
        if (!complete) return false;
        for (unsigned int i=0;i<n;++i) {
            const ProceduralObject& existing=rows[i].object;
            // An unresolved saved/native state difference must block creation,
            // not produce a second bed at the same physical placement.
            if (procedural::sameObject(existing,object) ||
                (strcmp(existing.sid,object.sid)==0 && existing.floor==object.floor &&
                 fabs(existing.x-object.x)<=0.005f && fabs(existing.y-object.y)<=0.005f &&
                 fabs(existing.z-object.z)<=0.005f)) return false;
        }
        float base=inside?inside->getPosition().y:g_terrainHeightFn(object.x,object.z);
        if (!_finite(base)) return false;
        Ogre::Vector3 p(object.x,object.y-base,object.z);
        // Preserve the native HOST quaternion. Renormalizing float components
        // can move the engine-derived yaw across the canonical 1e-4 boundary.
        Ogre::Quaternion rotation(object.rotationW,object.rotationX,object.rotationY,object.rotationZ);
        Building* bed=g_createBldgFn(world->theFactory,data,p,town,owner,rotation,0,0,0,0,inside,false,(object.stateFlags&PROC_COMPLETE)!=0,false,object.floor,(object.flags&PROC_OUTSIDE)!=0);
        if (!bed) return false;
        bed->_buildState.isPaused=(object.stateFlags&PROC_PAUSED)!=0;
        bed->_buildState.isDismantled=(object.stateFlags&PROC_DISMANTLED)!=0;
        bed->_buildState.constructionProgress=object.constructionProgress;
        bed->isNestItem=(object.stateFlags&PROC_NEST)!=0;
        Journal record; record.object=bed; record.data=data; record.generation=generation; record.replica=true; record.generated=true; record.unsafeReferences=false;
        if (!readObjectHand(bed,record.hand) || resolveObjectByHand(record.hand)!=bed) return false;
        lockRecords(); record.id=nextJournalId++; journal[bed]=record; unlockRecords();
        InterlockedIncrement64(&originRevision);
        setProceduralRegionReady(rx,rz,false);
        bed->updatePhysicalWithProgress();
        // The factory's standalone-furniture path can finish with its neutral
        // owner even when given the camp faction. Restore the snapshot owner
        // through the native ownership/list bookkeeping, not a raw field write.
        if (bed->owner!=owner) bed->setFaction(owner,0);
        ProceduralBedRead got;
        // Never adopt an approximate native result or blindly destroy it.
        const bool readable=readProceduralBedNative(world,bed,&got);
        if (!readable || !procedural::sameObject(got.object,object)) {
            char line[1200];
            _snprintf(line,sizeof(line)-1,
                "[procworld] CREATE_MISMATCH world=%I64u epoch=%u id=%I64u read=%d "
                "wantPos=%.9g,%.9g,%.9g gotPos=%.9g,%.9g,%.9g "
                "wantRot=%.9g,%.9g,%.9g,%.9g gotRot=%.9g,%.9g,%.9g,%.9g "
                "wantFloor=%d gotFloor=%d wantFlags=%u gotFlags=%u "
                "wantState=%u gotState=%u wantProgress=%.9g gotProgress=%.9g "
                "wantOwner='%s' gotOwner='%s' wantTown='%s' gotTown='%s' "
                "wantInside='%s' gotInside='%s'",
                token,epoch,object.id,readable?1:0,
                object.x,object.y,object.z,readable?got.object.x:0,readable?got.object.y:0,readable?got.object.z:0,
                object.rotationX,object.rotationY,object.rotationZ,object.rotationW,
                readable?got.object.rotationX:0,readable?got.object.rotationY:0,
                readable?got.object.rotationZ:0,readable?got.object.rotationW:0,
                (int)object.floor,readable?(int)got.object.floor:-1,
                (unsigned)object.flags,readable?(unsigned)got.object.flags:0,
                (unsigned)object.stateFlags,readable?(unsigned)got.object.stateFlags:0,
                object.constructionProgress,readable?got.object.constructionProgress:0,
                object.ownerSid,readable?got.object.ownerSid:"",
                object.townSid,readable?got.object.townSid:"",
                object.insideSid,readable?got.object.insideSid:"");
            line[sizeof(line)-1]=0;coop::logLine(line);
            return false;
        }
        memcpy(out,record.hand,20); return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
} }
