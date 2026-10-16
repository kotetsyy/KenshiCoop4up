// ZoneQuery.cpp - the zone-loaded query (Phase 1 spawn parity) and the sector
// mappings behind it (engine::cellProbe), quarantined in its own TU because
// kenshi/ZoneManager.h redefines ParticlePool (also defined by
// kenshi/CombatClass.h, which EngineInternal.cpp needs) - the two vendored
// headers cannot share a translation unit.
//
// Why the query exists: within a locally LOADED world block, every baked
// (shared-save) NPC resolves by hand. So an unresolvable census hand whose
// host-reported position sits in a loaded block is a genuine host RUNTIME
// spawn - safe to proxy-mint at any distance without duplicate risk. This
// generalizes the fixed spawnMintRadius_ gate (a cheap stand-in for "the
// block here is certainly loaded") to the engine's own answer.

// ZoneManager.h pulls boost/thread headers (shared_mutex members), whose
// auto-link pragma demands libboost_thread-*.lib. We only form member-function
// pointers on the class (never instantiate it), so no boost code is generated -
// suppress the auto-link instead of shipping the library.
#define BOOST_ALL_NO_LIB

#include <windows.h>
#include <stdio.h>
#include <string.h>             // memset (CellProbe zero-fill)
#include <stdlib.h>             // getenv (ground diagnostic gate)
#include <float.h>              // finite native transforms before absence verdicts

#include <core/Functions.h>     // KenshiLib::GetRealAddress
#include <kenshi/GameWorld.h>   // GameWorld::zoneMgr
#include <kenshi/ZoneManager.h> // ZoneManager::_NV_isZoneLoadedT / _NV_isZoneBeingLoadedT
#include <kenshi/Item.h>
#include <kenshi/GameData.h>
#include <kenshi/ZoneMapContent.h>
#include <kenshi/Building/Building.h>
#include <kenshi/SharedKing.h>
#include <kenshi/Town.h>

#include "Engine.h"             // coop::engine::CellProbe (POD; pulls no kenshi headers)
#include "EngineProcedural.h"
#ifdef KENSHICOOP_NET_DIAG
#include <kenshi/util/lektor.h>
#endif
#include "../CoopLog.h"         // the sector-call shape tripwire
// Globals.h's foliage namespace conflicts with ZoneMapContent's SDK class.
// Import only the native global this isolated registry TU actually consumes.
__declspec(dllimport) extern SharedKing* shou;

