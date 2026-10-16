#include "EngineInternal.h"
#include "EngineEnvironmentItems.h"
#include "EngineProcedural.h"
#include "../core/EnvironmentItemsState.h"
#include <kenshi/RootObjectFactory.h>
#include <kenshi/InstanceID.h>
#include <kenshi/AI/AI.h>
#include <map>
#include <set>
#include <cstring>

namespace coop { namespace engine {
namespace {
struct Key {
    unsigned int h[5];
    Key() { memset(h,0,sizeof(h)); }
    explicit Key(const unsigned int* p) { memcpy(h,p,sizeof(h)); }
    bool operator<(const Key& b) const { return memcmp(h,b.h,sizeof(h))<0; }
};
struct Birth { unsigned int generation; bool replica; Birth():generation(0),replica(false) {} };
struct Ticket {
    unsigned int generation; GameData* data; GameSaveState* save;
    FactoryCallbackInterface* callback; Ogre::Vector3 position;
};
CRITICAL_SECTION recordsLock;
bool lockReady=false,hooksReady=false,joinWorld=false;
unsigned int generation=1;
u64 journalRevision=0;
LONG active=0;
EnvironmentEncodeActorFn encodeActor=0;
EnvironmentResolveActorFn resolveActor=0;
std::map<Key,Birth> births;
std::set<Key> excluded;
std::map<void*,Ticket> tickets;
struct DeferredItem { unsigned int generation;GameData* data;Ogre::Vector3 position; };
std::map<Item*,DeferredItem> delayedItems;
std::map<std::pair<i32,i32>,procedural::NativeCensusFence> readiness;
__declspec(thread) unsigned int populationDepth=0;
__declspec(thread) unsigned int populationScopes[64];
__declspec(thread) unsigned int replicaDepth=0;
__declspec(thread) unsigned int savedLoadDepth=0;
struct Guard { Guard(){ EnterCriticalSection(&recordsLock); } ~Guard(){ LeaveCriticalSection(&recordsLock); } };
bool populationActive() { return populationDepth && populationDepth<=64 && populationScopes[populationDepth-1]==generation; }
bool nonPlayerBuilding(Building* b) {
    __try { return b && !(b->owner && b->owner->isPlayer) && !b->isThePlayer(); }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool ambiguousSavedDisplay(const unsigned int h[5]) {
    __try {
        GroundItemIdentityRow row;
        if (!readGroundItemIdentity(h,&row) || !row.savedInput || row.firstTime ||
            (!row.insideBuilding[3] && !row.insideBuilding[4]) || row.insideBuilding[0]!=(unsigned int)BUILDING) return false;
        RootObject* root=resolveObjectByHand(h);RootObject* parent=resolveObjectByHand(row.insideBuilding);
        return root && root->typeIsAnItem() && static_cast<Item*>(root)->itemGroup &&
            nonPlayerBuilding(static_cast<Building*>(parent));
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool persisted(GameSaveState* s) {
    __try { return s && !s->firstTime && s->dataSource && !s->instanceID.empty(); }
    __except(EXCEPTION_EXECUTE_HANDLER) { return true; }
}
bool readTicket(void* opaque,Ticket* out) {
    __try {
        RootObjectFactory::CreatelistItem* w=static_cast<RootObjectFactory::CreatelistItem*>(opaque);
        if (!w || !w->data || (w->data->type!=ITEM && w->data->type!=WEAPON && w->data->type!=ARMOUR && w->data->type!=CONTAINER)) return false;
        if (persisted(w->saveState) || (w->homeBuilding && !nonPlayerBuilding(w->homeBuilding))) return false;
        out->generation=generation;out->data=w->data;out->save=w->saveState;out->callback=w->callbackObject;out->position=w->position;
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool sameTicket(const Ticket& a,const Ticket& b) {
    return a.generation==b.generation && a.data==b.data && a.save==b.save && a.callback==b.callback &&
        a.position.x==b.position.x && a.position.y==b.position.y && a.position.z==b.position.z;
}
void remember(RootObject* object,bool replica,const char* origin) {
    unsigned int h[5]; InvItemEntry metadata;
    if (!object || !readPhysicalItemMetadata(reinterpret_cast<Item*>(object),&metadata,h) || resolveObjectByHand(h)!=object) return;
    bool fresh=false;
    { Guard guard;Key key(h);if (excluded.count(key)) return;
      fresh=births.find(key)==births.end();
      Birth b;b.generation=generation;b.replica=replica;births[key]=b;
      if (fresh) { readiness.clear();++journalRevision; }
    }
    if (fresh) {
        char line[384];_snprintf(line,sizeof(line)-1,
            "[environment-native] BIRTH generation=%u origin=%s hand=%u,%u,%u,%u,%u sid='%s' qty=%u",
            generation,origin,h[0],h[1],h[2],h[3],h[4],metadata.stringID,(unsigned)metadata.quantity);
        line[sizeof(line)-1]=0;coop::logLine(line);
    }
}
typedef void (__fastcall* PopulateFn)(RootObjectFactory*,Building*);
PopulateFn populateOriginal=0;
void __fastcall populateHook(RootObjectFactory* factory,Building* building) {
    if (!hooksReady) { populateOriginal(factory,building);return; }
    if (populationDepth<64) populationScopes[populationDepth]=nonPlayerBuilding(building)?generation:0;
    ++populationDepth;InterlockedIncrement(&active);
    __try { populateOriginal(factory,building); }
    __finally { --populationDepth;InterlockedDecrement(&active); }
}
typedef TownBase::DelayedSpawnMsg* (__fastcall* DelayedItemFn)(TownBase::DelayedSpawnMsg*,Item*,const Ogre::Vector3*);
DelayedItemFn delayedItemOriginal=0;
TownBase::DelayedSpawnMsg* __fastcall delayedItemHook(TownBase::DelayedSpawnMsg* self,Item* item,const Ogre::Vector3* p) {
    TownBase::DelayedSpawnMsg* result=delayedItemOriginal(self,item,p);
    // This native overload carries an already-created Item, not a nest template.
    // Only inherit an explicit population scope; arbitrary delayed items stay unknown.
    if (hooksReady && result==self && populationActive() && !savedLoadDepth) {
        DeferredItem token;token.generation=generation;token.data=item->getGameData();token.position=*p;
        { Guard guard;if (token.generation==generation) { delayedItems[item]=token;readiness.clear();++journalRevision; } }
        remember(item,false,"population-delayed-item");
    }
    return result;
}
typedef void (__fastcall* ActivateFn)(Item*,bool,const Ogre::Vector3*,const Ogre::Quaternion*,bool,YesNoMaybe,bool);
ActivateFn activateOriginal=0;
void __fastcall activateHook(Item* item,bool physical,const Ogre::Vector3* p,const Ogre::Quaternion* q,bool fixed,YesNoMaybe bare,bool dynamic) {
    activateOriginal(item,physical,p,q,fixed,bare,dynamic);
    if (!hooksReady) return;
    bool deferred=false;GameData* data=item->getGameData();
    { Guard guard;std::map<Item*,DeferredItem>::iterator it=delayedItems.find(item);
      if (it!=delayedItems.end()) { deferred=it->second.generation==generation && it->second.data==data;delayedItems.erase(it);++journalRevision; } }
    if ((populationActive() || deferred) && !replicaDepth && !savedLoadDepth) remember(item,false,deferred?"population-delayed-activate":"population-activate");
}
bool pickupPending(Item* item) {
    unsigned int h[5];
    if (!hooksReady || !item || !readObjectHand(item,h) || resolveObjectByHand(h)!=item) return false;
    if (environmentItemInteraction(h)>=0) return false;
    static DWORD last=0;DWORD now=GetTickCount();
    if (now-last>=1000) {
        last=now;char line[192];_snprintf(line,sizeof(line)-1,
            "[environment-native] PICKUP-BLOCK hand=%u,%u,%u,%u,%u before=permission",
            h[0],h[1],h[2],h[3],h[4]);line[sizeof(line)-1]=0;coop::logLine(line);
    }
    return true;
}
typedef void (__fastcall* UiPickupFn)(PlayerInterface*,Item*);
typedef bool (__fastcall* LootPermissionFn)(Character*,RootObject*,Item*);
typedef bool (__fastcall* AiLootableFn)(AI*,const hand*,const Ogre::Vector3*);
typedef bool (__fastcall* AiGroundPermissionFn)(AI*,Item*);
UiPickupFn uiPickupOriginal=0;
LootPermissionFn lootPermissionOriginal=0;
AiLootableFn aiLootableOriginal=0;
AiGroundPermissionFn aiGroundPermissionOriginal=0;
void __fastcall uiPickupHook(PlayerInterface* player,Item* item) {
    // SDK UI command boundary: veto before a pickup order can detach the object.
    if (!pickupPending(item)) uiPickupOriginal(player,item);
}
bool __fastcall lootPermissionHook(Character* who,RootObject* victim,Item* item) {
    return !pickupPending(item) && lootPermissionOriginal(who,victim,item);
}
bool __fastcall aiLootableHook(AI* ai,const hand* subject,const Ogre::Vector3* p) {
    if (!hooksReady) return aiLootableOriginal(ai,subject,p);
    unsigned int h[5]={ (unsigned int)subject->type,subject->container,subject->containerSerial,subject->index,subject->serial };
    if (environmentItemInteraction(h)<0) return false;
    return aiLootableOriginal(ai,subject,p);
}
bool __fastcall aiGroundPermissionHook(AI* ai,Item* item) {
    return !pickupPending(item) && aiGroundPermissionOriginal(ai,item);
}
bool hook(intptr_t address,void* replacement,void** original) {
    return *original || (address && KenshiLib::AddHook(address,replacement,original)==KenshiLib::SUCCESS && *original);
}
void copyHand(const hand& h,unsigned int out[5]) {
    out[0]=(unsigned int)h.type;out[1]=h.container;out[2]=h.containerSerial;out[3]=h.index;out[4]=h.serial;
}
bool anchor(const unsigned int h[5],EnvironmentAnchor* out) {
    memset(out,0,sizeof(*out));
    if (!h[3] && !h[4]) return true;
    RootObject* root=resolveObjectByHand(h);if (!root) return false;
    GameData* gd=root->getGameData();
    if (h[0]==(unsigned int)CHARACTER) {
        if ((root->owner && root->owner->isPlayer) || !encodeActor || !gd ||
            gd->stringID.empty() || gd->stringID.size()>=sizeof(out->sid)) return false;
        if (!encodeActor(h,&out->actorOwner,out->actorKey)) return false;
        out->present=1;out->type=(u32)CHARACTER;strcpy(out->sid,gd->stringID.c_str());
        return environment::anchorValid(*out);
    }
    // Only positively bound actors or native stable building descriptors are
    // cross-client identities. Unsupported owners never become unowned.
    if (h[0]!=(unsigned int)BUILDING || !nonPlayerBuilding(static_cast<Building*>(root))) return false;
    InstanceID* id=root->getInstanceID();
    if (!gd || gd->stringID.empty() || gd->stringID.size()>=sizeof(out->sid) || (id && id->uid.size()>=sizeof(out->instance))) return false;
    out->present=1;out->type=h[0];strcpy(out->sid,gd->stringID.c_str());
    if (id) strcpy(out->instance,id->uid.c_str());
    Ogre::Vector3 p=root->getPosition();out->x=p.x;out->y=p.y;out->z=p.z;
    return environment::anchorValid(*out);
}
bool describe(const unsigned int h[5],EnvironmentItemObject* out) {
    __try {
        GroundItemIdentityRow row;
        if (!readGroundItemIdentity(h,&row) || row.identityTruncated || row.item.equipped || row.item.locked) return false;
        RootObject* root=resolveObjectByHand(h);
        if (!root || !root->typeIsAnItem()) return false;
        Item* item=static_cast<Item*>(root);
        if (!physicalItemIsOnGround(item) || item->getInventory()) return false;
        memset(out,0,sizeof(*out));out->id=1;out->item=row.item;
        // Grid/equipment addresses belong to inventories, not free objects.
        out->item.slot=0;out->item.parentIdx=0;out->item.section=out->item.gridSection=out->item.gridX=out->item.gridY=0;
        Ogre::Vector3 p=item->getPosition();Ogre::Quaternion q=item->getOrientation();q.normalise();
        if (q.w<0 || (q.w==0 && (q.x<0 || (q.x==0 && (q.y<0 || (q.y==0 && q.z<0)))))) { q.w=-q.w;q.x=-q.x;q.y=-q.y;q.z=-q.z; }
        out->x=p.x;out->y=p.y;out->z=p.z;out->rotationW=q.w;out->rotationX=q.x;out->rotationY=q.y;out->rotationZ=q.z;
        out->condition=row.condition;out->charges=row.charges;out->fullCharge=row.fullCharge;out->floor=item->floorNum;
        out->flags=(item->fixedPhysicalPosition?ENV_FIXED:0)|(item->useDynamicPhysics?ENV_DYNAMIC:0)|
            (item->visible?ENV_VISIBLE:0)|(item->isPersistant()?ENV_PERSISTENT:0)|(item->isUnique?ENV_UNIQUE:0)|
            (item->isTradeItem?ENV_TRADE:0)|(item->deathItem?ENV_DEATH:0)|(item->existAsBareWeapon?ENV_BARE:0);
        unsigned int persistent[5];copyHand(item->persistant,persistent);
        if (!anchor(row.insideBuilding,&out->parent) || !anchor(row.properOwner,&out->properOwner) || !anchor(persistent,&out->persistentOwner)) return false;
        return environment::valid(*out);
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool safeNative(GameWorld* world,const unsigned int h[5]) {
    __try {
        RootObject* object=resolveObjectByHand(h);
        if (!world || !object || world->getIsInKillList(object) || !object->typeIsAnItem() ||
            !physicalItemIsOnGround(static_cast<Item*>(object)) || !world->theFactory ||
            !world->theFactory->todoList.empty() || !world->sysMessageList.empty()) return false;
        const ogre_unordered_set<Character*>::type& chars=world->charUpdateListMain;
        for (ogre_unordered_set<Character*>::type::const_iterator it=chars.begin();it!=chars.end();++it)
            if (!proceduralCharacterReferencesClear(*it,h)) return false;
        for (ogre_unordered_map<hand,Character*>::type::const_iterator it=world->deathParade.begin();it!=world->deathParade.end();++it)
            if (!proceduralCharacterReferencesClear(it->second,h)) return false;
        return world->sysMessageList.empty();
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool resolveAnchor(GameWorld* world,const EnvironmentAnchor& wanted,unsigned int out[5]) {
    memset(out,0,20);if (!wanted.present) return true;
    if (wanted.type==(u32)CHARACTER) {
        if (!resolveActor || !resolveActor(wanted.actorOwner,wanted.actorKey,out)) return false;
        EnvironmentAnchor resolved;
        return anchor(out,&resolved) && environment::sameAnchor(wanted,resolved);
    }
    if (wanted.type!=(u32)BUILDING) return false;
    // Enumeration is not an identity heuristic: require an exact full anchor
    // and uniqueness; overflow/deferred loading blocks reconciliation.
    static DeedRead buildings[2048];
    const unsigned n=enumBuildingsNear(world,200000.0f,buildings,2048);
    if (n>=2048) return false;
    unsigned found=0;
    for (unsigned i=0;i<n;++i) {
        EnvironmentAnchor got;
        if (anchor(buildings[i].hand,&got) && environment::sameAnchor(wanted,got)) { memcpy(out,buildings[i].hand,20);++found; }
    }
    return found==1;
}
bool assignNative(Item* item,const EnvironmentItemObject& o,const unsigned int parent[5],const unsigned int owner[5],const unsigned int persistent[5]) {
    __try {
        hand p((unsigned)parent[3],(unsigned)parent[4],(itemType)parent[0],parent[1],parent[2]);
        hand own((unsigned)owner[3],(unsigned)owner[4],(itemType)owner[0],owner[1],owner[2]);
        hand persist((unsigned)persistent[3],(unsigned)persistent[4],(itemType)persistent[0],persistent[1],persistent[2]);
        item->quantity=o.item.quantity;item->quality=o.condition;item->chargesLeft=o.charges;item->originalFullChargeAmount=o.fullCharge;
        item->floorNum=o.floor;item->isInsideBuilding=p;item->setProperOwner(own);
        item->setPersistant((o.flags&ENV_PERSISTENT)!=0);
        if (persistent[3] || persistent[4]) item->setPersistant(persist);
        item->isUnique=(o.flags&ENV_UNIQUE)!=0;item->isTradeItem=(o.flags&ENV_TRADE)!=0;item->deathItem=(o.flags&ENV_DEATH)!=0;
        item->activate(true,Ogre::Vector3(o.x,o.y,o.z),Ogre::Quaternion(o.rotationW,o.rotationX,o.rotationY,o.rotationZ),
            (o.flags&ENV_FIXED)!=0,(o.flags&ENV_BARE)?YesNoMaybe::YES:YesNoMaybe::NO,(o.flags&ENV_DYNAMIC)!=0);
        item->setVisible((o.flags&ENV_VISIBLE)!=0);
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
}

bool installEnvironmentItemHooks() {
    if (!lockReady) { InitializeCriticalSection(&recordsLock);lockReady=true; }
    bool ok=hook(KenshiLib::GetRealAddress(&RootObjectFactory::populateBuilding),(void*)&populateHook,(void**)&populateOriginal);
    ok=hook(KenshiLib::GetRealAddress(static_cast<TownBase::DelayedSpawnMsg* (TownBase::DelayedSpawnMsg::*)(Item*,const Ogre::Vector3&)>(&TownBase::DelayedSpawnMsg::_CONSTRUCTOR)),(void*)&delayedItemHook,(void**)&delayedItemOriginal) && ok;
    ok=hook(KenshiLib::GetRealAddress(static_cast<void (Item::*)(bool,const Ogre::Vector3&,const Ogre::Quaternion&,bool,YesNoMaybe,bool)>(&Item::_NV_activate)),(void*)&activateHook,(void**)&activateOriginal) && ok;
    ok=hook(KenshiLib::GetRealAddress(&PlayerInterface::pickupItem),(void*)&uiPickupHook,(void**)&uiPickupOriginal) && ok;
    ok=hook(KenshiLib::GetRealAddress(&Character::_NV_isItOkForMeToLoot),(void*)&lootPermissionHook,(void**)&lootPermissionOriginal) && ok;
    ok=hook(KenshiLib::GetRealAddress(&AI::isItemLootable),(void*)&aiLootableHook,(void**)&aiLootableOriginal) && ok;
    ok=hook(KenshiLib::GetRealAddress(&AI::itemOnGroundStealingCheckIsSafe),(void*)&aiGroundPermissionHook,(void**)&aiGroundPermissionOriginal) && ok;
    hooksReady=ok;return ok;
}
void setEnvironmentActorResolvers(EnvironmentEncodeActorFn encode,EnvironmentResolveActorFn resolve) {
    encodeActor=encode;resolveActor=resolve;
}
bool environmentItemHooksReady() { return hooksReady; }
u64 environmentOriginRevision() {
    if (!lockReady) return 0;Guard guard;return journalRevision;
}
void resetEnvironmentItemRecords() {
    if (!lockReady) return;Guard guard;
    ++generation;if (!generation) ++generation;++journalRevision;births.clear();tickets.clear();delayedItems.clear();excluded.clear();readiness.clear();
}
void environmentSavedLoadEnter() { ++savedLoadDepth; }
void environmentSavedLoadLeave() { if (savedLoadDepth) --savedLoadDepth; }
void environmentSavedItemLoaded(void* object,bool captured,bool firstTime) {
    if (captured && firstTime && populationActive() && !replicaDepth)
        remember(static_cast<RootObject*>(object),false,"population-firsttime-load");
}
void environmentFactoryQueued(void* work,bool proceduralGeneration) {
    if (!lockReady || !work) return;
    Ticket token;const bool generated=(populationActive() || proceduralGeneration) && !replicaDepth && !savedLoadDepth && readTicket(work,&token);
    Guard guard;if (tickets.erase(work)) ++journalRevision;if (generated && token.generation==generation) { tickets[work]=token;++journalRevision; }
}
u32 environmentFactoryWorkBegin(void* work) {
    if (!hooksReady || !work) return 0;
    Ticket current;const bool readable=readTicket(work,&current);
    u32 origin=0;
    { Guard guard;std::map<void*,Ticket>::iterator it=tickets.find(work);
      if (it!=tickets.end()) { if (readable && sameTicket(current,it->second)) origin=current.generation;
          tickets.erase(it);++journalRevision; } }
    return origin;
}
void environmentFactoryProcessed(void* result,u32 origin) {
    if (origin && origin==generation)
        remember(static_cast<RootObject*>(result),false,"accepted-factory-generation");
}
void environmentExcludeItem(const unsigned int h[5]) {
    if (!lockReady || !h) return;Guard guard;const bool added=excluded.insert(Key(h)).second;const bool removed=births.erase(Key(h))!=0;
    if (added || removed) ++journalRevision;readiness.clear();
}
bool environmentItemExcluded(const unsigned int h[5]) {
    if (!lockReady || !h) return false;
    unsigned int owner[5];bool inInventory=false;void* item=0;
    if (readPhysicalItemLocation(h,owner,&inInventory,&item) && inInventory) { environmentExcludeItem(h);return true; }
    Guard guard;return excluded.count(Key(h))!=0;
}
void setEnvironmentJoin(bool enabled) { if (!lockReady) return;Guard guard;joinWorld=enabled;readiness.clear(); }
void setEnvironmentRegionReady(i32 x,i32 z,const procedural::NativeCensusFence* census) {
    if (!lockReady) return;Guard guard;const std::pair<i32,i32> key(x,z);
    if (!census || !census->current(journalRevision)) readiness.erase(key);
    else readiness[key]=*census;
}
void clearEnvironmentRegionReadiness() { if (!lockReady) return;Guard guard;readiness.clear(); }
int environmentItemInteraction(const unsigned int h[5]) {
    if (!hooksReady || !h) return 0;
    bool managed=false;
    { Guard guard;if (!joinWorld) return 1;managed=births.count(Key(h))!=0;if (excluded.count(Key(h))) return 0; }
    EnvironmentItemObject object;
    if (!describe(h,&object)) return managed?-1:0;
    if (!managed) return 0; // unknown saved objects are never adopted or permission-managed
    i32 x,z;RootObject* root=resolveObjectByHand(h);
    if (!proceduralRegionAtObject(root,&x,&z)) return -1;
    Guard guard;
    if (!joinWorld) return 1;
    if (excluded.count(Key(h)) || !births.count(Key(h))) return 0;
    std::map<std::pair<i32,i32>,procedural::NativeCensusFence>::const_iterator it=readiness.find(std::make_pair(x,z));
    return it!=readiness.end() && it->second.current(journalRevision) ? 1 : -1;
}
bool captureEnvironmentItems(GameWorld* world,i32 x,i32 z,std::vector<EnvironmentNativeRow>& out,
        procedural::NativeCensusFence* census) {
    if (census) census->reset();
    out.clear();if (!hooksReady || InterlockedCompareExchange(&active,0,0)!=0 || !proceduralRegionGenerationIdle(world,x,z)) return false;
    std::vector<Ogre::Vector3> pending;u64 revision=0;
    { Guard guard;revision=journalRevision;if (census) census->begin(revision);
      for (std::map<void*,Ticket>::const_iterator it=tickets.begin();it!=tickets.end();++it) pending.push_back(it->second.position);
      for (std::map<Item*,DeferredItem>::const_iterator it=delayedItems.begin();it!=delayedItems.end();++it) pending.push_back(it->second.position);
    }
    for (unsigned i=0;i<pending.size();++i) {
        int rx,rz;if (!cellAt(world,pending[i].x,pending[i].z,&rx,&rz) || (rx==x && rz==z)) return false;
    }
    static GroundItemRow ground[ENVIRONMENT_ITEMS_MAX];GroundItemCensusStatus status;
    const unsigned n=captureLoadedGroundItems(world,ground,ENVIRONMENT_ITEMS_MAX,&status);
    if (!status.complete || n>=ENVIRONMENT_ITEMS_MAX) return false;
    for (unsigned i=0;i<n;++i) {
        if (ground[i].zoneX!=x || ground[i].zoneY!=z) continue;
        const Key key(ground[i].hand);bool known=false,replica=false,skip=false;
        { Guard guard;skip=excluded.count(key)!=0;std::map<Key,Birth>::const_iterator it=births.find(key);
          known=it!=births.end() && it->second.generation==generation;if (known) replica=it->second.replica; }
        if (skip) continue;
        if (!known && ambiguousSavedDisplay(ground[i].hand)) return false;
        EnvironmentNativeRow row;
        if (!describe(ground[i].hand,&row.object)) { if (known) return false;continue; }
        if (!known) continue; // UNKNOWN stays outside the managed category
        memcpy(row.hand,ground[i].hand,20);row.replica=replica;out.push_back(row);
    }
    if (InterlockedCompareExchange(&active,0,0)!=0 || !proceduralRegionGenerationIdle(world,x,z)) { out.clear();return false; }
    { Guard guard;
      if (journalRevision!=revision) { out.clear();return false; }
      if (census) census->finish(true,journalRevision);
    }
    return true;
}
static bool spawnUnambiguous(GameWorld* world,const EnvironmentItemObject& o) {
    int x,z;if (!cellAt(world,o.x,o.z,&x,&z)) return false;
    static GroundItemRow rows[ENVIRONMENT_ITEMS_MAX];GroundItemCensusStatus status;
    const unsigned n=captureLoadedGroundItems(world,rows,ENVIRONMENT_ITEMS_MAX,&status);
    if (!status.complete || n>=ENVIRONMENT_ITEMS_MAX) return false;
    for (unsigned i=0;i<n;++i) {
        if (rows[i].zoneX!=x || rows[i].zoneY!=z) continue;
        bool spoken=false;
        { Guard guard;spoken=births.count(Key(rows[i].hand))!=0 || excluded.count(Key(rows[i].hand))!=0; }
        if (spoken) continue;
        GroundItemIdentityRow native;
        if (!readGroundItemIdentity(rows[i].hand,&native) || native.identityTruncated) return false;
        // Unknown same-template objects are not evidence of a missing replica.
        // Defer, rather than fabricate a second copy or appropriate/delete one.
        if (native.item.itemType==o.item.itemType && !strcmp(native.item.stringID,o.item.stringID)) return false;
    }
    return true;
}
bool environmentPreflight(GameWorld* world,const EnvironmentItemObject& o,unsigned int parent[5],unsigned int owner[5],unsigned int persistent[5]) {
    if (!hooksReady || !environment::valid(o) || !world || !world->theFactory || !spawnUnambiguous(world,o)) return false;
    __try {
        return world->theFactory->todoList.empty() && world->sysMessageList.empty() &&
            findItemTemplateImpl(world,o.item.stringID,o.item.itemType) &&
            resolveAnchor(world,o.parent,parent) && resolveAnchor(world,o.properOwner,owner) && resolveAnchor(world,o.persistentOwner,persistent);
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool environmentSafeToRemove(GameWorld* world,const unsigned int h[5]) {
    if (!lockReady || !h || !hooksReady) return false;
    { Guard guard;std::map<Key,Birth>::const_iterator it=births.find(Key(h));if (it==births.end() || it->second.generation!=generation || excluded.count(Key(h))) return false; }
    EnvironmentItemObject object;
    return describe(h,&object) && safeNative(world,h);
}
bool removeEnvironmentItem(GameWorld* world,const unsigned int h[5]) {
    if (!environmentSafeToRemove(world,h)) return false;
    RootObject* root=resolveObjectByHand(h);
    if (!root || !removeWorldItemProxy(world,root)) return false;
    Guard guard;births.erase(Key(h));++journalRevision;return true;
}
bool spawnEnvironmentItem(GameWorld* world,const EnvironmentItemObject& object,unsigned int out[5]) {
    memset(out,0,20);unsigned int parent[5],owner[5],persistent[5];
    if (!environmentPreflight(world,object,parent,owner,persistent)) return false;
    Item* item=fabricateEnvironmentItem(world,object.item);if (!item) return false;
    ++replicaDepth;bool assigned=assignNative(item,object,parent,owner,persistent);--replicaDepth;
    if (!assigned || !readObjectHand(item,out) || resolveObjectByHand(out)!=item) {
        removeWorldItemProxy(world,item);memset(out,0,20);return false;
    }
    remember(item,true,"authoritative-replica");
    EnvironmentItemObject actual;
    if (!describe(out,&actual) || !environment::sameNative(actual,object)) {
        removeWorldItemProxy(world,item);Guard guard;births.erase(Key(out));++journalRevision;memset(out,0,20);return false;
    }
    return true;
}
bool environmentMerchantRoom(GameWorld* world,EnvironmentAnchor* out,i32* x,i32* z) {
    if (!world || !out || !x || !z) return false;
    __try {
        Character* lead=leader(world);if (!lead) return false;
        Ogre::Vector3 p=lead->getPosition();
        static DeedRead buildings[1024];
        unsigned n=enumBuildingsNear(world,1000.0f,buildings,1024);if (n>=1024) return false;
        float nearest=FLT_MAX;bool found=false;
        for (unsigned i=0;i<n;++i) {
            if (buildings[i].shop!=1 || buildings[i].owned!=0 || buildings[i].resolved!=1) continue;
            EnvironmentAnchor candidate;if (!anchor(buildings[i].hand,&candidate)) continue;
            float dx=candidate.x-p.x,dy=candidate.y-p.y,dz=candidate.z-p.z;
            float d=dx*dx+dy*dy+dz*dz;
            if (d>=nearest) continue;
            int rx,rz;if (!cellAt(world,candidate.x,candidate.z,&rx,&rz)) continue;
            *out=candidate;*x=rx;*z=rz;nearest=d;found=true;
        }
        return found;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
} }