namespace coop {
namespace engine {

namespace {
// this=RCX, const Ogre::Vector3& = pointer in RDX.
typedef bool (__fastcall* ZoneLoadedFn)(ZoneManager* self, const Ogre::Vector3* pos);
ZoneLoadedFn g_zoneLoadedFn      = 0;
ZoneLoadedFn g_zoneBeingLoadedFn = 0;
// Main-thread loaded-zone registry census, independent of the spatial query.
// getAllActiveZones writes into the caller's lektor. Keep the buffer across
// samples: lektor has no owning destructor, so a temporary would leak storage.
typedef void (__fastcall* ActiveZonesFn)(ZoneManager*, lektor<ZoneMap*>*);
ActiveZonesFn g_activeZonesFn = 0;
typedef bool (__fastcall* ZoneStateFn)(ZoneMap*);
ZoneStateFn g_zoneIsLoadedFn = 0;
ZoneStateFn g_zoneIsLoadingFn = 0;
typedef bool (__fastcall* GroundOnGroundFn)(const InventoryItemBase*);
GroundOnGroundFn g_groundOnGroundFn = 0;
typedef int (__fastcall* GlobalZoneLoadingFn)(ZoneManager*);
GlobalZoneLoadingFn g_globalZoneLoadingFn = 0;
typedef lektor<RootObject*>* (__fastcall* AllNativeTownsFn)(TownList*);
AllNativeTownsFn g_allNativeTownsFn = 0;

// The vendored CategorisedSet<BuildingFunction, Building*> spells its set
// element T2* (Building**), although the actual registry stores Building*
// pointer values. Reinterpret the VALUE only, never dereference that extra *.
typedef std::set<Building**, std::less<Building**>,
                 Ogre::STLAllocator<Building**, Ogre::GeneralAllocPolicy> > BedSet;
typedef std::map<BuildingFunction, BedSet, std::less<BuildingFunction>,
                 Ogre::STLAllocator<std::pair<BuildingFunction const, BedSet>,
                                    Ogre::GeneralAllocPolicy> > BedMap;
#ifdef KENSHICOOP_NET_DIAG

struct BedRow {
    unsigned int hand[5], inside[5];
    int zoneX, zoneY, zoneFile, fun, floor, complete, visibleFlag, visibleGet, interiorVisible, interior, ownerSet;
    int baseIndex, modIndex, modified;
    float x, y, z, yaw;
    char sid[48], uid[80], layout[48];
};

void copyBedText(char* dst, unsigned int cap, const std::string& src) {
    strncpy(dst, src.c_str(), cap - 1);
    dst[cap - 1] = '\0';
}

void fillBedRow(Building* b, ZoneMap* zone, BuildingFunction fun, BedRow* row) {
    memset(row, 0, sizeof(*row));
    const hand& h = b->handle;
    row->hand[0] = (unsigned int)h.type;
    row->hand[1] = h.container; row->hand[2] = h.containerSerial;
    row->hand[3] = h.index; row->hand[4] = h.serial;
    const hand& in = b->isInsideBuilding;
    row->inside[0] = (unsigned int)in.type;
    row->inside[1] = in.container; row->inside[2] = in.containerSerial;
    row->inside[3] = in.index; row->inside[4] = in.serial;
    row->zoneX = zone->coordinates.x; row->zoneY = zone->coordinates.y;
    row->zoneFile = zone->hasFile ? 1 : 0;
    row->fun = (int)fun;
    row->floor = b->floorNum; row->complete = b->_buildState.isComplete ? 1 : 0;
    row->visibleFlag = b->visible ? 1 : 0;
    row->visibleGet = b->getVisible() ? 1 : 0;
    row->interiorVisible = b->interiorVisibility ? 1 : 0;
    row->interior = b->isAnInteriorObject ? 1 : 0;
    row->ownerSet = b->owner ? 1 : 0;
    row->baseIndex = b->instanceID.baseIndex;
    row->modIndex = b->instanceID.modIndex;
    row->modified = b->instanceID.modified ? 1 : 0;
    const Ogre::Vector3 p = b->getPosition();
    row->x = p.x; row->y = p.y; row->z = p.z;
    row->yaw = b->getOrientation().getYaw().valueRadians();
    GameData* d = b->getGameData();
    if (d) copyBedText(row->sid, sizeof(row->sid), d->stringID);
    copyBedText(row->uid, sizeof(row->uid), b->instanceID.uid);
    copyBedText(row->layout, sizeof(row->layout), b->layoutInstanceID);
}
#endif

// Sector mappings, for cell authority (engine::cellAt) and its measurement
// (engine::cellProbe). Measured 2026-08-03: getMapSector at (-51178, 2655)
// returns (20,32) and the save folder holds zone.20.32.zone - the mapping IS
// the zone-file coordinate.
//
// The hidden return-buffer pointer is spelled out ON PURPOSE. iVector2 declares
// its own constructors, and MSVC returns EVERY class with a non-trivial
// constructor through a caller-provided buffer regardless of size - the "8 bytes
// so it comes back in RAX" rule only holds for trivially copyable PODs. This
// file used to carry the by-value spelling, which looked right and even READ
// right, and was corrupting the engine on every call: spelled by-value, MSVC
// passed OUR buffer in RCX and zm in RDX, while the real member wants `this` in
// RCX and the buffer in RDX. So getMapSector wrote its answer over the first
// qword of the ZoneManager - a class with pure-virtual bases, so that qword is
// its VTABLE POINTER - then returned that same address in RAX, from which we
// copied the correct sector. Right answer, stomped engine object, which is
// exactly why it survived the cell_probe validation.
// Field evidence 2026-08-03: with cellAuth on, both clients faulted at the
// identical instruction inside ZoneManager's own code (kenshi_x64.exe+0xa137b3,
// the 0xA0-0xA1 band these methods live in) reading 0x0000001F00000025 - a
// packed (x,y) sector pair whose y matched each client's own camera cell
// exactly, 31 on the host and 30 on the join.
typedef iVector2* (__fastcall* SectorXZFn)(ZoneManager* self, iVector2* ret,
                                           float x, float z);
SectorXZFn g_mapSectorFn = 0;
SectorXZFn g_subSectorFn = 0;
SectorXZFn g_resCoordFn  = 0;

// One sector call, plus a standing tripwire on the object's first qword. The
// buffer is a bare int pair so we never have to construct an iVector2 (its
// constructors live in the engine); the layout is {int x; int y} at offset 0.
// The tripwire is not paranoia - it is the specific check that would have caught
// the bug above at its first call instead of as an access violation minutes
// later in unrelated engine code, and it is what makes a future re-spelling of
// SectorXZFn fail loudly. Caller is inside SEH.
bool sectorCall(SectorXZFn fn, ZoneManager* zm, float x, float z,
                int* outX, int* outY) {
    if (!fn || !zm) return false;
    void* head = *(void**)zm;              // vptr; never a legal write target
    int ret[2] = { 0, 0 };
    fn(zm, (iVector2*)ret, x, z);
    if (*(void**)zm != head) {
        static bool told = false;
        if (!told) {
            told = true;
            coop::logLine("[cell] SHAPE getMapSector wrote through the "
                          "ZoneManager instead of the return buffer - the "
                          "calling convention is wrong (see SectorXZFn); "
                          "cell authority disabled to stop the corruption");
        }
        return false;
    }
    if (outX) *outX = ret[0];
    if (outY) *outY = ret[1];
    return true;
}

// getZoneBoundsT is deliberately NOT bound. It would hand back a cell's rect in
// one call, but it returns a 16-byte class through a hidden buffer pointer, and
// nothing in the headers says whether that pointer or `this` takes RCX. Both
// spellings were tried on 2026-08-03: the by-value typedef produced four
// identical junk floats and the explicit pointer form failed the "does the rect
// contain the point I asked about" check, i.e. neither convention matched.
//
// It is not needed. Walking getMapSector until the coord changes measures the
// same geometry using only an int pair in RAX, and the cell_probe scenario did
// exactly that: 4608 x 4608 u cells on an origin of -147456 (= -32 cells), with
// the predicted coord matching the engine's on both axes. A rect is a
// convenience; a wrong rect used to decide what is inside a cell is a bug.
} // namespace

// Called from engine::resolve(). Resolved via the _NV_ non-virtual aliases
// (concrete RVAs; the virtual names resolve to vtable thunks).
void resolveZoneQuery() {
    g_zoneLoadedFn = (ZoneLoadedFn)KenshiLib::GetRealAddress(
        &ZoneManager::_NV_isZoneLoadedT);
    g_zoneBeingLoadedFn = (ZoneLoadedFn)KenshiLib::GetRealAddress(
        &ZoneManager::_NV_isZoneBeingLoadedT);

    g_activeZonesFn = (ActiveZonesFn)KenshiLib::GetRealAddress(
        &ZoneManager::getAllActiveZones);
    g_zoneIsLoadedFn = (ZoneStateFn)KenshiLib::GetRealAddress(
        &ZoneMap::isLoadedMT);
    g_zoneIsLoadingFn = (ZoneStateFn)KenshiLib::GetRealAddress(
        &ZoneMap::isBeingLoadedMT);
    g_groundOnGroundFn = (GroundOnGroundFn)KenshiLib::GetRealAddress(
        &InventoryItemBase::onGround);
    g_globalZoneLoadingFn = (GlobalZoneLoadingFn)KenshiLib::GetRealAddress(
        &ZoneManager::isLoading);
    g_allNativeTownsFn = (AllNativeTownsFn)KenshiLib::GetRealAddress(
        &TownList::getAllTowns);

    // Non-virtual, so the member pointer is the real address (the RootObjectFactory
    // ::createItem precedent). getMapSector is overloaded, so it needs a typed
    // member pointer to pick the (float,float) one.
    iVector2 (ZoneManager::*pMap)(float, float) const = &ZoneManager::getMapSector;
    g_mapSectorFn = (SectorXZFn)KenshiLib::GetRealAddress(pMap);
    g_subSectorFn = (SectorXZFn)KenshiLib::GetRealAddress(&ZoneManager::getSubMapSector);
    g_resCoordFn  = (SectorXZFn)KenshiLib::GetRealAddress(
        &ZoneManager::getZoneMapFromResolutionCoord);

}

bool isZoneLoadedAt(GameWorld* gw, float x, float y, float z) {
    if (!gw || !g_zoneLoadedFn) return false;
    __try {
        ZoneManager* zm = gw->zoneMgr;
        if (!zm) return false;
        Ogre::Vector3 p(x, y, z);
        if (!g_zoneLoadedFn(zm, &p)) return false;
        // A block MID-LOAD hasn't materialized its baked NPCs yet - treating it
        // as loaded would far-mint a duplicate of a body about to appear.
        if (g_zoneBeingLoadedFn && g_zoneBeingLoadedFn(zm, &p)) return false;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool cellAt(GameWorld* gw, float x, float z, int* outCx, int* outCz) {
    if (!gw || !g_mapSectorFn) return false;
    __try {
        ZoneManager* zm = gw->zoneMgr;
        if (!zm) return false;
        return sectorCall(g_mapSectorFn, zm, x, z, outCx, outCz);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool cellProbe(GameWorld* gw, float x, float y, float z, CellProbe* out) {
    if (!gw || !out) return false;
    memset(out, 0, sizeof(*out));
    __try {
        ZoneManager* zm = gw->zoneMgr;
        if (!zm) return false;
        if (sectorCall(g_mapSectorFn, zm, x, z, &out->mapX, &out->mapY))
            out->haveMap = 1;
        if (sectorCall(g_subSectorFn, zm, x, z, &out->subX, &out->subY))
            out->haveSub = 1;
        if (sectorCall(g_resCoordFn, zm, x, z, &out->resX, &out->resY))
            out->haveRes = 1;
        (void)y;   // the sector grid is 2D; y is accepted only to match callers
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

namespace {
struct LoadedObjectHandScan {
    unsigned int nativeType,capacity,written;
    unsigned int (*out)[5];
    float x,y,z,radius;
    bool complete;
};

void appendLoadedObjectHand(LoadedObjectHandScan* scan,RootObject* object) {
    if (!object || (unsigned int)object->getDataType()!=scan->nativeType) return;
    unsigned int key[5];
    if (!readObjectHand(object,key) || resolveObjectByHand(key)!=object) {
        scan->complete=false; return;
    }
    Ogre::Vector3 p=object->getPosition();
    if (!_finite(p.x) || !_finite(p.y) || !_finite(p.z)) {
        scan->complete=false; return;
    }
    float dx=p.x-scan->x,dy=p.y-scan->y,dz=p.z-scan->z;
    if (dx*dx+dy*dy+dz*dz>scan->radius*scan->radius) return;
    // The near-point result is bounded; duplicate native category/zone
    // registrations must not turn one physical fixture into an ambiguity.
    for (unsigned int i=0;i<scan->written;++i)
        if (memcmp(scan->out[i],key,sizeof(key))==0) return;
    if (scan->written>=scan->capacity) { scan->complete=false; return; }
    memcpy(scan->out[scan->written++],key,sizeof(key));
}

void collectLoadedBuildingHands(GameWorld* gw,LoadedObjectHandScan* scan) {
    ZoneManager* zm=gw->zoneMgr;
    int minX,minY,maxX,maxY;
    if (!zm || !cellAt(gw,scan->x-scan->radius,scan->z-scan->radius,&minX,&minY) ||
        !cellAt(gw,scan->x+scan->radius,scan->z+scan->radius,&maxX,&maxY) ||
        minX<0 || minY<0 || maxX>=64 || maxY>=64 || minX>maxX || minY>maxY) {
        scan->complete=false; return;
    }
    static lektor<ZoneMap*> zones;
    zones.clear();
    g_activeZonesFn(zm,&zones);
    if (zones.size()>4096) { scan->complete=false; return; }
    unsigned int loaded=0;
    for (unsigned int zi=0;zi<zones.size();++zi) {
        ZoneMap* zone=zones[zi];
        if (!zone) { scan->complete=false; continue; }
        if (zone->coordinates.x<minX || zone->coordinates.x>maxX ||
            zone->coordinates.y<minY || zone->coordinates.y>maxY) continue;
        if (!g_zoneIsLoadedFn(zone) || g_zoneIsLoadingFn(zone)) {
            scan->complete=false; continue;
        }
        ZoneMapContent* content=zone->mapContent;
        if (!content || !content->loaded) { scan->complete=false; continue; }
        ++loaded;
        const BedMap& registry=content->buildingList.list;
        const size_t before=registry.size();
        for (BedMap::const_iterator cat=registry.begin();cat!=registry.end();++cat) {
            const size_t categoryBefore=cat->second.size();
            for (BedSet::const_iterator it=cat->second.begin();it!=cat->second.end();++it) {
                if (!*it) { scan->complete=false; continue; }
                // BF_ANY can repeat a pointer in its functional category.
                // Consult previous native sets without allocating a copied set.
                bool seen=false;
                for (BedMap::const_iterator prior=registry.begin();prior!=cat;++prior)
                    if (prior->second.find(*it)!=prior->second.end()) { seen=true; break; }
                if (!seen)
                    appendLoadedObjectHand(scan,reinterpret_cast<Building*>(*it));
            }
            if (categoryBefore!=cat->second.size()) scan->complete=false;
        }
        if (before!=registry.size() || !content->loaded ||
            !g_zoneIsLoadedFn(zone) || g_zoneIsLoadingFn(zone)) scan->complete=false;
    }
    const unsigned int expected=(unsigned int)((maxX-minX+1)*(maxY-minY+1));
    if (loaded!=expected) scan->complete=false;
}

void collectLoadedTownHands(GameWorld* gw,LoadedObjectHandScan* scan) {
    if (!gw->zoneMgr || !::shou || !::shou->townList) {
        scan->complete=false; return;
    }
    // Town root objects remain native/identifiable when the town is inactive.
    // Do not equate inactivity or an unloaded town-center terrain cell with
    // absence. Global save/world initialization still forbids absence verdicts.
    if (g_globalZoneLoadingFn(gw->zoneMgr)) scan->complete=false;
    lektor<RootObject*>* towns=g_allNativeTownsFn(::shou->townList);
    if (!towns) { scan->complete=false; return; }
    const unsigned int before=towns->size();
    for (unsigned int i=0;i<before;++i) appendLoadedObjectHand(scan,(*towns)[i]);
    if (before!=towns->size() || g_globalZoneLoadingFn(gw->zoneMgr)) scan->complete=false;
}

bool collectLoadedObjectHandsGuarded(GameWorld* gw,LoadedObjectHandScan* scan) {
    __try {
        if (scan->nativeType==(unsigned int)BUILDING) collectLoadedBuildingHands(gw,scan);
        else collectLoadedTownHands(gw,scan);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void collectGroundRows(GameWorld* gw,GroundItemRow* rows,unsigned int capacity,
                       GroundItemCensusStatus* status) {
    ZoneManager* zm=gw->zoneMgr;
    if (!zm) { status->unavailable=true; return; }
    // getAllActiveZones appends to caller-owned storage. Retain its native
    // lektor buffer (which has no owning destructor), and never copy registries.
    static lektor<ZoneMap*> zones;
    zones.clear();
    g_activeZonesFn(zm,&zones);
    status->activeZones=zones.size();
    if (status->activeZones>4096) { status->fault=true; return; }
    for (unsigned int zi=0;zi<status->activeZones;++zi) {
        ZoneMap* zone=zones[zi];
        if (!zone) { ++status->noContentZones; continue; }
        if (!g_zoneIsLoadedFn(zone) || g_zoneIsLoadingFn(zone)) {
            ++status->loadingZones; continue;
        }
        ZoneMapContent* content=zone->mapContent;
        if (!content || !content->loaded) { ++status->noContentZones; continue; }
        ++status->loadedZones;
        const ogre_unordered_set<hand>::type& items=content->items;
        const size_t before=items.size();
        for (ogre_unordered_set<hand>::type::const_iterator it=items.begin();it!=items.end();++it) {
            ++status->registryItems;
            const hand& h=*it;
            unsigned int key[5]={(unsigned int)h.type,h.container,h.containerSerial,h.index,h.serial};
            RootObject* object=resolveObjectByHand(key);
            unsigned int actual[5];
            if (!object || !object->typeIsAnItem() ||
                !readObjectHand(object,actual) || memcmp(key,actual,sizeof(key))!=0) {
                ++status->unresolved; continue;
            }
            Item* item=static_cast<Item*>(object);
            // Native onGround distinguishes floor Items from NPC render
            // attachments. Explicit inventory/equipped exclusions remain
            // necessary even when the zone registry still holds a hand.
            if (item->isInInventory) { ++status->inventoryItems; continue; }
            if (item->isEquipped) { ++status->equippedItems; continue; }
            if (!g_groundOnGroundFn(item)) { ++status->notGround; continue; }
            ++status->groundItems;
            if (status->written>=capacity) { ++status->overflow; continue; }
            Ogre::Vector3 position=item->getPosition();
            const int floor=item->floorNum;
            GroundItemRow& row=rows[status->written];
            memcpy(row.hand,actual,sizeof(row.hand));
            row.zoneX=zone->coordinates.x; row.zoneY=zone->coordinates.y;
            row.floor=floor;
            row.x=position.x; row.y=position.y; row.z=position.z;
            ++status->written;
        }
        if (before!=items.size()) status->fault=true;
        if (!g_zoneIsLoadedFn(zone) || g_zoneIsLoadingFn(zone) || !content->loaded)
            ++status->loadingZones;
    }
}
bool collectGroundRowsGuarded(GameWorld* gw,GroundItemRow* rows,unsigned int capacity,
                              GroundItemCensusStatus* status) {
    // STL iterator unwinding must stay out of this SEH wrapper on v100.
    __try {
        collectGroundRows(gw,rows,capacity,status);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
} // namespace

unsigned int captureLoadedGroundItems(GameWorld* gw,GroundItemRow* rows,unsigned int capacity,
                                     GroundItemCensusStatus* status) {
    if (!status) return 0;
    memset(status,0,sizeof(*status));
    if (!gw || (!rows && capacity) || !g_activeZonesFn ||
        !g_zoneIsLoadedFn || !g_zoneIsLoadingFn || !g_groundOnGroundFn) {
        status->unavailable=true; return 0;
    }
    if (!collectGroundRowsGuarded(gw,rows,capacity,status)) status->fault=true;
    status->complete=status->activeZones>0 && !status->unavailable && !status->fault &&
        !status->loadingZones && !status->noContentZones && !status->unresolved && !status->overflow;
    return status->written;
}

void logLoadedGroundItems(GameWorld* gw) {
    const char* enabled=getenv("KENSHICOOP_INV_DUMP");
    if (!gw || !enabled || enabled[0]!='1') return;
    static unsigned long lastMs=0;
    unsigned long now=GetTickCount();
    if (lastMs && now-lastMs<15000) return;
    lastMs=now;
    static GroundItemRow rows[1024];
    GroundItemCensusStatus status;
    unsigned int count=captureLoadedGroundItems(gw,rows,1024,&status);
    char line[4096];
    _snprintf(line,sizeof(line)-1,
        "[wi] GROUND-CENSUS active=%u loaded=%u loading=%u noContent=%u registry=%u "
        "ground=%u logged=%u overflow=%u unresolved=%u notGround=%u inventory=%u equipped=%u "
        "unavailable=%d fault=%d complete=%d",
        status.activeZones,status.loadedZones,status.loadingZones,status.noContentZones,
        status.registryItems,status.groundItems,status.written,status.overflow,status.unresolved,
        status.notGround,status.inventoryItems,status.equippedItems,
        status.unavailable ? 1 : 0,status.fault ? 1 : 0,status.complete ? 1 : 0);
    line[sizeof(line)-1]='\0'; coop::logLine(line);
    for (unsigned int i=0;i<count;++i) {
        GroundItemIdentityRow identity;
        bool readable=readGroundItemIdentity(rows[i].hand,&identity);
        const GroundItemRow& row=rows[i];
        _snprintf(line,sizeof(line)-1,
            "[wi] GROUND-ROW zone=%d,%d floor=%d hand=%u.%u.%u.%u.%u pos=%.3f,%.3f,%.3f "
            "read=%d sid='%s' type=%u qty=%u condition=%.3f grade=%u maker='%s' material='%s' "
            "charges=%.3f/%.3f saved=%d firstTime=%d truncated=%d "
            "instance='%s' source='%s' ref='%s' base='%s' "
            "properOwner=%u.%u.%u.%u.%u inventoryOwner=%u.%u.%u.%u.%u inside=%u.%u.%u.%u.%u",
            row.zoneX,row.zoneY,row.floor,row.hand[0],row.hand[1],row.hand[2],row.hand[3],row.hand[4],
            row.x,row.y,row.z,readable ? 1 : 0,identity.item.stringID,identity.item.itemType,
            (unsigned int)identity.item.quantity,identity.condition,(unsigned int)identity.item.level,
            identity.item.manufacturer,identity.item.material,identity.charges,identity.fullCharge,
            identity.savedInput ? 1 : 0,identity.firstTime ? 1 : 0,identity.identityTruncated ? 1 : 0,
            identity.instanceID,identity.sourceName,identity.instanceRef,identity.baseSID,
            identity.properOwner[0],identity.properOwner[1],identity.properOwner[2],
            identity.properOwner[3],identity.properOwner[4],identity.inventoryOwner[0],
            identity.inventoryOwner[1],identity.inventoryOwner[2],identity.inventoryOwner[3],
            identity.inventoryOwner[4],identity.insideBuilding[0],identity.insideBuilding[1],
            identity.insideBuilding[2],identity.insideBuilding[3],identity.insideBuilding[4]);
        line[sizeof(line)-1]='\0'; coop::logLine(line);
    }
}
#ifdef KENSHICOOP_NET_DIAG
struct BedCensusSample {
    enum { ROWS = 32 };
    BedRow rows[ROWS];
    float anchors[12];
    unsigned int na, active, loaded, loading, noContent, allBeds, nearBeds, written;
    bool fault;
};

static void collectBedRows(GameWorld* gw, BedCensusSample* s) {
    static lektor<ZoneMap*> zones; // reuse engine-allocated storage
    s->na = interestAnchors(gw, s->anchors);
    ZoneManager* zm = gw->zoneMgr;
    if (!zm) { s->fault = true; return; }
    zones.clear();
    g_activeZonesFn(zm, &zones);
    s->active = zones.size();
    if (s->active > 4096) { s->fault = true; s->active = 0; return; }
    for (unsigned int zi = 0; zi < s->active; ++zi) {
        ZoneMap* zone = zones[zi];
        if (!zone) continue;
        if (!g_zoneIsLoadedFn(zone) || g_zoneIsLoadingFn(zone)) {
            ++s->loading;
            continue;
        }
        ZoneMapContent* content = zone->mapContent;
        if (!content || !content->loaded) { ++s->noContent; continue; }
        ++s->loaded;
        const BedMap& bm = content->buildingList.list;
        const BuildingFunction kinds[2] = { BF_BED, BF_SKELETON_BED };
        for (int ki = 0; ki < 2; ++ki) {
            BedMap::const_iterator cat = bm.find(kinds[ki]);
            if (cat == bm.end()) continue;
            for (BedSet::const_iterator it = cat->second.begin();
                 it != cat->second.end(); ++it) {
                Building* b = reinterpret_cast<Building*>(*it);
                if (!b) continue;
                ++s->allBeds;
                Ogre::Vector3 p = b->getPosition();
                bool insideRadius = false;
                for (unsigned int ai = 0; ai < s->na; ++ai) {
                    float dx = p.x - s->anchors[3 * ai];
                    float dz = p.z - s->anchors[3 * ai + 2];
                    if (dx * dx + dz * dz <= 120.0f * 120.0f) {
                        insideRadius = true;
                        break;
                    }
                }
                if (!insideRadius) continue;
                ++s->nearBeds;
                if (s->written < BedCensusSample::ROWS) {
                    fillBedRow(b, zone, kinds[ki], &s->rows[s->written]);
                    ++s->written;
                }
            }
        }
    }
}

// v100 cannot put SEH in a function with STL iterator unwinding.
static bool collectBedRowsGuarded(GameWorld* gw, BedCensusSample* sample) {
    __try {
        collectBedRows(gw, sample);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void logBedCensus(GameWorld* gw) {
    if (!gw) return;
    static unsigned long lastMs = 0;
    const unsigned long now = GetTickCount();
    if (lastMs && now - lastMs < 15000) return;
    lastMs = now;
    if (!g_activeZonesFn || !g_zoneIsLoadedFn || !g_zoneIsLoadingFn) {
        coop::logLine("[bed] CENSUS unavailable: zone registry/state functions unresolved");
        return;
    }

    BedCensusSample sample = { 0 };
    if (!collectBedRowsGuarded(gw, &sample)) sample.fault = true;

    char line[768];
    _snprintf(line, sizeof(line) - 1,
              "[bed] CENSUS active=%u loaded=%u pending=%u noContent=%u "
              "all=%u near120=%u logged=%u anchors=%u fault=%d",
              sample.active, sample.loaded, sample.loading, sample.noContent,
              sample.allBeds, sample.nearBeds, sample.written, sample.na,
              sample.fault ? 1 : 0);
    line[sizeof(line) - 1] = '\0'; coop::logLine(line);
    for (unsigned int ai = 0; ai < sample.na; ++ai) {
        int cellX = 0, cellY = 0;
        bool haveCell = cellAt(gw, sample.anchors[ai * 3],
                               sample.anchors[ai * 3 + 2], &cellX, &cellY);
        bool zoneLoaded = isZoneLoadedAt(gw, sample.anchors[ai * 3],
                                        sample.anchors[ai * 3 + 1],
                                        sample.anchors[ai * 3 + 2]);
        _snprintf(line, sizeof(line) - 1,
                  "[bed] ANCHOR i=%u pos=%.1f,%.1f,%.1f cell=%d,%d mapped=%d loaded=%d",
                  ai, sample.anchors[ai * 3], sample.anchors[ai * 3 + 1],
                  sample.anchors[ai * 3 + 2], cellX, cellY, haveCell ? 1 : 0,
                  zoneLoaded ? 1 : 0);
        line[sizeof(line) - 1] = '\0'; coop::logLine(line);
    }
    for (unsigned int i = 0; i < sample.written; ++i) {
        const BedRow& r = sample.rows[i];
        _snprintf(line, sizeof(line) - 1,
                  "[bed] ROW zone=%d,%d zoneFile=%d fun=%d hand=%u.%u.%u.%u.%u "
                  "inside=%u.%u.%u.%u.%u pos=%.2f,%.2f,%.2f yaw=%.3f "
                  "floor=%d complete=%d visibleFlag=%d visibleGet=%d interiorVisible=%d "
                  "interior=%d ownerSet=%d base=%d mod=%d modified=%d "
                  "sid='%s' uid='%s' layout='%s'",
                  r.zoneX, r.zoneY, r.zoneFile, r.fun, r.hand[0], r.hand[1],
                  r.hand[2], r.hand[3], r.hand[4], r.inside[0], r.inside[1],
                  r.inside[2], r.inside[3], r.inside[4], r.x, r.y, r.z, r.yaw,
                  r.floor, r.complete, r.visibleFlag, r.visibleGet,
                  r.interiorVisible, r.interior, r.ownerSet, r.baseIndex,
                  r.modIndex, r.modified, r.sid, r.uid, r.layout);
        line[sizeof(line) - 1] = '\0'; coop::logLine(line);
    }
}
#endif

unsigned int listLoadedObjectHands(GameWorld* gw,unsigned int nativeType,
                                   float x,float y,float z,float radius,
                                   unsigned int (*out)[5],unsigned int maxOut,bool* complete) {
    if (complete) *complete=false;
    if (!complete || !gw || (!out && maxOut) || radius<0 ||
        !_finite(x) || !_finite(y) || !_finite(z) || !_finite(radius)) return 0;
    if (nativeType==(unsigned int)BUILDING) {
        if (!g_activeZonesFn || !g_zoneIsLoadedFn || !g_zoneIsLoadingFn) return 0;
    } else if (nativeType==(unsigned int)TOWN) {
        if (!g_allNativeTownsFn || !g_globalZoneLoadingFn) return 0;
    } else return 0;
    LoadedObjectHandScan scan;
    scan.nativeType=nativeType; scan.out=out; scan.capacity=maxOut; scan.written=0;
    scan.x=x; scan.y=y; scan.z=z; scan.radius=radius; scan.complete=true;
    if (!collectLoadedObjectHandsGuarded(gw,&scan)) scan.complete=false;
    *complete=scan.complete;
    return scan.written;
}
namespace {
unsigned int proceduralZones(GameWorld* world,ProceduralRegionRead* out,unsigned int cap,bool* complete) {
    static lektor<ZoneMap*> zones;
    zones.clear(); g_activeZonesFn(world->zoneMgr,&zones);
    if (zones.size()>4096) { *complete=false; return 0; }
    unsigned int n=0;
    for (unsigned int i=0;i<zones.size();++i) {
        ZoneMap* zone=zones[i];
        if (!zone) { *complete=false; continue; }
        if (!g_zoneIsLoadedFn(zone) || g_zoneIsLoadingFn(zone)) { *complete=false; continue; }
        if (n==cap) { *complete=false; continue; }
        out[n].x=zone->coordinates.x; out[n].z=zone->coordinates.y;
        out[n].complete=zone->mapContent && zone->mapContent->loaded;
        if (!out[n].complete) *complete=false;
        ++n;
    }
    return n;
}
unsigned int proceduralBeds(GameWorld* world,i32 x,i32 z,ProceduralBedRead* out,unsigned int cap,bool* complete) {
    static lektor<ZoneMap*> zones;
    zones.clear(); g_activeZonesFn(world->zoneMgr,&zones);
    if (zones.size()>4096) { *complete=false; return 0; }
    unsigned int n=0,matched=0;
    for (unsigned int i=0;i<zones.size();++i) {
        ZoneMap* zone=zones[i];
        if (!zone) { *complete=false; continue; }
        if (zone->coordinates.x!=x || zone->coordinates.y!=z) continue;
        ++matched;
        if (!g_zoneIsLoadedFn(zone) || g_zoneIsLoadingFn(zone) || !zone->mapContent || !zone->mapContent->loaded) { *complete=false; continue; }
        ZoneMapContent* content=zone->mapContent;
        const BedMap& registry=content->buildingList.list;
        const BuildingFunction kinds[2]={BF_BED,BF_SKELETON_BED};
        for (int kind=0;kind<2;++kind) {
            BedMap::const_iterator category=registry.find(kinds[kind]);
            if (category==registry.end()) continue;
            const size_t before=category->second.size();
            for (BedSet::const_iterator it=category->second.begin();it!=category->second.end();++it) {
                Building* bed=reinterpret_cast<Building*>(*it);
                if (!bed) { *complete=false; continue; }
                ProceduralBedRead row;
                if (!readProceduralBedNative(world,bed,&row)) { *complete=false; continue; }
                if (proceduralBedTemplate(row.object.sid) && proceduralBedOrigin(row.hand)<0) *complete=false;
                bool duplicate=false;
                for (unsigned int j=0;j<n;++j) if (memcmp(out[j].hand,row.hand,20)==0) { duplicate=true; break; }
                if (duplicate) continue;
                if (n==cap) { *complete=false; continue; }
                out[n++]=row;
            }
            if (before!=category->second.size()) *complete=false;
        }
        if (!content->loaded || !g_zoneIsLoadedFn(zone) || g_zoneIsLoadingFn(zone)) *complete=false;
    }
    if (matched!=1) *complete=false;
    return n;
}
}
unsigned int enumerateProceduralRegions(GameWorld* world,ProceduralRegionRead* out,unsigned int cap,bool* complete) {
    if (complete) *complete=false;
    if (!complete || !world || !world->zoneMgr || (!out && cap) || !g_activeZonesFn || !g_zoneIsLoadedFn || !g_zoneIsLoadingFn || !g_globalZoneLoadingFn) return 0;
    unsigned int n=0;
    __try { *complete=!g_globalZoneLoadingFn(world->zoneMgr); n=proceduralZones(world,out,cap,complete); }
    __except(EXCEPTION_EXECUTE_HANDLER) { *complete=false; }
    return n;
}
unsigned int captureProceduralBeds(GameWorld* world,i32 x,i32 z,ProceduralBedRead* out,unsigned int cap,bool* complete) {
    if (complete) *complete=false;
    if (!complete || !world || !world->zoneMgr || (!out && cap) || !g_activeZonesFn || !g_zoneIsLoadedFn || !g_zoneIsLoadingFn || !g_globalZoneLoadingFn) return 0;
    unsigned int n=0;
    bool hooks=false,loading=true,idle=false,stable=false;
    __try {
        const u64 revision=proceduralOriginRevision();
        hooks=proceduralWorldHooksReady();
        loading=g_globalZoneLoadingFn(world->zoneMgr);
        idle=proceduralRegionGenerationIdle(world,x,z);
        *complete=hooks && !loading && idle;
        n=proceduralBeds(world,x,z,out,cap,complete);
        stable=revision==proceduralOriginRevision();
        idle=idle && proceduralRegionGenerationIdle(world,x,z);
        if (!stable || !idle) *complete=false;
    }
    __except(EXCEPTION_EXECUTE_HANDLER) { *complete=false; }
#ifdef KENSHICOOP_HARNESS
    struct CensusDiagnostic { i32 x,z; DWORD at; bool used,complete; };
    static CensusDiagnostic diagnostics[32]={};
    static unsigned int nextDiagnostic=0;
    CensusDiagnostic* diagnostic=0;
    for (unsigned int i=0;i<32;++i)
        if (diagnostics[i].used && diagnostics[i].x==x && diagnostics[i].z==z) { diagnostic=&diagnostics[i]; break; }
    if (!diagnostic) {
        diagnostic=&diagnostics[nextDiagnostic++%32];
        diagnostic->used=false;
    }
    const DWORD now=GetTickCount();
    if (!diagnostic->used || diagnostic->complete!=*complete || (!*complete && now-diagnostic->at>=2000)) {
        unsigned int generated=0,replicas=0,unknown=0;
        for (unsigned int i=0;i<n;++i) {
            if (out[i].procedural) ++generated;
            if (out[i].replica) ++replicas;
            if (proceduralBedTemplate(out[i].object.sid) && proceduralBedOrigin(out[i].hand)<0) ++unknown;
        }
        char line[256];
        _snprintf(line,sizeof(line)-1,
            "[procworld] NATIVE region=%d,%d complete=%d beds=%u generated=%u replicas=%u unknown=%u hooks=%d loading=%d idle=%d stable=%d",
            x,z,*complete?1:0,n,generated,replicas,unknown,hooks?1:0,loading?1:0,idle?1:0,stable?1:0);
        line[sizeof(line)-1]=0; coop::logLine(line);
        diagnostic->x=x; diagnostic->z=z; diagnostic->at=now;
        diagnostic->used=true; diagnostic->complete=*complete;
    }
#endif
    return n;
}
bool proceduralRegionAtObject(void* opaque,i32* x,i32* z) {
    if (!opaque || !x || !z) return false;
    __try {
        ZoneMap* zone=static_cast<Building*>(opaque)->getZoneMapLocation();
        if (!zone || !g_zoneIsLoadedFn || !g_zoneIsLoadingFn || !g_zoneIsLoadedFn(zone) || g_zoneIsLoadingFn(zone) || !zone->mapContent || !zone->mapContent->loaded) return false;
        *x=zone->coordinates.x; *z=zone->coordinates.y; return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
namespace {
bool loadedReferencesClear(GameWorld* world,Building* bed) {
    unsigned int h[5]; if (!readObjectHand(bed,h)) return false;
    static lektor<ZoneMap*> zones;
    zones.clear(); g_activeZonesFn(world->zoneMgr,&zones);
    if (zones.size()>4096 || g_globalZoneLoadingFn(world->zoneMgr)) return false;
    for (unsigned int i=0;i<zones.size();++i) {
        ZoneMap* zone=zones[i];
        if (!zone || !g_zoneIsLoadedFn(zone) || g_zoneIsLoadingFn(zone) || !zone->mapContent || !zone->mapContent->loaded) return false;
        ZoneMapContent* content=zone->mapContent;
        const BedMap& registry=content->buildingList.list;
        for (BedMap::const_iterator cat=registry.begin();cat!=registry.end();++cat)
            for (BedSet::const_iterator it=cat->second.begin();it!=cat->second.end();++it) {
                Building* other=reinterpret_cast<Building*>(*it);
                if (!other) return false;
                if (other==bed) continue;
                const hand& inside=other->isInsideBuilding;
                unsigned int parent[5]={(unsigned int)inside.type,inside.container,inside.containerSerial,inside.index,inside.serial};
                if (memcmp(parent,h,20)==0 || other->furnitureParentBuilding()==bed) return false;
            }
        const unsigned int before=content->things.size();
        if (before>65536) return false;
        for (unsigned int j=0;j<before;++j) {
            RootObject* object=content->things[j];
            if (!object) return false;
            if (object->getDataType()==CHARACTER && !proceduralCharacterReferencesClear(object,h)) return false;
        }
        if (before!=content->things.size() || !content->loaded || g_zoneIsLoadingFn(zone)) return false;
    }
    return true;
}
}
bool proceduralLoadedReferencesClear(GameWorld* world,void* bed) {
    if (!world || !world->zoneMgr || !bed || !g_activeZonesFn || !g_globalZoneLoadingFn || !g_zoneIsLoadedFn || !g_zoneIsLoadingFn) return false;
    __try { return loadedReferencesClear(world,static_cast<Building*>(bed)); }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
} // namespace engine
} // namespace coop
