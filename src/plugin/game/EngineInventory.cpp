// EngineInventory.cpp - inventory + world items (monolith split from
// EngineInternal.cpp, 2026-07-12): Phase 4a container-contents capture/reconcile,
// item template finders + createItemAndAdd, equip/unequip levers, Phase W0/W1
// world-item (ground drop) diagnostics + replication proxies, and the spike-401
// research tech-tree store probe. (The spike-451 weapon-mint recipe trace +
// diagWeaponCreate moved to the harness-only EngineProbe.cpp in Phase 5e.)
//
// Owner state: section-private statics/anon-namespace helpers only (r401 lever
// cache).
// Must NOT: define shared g_* engine pointers (EngineInternal.cpp owns them), or
// change existing oracle log strings. Saved-ground input identity hooks below
// own only their private trampolines/records; no replication verdict uses them
// until actual paired-engine identity evidence establishes a safe namespace.

#include "EngineInternal.h"
#include "EngineProcedural.h"
#include "EngineEnvironmentItems.h"
#include <kenshi/GameDataManager.h>
#include <kenshi/Building/ProductionBuilding.h>
#include <kenshi/gui/InventoryGUI.h>
#include <kenshi/gui/InventoryTraderGUI.h>
#include <kenshi/ShopTraderInventory.h>
#include <algorithm>
#include <memory>
#include "InventoryTransactionBridge.h"
#include "../core/InventoryTransactionRules.h"

namespace coop {
namespace engine {

// ---- Phase 4a: container-contents (inventory) capture / reconcile ----------

RootObject* resolveObjectByHand(const unsigned int cHand[5]) {
    // object-order adapter over the typed resolve: cHand = {type, container,
    // containerSerial, index, serial}. resolveObject (EngineEntity.cpp) owns the
    // native hand-ctor arg order + SEH; this just converts the legacy array.
    if (!cHand) return 0;
    return resolveObject(ObjectHand::fromObjOrder(cHand));
}

namespace {
bool inventoryContainsPhysicalItem(Inventory* inventory,Item* item);

// invEntryHash / sectionNameHash moved to ../../netproto/ContentHash.h (shared
// with the prototest unit layer, which locks their cross-client stability).
using coop::sectionNameHash;
using coop::invEntryHash;

// After removeItemAutoDestroy the loot GUI can leave 0xFFF... in a section
// slot. Walking that Item* is the APPLY-empty crash (READ of -1).
static bool itemPtrLive(Item* it) {
    if (!it) return false;
    unsigned long long p = (unsigned long long)(size_t)it;
    if (p < 0x10000ull) return false;
    if ((p + 1ull) == 0ull) return false;
    return true;
}

// True while an inventory PANEL is open on this container (loot window, trade
// window, a character sheet, an opened backpack). The engine hands every icon in
// that panel a raw Item*, and it never revalidates them: destroying a stack under
// a live window leaves the panel pointing at freed memory, and the next repaint
// dereferences it on the RENDER thread - outside every __except we own. That is
// the APPLY-empty crash, and it is why itemPtrLive/SEH could not fix it: they
// only caught OUR three first-chance reads of -1, while the fatal one belonged to
// kenshi_x64.exe on another thread. So the destructive half of the reconcile asks
// this first and defers instead. Unresolved lever (CAP_INV_GUI off) reports "not
// open": that restores the pre-guard behaviour rather than freezing all looting,
// and the missing lever is already visible in the [engine] CAPS line.
bool inventoryGuiOpen(Inventory* inv) {
    if (!inv || !g_getInvGuiFn) return false;
    __try {
        return g_getInvGuiFn(inv) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Rebuild an open panel's icons from the inventory as it stands now; returns
// whether that actually happened. This is what makes destroying a stack under a
// live window safe: the icons hold raw Item* pointers, and re-making them from
// the current contents before we hand control back leaves nothing stale for the
// render thread to walk. False means we could not refresh (lever unresolved, no
// panel, or a fault) and the caller must not destroy.
bool inventoryGuiRefresh(Inventory* inv) {
    if (!inv || !g_getInvGuiFn || !g_invGuiRefreshFn) return false;
    __try {
        InventoryGUI* g = g_getInvGuiFn(inv);
        if (!g) return false;
        g_invGuiRefreshFn(g);
        g->needItemsUpdate = true; // also ask for the engine's own next pass
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}


} // namespace
// Copy native provenance for reconstruction and identity. For weapons these are
// manufacturer/material records; blueprints carry research and trade goods also
// retain native material data needed for stack compatibility.
void fillItemProvenance(Item* it, InvItemEntry& e) {
    __try {
        GameData* man = it->manufacturerData;
        GameData* mat = it->materialData;
        if (man) { const char* s = man->stringID.c_str(); strncpy(e.manufacturer, s ? s : "", sizeof(e.manufacturer) - 1); }
        if (mat) { const char* s = mat->stringID.c_str(); strncpy(e.material,     s ? s : "", sizeof(e.material) - 1); }
        if (!e.equipped && it->isInInventory &&
            it->inventoryPos.x>=0 && it->inventoryPos.y>=0) {
            e.gridSection=sectionNameHash(it->inventorySection.c_str());
            e.gridX=(unsigned short)it->inventoryPos.x;
            e.gridY=(unsigned short)it->inventoryPos.y;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// SEH-guarded: read a container's contents - both LOOSE items and EQUIPPED gear
// (each tagged with its equipped flag + slot) - into out[] and, when outItems != 0,
// the matching Item* for each entry (used by the reconcile to remove excess stacks).
// Reads template stringID + type + stack quantity + quality bucket off each Item.
// Returns the count written.
//
// CAPTURE ORDER IS LOAD-BEARING: worn gear (equip sections, then the held-weapon
// pointers) is captured BEFORE the loose _allItems list. maxOut is a hard cap, and a
// character carrying more than maxOut loose stacks (routine with a backpack) used to
// fill the buffer with loose items and emit ZERO equipped entries - which the peer's
// reconcile read as desiredEq==0 and answered by stripping the character's entire
// kit. Gear is the irreplaceable part and the smallest set, so it goes first; loose
// clutter is what gets cut when a container overflows.
//
// When *outTruncated is provided it is set true if any qualifying item did NOT fit in
// out[] - the caller must then treat the result as an INCOMPLETE description of the
// container and never let a reconcile delete against it (see applyContainerContents).
//
// includeNested (protocol 48) additionally appends what worn CONTAINERS hold, each tagged
// with parentIdx. It is OFF by default and only the inventory PUBLISHER turns it on, because
// a nested entry does not describe THIS inventory: a caller that counts or reconciles against
// the result would read a bagged item as a loose one. That mistake is not theoretical - the
// top-level reconcile did exactly that, saw every bagged item as surplus, and ran a removal
// for it against the wrong Inventory on every single apply.
// External linkage (EngineInternal.h): the world TU's vendor probe shares it.
unsigned int readInvItems(Inventory* inv, InvItemEntry* out, Item** outItems,
                          unsigned int maxOut, bool* outTruncated, bool includeNested) {
    if (outTruncated) *outTruncated = false;
    if (!inv || !out || maxOut == 0) return 0;
    unsigned int n = 0;
    bool trunc = false;
    // Full staged snapshots include real merchant stock. Game-thread capture
    // never recurses, so reuse a bounded physical-pointer mirror.
    const unsigned int MIRROR_MAX = INV_SNAPSHOT_ITEMS_MAX;
    static Item* mine[INV_SNAPSHOT_ITEMS_MAX];
    // EQUIPPED gear (Phase 4a equipment sync). Worn items - EVERY equippable slot:
    // weapons, all armour pieces, belt, worn backpack, ... - live in the inventory's
    // equip SECTIONS, not the loose _allItems list. Walk every section and capture the
    // ones flagged isAnEquippedItemSection, tagging each item equipped=1 with the
    // section's slot. This covers all slots uniformly (the prior armour/weapon getters
    // silently missed the weapon holster, so weapon unequips never replicated).
    if (g_getSectionsFn) {
        __try {
            lektor<InventorySection*>* secs = g_getSectionsFn(inv);
            unsigned int ns = secs ? secs->size() : 0;
            for (unsigned int s = 0; s < ns; ++s) {
                InventorySection* sec = (*secs)[s];
                if (!sec || !sec->isAnEquippedItemSection) continue;
                const Ogre::vector<InventorySection::SectionItem>::type& its = sec->items;
                unsigned int ni = (unsigned int)its.size();
                for (unsigned int i = 0; i < ni; ++i) {
                    Item* it = its[i].item;
                    if (!itemPtrLive(it)) continue;
                    GameData* gd = it->getGameData();
                    if (!gd) continue;
                    if (n >= maxOut) { trunc = true; break; }
                    memset(&out[n], 0, sizeof(InvItemEntry));
                    const char* sid = gd->stringID.c_str();
                    strncpy(out[n].stringID, sid ? sid : "", sizeof(out[n].stringID) - 1);
                    out[n].itemType = (unsigned int)gd->type;
                    int q = it->quantity; if (q < 1) q = 1;
                    out[n].quantity = (q > 65535) ? (unsigned short)65535 : (unsigned short)q;
                    out[n].quality = qualityBucketOf(it->quality);
                    out[n].level    = gradeLevelOf(it);
                    out[n].equipped = 1;
                    out[n].slot     = (unsigned char)((unsigned int)sec->limitedSlot & 0xFFu);
                    // Phase 6b: locked shackle bit on the equipped LockedArmour.
                    { LockedArmour* la = it->isLockedArmour(); out[n].locked = (la && la->lock) ? 1 : 0; }
                    // Carry the SECTION identity so the two weapon slots ('hip' vs
                    // 'back', both ATTACH_WEAPON) replicate to the SAME slot on the peer.
                    out[n].section  = sectionNameHash(sec->name.c_str());
                    fillItemProvenance(it, out[n]); // weapon manufacturer/material
                    if (outItems) outItems[n] = it;
                    if (n < MIRROR_MAX) mine[n] = it;
                    ++n;
                }
                if (trunc) break;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            if (outTruncated) *outTruncated = true; // partial read: not a full description
            return n;
        }
    }
    // Worn WEAPONS may not live in any section: a HELD/primary weapon is tracked only by
    // getPrimaryWeapon()/getSecondaryWeapon(). A sheathed weapon often ALSO appears in a
    // 'hip'/'back' equip section (captured above), so dedup by (stringID, itemType) below.
    // Capturing both pointers guarantees equipping/unequipping ANY weapon replicates -
    // without this a re-equipped weapon (routed to the held slot, which is in no section)
    // disappears from the snapshot and the peer never sees it come back.
    if (g_getPrimaryWeaponFn || g_getSecondaryWeaponFn) {
        __try {
            Item* wpns[2];
            wpns[0] = g_getPrimaryWeaponFn ? g_getPrimaryWeaponFn(inv) : 0;
            wpns[1] = g_getSecondaryWeaponFn ? g_getSecondaryWeaponFn(inv) : 0;
            for (int w = 0; w < 2; ++w) {
                Item* it = wpns[w];
                if (!itemPtrLive(it)) continue;
                GameData* gd = it->getGameData();
                if (!gd) continue;
                const char* sid = gd->stringID.c_str();
                unsigned int wtype = (unsigned int)gd->type;
                // A section and a held-slot getter may expose the SAME physical
                // weapon. Distinct weapons of one template are never duplicates.
                bool dup=false;
                for (unsigned int j=0;j<n && j<MIRROR_MAX;++j)
                    if (mine[j]==it) { dup=true; break; }
                if (dup) continue;
                if (n >= maxOut) { trunc = true; break; }
                memset(&out[n], 0, sizeof(InvItemEntry));
                strncpy(out[n].stringID, sid ? sid : "", sizeof(out[n].stringID) - 1);
                out[n].itemType = (unsigned int)gd->type;
                int q = it->quantity; if (q < 1) q = 1;
                out[n].quantity = (q > 65535) ? (unsigned short)65535 : (unsigned short)q;
                out[n].quality = qualityBucketOf(it->quality);
                out[n].level    = gradeLevelOf(it);
                out[n].equipped = 1;
                out[n].slot     = (unsigned char)((unsigned int)it->slotType & 0xFFu);
                // Native slot context survives drawing; use the same actual
                // section identity captured by the pre-drop native hook.
                out[n].section=sectionNameHash(it->inventorySection.c_str());
                fillItemProvenance(it, out[n]); // weapon manufacturer/material (required to recreate)
                if (outItems) outItems[n] = it;
                if (n < MIRROR_MAX) mine[n] = it;
                ++n;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            if (outTruncated) *outTruncated = true;
            return n;
        }
    }
    // LOOSE items last: whatever cap remains after the irreplaceable worn kit.
    __try {
        lektor<Item*>& all = inv->_allItems;
        unsigned int total = all.size();
        for (unsigned int i = 0; i < total; ++i) {
            Item* it = all[i];
            if (!itemPtrLive(it)) continue;
            GameData* gd = it->getGameData();
            if (!gd) continue;
            if (it->isEquipped) continue; // worn gear was captured above, from equip sections
            if (n >= maxOut) { trunc = true; break; }
            memset(&out[n], 0, sizeof(InvItemEntry));
            const char* sid = gd->stringID.c_str();
            strncpy(out[n].stringID, sid ? sid : "", sizeof(out[n].stringID) - 1);
            out[n].itemType = (unsigned int)gd->type;
            int q = it->quantity; if (q < 1) q = 1;
            out[n].quantity = (q > 65535) ? (unsigned short)65535 : (unsigned short)q;
            out[n].quality = qualityBucketOf(it->quality);
            out[n].level    = gradeLevelOf(it);
            out[n].equipped = 0;
            out[n].slot     = (unsigned char)((unsigned int)it->slotType & 0xFFu);
            out[n].section  = 0; // loose: no equip section
            // Phase 6b: locked shackle bit (LockedArmour with a live lock). Direct
            // virtual dispatch (like getGameData above) discriminates the item; the
            // outer __try covers a fault.
            { LockedArmour* la = it->isLockedArmour(); out[n].locked = (la && la->lock) ? 1 : 0; }
            fillItemProvenance(it, out[n]); // weapon manufacturer/material (empty otherwise)
            if (outItems) outItems[n] = it;
            if (n < MIRROR_MAX) mine[n] = it;
            ++n;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (outTruncated) *outTruncated = true;
        return n;
    }
    // NESTED CONTAINERS last (protocol 48). A worn backpack owns a PRIVATE Inventory - its
    // contents are in no section of the wearer's own inventory, so everything above missed
    // them entirely and a bagged item was described by no snapshot at all. Each nested item is
    // appended with parentIdx = <its bag's index> + 1 so the peer can put it back INSIDE the
    // right bag rather than merely somewhere on the character.
    //
    // One level only, and deliberately: Kenshi does not nest bags inside bags, and a bounded
    // walk cannot be talked into recursing forever by a malformed object graph. The parent
    // items themselves were captured above, so this scans a snapshot of the count taken BEFORE
    // appending - `nTop` - or it would walk its own output.
    const unsigned int nTop = (n < MIRROR_MAX) ? n : MIRROR_MAX;
    if (includeNested && g_getSectionsFn) {
        // Nested contents come LAST, so on a full inventory they are the part the cap cuts -
        // and then a bag's contents replicate not at all, which looks exactly like the bug this
        // capture exists to fix. Truncation itself is handled (the reconcile degrades to
        // additive-only); say it once per session so it is not a mystery.
        if (n >= maxOut) {
            static bool told = false;
            if (!told) {
                told = true;
                char b[220]; _snprintf(b, sizeof(b) - 1,
                    "[inv] NESTED-CAPPED: the top level already filled the %u-entry snapshot, so "
                    "no container contents fit. Bagged items will not replicate for this "
                    "character until it carries less.", maxOut);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
        }
        for (unsigned int p = 0; p < nTop && !trunc; ++p) {
            Item* parent = mine[p];
            if (!parent) continue;
            __try {
                Inventory* sub = parent->getInventory();
                if (!sub || sub == inv) continue; // not a container item
                lektor<InventorySection*>* subSecs = g_getSectionsFn(sub);
                unsigned int nss = subSecs ? subSecs->size() : 0;
                for (unsigned int s = 0; s < nss && !trunc; ++s) {
                    InventorySection* sec = (*subSecs)[s];
                    if (!sec) continue;
                    const Ogre::vector<InventorySection::SectionItem>::type& its = sec->items;
                    unsigned int ni = (unsigned int)its.size();
                    for (unsigned int i = 0; i < ni; ++i) {
                        Item* it = its[i].item;
                        if (!it) continue;
                        GameData* gd = it->getGameData();
                        if (!gd) continue;
                        if (n >= maxOut) { trunc = true; break; }
                        memset(&out[n], 0, sizeof(InvItemEntry));
                        const char* sid = gd->stringID.c_str();
                        strncpy(out[n].stringID, sid ? sid : "", sizeof(out[n].stringID) - 1);
                        out[n].itemType = (unsigned int)gd->type;
                        int q = it->quantity; if (q < 1) q = 1;
                        out[n].quantity = (q > 65535) ? (unsigned short)65535 : (unsigned short)q;
                        out[n].quality = qualityBucketOf(it->quality);
                        out[n].level     = gradeLevelOf(it);
                        out[n].equipped  = 0;
                        out[n].slot      = (unsigned char)((unsigned int)it->slotType & 0xFFu);
                        out[n].section   = 0;
                        out[n].parentIdx = (unsigned short)(p + 1);
                        fillItemProvenance(it, out[n]);
                        if (outItems) outItems[n] = it;
                        ++n;
                    }
                    if (trunc) break;
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                trunc = true; // a bag we could not finish reading is an incomplete description
            }
        }
    }
    if (outTruncated) *outTruncated = trunc;
    return n;
}

// SEH-guarded: find an item template by stringID within its itemType category.
// External linkage (EngineInternal.h): the spawn/combat TU's limb-item mint shares it.
GameData* findItemTemplateImpl(GameWorld* gw, const char* sid, unsigned int typeCat) {
    if (!gw || !g_getDataOfTypeFn || !sid || !sid[0]) return 0;
    __try {
        g_dataScratch.clear();
        g_getDataOfTypeFn(&gw->gamedata, &g_dataScratch, (itemType)typeCat);
        unsigned int n = g_dataScratch.size();
        for (unsigned int i = 0; i < n; ++i) {
            GameData* gd = g_dataScratch[i];
            if (gd && strcmp(gd->stringID.c_str(), sid) == 0) return gd;
        }
        static int dbg = -1;
        if (dbg < 0) { const char* e = getenv("KENSHICOOP_INV_DUMP"); dbg = (e && e[0] == '1') ? 1 : 0; }
        if (dbg) {
            char b[200];
            _snprintf(b, sizeof(b) - 1, "[tmpl] MISS sid='%s' type=%u scanned=%u sample0='%s'",
                sid, typeCat, n, (n > 0 && g_dataScratch[0]) ? g_dataScratch[0]->stringID.c_str() : "(none)");
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return 0;
}

// The material spec is a WEAPON-ONLY argument, and MEASURED to be so. Because
// fillItemProvenance captures materialData for every item type while this lookup only ever
// searched MATERIAL_SPECS_WEAPON, it looked like armour was being rebuilt material-less by
// a category bug. Resolving armour materials under MATERIAL_SPECS_CLOTHING / MATERIAL_SPEC
// and passing them here does resolve them - and produces an item for which isGear() returns
// NULL, i.e. not gear at all, so the rebuilt piece has no craft level, no resistances and
// cannot be worn. The same call with a null material and an explicit level yields a proper
// Gear (that is how mintGradedGearForTest works). So this slot is not "the material" in the
// non-weapon shape, null is the correct value for it, and there was no category bug.
static GameData* findMaterialSpec(GameWorld* gw, const char* sid, unsigned int typeCat) {
    if (!sid || !sid[0]) return 0;
    if ((itemType)typeCat != WEAPON) return 0;
    return findItemTemplateImpl(gw, sid, (unsigned int)MATERIAL_SPECS_WEAPON);
}

// SEH-guarded: a generic WEAPON_MANUFACTURER GameData for weapon fabrication when the
// wire carried no manufacturer sid (spike 451: the manufacturer record is the REQUIRED
// first arg of a weapon createItem - without one the weapon cannot fabricate at all,
// so a generic maker beats a lost weapon). First enumerated record, cached per session.
// External linkage (Phase 5e): declared in EngineInternal.h so the weapon-mint probes
// (now in EngineProbe.cpp) can reuse it.
GameData* fallbackWeaponManufacturer(GameWorld* gw) {
    static GameData* cached = 0;
    if (cached) return cached;
    if (!gw || !g_getDataOfTypeFn) return 0;
    __try {
        g_dataScratch.clear();
        g_getDataOfTypeFn(&gw->gamedata, &g_dataScratch, WEAPON_MANUFACTURER);
        if (g_dataScratch.size() > 0) cached = g_dataScratch[0];
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return cached;
}

typedef GameData* (__fastcall* RecordBySidFn)(GameDataContainer*, const std::string&);
static GameData* recordBySidSeh(GameWorld* gw, const std::string& sid, RecordBySidFn get) {
    __try { return gw && get ? get(&gw->gamedata,sid) : 0; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static GameData* recordBySid(GameWorld* gw, const char* sid) {
    if (!sid || !sid[0]) return 0;
    static RecordBySidFn get=(RecordBySidFn)KenshiLib::GetRealAddress(
        static_cast<GameData* (GameDataContainer::*)(const std::string&)>(&GameDataContainer::getData));
    return recordBySidSeh(gw,std::string(sid),get);
}


// Native by-value hand construction needs C++ unwinding outside the SEH caller.
static Item* constructExactSword(GameData* base, GameData* maker, GameData* material,
                                 const hand& handle, int level) {
    return new Sword(base, maker, material, handle, level);
}
// createItem/the exact Sword constructor accept a detached, empty hand.
// Inventory placement does not register it. copyItem is the SDK's native
// creation path that also allocates a live item handle; keep its prototype
// detached and scoped, rather than inserting an unresolvable item into stock.
static Item* registerFabricatedItem(RootObjectFactory* factory,Item* item) {
    std::auto_ptr<Item> prototype(item);
    unsigned int actual[5];
    if (readObjectHand(item,actual) && resolveObjectByHand(actual)==item)
        return prototype.release();
    return factory->copyItem(prototype.get());
}
// Create registered native items off-inventory so layout can be validated before mutation.
static Item* fabricateItem(GameWorld* gw, const char* sid, unsigned int typeCat,
                           int qualityBucket, const char* manufacturer,
                           const char* material, unsigned char level) {
    if (!gw || !gw->theFactory || !g_createItemFn || !sid) return 0;
    static int dbg = -1;
    if (dbg < 0) { const char* e = getenv("KENSHICOOP_INV_DUMP"); dbg = (e && e[0] == '1') ? 1 : 0; }
    __try {
        GameData* tmpl = findItemTemplateImpl(gw, sid, typeCat);
        if (!tmpl) { if (dbg) coop::logLine("[mk] tmpl-null"); return 0; }
        // Blueprint research and ordinary trade-item provenance use these same
        // native fields; they are not restricted to weapon manufacturers/materials.
        GameData* man=recordBySid(gw,manufacturer);
        GameData* mat=recordBySid(gw,material);
        if ((manufacturer && manufacturer[0] && !man) ||
            (material && material[0] && !mat)) return 0;
        // Condition and native craft level are independent. The procedural weapon
        // factory selects level from the manufacturer's material/model list and
        // ignores levelOverride in its weapon branch. For a known wire level,
        // construct the native Sword directly with the Ogre allocator. Its
        // detached handle is registered through the native copy path below.
        // Unknown levels retain native procedural generation.
        static int lvlOn = -1;
        if (lvlOn < 0) { const char* e = getenv("KENSHICOOP_GEAR_LEVEL"); lvlOn = (e && e[0] == '0') ? 0 : 1; }
        int gradeLevel = -1; // -1 = "no wire grade": use the per-branch engine default
        if (lvlOn && level != GRADE_NA) gradeLevel = (int)level;
        Item* it = 0;
        if ((itemType)typeCat == WEAPON) {
            // KENSHICOOP_WEAPON_FAB=0: escape hatch back to the pre-spike-451 behaviour
            // (weapons never fabricate - conservation-only gear sync). Covers EVERY
            // weapon-fabrication site at once: reconcile CREATE, xfer shortfall, probes.
            static int fabOn = -1;
            if (fabOn < 0) { const char* e = getenv("KENSHICOOP_WEAPON_FAB"); fabOn = (e && e[0] == '0') ? 0 : 1; }
            if (!fabOn) {
                if (dbg) coop::logLine("[mk] weapon-fab disabled (KENSHICOOP_WEAPON_FAB=0)");
                return 0;
            }
            // The procedural weapon factory takes manufacturer FIRST and template
            // THIRD. An exact native constructor instead takes template FIRST.
            if (!man) man = fallbackWeaponManufacturer(gw);
            if (man) {
                char wb[sizeof(hand) + 16];
                memset(wb, 0, sizeof(wb));
                hand* wh = reinterpret_cast<hand*>(wb);
                g_handCtorFn(wh, 0, 0, NULL_ITEM, 0, 0);
                if (gradeLevel >= 0)
                    it = constructExactSword(tmpl, man, mat, *wh, gradeLevel);
                else
                    it = g_createItemFn(gw->theFactory, man, wh, tmpl, mat, 0, 0);
            } else if (dbg) {
                coop::logLine("[mk] weapon manufacturer unresolved (no provenance, no fallback)");
            }
        } else {
            char buf[sizeof(hand) + 16];
            memset(buf, 0, sizeof(buf));
            hand* h = reinterpret_cast<hand*>(buf);
            g_handCtorFn(h, 0, 0, (itemType)typeCat, 0, 0);
            // Native factory makes a BlueprintItem from RESEARCH, not from
            // the shared BLUEPRINT_ITEM display template.
            GameData* recipe=(man && man->type==RESEARCH) ? man : tmpl;
            it = g_createItemFn(gw->theFactory, recipe, h, 0, 0, gradeLevel, 0);
            if (it) {
                // Null means native fabrication without a supplied snapshot.
                // An explicit wire string (including empty) restores provenance.
                if (manufacturer) it->manufacturerData=man;
                if (material) it->materialData=mat;
            }
        }
        if (!it) { if (dbg) { char b[140]; _snprintf(b,sizeof(b)-1,"[mk] createItem-null sid='%s' type=%u man=%d mat=%d",sid,typeCat,man?1:0,mat?1:0); b[sizeof(b)-1]='\0'; coop::logLine(b);} return 0; }
        if (qualityBucket > 0) it->quality = (float)qualityBucket / 100.0f;
        return registerFabricatedItem(gw->theFactory,it);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (dbg) coop::logLine("[mk] SEH-except");
        return 0;
    }
}
Item* fabricateEnvironmentItem(GameWorld* gw,const InvItemEntry& item) {
    return fabricateItem(gw,item.stringID,item.itemType,item.quality,
                         item.manufacturer,item.material,item.level);
}

bool createItemAndAdd(GameWorld* gw, Inventory* inv, const char* sid,
                      unsigned int typeCat, int qty, int qualityBucket, bool equip,
                      const char* manufacturer, const char* material,
                      unsigned char level) {
    if (!inv || qty<=0) return false;
    Item* it=fabricateItem(gw,sid,typeCat,qualityBucket,manufacturer,material,level);
    if (!it) return false;
    __try {
        if (!inv->tryAddItem(it,qty)) return false;
        if (equip && g_equipItemFn) g_equipItemFn(inv,it);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

namespace {

// SEH-guarded: remove up to `qty` units of (sid, typeCat, equipped) by walking the
// captured stacks. removeItemAutoDestroy frees the stack, so items[i] is never
// touched again. `wantEquipped` keeps loose and worn copies of the same item distinct
// (so unequipping/dropping a worn item doesn't accidentally remove the loose one).
int removeByKey(Inventory* inv, Item** items, InvItemEntry* meta, unsigned int n,
                const char* sid, unsigned int typeCat, const char* manufacturer,
                const char* material, int qty, int wantEquipped) {
    if (!inv || !items || !meta || !sid || qty <= 0) return 0;
    int removed = 0;
    __try {
        for (unsigned int i = 0; i < n && removed < qty; ++i) {
            if (!itemPtrLive(items[i])) continue;
            if (meta[i].itemType != typeCat) continue;
            if ((int)meta[i].equipped != wantEquipped) continue;
            if (strcmp(meta[i].stringID, sid) != 0) continue;
            if ((manufacturer && strcmp(meta[i].manufacturer,manufacturer)!=0) ||
                (material && strcmp(meta[i].material,material)!=0)) continue;
            int have = meta[i].quantity; if (have < 1) have = 1;
            int take = qty - removed; if (take > have) take = have;
            inv->removeItemAutoDestroy(items[i], take); // virtual; destroys the stack
            removed += take;
            meta[i].quantity-=take;
            if (meta[i].quantity==0) items[i]=0;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return removed;
    }
    return removed;
}

// SEH-guarded: EQUIP an item the container ALREADY holds loose (move loose -> slot).
// Unlike createItemAndAdd(equip=true) - which fabricates a blank-handle item that the
// engine discards within a tick (d25) - this equips a REAL, already-resolved item, so it
// PERSISTS. This is what makes the re-equip (up) path replicate. Returns 1 on success.
int equipExisting(Inventory* inv, Item* it) {
    if (!inv || !it || !g_equipItemFn) return 0;
    __try {
        return g_equipItemFn(inv, it) ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// SEH-guarded: UNEQUIP a worn item back to loose inventory (move slot -> loose) WITHOUT
// destroying it, so its identity/quality survive for a later re-equip. removeItemDontDestroy
// detaches it from its equip section; then we place it into a LOOSE (non-equip) storage
// section DIRECTLY - the inventory-level add (tryAddItem) auto-routes an equippable item
// straight back into its slot (re-equipping the thing we just took off), so we must target
// a loose section's own addItem to keep it off. Returns 1 on success.
int unequipToLoose(Inventory* inv, Item* it) {
    if (!inv || !it) return 0;
    __try {
        Item* taken = inv->removeItemDontDestroy_returnsItem(it, 1, false); // virtual
        if (!taken) return 0;
        GameData* gd = taken->getGameData();
        if (g_getSectionsFn) {
            lektor<InventorySection*>* secs = g_getSectionsFn(inv);
            unsigned int ns = secs ? secs->size() : 0;
            for (unsigned int s = 0; s < ns; ++s) {
                InventorySection* sec = (*secs)[s];
                if (!sec || sec->isAnEquippedItemSection || sec->containerSlot) continue;
                if (gd && !sec->hasRoomForItem(gd, 1)) continue;  // virtual
                if (sec->addItem(taken, 1)) return 1;             // virtual; loose placement
            }
        }
        // Fallback: generic add (may re-equip, but never leak the item).
        return inv->tryAddItem(taken, 1) ? 1 : 0;                 // virtual
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// SEH-guarded: ensure a WORN weapon of (sid,type) sits in the weapon section whose name
// hashes to wantSection. The two weapon slots ('hip'=Weapon II, 'back'=Weapon I) share
// AttachSlot ATTACH_WEAPON, so equipItem auto-routes to a default slot - it cannot honour
// the author's slot choice. This moves the REAL worn item between the two weapon sections
// (detach + section-level addItem, the same direct placement unequipToLoose uses) so the
// peer mirrors Weapon I vs II. No-op unless wantSection names an EXISTING weapon section,
// the item is worn in a DIFFERENT weapon section, and the target slot doesn't already hold
// it. Armour is never touched (its sections are unique per slot, so they never mismatch).
// Returns 1 if it moved an item, else 0.
int correctWeaponSlot(Inventory* inv, const char* sid, unsigned int type,
                      unsigned short wantSection) {
    if (!inv || !sid || !sid[0] || wantSection == 0 || !g_getSectionsFn) return 0;
    __try {
        lektor<InventorySection*>* secs = g_getSectionsFn(inv);
        unsigned int ns = secs ? secs->size() : 0;
        InventorySection* target = 0;
        Item* srcItem = 0;
        for (unsigned int s = 0; s < ns; ++s) {
            InventorySection* sec = (*secs)[s];
            if (!sec || !sec->isAnEquippedItemSection) continue;
            if ((unsigned int)sec->limitedSlot != (unsigned int)ATTACH_WEAPON) continue;
            unsigned short h = sectionNameHash(sec->name.c_str());
            const Ogre::vector<InventorySection::SectionItem>::type& its = sec->items;
            unsigned int ni = (unsigned int)its.size();
            Item* found = 0;
            for (unsigned int i = 0; i < ni; ++i) {
                Item* it = its[i].item; if (!it) continue;
                GameData* gd = it->getGameData(); if (!gd) continue;
                if ((unsigned int)gd->type != type) continue;
                if (strcmp(gd->stringID.c_str(), sid) != 0) continue;
                found = it; break;
            }
            if (h == wantSection) {
                target = sec;
                if (found) return 0;            // already in the desired slot - nothing to do
            } else if (found && !srcItem) {
                srcItem = found;                // worn in the OTHER weapon slot - candidate to move
            }
        }
        if (!target || !srcItem) return 0;       // target slot unknown, or item not worn elsewhere
        Item* taken = inv->removeItemDontDestroy_returnsItem(srcItem, 1, false); // virtual
        if (!taken) return 0;
        if (target->addItem(taken, 1)) return 1;                  // virtual: direct slot placement
        // Targeted placement failed - re-equip generically so the weapon is never leaked.
        if (g_equipItemFn) g_equipItemFn(inv, taken);
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

} // namespace

bool containerInventoryAvailable(const unsigned int cHand[5]) {
    return transactionInventory(cHand)!=0;
}

unsigned int captureContainerContents(GameWorld* gw, const unsigned int cHand[5],
                                      InvItemEntry* out, unsigned int maxOut,
                                      unsigned int* outHash, bool* outTruncated,
                                      bool includeNested) {
    if (outHash) *outHash = 0;
    if (outTruncated) *outTruncated = false;
    if (!gw || !out || maxOut == 0) return 0;
    Inventory* inv = transactionInventory(cHand);
    if (!inv) return 0;
    unsigned int n = readInvItems(inv, out, 0, maxOut, outTruncated, includeNested);
    if (outHash) {
        unsigned int h = 0;
        for (unsigned int i = 0; i < n; ++i) h += invEntryHash(out[i]);
        *outHash = h; // 0 == empty container (a meaningful, distinct fingerprint)
    }
    return n;
}

static bool sameStockItem(const InvItemEntry& a, const InvItemEntry& b) {
    return a.itemType==b.itemType && a.quality==b.quality && a.level==b.level &&
        a.locked==b.locked && strcmp(a.stringID,b.stringID)==0 &&
        strcmp(a.manufacturer,b.manufacturer)==0 && strcmp(a.material,b.material)==0;
}

// Complete stock uses the host's physical stacks and positions, not greedy packing.
// Reuse native items and validate every rectangle before detaching anything.
static bool applyStockLayout(GameWorld* gw, Inventory* inv,
                             const InvItemEntry* desired, unsigned int count,
                             const InvItemEntry* cur, Item** curItems, unsigned int ncur) {
    if (count>INV_SNAPSHOT_ITEMS_MAX || ncur>INV_SNAPSHOT_ITEMS_MAX) return false;
    static Item* placed[INV_SNAPSHOT_ITEMS_MAX];
    static InventorySection* sections[INV_SNAPSHOT_ITEMS_MAX];
    static bool reused[INV_SNAPSHOT_ITEMS_MAX],used[INV_SNAPSHOT_ITEMS_MAX];
    memset(placed,0,count*sizeof(placed[0])); memset(sections,0,count*sizeof(sections[0]));
    memset(reused,0,count*sizeof(reused[0])); memset(used,0,ncur*sizeof(used[0]));
    bool changed=count!=ncur;
    bool valid=true;
    // Snapshot relocation is not a mouse drag or a new transfer intent.
    const bool vetoSaved=g_invVetoSuspend;
    g_invVetoSuspend=true;
    __try {
        lektor<InventorySection*>* all=g_getSectionsFn(inv);
        for (unsigned int i=0; i<count && valid; ++i) {
            const InvItemEntry& e=desired[i];
            if (e.equipped || !e.gridSection || !e.quantity) { valid=false; break; }
            for (unsigned int s=0; all && s<all->size(); ++s) {
                InventorySection* sec=(*all)[s];
                if (sec && !sec->isAnEquippedItemSection &&
                    sectionNameHash(sec->name.c_str())==e.gridSection) {
                    sections[i]=sec; break;
                }
            }
            if (!sections[i]) { valid=false; break; }
            for (unsigned int j=0; j<ncur; ++j) {
                if (!used[j] && !cur[j].equipped && sameStockItem(e,cur[j])) {
                    placed[i]=curItems[j]; reused[i]=true; used[j]=true;
                    if (e.quantity!=cur[j].quantity || e.gridSection!=cur[j].gridSection ||
                        e.gridX!=cur[j].gridX || e.gridY!=cur[j].gridY) changed=true;
                    break;
                }
            }
            if (!placed[i]) {
                changed=true;
                placed[i]=fabricateItem(gw,e.stringID,e.itemType,e.quality,
                                        e.manufacturer,e.material,e.level);
            }
            Item* it=placed[i];
            InventorySection* sec=sections[i];
            if (!it || it->itemWidth<=0 || it->itemHeight<=0 ||
                e.gridX+it->itemWidth>sec->width || e.gridY+it->itemHeight>sec->height ||
                (e.quantity>1 && e.quantity>it->isStackable(sec))) { valid=false; break; }
            for (unsigned int j=0; j<i; ++j) {
                if (sections[j]!=sec) continue;
                if (e.gridX<desired[j].gridX+placed[j]->itemWidth &&
                    desired[j].gridX<e.gridX+it->itemWidth &&
                    e.gridY<desired[j].gridY+placed[j]->itemHeight &&
                    desired[j].gridY<e.gridY+it->itemHeight) { valid=false; break; }
            }
        }
        if (valid && changed) {
            for (unsigned int j=0; j<ncur; ++j) {
                if (used[j]) {
                    if (inv->removeItemDontDestroy_returnsItem(curItems[j],cur[j].quantity,false)
                        !=curItems[j]) { valid=false; break; }
                } else {
                    inv->removeItemAutoDestroy(curItems[j],cur[j].quantity);
                }
            }
            if (valid) for (unsigned int i=0; i<count; ++i) {
                placed[i]->quantity=desired[i].quantity;
                sections[i]->_addItem(placed[i],desired[i].gridX,desired[i].gridY);
                if (!inv->hasItem(placed[i])) { valid=false; break; }
                placed[i]=0; // native section callback owns it now
            }
            inventoryGuiRefresh(inv);
        }
        for (unsigned int i=0; i<count; ++i) {
            if (placed[i] && !reused[i])
                g_destroyObjFn(gw,placed[i],false,"coop-stock-staging");
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { valid=false; }
    g_invVetoSuspend=vetoSaved;
    if (!valid) coop::logLine("[stock] native layout could not be applied");
    return valid && changed;
}

// The reconcile itself, against ONE inventory. Split out from applyContainerContents so the
// same logic can be pointed at a nested container's private inventory (a worn backpack's
// 'backpack_content'), which is the only way a bagged item can be placed back INSIDE the bag
// rather than merely somewhere on the character. Nothing in here needs the owner's hand.
static bool reconcileInventory(GameWorld* gw, Inventory* inv,
                             const InvItemEntry* items, unsigned int count,
                             bool truncated) {
    if (!gw || !inv || count>INV_SNAPSHOT_ITEMS_MAX || (count && !items)) return false;

    // Snapshot current contents (with parallel Item* for removal). If OUR OWN read
    // overflowed, `cur` undercounts what we hold, so every apparent shortfall might be an
    // item we simply did not look at - creating against that mints duplicates. Treat a
    // truncated local read as "cannot create" the same way a truncated desired list means
    // "cannot delete": for a container neither side can fully describe, the reconcile
    // degrades to moves only rather than guessing in either direction.
    const unsigned int MAXC = INV_SNAPSHOT_ITEMS_MAX;
    static InvItemEntry cur[INV_SNAPSHOT_ITEMS_MAX];
    static Item* curItems[INV_SNAPSHOT_ITEMS_MAX];
    bool curTruncated = false;
    unsigned int ncur = readInvItems(inv, cur, curItems, MAXC, &curTruncated);
    if (inv->owner && inv->owner->getDataType()==BUILDING && !truncated &&
        !curTruncated && count<=INV_SNAPSHOT_ITEMS_MAX && g_getSectionsFn && g_destroyObjFn) {
        if (inventoryGuiOpen(inv) && !g_invGuiRefreshFn) return false;
        return applyStockLayout(gw,inv,items,count,cur,curItems,ncur);
    }

    // Group by template AND native provenance, tracking the EQUIPPED vs LOOSE
    // split separately. A change in the split while the group's TOTAL count is
    // unchanged is an in-place MOVE (equip/unequip an existing item): we transition the
    // REAL item rather than destroy+recreate, so (a) identity/quality survive and (b) the
    // equip PERSISTS - fabricated blank-handle equips are discarded by the engine (d25),
    // which is why the re-equip (up) path used to vanish. Only a genuine total-count
    // change creates (loose) or destroys items. Counts are tiny, so O(k^2) PODs is fine
    // (SEH-safe: engine mutation is confined to the equip/unequip/create/remove helpers).
    struct Grp {
        char sid[48]; unsigned int type;
        int desiredEq, desiredLoose, curEq, curLoose; int qualEq, qualLoose;
        // Craft grade per equip state (protocol 51), so a rebuilt piece is minted at the
        // author's grade instead of the factory default. GRADE_NA = the author had none.
        unsigned char lvlEq, lvlLoose;
        char manufacturer[48]; char material[48]; // weapon provenance (needed to recreate)
    };
    static Grp g[INV_SNAPSHOT_ITEMS_MAX*2];
    unsigned int ng = 0;
    for (unsigned int i = 0; i < count; ++i) {
        unsigned int j = 0;
        for (; j < ng; ++j)
            if (g[j].type == items[i].itemType && strcmp(g[j].sid, items[i].stringID) == 0 &&
                strcmp(g[j].manufacturer,items[i].manufacturer)==0 &&
                strcmp(g[j].material,items[i].material)==0) break;
        if (j == ng && ng < INV_SNAPSHOT_ITEMS_MAX*2) {
            memset(&g[ng], 0, sizeof(Grp));
            g[ng].lvlEq = g[ng].lvlLoose = GRADE_NA; // 0 would mean "mint at level 0"
            strncpy(g[ng].sid, items[i].stringID, sizeof(g[ng].sid) - 1);
            g[ng].type = items[i].itemType; j = ng; ++ng;
        }
        if (j < ng) {
            int q = (items[i].quantity < 1) ? 1 : items[i].quantity;
            if (items[i].equipped) { g[j].desiredEq += q; g[j].qualEq = items[i].quality; g[j].lvlEq = items[i].level; }
            else                   { g[j].desiredLoose += q; g[j].qualLoose = items[i].quality; g[j].lvlLoose = items[i].level; }
            // Immutable provenance distinguishes otherwise identical templates,
            // including different research blueprints.
            if (!g[j].manufacturer[0] && items[i].manufacturer[0])
                strncpy(g[j].manufacturer, items[i].manufacturer, sizeof(g[j].manufacturer) - 1);
            if (!g[j].material[0] && items[i].material[0])
                strncpy(g[j].material, items[i].material, sizeof(g[j].material) - 1);
        }
    }
    for (unsigned int i = 0; i < ncur; ++i) {
        unsigned int j = 0;
        for (; j < ng; ++j)
            if (g[j].type == cur[i].itemType && strcmp(g[j].sid, cur[i].stringID) == 0 &&
                strcmp(g[j].manufacturer,cur[i].manufacturer)==0 &&
                strcmp(g[j].material,cur[i].material)==0) break;
        if (j == ng && ng < INV_SNAPSHOT_ITEMS_MAX*2) {
            memset(&g[ng], 0, sizeof(Grp));
            g[ng].lvlEq = g[ng].lvlLoose = GRADE_NA; // 0 would mean "mint at level 0"
            strncpy(g[ng].sid, cur[i].stringID, sizeof(g[ng].sid) - 1);
            strncpy(g[ng].manufacturer,cur[i].manufacturer,sizeof(g[ng].manufacturer)-1);
            strncpy(g[ng].material,cur[i].material,sizeof(g[ng].material)-1);
            g[ng].type = cur[i].itemType; j = ng; ++ng;
        }
        if (j < ng) {
            int q = (cur[i].quantity < 1) ? 1 : cur[i].quantity;
            if (cur[i].equipped) g[j].curEq += q; else g[j].curLoose += q;
        }
    }

    static int dbg = -1;
    if (dbg < 0) { const char* e = getenv("KENSHICOOP_INV_DUMP"); dbg = (e && e[0] == '1') ? 1 : 0; }

    // Destroying a stack that an OPEN panel still lists is the loot crash (see
    // inventoryGuiOpen). Read the window state ONCE, up front: the loop below must
    // not change its mind about it half way through a container.
    const bool guiOpen = inventoryGuiOpen(inv);
    // Can the panel's icons be rebuilt after we mutate? If yes the reconcile runs
    // in FULL and loot changes show up live on the other screen; if not it stays
    // additive-only - which is what shipped from 0.1.2 and is exactly why taking
    // an item did not appear on the other side until the window was reopened.
    const bool canRefreshGui = guiOpen && (g_invGuiRefreshFn != 0);

    bool changed = false;
    for (unsigned int k = 0; k < ng; ++k) {
        if (dbg) {
            char b[200];
            _snprintf(b, sizeof(b) - 1,
                "[recon] grp type=%u sid='%s' desire(eq=%d,loose=%d) cur(eq=%d,loose=%d)",
                g[k].type, g[k].sid, g[k].desiredEq, g[k].desiredLoose, g[k].curEq, g[k].curLoose);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
        // 1) MOVE UP: equip an existing LOOSE copy when we need more worn AND have loose
        //    to spare. Equipping a real item persists (unlike a fabricated one).
        int upMoves;
        { int needEq = g[k].desiredEq - g[k].curEq, surpLoose = g[k].curLoose - g[k].desiredLoose;
          upMoves = (needEq < surpLoose) ? needEq : surpLoose; if (upMoves < 0) upMoves = 0; }
        for (int m = 0; m < upMoves; ++m) {
            int f = -1;
            for (unsigned int i = 0; i < ncur; ++i) {
                if (!curItems[i] || cur[i].equipped || cur[i].itemType != g[k].type) continue;
                if (strcmp(cur[i].stringID, g[k].sid) != 0) continue;
                if (strcmp(cur[i].manufacturer,g[k].manufacturer)!=0 ||
                    strcmp(cur[i].material,g[k].material)!=0) continue;
                f = (int)i; break;
            }
            int ok = (f < 0) ? -1 : equipExisting(inv, curItems[f]);
            if (dbg) { char b[96]; _snprintf(b, sizeof(b)-1, "[recon]   MOVE-UP f=%d ok=%d", f, ok); b[sizeof(b)-1]='\0'; coop::logLine(b); }
            if (f < 0 || !ok) break;
            curItems[f] = 0;                 // consumed: now worn, not loose
            g[k].curLoose--; g[k].curEq++; changed = true;
        }
        // 2) MOVE DOWN: unequip an existing WORN copy to loose (preserve it) when we need
        //    more loose AND have worn to spare.
        int downMoves;
        { int needLoose = g[k].desiredLoose - g[k].curLoose, surpEq = g[k].curEq - g[k].desiredEq;
          downMoves = (needLoose < surpEq) ? needLoose : surpEq; if (downMoves < 0) downMoves = 0; }
        for (int m = 0; m < downMoves; ++m) {
            int f = -1;
            for (unsigned int i = 0; i < ncur; ++i) {
                if (!curItems[i] || !cur[i].equipped || cur[i].itemType != g[k].type) continue;
                if (strcmp(cur[i].stringID, g[k].sid) != 0) continue;
                if (strcmp(cur[i].manufacturer,g[k].manufacturer)!=0 ||
                    strcmp(cur[i].material,g[k].material)!=0) continue;
                f = (int)i; break;
            }
            int ok = (f < 0) ? -1 : unequipToLoose(inv, curItems[f]);
            if (dbg) { char b[96]; _snprintf(b, sizeof(b)-1, "[recon]   MOVE-DOWN f=%d ok=%d", f, ok); b[sizeof(b)-1]='\0'; coop::logLine(b); }
            if (f < 0 || !ok) break;
            curItems[f] = 0;                 // consumed: now loose, not worn
            g[k].curEq--; g[k].curLoose++; changed = true;
        }
        // 3) RESIDUAL REMOVE (genuine removals; destroy surplus). Moved items were nulled
        //    in curItems above, so removeByKey won't touch them.
        //    SKIPPED ENTIRELY when the desired list is TRUNCATED: an incomplete snapshot
        //    cannot distinguish "the author no longer has this" from "it didn't fit on the
        //    wire", and guessing wrong destroys the peer's items for good. Additive-only
        //    leaves the two containers diverged (recoverable) instead of losing gear.
        if (truncated || curTruncated) {
            if (dbg) { char b[180]; _snprintf(b, sizeof(b) - 1,
                "[recon]   REMOVE-SKIP (truncated desired=%d local=%d) sid='%s' surplusLoose=%d surplusEq=%d",
                truncated ? 1 : 0, curTruncated ? 1 : 0,
                g[k].sid, g[k].curLoose - g[k].desiredLoose, g[k].curEq - g[k].desiredEq);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b); }
            continue;
        }
        //    SKIPPED while a PANEL is open on this container. removeItemAutoDestroy frees a
        //    stack the window still holds an InventoryIcon for, and the engine repaints that
        //    icon on the RENDER thread - a use-after-free none of our __except frames are on
        //    the stack for. Callers that can retry (applyInventories defers the whole
        //    snapshot and re-runs it when the window closes) make this a delay; a caller that
        //    cannot degrades to additive-only, the same recoverable divergence a truncated
        //    snapshot already produces. Never destroy under a live window.
        if (guiOpen && !canRefreshGui &&
            (g[k].curLoose > g[k].desiredLoose || g[k].curEq > g[k].desiredEq)) {
            char b[200]; _snprintf(b, sizeof(b) - 1,
                "[recon]   REMOVE-SKIP-GUI sid='%s' surplusLoose=%d surplusEq=%d (panel open, "
                "no refresh lever; destroying under it is the loot UAF)",
                g[k].sid, g[k].curLoose - g[k].desiredLoose, g[k].curEq - g[k].desiredEq);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            continue;
        }
        if (g[k].curLoose > g[k].desiredLoose) {
            int r = removeByKey(inv, curItems, cur, ncur, g[k].sid, g[k].type,
                            g[k].manufacturer,g[k].material,g[k].curLoose - g[k].desiredLoose, 0);
            if (dbg) { char b[96]; _snprintf(b, sizeof(b)-1, "[recon]   REMOVE-LOOSE n=%d got=%d", g[k].curLoose-g[k].desiredLoose, r); b[sizeof(b)-1]='\0'; coop::logLine(b); }
            if (r > 0) changed = true;
        }
        if (g[k].curEq > g[k].desiredEq) {
            int r = removeByKey(inv, curItems, cur, ncur, g[k].sid, g[k].type,
                            g[k].manufacturer,g[k].material,g[k].curEq - g[k].desiredEq, 1);
            if (dbg) { char b[96]; _snprintf(b, sizeof(b)-1, "[recon]   REMOVE-EQ n=%d got=%d", g[k].curEq-g[k].desiredEq, r); b[sizeof(b)-1]='\0'; coop::logLine(b); }
            if (r > 0) changed = true;
        }
    }
    for (unsigned int k = 0; k < ng; ++k) {
        // 4) RESIDUAL CREATE (genuine additions). ALWAYS create LOOSE - a fabricated loose
        //    item persists, but a fabricated-AND-equipped one is discarded within a tick by
        //    the engine's equipment validation (d25; this is the "picked-up weapon flickers
        //    into slot 1 then vanishes" bug). So any EQUIP shortfall for which we have no
        //    real copy to MOVE-UP is created loose here and equipped on a LATER reconcile
        //    tick: by then the loose copy is a real, established factory item, and MOVE-UP's
        //    equipExisting moves it into its slot persistently (the inv_reequip path).
        //    All removals have already run, so they cannot strip these new items.
        int createLoose = 0;
        if (g[k].desiredLoose > g[k].curLoose) createLoose += g[k].desiredLoose - g[k].curLoose;
        if (g[k].desiredEq    > g[k].curEq)    createLoose += g[k].desiredEq    - g[k].curEq;
        if (createLoose > 0 && curTruncated) {
            if (dbg) { char b[160]; _snprintf(b, sizeof(b) - 1,
                "[recon]   CREATE-SKIP (local truncated) sid='%s' shortfall=%d",
                g[k].sid, createLoose); b[sizeof(b) - 1] = '\0'; coop::logLine(b); }
            createLoose = 0;
        }
        // A complete snapshot includes the bag's children (or explicitly none).
        // applyContainerContents fills them after this top-level creation.
        // An incomplete snapshot cannot safely reconstruct a container.
        if (truncated && isContainerItemType(g[k].type)) createLoose = 0;
        if (createLoose > 0) {
            bool fromEq = (g[k].desiredEq > g[k].curEq);
            int qb = fromEq ? g[k].qualEq : g[k].qualLoose;
            unsigned char lv = fromEq ? g[k].lvlEq : g[k].lvlLoose;
            bool ok = createItemAndAdd(gw, inv, g[k].sid, g[k].type, createLoose, qb, false,
                                       g[k].manufacturer, g[k].material, lv);
            if (dbg) { char b[120]; _snprintf(b, sizeof(b)-1, "[recon]   CREATE-LOOSE n=%d (loose+eqDefer) ok=%d", createLoose, ok?1:0); b[sizeof(b)-1]='\0'; coop::logLine(b); }
            if (ok) changed = true;
        }
    }
    // SLOT-FIDELITY pass (weapons). The count reconcile above equips the right NUMBER of
    // each weapon, but equipItem auto-routes to a default weapon slot, so a weapon the
    // author wears in Weapon I ('back') can land in Weapon II ('hip') on the peer. Both
    // weapon sections share AttachSlot ATTACH_WEAPON (identical `slot`), so we steer by the
    // replicated SECTION hash and move each worn weapon into the slot the author chose.
    for (unsigned int i = 0; i < count; ++i) {
        if (!items[i].equipped || items[i].section == 0) continue;
        if (correctWeaponSlot(inv, items[i].stringID, items[i].itemType, items[i].section)) {
            changed = true;
            if (dbg) { char b[160]; _snprintf(b, sizeof(b)-1,
                "[recon]   SLOT-MOVE sid='%s' -> section=%u", items[i].stringID, items[i].section);
                b[sizeof(b)-1]='\0'; coop::logLine(b); }
        }
    }
    // Rebuild the open panel's icons BEFORE returning to the engine. Every Item*
    // we may have destroyed above is still referenced by an icon until this runs,
    // and the next repaint would walk it on the render thread - the fault that
    // made 0.1.2 defer the whole apply while a window was open. Doing it here, in
    // the same call, means there is no moment where a stale icon is reachable, so
    // loot can change live on both screens instead of only after a reopen.
    if (changed && canRefreshGui) inventoryGuiRefresh(inv);
    return changed;
}

static bool applyToInventory(GameWorld* gw, Inventory* inv,
                             const InvItemEntry* items, unsigned int count,
                             bool truncated) {
    // A native pickup has already detached/split stock but has not chosen its
    // destination. Reconciliation here would repopulate that stock under it.
    if (nativeInventoryCursorActive()) return false;
    // Snapshot moves are not cursor moves. Section-level adds bypass the
    // inventory add hook, so their removals must never enter cursor capture.
    const bool saved = g_invVetoSuspend;
    g_invVetoSuspend = true;
    __try {
        return reconcileInventory(gw, inv, items, count, truncated);
    } __finally {
        g_invVetoSuspend = saved;
    }
}

namespace {
// The private Inventory of the CONTAINER item held at cHand at position `which` in carry
// order (0 = the first one), or 0 when the character carries fewer than which+1 containers.
// The read is top-level by default, so every entry examined here is genuinely this
// character's own - a nested entry would describe a bag's contents, not a bag.
// Caller holds SEH via the public wrappers below.
Inventory* nestedInventoryOf(GameWorld* gw, const unsigned int cHand[5], unsigned int which) {
    if (!gw) return 0;
    RootObject* ro = resolveObjectByHand(cHand);
    if (!ro) return 0;
    Inventory* inv = invOf(ro);
    if (!inv) return 0;
    InvItemEntry cur[INV_ITEMS_MAX];
    Item* curItems[INV_ITEMS_MAX];
    unsigned int n = readInvItems(inv, cur, curItems, INV_ITEMS_MAX, 0);
    unsigned int seen = 0;
    for (unsigned int i = 0; i < n; ++i) {
        if (!curItems[i]) continue;
        Inventory* sub = 0;
        __try { sub = curItems[i]->getInventory(); } __except (EXCEPTION_EXECUTE_HANDLER) { sub = 0; }
        if (!sub || sub == inv) continue;
        if (seen == which) return sub;
        ++seen;
    }
    return 0;
}
} // namespace

// SEH-guarded: how many CONTAINERS the character at cHand carries (worn or loose). The
// two-bag gate needs this to tell "the second bag is not here" apart from "it is here and
// empty" - and same-template bags are exactly the case the apply side can confuse.
int nestedContainerCount(GameWorld* gw, const unsigned int cHand[5]) {
    if (!gw) return -1;
    int total = -1;
    __try {
        RootObject* ro = resolveObjectByHand(cHand);
        if (!ro) return -1;
        Inventory* inv = invOf(ro);
        if (!inv) return -1;
        InvItemEntry cur[INV_ITEMS_MAX];
        Item* curItems[INV_ITEMS_MAX];
        unsigned int n = readInvItems(inv, cur, curItems, INV_ITEMS_MAX, 0);
        total = 0;
        for (unsigned int i = 0; i < n; ++i) {
            if (!curItems[i]) continue;
            Inventory* sub = 0;
            __try { sub = curItems[i]->getInventory(); } __except (EXCEPTION_EXECUTE_HANDLER) { sub = 0; }
            if (sub && sub != inv) ++total;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { total = -1; }
    return total;
}

// SEH-guarded: put `qty` of (sid,type) INSIDE the container the character at cHand carries
// (its worn backpack's private inventory), not merely into the character's own grid. The
// nested-contents gate needs to create exactly the state a player creates by dragging
// something into their bag, and no other primitive can reach that inventory. `which` selects
// among several carried containers in carry order.
int addItemToNestedContainer(GameWorld* gw, const unsigned int cHand[5], const char* sid,
                             unsigned int typeCat, int qty, unsigned int which) {
    if (!gw || !sid || !sid[0] || qty <= 0) return 0;
    int added = 0;
    __try {
        Inventory* sub = nestedInventoryOf(gw, cHand, which);
        if (!sub) return 0;
        if (createItemAndAdd(gw, sub, sid, typeCat, qty, 0, /*equip=*/false)) added = qty;
    } __except (EXCEPTION_EXECUTE_HANDLER) { added = 0; }
    return added;
}

// SEH-guarded: drop `qty` of (sid,type) out of a CARRIED CONTAINER straight onto the ground,
// searching every container the character holds in carry order. This is the reach that
// dropItemFromInventory deliberately lacks - it reads top-level only, because a bagged item
// belongs to a different inventory and must not be counted as this character's loose kit.
//
// The drop MIRROR needs that reach anyway: a peer who hoovers a pile of loot into one character
// overflows the grid, so the engine puts the tail inside the worn backpack. When that character
// later drops such an item, a top-level-only search finds nothing. The former reconstruction
// fallback then minted a duplicate while the saved object remained in the bag.
// Relocating out of the bag conserves the object instead.
int dropItemFromNestedContainer(GameWorld* gw, const unsigned int cHand[5], const char* sid,
                                unsigned int typeCat, int qty, void** outLastDropped) {
    if (outLastDropped) *outLastDropped = 0;
    if (!gw || !sid || !sid[0] || qty <= 0) return 0;
    int dropped = 0;
    __try {
        int bags = nestedContainerCount(gw, cHand);
        for (int which = 0; which >= 0 && which < bags && dropped < qty; ++which) {
            for (;;) {
                // Re-resolve each iteration: dropItem mutates the sub-inventory's item list,
                // so a cached Item* (or Inventory*) could dangle.
                Inventory* sub = nestedInventoryOf(gw, cHand, (unsigned int)which);
                if (!sub) break;
                InvItemEntry cur[INV_ITEMS_MAX];
                Item* curItems[INV_ITEMS_MAX];
                unsigned int n = readInvItems(sub, cur, curItems, INV_ITEMS_MAX, 0);
                Item* victim = 0;
                for (unsigned int i = 0; i < n; ++i) {
                    if (cur[i].itemType != typeCat) continue;
                    if (strcmp(cur[i].stringID, sid) != 0) continue;
                    victim = curItems[i]; break;
                }
                if (!victim) break;
                sub->dropItem(victim);                          // virtual: bag -> ground
                if (outLastDropped) *outLastDropped = victim;    // the now-grounded object
                if (++dropped >= qty) break;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return dropped;
}

// SEH-guarded: how many of (sid,type) sit INSIDE the carried container `which` (summed
// quantities). -1 when the character holds no such container, so a gate can tell "the bag is
// not here" apart from "the bag is here and empty" - the difference between an unfinished
// relocation and lost contents.
int countInNestedContainer(GameWorld* gw, const unsigned int cHand[5], const char* sid,
                           unsigned int typeCat, unsigned int which) {
    if (!gw || !sid || !sid[0]) return -1;
    int total = -1;
    __try {
        Inventory* sub = nestedInventoryOf(gw, cHand, which);
        if (!sub) return -1;
        InvItemEntry ent[INV_ITEMS_MAX];
        unsigned int n = readInvItems(sub, ent, 0, INV_ITEMS_MAX, 0);
        total = 0;
        for (unsigned int i = 0; i < n; ++i) {
            if (typeCat != 0 && ent[i].itemType != typeCat) continue;
            if (strcmp(ent[i].stringID, sid) != 0) continue;
            int q = ent[i].quantity; if (q < 1) q = 1;
            total += q;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { total = -1; }
    return total;
}

static bool applyInventoryContents(GameWorld* gw, Inventory* inv,
                                   const InvItemEntry* items, unsigned int count,
                                   bool truncated) {
    if (!gw || !inv || count>INV_SNAPSHOT_ITEMS_MAX || (count && !items)) return false;
    if (nativeInventoryCursorActive()) return false;

    bool changed = false;
    __try {
    // Split the snapshot by its parent reference (protocol 48). The TOP level describes this
    // container; entries with parentIdx > 0 describe what is inside one of those items. They
    // must be reconciled separately or the two would fight: a bagged item counted at the top
    // level looks like a loose item the peer is missing, so the reconcile mints a second copy
    // out on the character and the bag stays empty.
    static InvItemEntry top[INV_SNAPSHOT_ITEMS_MAX];
    static unsigned short topSrcIdx[INV_SNAPSHOT_ITEMS_MAX];
    unsigned int nTop = 0;
    unsigned int nNested = 0;
    for (unsigned int i = 0; i < count && nTop < INV_SNAPSHOT_ITEMS_MAX; ++i) {
        if (items[i].parentIdx != 0) { ++nNested; continue; }
        topSrcIdx[nTop] = (unsigned short)i;
        top[nTop++] = items[i];
    }
    changed = applyToInventory(gw, inv, nTop ? top : 0, nTop, truncated);

    // Re-read AFTER the top-level pass: it may have created or moved the very bag we are about
    // to fill, and a bag identified from the stale read would be the wrong object (or gone).
    static InvItemEntry cur[INV_SNAPSHOT_ITEMS_MAX];
    static Item* curItems[INV_SNAPSHOT_ITEMS_MAX];
    unsigned int ncur = readInvItems(inv, cur, curItems, INV_SNAPSHOT_ITEMS_MAX, 0);

    static int dbg = -1;
    if (dbg < 0) { const char* e = getenv("KENSHICOOP_INV_DUMP"); dbg = (e && e[0] == '1') ? 1 : 0; }

    // Containers already reconciled in this pass. Two bags of the SAME template are otherwise
    // indistinguishable by (sid,type), and a plain first-match rule pointed BOTH parents'
    // contents at whichever we happened to read first: the second apply reconciled away what
    // the first had just placed, and the other bag was never reconciled at all.
    static Inventory* used[INV_SNAPSHOT_ITEMS_MAX];
    unsigned int nUsed = 0;
    unsigned int claimed = 0; // nested entries some parent accounted for

    for (unsigned int t = 0; t < nTop; ++t) {
        if (!isContainerItemType(top[t].itemType)) continue;
        // Gather this parent's children. The wire names the parent by its index in the
        // ORIGINAL entry array, which survives the split via topSrcIdx.
        static InvItemEntry kids[INV_SNAPSHOT_ITEMS_MAX];
        unsigned int nk = 0;
        for (unsigned int i = 0; i < count && nk < INV_SNAPSHOT_ITEMS_MAX; ++i)
            if (items[i].parentIdx == (unsigned short)(topSrcIdx[t] + 1)) kids[nk++] = items[i];
        claimed += nk;

        // Which copy of this template is it, in the SENDER's order? The sender's Nth bag of a
        // template binds to our Nth, so identical bags keep their own contents.
        unsigned int rank = 0;
        for (unsigned int u = 0; u < t; ++u)
            if (top[u].itemType == top[t].itemType &&
                strcmp(top[u].stringID, top[t].stringID) == 0) ++rank;

        // OUR copies of that template, in local carry order. Only an entry that actually owns
        // an inventory qualifies - matching on template alone could hand us a same-sid item
        // that is not a container at all.
        static Inventory* cands[INV_SNAPSHOT_ITEMS_MAX];
        unsigned int ncand = 0;
        for (unsigned int c = 0; c < ncur && ncand < INV_SNAPSHOT_ITEMS_MAX; ++c) {
            if (cur[c].itemType != top[t].itemType) continue;
            if (strcmp(cur[c].stringID, top[t].stringID) != 0) continue;
            if (!curItems[c]) continue;
            Inventory* s = 0;
            __try {
                s = curItems[c]->getInventory();
            } __except (EXCEPTION_EXECUTE_HANDLER) { s = 0; }
            if (s && s != inv) cands[ncand++] = s;
        }
        // Rank-aligned when we can, but NEVER a bag already reconciled this pass: our carry
        // order need not match the sender's, and reconciling one bag against two different
        // desired lists destroys the first list's items.
        Inventory* sub = (rank < ncand) ? cands[rank] : 0;
        for (unsigned int j = 0; j < nUsed; ++j) if (used[j] == sub) { sub = 0; break; }
        if (!sub) {
            for (unsigned int k = 0; k < ncand && !sub; ++k) {
                bool taken = false;
                for (unsigned int j = 0; j < nUsed; ++j) if (used[j] == cands[k]) { taken = true; break; }
                if (!taken) sub = cands[k];
            }
            if (sub && ncand > 1) {
                char b[220]; _snprintf(b, sizeof(b) - 1,
                    "[recon] NESTED-REBIND sid='%s' type=%u rank=%u local=%u (carry order differs "
                    "from the sender's; bound the first bag not already reconciled)",
                    top[t].stringID, top[t].itemType, rank, ncand);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
        }
        if (!sub) {
            // The bag is not here (still relocating over the conservation channel, or this
            // client genuinely lacks it). Its contents are NOT reconciled anywhere else, so
            // say so: silently skipping is how bagged items went missing in the first place.
            char b[220]; _snprintf(b, sizeof(b) - 1,
                "[recon] NESTED-SKIP sid='%s' type=%u kids=%u (no local copy of the container "
                "yet; its contents stay unreconciled until it arrives)",
                top[t].stringID, top[t].itemType, nk);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            continue;
        }
        if (nUsed < INV_SNAPSHOT_ITEMS_MAX) used[nUsed++] = sub;
        if (dbg) { char b[220]; _snprintf(b, sizeof(b) - 1,
            "[recon] NESTED sid='%s' type=%u kids=%u rank=%u local=%u",
            top[t].stringID, top[t].itemType, nk, rank, ncand);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b); }
        if (applyToInventory(gw, sub, kids, nk, truncated)) changed = true;
    }
    // Nested entries no top-level entry claimed. Their parent reference points at nothing we
    // can see, so they are reconciled NOWHERE - the same silent hole as a missing container,
    // and worth the same noise (a mismatched sender/receiver split would show up here).
    if (claimed < nNested) {
        char b[220]; _snprintf(b, sizeof(b) - 1,
            "[recon] NESTED-ORPHAN entries=%u (of %u nested; parentIdx names no top-level item "
            "in this snapshot, so their contents are reconciled nowhere)",
            nNested - claimed, nNested);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return changed;
    }
    return changed;
}

bool applyContainerContents(GameWorld* gw, const unsigned int cHand[5],
                            const InvItemEntry* items, unsigned int count,
                            bool truncated) {
    // A shop catalog is a native projection over physical furniture stock.
    // Rewriting it alongside those furniture snapshots creates two writers.
    if (isMerchantInventory(cHand)) return false;
    return applyInventoryContents(gw,transactionInventory(cHand),items,count,truncated);
}

bool captureBagContents(Item* bag, std::vector<InvItemEntry>& contents) {
    contents.clear();
    __try {
        if (!bag || !isContainerItemType((unsigned int)bag->getDataType())) return false;
        Inventory* inv = bag->getInventory();
        if (!inv) return false;
        InvItemEntry items[INV_ITEMS_MAX + 1];
        bool truncated = false;
        unsigned int n = readInvItems(inv, items, 0, INV_ITEMS_MAX + 1, &truncated, true);
        if (truncated || n > INV_ITEMS_MAX) return false;
        contents.assign(items, items + n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool applyTransferredBagContents(GameWorld* gw, Item* bag,
                                  const std::vector<InvItemEntry>& contents) {
    bool saved = g_invVetoSuspend;
    g_invVetoSuspend = true;
    bool valid = false;
    __try {
        if (bag && isContainerItemType((unsigned int)bag->getDataType()) && bag->getInventory()) {
            applyInventoryContents(gw, bag->getInventory(), contents.empty() ? 0 : &contents[0],
                                    (unsigned int)contents.size(), false);
            valid = true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    g_invVetoSuspend = saved;
    return valid;
}

// True while ANY inventory panel is open on this container - its own window, or a
// window on a container it carries (an opened backpack is its own panel, and the
// nested reconcile destroys stacks in there too, which is the 5.10 bag surface).
// The replicator asks this BEFORE applying a snapshot so it can defer the whole
// thing rather than half-apply it: a create-now/destroy-later split would leave the
// two sides diverged in a way no later snapshot describes.
bool containerGuiOpen(GameWorld* gw, const unsigned int cHand[5]) {
    if (!gw || !cHand) return false;
    Inventory* inv = transactionInventory(cHand);
    if (!inv) return false;
    if (inventoryGuiOpen(inv)) return true;
    // Carried containers: their private inventories each get their own panel.
    InvItemEntry cur[INV_ITEMS_MAX];
    Item* curItems[INV_ITEMS_MAX];
    unsigned int n = readInvItems(inv, cur, curItems, INV_ITEMS_MAX, 0);
    for (unsigned int i = 0; i < n; ++i) {
        if (!itemPtrLive(curItems[i])) continue;
        Inventory* sub = 0;
        __try { sub = curItems[i]->getInventory(); } __except (EXCEPTION_EXECUTE_HANDLER) { sub = 0; }
        if (!sub || sub == inv) continue;
        if (inventoryGuiOpen(sub)) return true;
    }
    return false;
}

bool containerGuiNeedsDefer(GameWorld* gw, const unsigned int cHand[5]) {
    // With the refresh lever present a live reconcile rebuilds the panel's icons
    // itself, so there is nothing to wait for and loot changes are visible on
    // both screens at once. Without it we are back to the only safe option:
    // leave the container alone until the window closes.
    if (g_invGuiRefreshFn != 0) return false;
    return containerGuiOpen(gw, cHand);
}

namespace {
// Find a "common", general-inventory-friendly item template (stackable trade goods /
// food the player squad's backpack always accepts). Falls back to the first ITEM
// template. Caller holds SEH. Returns the template + writes its itemType category.
GameData* findCommonItemTemplate(GameWorld* gw, unsigned int* outType) {
    if (!gw || !g_getDataOfTypeFn) return 0;
    const char* prefs[] = {
        "iron plate", "copper", "building materials", "raw meat", "dustwich",
        "foodcube", "ration", "rock", "cotton", "fabric"
    };
    const unsigned int np = sizeof(prefs) / sizeof(prefs[0]);
    __try {
        g_dataScratch.clear();
        g_getDataOfTypeFn(&gw->gamedata, &g_dataScratch, ITEM);
        unsigned int n = g_dataScratch.size();
        for (unsigned int k = 0; k < np; ++k)
            for (unsigned int i = 0; i < n; ++i) {
                GameData* gd = g_dataScratch[i];
                if (gd && ciContains(gd->name.c_str(), prefs[k])) {
                    if (outType) *outType = (unsigned int)ITEM;
                    return gd;
                }
            }
        for (unsigned int i = 0; i < n; ++i)
            if (g_dataScratch[i]) { if (outType) *outType = (unsigned int)ITEM; return g_dataScratch[i]; }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return 0;
}
} // namespace

bool pickInventoryContainer(GameWorld* gw, unsigned int out[5]) {
    if (out) { for (int i = 0; i < 5; ++i) out[i] = 0; }
    if (!gw || !gw->player) return false;
    __try {
        if (gw->player->playerCharacters.size() == 0) return false;
        Character* ld = gw->player->playerCharacters[0];
        if (!ld) return false;
        // v1 anchor: the leader's own inventory - a container that EXISTS in every save
        // (stable character hand, resolves cross-client) and accepts arbitrary items
        // (unlike resource-limited storage buildings). Both setup + resolve agree on it.
        return readObjectHand(static_cast<RootObject*>(ld), out);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

int addTestItemsToContainer(GameWorld* gw, const unsigned int cHand[5], int qty,
                            char* outStringID, unsigned int outLen) {
    if (outStringID && outLen) outStringID[0] = '\0';
    if (!gw || qty <= 0) return 0;
    RootObject* ro = resolveObjectByHand(cHand);
    if (!ro) return 0;
    Inventory* inv = invOf(ro);
    if (!inv) return 0;
    unsigned int typeCat = 0;
    GameData* gd = findCommonItemTemplate(gw, &typeCat);
    if (!gd) return 0;
    char sid[48]; sid[0] = '\0';
    __try {
        const char* s = gd->stringID.c_str();
        strncpy(sid, s ? s : "", sizeof(sid) - 1); sid[sizeof(sid) - 1] = '\0';
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    if (!sid[0]) return 0;
    if (outStringID && outLen) { strncpy(outStringID, sid, outLen - 1); outStringID[outLen - 1] = '\0'; }
    // createItemAndAdd is SEH-guarded; it re-resolves the template by stringID+type.
    bool ok = createItemAndAdd(gw, inv, sid, typeCat, qty, 0, /*equip=*/false);
    return ok ? qty : 0;
}

int probeAddAnyToContainer(GameWorld* gw, const unsigned int cHand[5], int qty,
                           char* outStringID, unsigned int outLen) {
    if (outStringID && outLen) outStringID[0] = '\0';
    if (!gw || qty <= 0) return 0;
    RootObject* ro = resolveObjectByHand(cHand);
    if (!ro) return 0;
    Inventory* inv = invOf(ro);
    if (!inv) return 0;
    // Storage buildings are usually item-type-LIMITED (run-171231: a Fabric
    // Chest refused the iron-plate sentinel), so walk the common stackables
    // until tryAddItem accepts one. This mirrors the protocol-34 apply path,
    // which only ever fabricates items the AUTHOR's copy of the container
    // already holds - i.e. items the container accepts by definition.
    const char* prefs[] = {
        "iron plate", "copper", "building materials", "raw meat", "dustwich",
        "foodcube", "ration", "rock", "cotton", "fabric"
    };
    const unsigned int np = sizeof(prefs) / sizeof(prefs[0]);
    for (unsigned int k = 0; k < np; ++k) {
        char sid[48]; sid[0] = '\0';
        __try {
            g_dataScratch.clear();
            g_getDataOfTypeFn(&gw->gamedata, &g_dataScratch, ITEM);
            unsigned int n = g_dataScratch.size();
            for (unsigned int i = 0; i < n; ++i) {
                GameData* gd = g_dataScratch[i];
                if (gd && ciContains(gd->name.c_str(), prefs[k])) {
                    const char* s = gd->stringID.c_str();
                    strncpy(sid, s ? s : "", sizeof(sid) - 1);
                    sid[sizeof(sid) - 1] = '\0';
                    break;
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { sid[0] = '\0'; }
        if (!sid[0]) continue;
        // createItemAndAdd is SEH-guarded; false = template miss OR the
        // container refused it (type filter) - try the next candidate.
        if (createItemAndAdd(gw, inv, sid, (unsigned int)ITEM, qty, 0,
                             /*equip=*/false)) {
            if (outStringID && outLen) {
                strncpy(outStringID, sid, outLen - 1);
                outStringID[outLen - 1] = '\0';
            }
            return qty;
        }
    }
    return 0;
}

int removeTestItemsFromContainer(GameWorld* gw, const unsigned int cHand[5], int qty) {
    if (!gw || qty <= 0) return 0;
    RootObject* ro = resolveObjectByHand(cHand);
    if (!ro) return 0;
    Inventory* inv = invOf(ro);
    if (!inv) return 0;
    unsigned int typeCat = 0;
    GameData* gd = findCommonItemTemplate(gw, &typeCat);
    if (!gd) return 0;
    char sid[48]; sid[0] = '\0';
    __try {
        const char* s = gd->stringID.c_str();
        strncpy(sid, s ? s : "", sizeof(sid) - 1); sid[sizeof(sid) - 1] = '\0';
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    if (!sid[0]) return 0;
    // Snapshot current contents (with parallel Item* for removal), then remove by key.
    const unsigned int MAXC = 64;
    InvItemEntry cur[64];
    Item* curItems[64];
    unsigned int ncur = readInvItems(inv, cur, curItems, MAXC);
    return removeByKey(inv, curItems, cur, ncur, sid, typeCat, 0, 0, qty, /*wantEquipped=*/0);
}

int commonTestItemSid(GameWorld* gw, char* outSid, unsigned int outLen,
                      unsigned int* outType) {
    if (outSid && outLen) outSid[0] = '\0';
    if (outType) *outType = 0;
    if (!gw || !outSid || outLen == 0) return 0;
    unsigned int typeCat = (unsigned int)ITEM;
    // Both peers may use different languages; template names are localized.
    GameData* gd = findItemTemplateImpl(gw, "42159-gamedata.base", typeCat);
    if (!gd) return 0;
    __try {
        const char* s = gd->stringID.c_str();
        if (!s || !s[0]) return 0;
        strncpy(outSid, s, outLen - 1); outSid[outLen - 1] = '\0';
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    if (outType) *outType = typeCat;
    return 1;
}

int readGearGradeBySid(GameWorld* gw, const unsigned int cHand[5], const char* sid,
                       unsigned int typeCat, int* outBucket, int* outLevel,
                       int* outLevel100) {
    if (outBucket)   *outBucket   = -1;
    if (outLevel)    *outLevel    = -1;
    if (outLevel100) *outLevel100 = -1;
    if (!gw || !sid || !sid[0]) return 0;
    RootObject* ro = resolveObjectByHand(cHand);
    if (!ro) return 0;
    Inventory* inv = invOf(ro);
    if (!inv) return 0;
    const unsigned int MAXC = 64;
    InvItemEntry cur[64];
    Item* curItems[64];
    unsigned int ncur = readInvItems(inv, cur, curItems, MAXC);
    __try {
        for (unsigned int i = 0; i < ncur; ++i) {
            if (!curItems[i]) continue;
            if (cur[i].itemType != typeCat) continue;
            if (strcmp(cur[i].stringID, sid) != 0) continue;
            if (outBucket) *outBucket = (int)cur[i].quality;
            // The craft LEVEL, not the quality float: the wire carries quality and the
            // fabricate path writes it back, so quality agrees across clients by
            // construction. Only getLevel() reveals a copy that was minted at the
            // factory's default grade.
            Gear* g = curItems[i]->isGear();                            // virtual
            if (g) {
                if (outLevel)    *outLevel    = curItems[i]->getLevel(); // virtual
                if (outLevel100) *outLevel100 = g->level_0_100;
            }
            return 1;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return 0;
}

int addItemsToContainerBySid(GameWorld* gw, const unsigned int cHand[5],
                             const char* sid, unsigned int typeCat, int qty,
                             int qualityBucket, const char* manufacturer,
                             const char* material, unsigned char level) {
    if (!gw || !sid || !sid[0] || qty <= 0) return 0;
    RootObject* ro = resolveObjectByHand(cHand);
    if (!ro) return 0;
    Inventory* inv = invOf(ro);
    if (!inv) return 0;
    bool ok = createItemAndAdd(gw, inv, sid, typeCat, qty, qualityBucket,
                               /*equip=*/false, manufacturer, material, level);
    return ok ? qty : 0;
}

#ifdef KENSHICOOP_HARNESS
Item* holdInventoryBag(GameWorld* gw, const unsigned int source[5], const char* sid) {
    if (!gw || !sid) return 0;
    __try {
        Inventory* inventory = invOf(resolveObjectByHand(source));
        if (!inventory) return 0;
        InvItemEntry rows[INV_ITEMS_MAX]; Item* items[INV_ITEMS_MAX];
        unsigned int count = readInvItems(inventory, rows, items, INV_ITEMS_MAX, 0);
        for (unsigned int i = 0; i < count; ++i)
            if (items[i] && isContainerItemType(rows[i].itemType) &&
                strcmp(rows[i].stringID, sid) == 0)
                return inventory->removeItemDontDestroy_returnsItem(items[i], 1, true);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return 0;
}

bool putHeldInventoryBag(GameWorld* gw, const unsigned int destination[5], Item* bag) {
    if (!gw || !bag) return false;
    __try {
        Inventory* inventory = invOf(resolveObjectByHand(destination));
        return inventory && (inventory->tryAddItem(bag, 1) || equipExisting(inventory, bag) != 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}

bool inventoryBagHand(GameWorld* gw, const unsigned int carrier[5], unsigned int out[5]) {
    __try {
        Inventory* sub = nestedInventoryOf(gw, carrier, 0);
        return sub && sub->owner && readObjectHand(sub->owner, out);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}
#endif

int moveItemBetweenContainers(GameWorld* gw, const unsigned int srcHand[5],
                              const unsigned int dstHand[5],
                              const char* sid, unsigned int typeCat, int qty,
                              bool suspendVeto,
                              const std::vector<InvItemEntry>* suppliedBag,
                              std::vector<InvItemEntry>* actualBag,
                              void* exactSourceItem,
                              bool respectReservations) {
    if (!gw || !sid || !sid[0] || qty <= 0) return 0;
    RootObject* srcRo = resolveObjectByHand(srcHand);
    RootObject* dstRo = resolveObjectByHand(dstHand);
    if (!srcRo || !dstRo || srcRo == dstRo) return 0;
    Inventory* src = invOf(srcRo);
    Inventory* dst = invOf(dstRo);
    if (!src || !dst) return 0;
    const unsigned int MAXC = INV_SNAPSHOT_ITEMS_MAX;
    static InvItemEntry cur[INV_SNAPSHOT_ITEMS_MAX];
    static Item* curItems[INV_SNAPSHOT_ITEMS_MAX];
    bool truncatedSource=false;
    unsigned int ncur = readInvItems(src, cur, curItems, MAXC, &truncatedSource, true);
    if (truncatedSource) return 0;
    int moved = 0;
    // Sanctioned sync moves suspend veto/capture, but still respect reservations
    // below unless rollback explicitly opts out. Native UI-like moves leave the
    // hooks active, including their reservation checks.
    bool vetoSav = g_invVetoSuspend; if (suspendVeto) g_invVetoSuspend = true;
    __try {
        // Loose stacks first (the ordinary bag-to-bag drag), then worn copies (the
        // "drag straight out of an equip slot" trade the field report describes).
        for (int pass = 0; pass < 2 && moved < qty; ++pass) {
            for (unsigned int i = 0; i < ncur && moved < qty; ++i) {
                if (!curItems[i]) continue;
                if (exactSourceItem && curItems[i] != exactSourceItem) continue;
                if (cur[i].itemType != typeCat) continue;
                if ((int)cur[i].equipped != pass) continue;
                if (strcmp(cur[i].stringID, sid) != 0) continue;
                int have = cur[i].quantity; if (have < 1) have = 1;
                int take = qty - moved; if (take > have) take = have;
                Inventory* from = src;
                if (cur[i].parentIdx) {
                    unsigned int p = cur[i].parentIdx - 1;
                    if (p >= ncur || !curItems[p]) continue;
                    from = curItems[p]->getInventory();
                    if (!from) continue;
                }
                if (respectReservations && suspendVeto &&
                    transferVerdictPending(from, curItems[i])) continue;
                Item* taken = from->removeItemDontDestroy_returnsItem(curItems[i], take, true);
                if (!taken) continue;
                if (isContainerItemType(typeCat)) {
                    if (suppliedBag && !applyTransferredBagContents(gw, taken, *suppliedBag)) {
                        from->tryAddItem(taken, take);
                        continue;
                    }
                    if (actualBag) captureBagContents(taken, *actualBag);
                }
                bool added = dst->tryAddItem(taken, take);
                if (!added && !isContainerItemType(typeCat)) {
                    static InvItemEntry bags[INV_SNAPSHOT_ITEMS_MAX];
                    static Item* bagItems[INV_SNAPSHOT_ITEMS_MAX];
                    unsigned int nb = readInvItems(dst,bags,bagItems,INV_SNAPSHOT_ITEMS_MAX,0);
                    for (unsigned int b=0;b<nb && !added;++b) {
                        if (!bagItems[b]) continue;
                        Inventory* sub=bagItems[b]->getInventory();
                        if (sub && sub!=dst) added=sub->tryAddItem(taken,take);
                    }
                }
                if (!added && isContainerItemType(typeCat)) added=equipExisting(dst,taken)!=0;
                if (!added) {
                    // Restore the actual object, including contents and its worn slot.
                    g_invVetoSuspend = true;
                    bool restored = cur[i].equipped && equipExisting(from,taken);
                    if (!restored) restored=from->tryAddItem(taken,take);
                    if (!restored) coop::logLine("[xfer] real-item rollback refused");
                    g_invVetoSuspend = vetoSav;
                    return moved;
                }
                curItems[i] = 0; // detached; never touch this stack again
                moved += take;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_invVetoSuspend = vetoSav;
        return moved;
    }
    g_invVetoSuspend = vetoSav;
    return moved;
}

namespace {
struct TransactionHand {
    unsigned int words[5];
    bool operator<(const TransactionHand& other) const {
        for (unsigned int i=0;i<5;++i)
            if (words[i]!=other.words[i]) return words[i]<other.words[i];
        return false;
    }
};
typedef inventory_transaction::Revision TransactionRevision;
std::map<TransactionHand,TransactionRevision> transactionRevisions;
std::map<TransactionHand,TransactionHand> merchantInventoryOwners;
struct MerchantStockRow {
    unsigned int source[5],hand[5],qualityBits,chargeBits,properOwner[5];
    InventorySection* section;
    Item* item;
    Faction* uniform;
    bool unique;
    InvItemEntry metadata;
    bool operator==(const MerchantStockRow& other) const {
        return section==other.section && item==other.item && qualityBits==other.qualityBits &&
            chargeBits==other.chargeBits && unique==other.unique && uniform==other.uniform &&
            !memcmp(source,other.source,sizeof(source)) && !memcmp(hand,other.hand,sizeof(hand)) &&
            !memcmp(properOwner,other.properOwner,sizeof(properOwner)) &&
            !memcmp(&metadata,&other.metadata,sizeof(metadata));
    }
};
struct MerchantStockView {
    ShopTrader* native;
    Inventory* projection;
    bool stockKnown;
    std::vector<MerchantStockRow> stock,observed;
    MerchantStockView():native(0),projection(0),stockKnown(false) {}
};
std::map<TransactionHand,MerchantStockView> merchantViews;
std::set<TransactionHand> inventoryCheckpointsPending;
NativeShopIntent shopIntents[64];
unsigned int shopIntentCount=0;
unsigned long merchantCensusMs=0;
unsigned int merchantHands[64][5],merchantCount=0;
bool transactionHost=true, transactionEnabled=false, transactionCommerce=false;
TransactionHand transactionHand(const unsigned int hand[5]) {
    TransactionHand key; memcpy(key.words,hand,sizeof(key.words)); return key;
}
bool transactionOwner(RootObject* owner,unsigned int out[5]) {
    unsigned int native[5];
    if (!owner || !readObjectHand(owner,native)) return false;
    for (std::map<TransactionHand,TransactionHand>::const_iterator it=merchantInventoryOwners.begin();
         it!=merchantInventoryOwners.end();++it) {
        if (memcmp(native,it->second.words,sizeof(native))) continue;
        // ShopTrader handles are nonphysical and shared by several traders.
        // Resolve the actual backing character, never the first equal handle.
        Character* trader=g_shopGetTraderFn ? g_shopGetTraderFn(static_cast<ShopTrader*>(owner)) : 0;
        return trader && readObjectHand(trader,out) &&
            merchantInventoryOwners.count(transactionHand(out))!=0;
    }
    return readPhysicalInventoryRootOwner(owner,out);
}
// ShopTrader is a nonphysical view over the shop's real furniture inventories.
// Own the view, never its stock; native construction/destruction preserves the
// underlying items just as opening/closing the game's trade window does.
ShopTrader* createMerchantView(Character* trader) { return new ShopTrader(trader); }
void destroyMerchantView(MerchantStockView& view) {
    delete view.native; view.native=0; view.projection=0; view.stockKnown=false;
    view.stock.clear(); view.observed.clear();
}
bool captureMerchantSources(ShopTraderInventory* inventory,std::vector<MerchantStockRow>& out) {
    typedef ogre_unordered_map<hand,InventorySection*>::type Sources;
    const Sources& sources=inventory->inventories;
    const size_t sourceCount=sources.size();
    size_t total=0;
    for (Sources::const_iterator it=sources.begin();it!=sources.end();++it) {
        if (!it->second || it->second->items.size()>static_cast<size_t>(-1)-total) return false;
        total+=it->second->items.size();
    }
    out.resize(total); // Retain both buffers; unchanged reads allocate no storage.
    size_t written=0;
    for (Sources::const_iterator it=sources.begin();it!=sources.end();++it) {
        InventorySection* section=it->second;
        const size_t count=section->items.size();
        if (count>total-written) return false;
        for (size_t i=0;i<count;++i) {
            MerchantStockRow& row=out[written++];
            row.source[0]=(unsigned int)it->first.type; row.source[1]=it->first.container;
            row.source[2]=it->first.containerSerial; row.source[3]=it->first.index;
            row.source[4]=it->first.serial;
            row.section=section; row.item=section->items[i].item;
            if (!readPhysicalItemMetadata(row.item,&row.metadata,row.hand)) return false;
            memcpy(&row.qualityBits,&row.item->quality,sizeof(row.qualityBits));
            // Native copyItem also copies remaining charges, uniqueness and
            // ownership; these are not represented by the network entry.
            memcpy(&row.chargeBits,&row.item->chargesLeft,sizeof(row.chargeBits));
            row.unique=row.item->isUnique; row.uniform=row.item->isAFactionUniform();
            const hand& owner=row.item->getProperOwner();
            row.properOwner[0]=(unsigned int)owner.type; row.properOwner[1]=owner.container;
            row.properOwner[2]=owner.containerSerial; row.properOwner[3]=owner.index;
            row.properOwner[4]=owner.serial;
            LockedArmour* locked=row.item->isLockedArmour();
            row.metadata.locked=locked && locked->lock ? 1 : 0;
        }
        if (count!=section->items.size()) return false;
    }
    return written==total && sourceCount==sources.size();
}
Inventory* currentMerchantView(MerchantStockView& view,ShopTrader* native) {
    Inventory* inventory=invOf(native);
    if (!inventory || !captureMerchantSources(static_cast<ShopTraderInventory*>(inventory),view.observed))
        return 0;
    const bool unchanged=view.projection==inventory && view.stockKnown &&
        view.stock.size()==view.observed.size() &&
        std::equal(view.stock.begin(),view.stock.end(),view.observed.begin());
    if (!unchanged) {
        // A native refresh destroys/rebuilds the display's Item copies. Never
        // do that for an unchanged physical stock set, nor use a timer/hash as
        // evidence that the full item identities and properties still match.
        if (inventoryGuiOpen(inventory) && !g_invGuiRefreshFn) return 0;
        native->updateInventory();
        inventoryGuiRefresh(inventory);
        view.stock.swap(view.observed); view.projection=inventory; view.stockKnown=true;
    }
    return inventory;
}
CRITICAL_SECTION productionReplicaLock;
bool productionReplicaLockReady=false,productionHookAttempted=false;
volatile LONG productionReplicaActive=0; // Win32 aligned read; configure publishes after initialization.
typedef void (__fastcall* ProductionOutputFn)(ProductionBuilding*,float);
ProductionOutputFn productionOutputOriginal=0;
void* productionOutputHookEntry=0;
std::map<TransactionHand,bool> hostProductionReplicas;

bool productionReplicaHand(Building* building,unsigned int hand[5],bool admitted=false) {
    __try {
        return building && building->classType==BCTYPE_PRODUCTION &&
            building->_buildState.isComplete &&
            (admitted || !(building->owner && building->owner->isPlayer)) &&
            static_cast<ProductionBuilding*>(building)->consumptionItems.size()==0 &&
            readObjectHand(building,hand);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void* productionOutputVirtualEntry(Building* building) {
    __try {
        // ProductionBuilding::updateOutput(float), native 1.0.65 SDK vtable.
        return (*reinterpret_cast<void***>(building))[0x5C0/sizeof(void*)];
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
void __fastcall hostProductionReplicaOutput(ProductionBuilding* building,float amount) {
    if (!productionReplicaActive) {
        if (productionOutputOriginal) productionOutputOriginal(building,amount);
        return;
    }
    unsigned int hand[5];bool block=false,first=false;
    // Native work adopts a neutral resource node into the worker's faction.
    // Authority follows the admitted full lifetime hand, not that mutable owner.
    // The map lookup below still excludes every unadmitted/player-built object.
    if (productionReplicaHand(building,hand,true)) {
        EnterCriticalSection(&productionReplicaLock);
        if (transactionEnabled && !transactionHost) {
            std::map<TransactionHand,bool>::iterator found=hostProductionReplicas.find(transactionHand(hand));
            if (found!=hostProductionReplicas.end()) {
                block=true;first=!found->second;found->second=true;
            }
        }
        LeaveCriticalSection(&productionReplicaLock);
    }
    if (first) {
        char line[160];_snprintf(line,sizeof(line)-1,
            "[prod] NATIVE-REPLICA-GUARD hand=%u,%u,%u,%u,%u",
            hand[0],hand[1],hand[2],hand[3],hand[4]);
        line[sizeof(line)-1]=0;coop::logLine(line);
    }
    if (!block && productionOutputOriginal) productionOutputOriginal(building,amount);
}
}

unsigned int enumMerchantInventoryHands(GameWorld* gw,unsigned int (*out)[5],unsigned int capacity) {
    if (!gw || !out || !capacity) return 0;
    if (capacity>64) capacity=64;
    const unsigned long now=GetTickCount();
    if (merchantCensusMs && now-merchantCensusMs<1000) {
        unsigned int count=merchantCount<capacity ? merchantCount : capacity;
        if (count) memcpy(out,merchantHands,count*sizeof(merchantHands[0]));
        return count;
    }
    merchantCensusMs=now;
    unsigned int written=0;
    merchantInventoryOwners.clear();
    __try {
        if (!g_getCharsFn || !g_shopGetTraderFn) return 0;
        static ContRead candidates[128];
        unsigned int count=enumLootInventoriesNear(gw,400.0f,candidates,128,
            transactionHost && transactionEnabled);
        for (unsigned int i=0;i<count && written<64;++i) {
            if (!candidates[i].isTrader) continue;
            TransactionHand stable=transactionHand(candidates[i].hand),native;
            if (merchantInventoryOwners.count(stable)) continue;
            RootObject* actor=resolveObjectByHand(stable.words);
            if (!actor || isPlayerSquad(gw,actor)) continue;
            MerchantStockView& view=merchantViews[stable];
            if (view.native && g_shopGetTraderFn(view.native)!=actor) destroyMerchantView(view);
            if (!view.native) view.native=createMerchantView(static_cast<Character*>(actor));
            if (!view.native || !invOf(view.native) || !readObjectHand(view.native,native.words)) continue;
            // Refresh at the read boundary: a native purchase can invalidate
            // borrowed item rows between census passes.
            merchantInventoryOwners[stable]=native;
            memcpy(merchantHands[written++],stable.words,sizeof(stable.words));
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        coop::logLine("[shop] census fault: no fabricated merchant stock");
    }
    for (std::map<TransactionHand,MerchantStockView>::iterator it=merchantViews.begin();
         it!=merchantViews.end();) {
        if (merchantInventoryOwners.count(it->first)) { ++it;continue; }
        destroyMerchantView(it->second);
        merchantViews.erase(it++);
    }
    merchantCount=written;
    const unsigned int count=written<capacity ? written : capacity;
    if (count) memcpy(out,merchantHands,count*sizeof(merchantHands[0]));
    return count;
}

Inventory* transactionInventory(const unsigned int hand[5]) {
    std::map<TransactionHand,TransactionHand>::const_iterator it=
        merchantInventoryOwners.find(transactionHand(hand));
    if (it!=merchantInventoryOwners.end()) {
        std::map<TransactionHand,MerchantStockView>::iterator view=merchantViews.find(it->first);
        RootObject* actor=resolveObjectByHand(it->first.words);
        if (view==merchantViews.end() || !view->second.native || !actor ||
            g_shopGetTraderFn(view->second.native)!=actor) return 0;
        __try {
            // Reuse an already-open native trade view so reconciliation sees
            // its GUI and refreshes its icons before deleting a physical item.
            // Looking up a window never opens one or replaces the user's view.
            InventoryGUI* window=::gui ? ::gui->getInventoryWindow(view->second.native->getHandle()) : 0;
            if (window) {
                Inventory* open=window->getInventory();
                unsigned int owner[5];
                if (open && readObjectHand(open->owner,owner) &&
                    !memcmp(owner,it->second.words,sizeof(owner)) &&
                    g_shopGetTraderFn(static_cast<ShopTrader*>(open->owner))==actor)
                    return currentMerchantView(view->second,static_cast<ShopTrader*>(open->owner));
            }
            return currentMerchantView(view->second,view->second.native);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            coop::logLine("[shop] native stock view unavailable; transaction deferred");
            return 0;
        }
    }
    return invOf(resolveObjectByHand(hand));
}
bool isMerchantInventory(const unsigned int hand[5]) {
    TransactionHand key=transactionHand(hand);
    if (merchantInventoryOwners.count(key)) return true;
    for (std::map<TransactionHand,TransactionHand>::const_iterator m=merchantInventoryOwners.begin();
         m!=merchantInventoryOwners.end();++m)
        if (!memcmp(hand,m->second.words,sizeof(m->second.words))) return true;
    return false;
}

void configureInventoryTransactions(bool host,bool enabled,bool commerce) {
    if (!productionReplicaLockReady) {
        InitializeCriticalSection(&productionReplicaLock);productionReplicaLockReady=true;
    }
    EnterCriticalSection(&productionReplicaLock);
    transactionHost=host; transactionEnabled=enabled; transactionCommerce=commerce;
    if (!enabled || host) hostProductionReplicas.clear();
    productionReplicaActive=(enabled && !host)?1:0;
    LeaveCriticalSection(&productionReplicaLock);
}
bool noteHostProductionReplica(const unsigned int localHand[5]) {
    if (!localHand || localHand[0]!=(unsigned int)BUILDING ||
        !productionReplicaLockReady || !transactionEnabled || transactionHost) return false;
    EnterCriticalSection(&productionReplicaLock);
    const bool admitted=hostProductionReplicas.count(transactionHand(localHand))!=0;
    LeaveCriticalSection(&productionReplicaLock);
    RootObject* root=resolveObjectByHand(localHand);
    unsigned int actual[5];
    if (!root || !productionReplicaHand(static_cast<Building*>(root),actual,admitted) ||
        memcmp(localHand,actual,sizeof(actual))) return false;
    // Admission runs on the game thread. Hook the instance's real virtual entry:
    // the SDK RVA can name a different body in a patched executable.
    void* entry=productionOutputVirtualEntry(static_cast<Building*>(root));
    if (!entry) return false;
    if (!productionHookAttempted) {
        productionHookAttempted=true;
        if (KenshiLib::AddHook(entry,(void*)&hostProductionReplicaOutput,
            (void**)&productionOutputOriginal)==KenshiLib::SUCCESS && productionOutputOriginal) {
            productionOutputHookEntry=entry;
            coop::logLine("[prod] native virtual output authority hook installed");
        } else coop::logLine("[prod] native virtual output hook unavailable; replica production remains unguarded");
    }
    // Do not claim authority over another implementation we have not hooked.
    if (!productionOutputOriginal || entry!=productionOutputHookEntry) return false;
    EnterCriticalSection(&productionReplicaLock);
    const bool enabled=transactionEnabled && !transactionHost;
    const bool inserted=enabled &&
        hostProductionReplicas.insert(std::make_pair(transactionHand(localHand),false)).second;
    LeaveCriticalSection(&productionReplicaLock);
    if (inserted) {
        char line[160];_snprintf(line,sizeof(line)-1,
            "[prod] NATIVE-REPLICA-ADMIT hand=%u,%u,%u,%u,%u",
            localHand[0],localHand[1],localHand[2],localHand[3],localHand[4]);
        line[sizeof(line)-1]=0;coop::logLine(line);
    }
    return enabled;
}
void clearInventoryTransactionState() {
    transactionRevisions.clear(); shopIntentCount=0;
    merchantInventoryOwners.clear();
    for (std::map<TransactionHand,MerchantStockView>::iterator it=merchantViews.begin();
         it!=merchantViews.end();++it) destroyMerchantView(it->second);
    merchantViews.clear();
    inventoryCheckpointsPending.clear();
    if (productionReplicaLockReady) {
        EnterCriticalSection(&productionReplicaLock);
        hostProductionReplicas.clear();
        LeaveCriticalSection(&productionReplicaLock);
    }
    merchantCensusMs=0; merchantCount=0;
}
void setInventoryCheckpointPending(const unsigned int hand[5],bool pending) {
    if (pending) inventoryCheckpointsPending.insert(transactionHand(hand));
    else inventoryCheckpointsPending.erase(transactionHand(hand));
}
bool inventoryCheckpointReady(Inventory* inventory) {
    if (!transactionEnabled || transactionHost) return true;
    __try {
        unsigned int root[5];
        if (!inventory || !transactionOwner(inventory->owner,root)) return false;
        return inventoryCheckpointsPending.count(transactionHand(root))==0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void noteInventoryRevision(const unsigned int hand[5],unsigned int revision) {
    transactionRevisions[transactionHand(hand)].adopt(revision);
}
void retireInventoryRevision(const unsigned int hand[5]) {
    transactionRevisions[transactionHand(hand)].retire();
    setInventoryCheckpointPending(hand,true);
}
bool inventoryCheckpointPending(const unsigned int hand[5]) {
    return inventoryCheckpointsPending.count(transactionHand(hand))!=0;
}
bool admitInventoryCheckpoint(const unsigned int hand[5],unsigned int revision,
    const InvItemEntry* wanted,unsigned int wantCount,
    const InvItemEntry* actual,unsigned int actualCount,bool wantedTruncated,bool actualTruncated) {
    return inventory_transaction::admitCheckpoint(transactionRevisions[transactionHand(hand)],
        revision,wanted,wantCount,actual,actualCount,wantedTruncated,actualTruncated);
}
unsigned int inventoryRevision(const unsigned int hand[5]) {
    std::map<TransactionHand,TransactionRevision>::const_iterator it=
        transactionRevisions.find(transactionHand(hand));
    return it==transactionRevisions.end() ? 0 : it->second.current();
}
unsigned int observeInventoryRevision(const unsigned int hand[5],unsigned int hash) {
    return transactionRevisions[transactionHand(hand)].observe(hash);
}
unsigned int nextInventoryCheckpoint(const unsigned int hand[5]) {
    return transactionRevisions[transactionHand(hand)].checkpoint();
}

bool inventoryTransactionRemovalReady(Inventory* source) {
    if (!transactionEnabled || transactionHost) return true;
    __try {
        unsigned int hand[5];
        if (!source || !transactionOwner(source->owner,hand)) return false;
        if (inventoryCheckpointsPending.count(transactionHand(hand))) return false;
        if (isMerchantInventory(hand)) return false; // only deferred player commerce may trade
        if (inventoryOwnerClass(hand)!=0) return true; // ordinary squad inventory management
        return inventoryRevision(hand)!=0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
namespace {
struct NativeTradePriceContext {
    Inventory* payer;
    unsigned int operation;
    int quote,quantity,actual;
    bool seen,allowed;
};
__declspec(thread) NativeTradePriceContext* nativeTradePriceContext=0;
int executeNativePlayerTrade(Inventory* source,Inventory* destination,Item* item,
    bool sourceMerchant,bool destinationMerchant,const InvXferPacket& request);
}

int observeNativeTradePrice(int nativePrice) {
    if (nativeTradePriceContext) {
        nativeTradePriceContext->actual=nativePrice;
        nativeTradePriceContext->seen=true;
        nativeTradePriceContext->allowed=inventory_transaction::nativeBatchPriceWithinQuote(
            nativeTradePriceContext->operation,nativePrice,
            nativeTradePriceContext->quantity,nativeTradePriceContext->quote);
    }
    return nativePrice;
}

int constrainNativeTradeFunds(Inventory* payer,int nativeFunds) {
    // Let the native insufficient-funds path restore its own split item before
    // insertion/payment. Never alter prices or funds outside this exact call.
    NativeTradePriceContext* context=nativeTradePriceContext;
    return context && context->payer==payer && context->seen && !context->allowed ?
        -1 : nativeFunds;
}

bool nativeShopTransactionsDeferred() {
    return transactionEnabled && transactionCommerce && !transactionHost;
}

bool deferNativeShopTransaction(Inventory* source,Item* item,Inventory* destination,
    unsigned int quantity) {
    if (!nativeShopTransactionsDeferred()) return false;
    // The native shop call must not run on JOIN, including before stock is
    // certified. A refusal leaves both the purse and physical stock untouched.
    __try {
        if (!source || !source->owner || !destination || !destination->owner) return false;
        NativeShopIntent request; memset(&request,0,sizeof(request));
        if (!transactionOwner(source->owner,request.source) ||
            !transactionOwner(destination->owner,request.destination)) return false;
        if (!isMerchantInventory(request.source) && !isMerchantInventory(request.destination))
            return false;
        // RMB must not publish another drag's detached actor baseline. The drag
        // frontend restores its source and clears the held cursor before enqueue.
        if (nativeInventoryCursorActive()) return true;
        if (!item || !nativePlayerTradeReady()) return true;
        if (inventoryCheckpointsPending.count(transactionHand(request.source)) ||
            inventoryCheckpointsPending.count(transactionHand(request.destination))) return true;
        const int sourceClass=inventoryOwnerClass(request.source);
        const int destinationClass=inventoryOwnerClass(request.destination);
#ifdef KENSHICOOP_HARNESS
        char t[224];_snprintf(t,sizeof(t),
            "[test] NATIVE_TRADE_CLASSES sourceClass=%d destinationClass=%d sourceRevision=%u destinationRevision=%u",
            sourceClass,destinationClass,inventoryRevision(request.source),inventoryRevision(request.destination));
        t[sizeof(t)-1]=0;coop::logLine(t);
#endif
        request.operation=sourceClass==1 && destinationClass==0 ? 2 :
                          sourceClass==0 && destinationClass==1 ? 1 : 0;
        if (!request.operation || shopIntentCount==64) return true;
        request.revision=inventoryRevision(request.source);
        if (!request.revision) {
            coop::logLine("[shop] WAIT authoritative stock revision"); return true;
        }
        unsigned int itemHand[5];
        if (!readPhysicalItemMetadata(item,&request.item,itemHand) ||
            !inventoryContainsPhysicalItem(source,item)) return true;
        if (request.operation==2) {
            unsigned int root[5];bool inInventory=false;void* physical=0;
            if (!readPhysicalItemLocation(itemHand,root,&inInventory,&physical) ||
                !inInventory || physical!=item || memcmp(root,request.source,sizeof(root))) return true;
        }
        // RMB explicitly selects one; drag carries the selected held quantity
        // after restoring/reacquiring the real source stack.
        if (!quantity || quantity>65535 || quantity>request.item.quantity) return true;
        request.item.quantity=(unsigned short)quantity;
        const int unit=item->getValueSingle(sourceClass==1);
        if (unit<0 || unit>INT_MAX/request.item.quantity) return true;
        request.quotedTotal=unit*request.item.quantity;
        shopIntents[shopIntentCount++]=request;
        // Freeze the actor immediately, not only when a HOST checkpoint
        // arrives. Merchant removal already requires this deferred shop path.
        setInventoryCheckpointPending(request.operation==2 ?
            request.source : request.destination,true);
        coop::logLine("[shop] DEFER native trade awaiting host verdict");
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        coop::logLine("[shop] REFUSE invalid native trade");
    }
    return true;
}

unsigned int drainNativeShopTransactions(NativeShopIntent* out,unsigned int capacity) {
    unsigned int count=shopIntentCount<capacity ? shopIntentCount : capacity;
    if (count) memcpy(out,shopIntents,count*sizeof(NativeShopIntent));
    shopIntentCount-=count;
    if (shopIntentCount) memmove(shopIntents,shopIntents+count,
                                shopIntentCount*sizeof(NativeShopIntent));
    return count;
}

int commitNativeShopTransaction(GameWorld* gw,const unsigned int source[5],
    const unsigned int destination[5],const InvXferPacket& request,int* wallet) {
    if (wallet) *wallet=-1;
    if (!transactionEnabled || !transactionCommerce || !transactionHost || !gw || !nativePlayerTradeReady() ||
        (request.operation!=1 && request.operation!=2)) return 0;
    __try {
        Inventory* from=transactionInventory(source); Inventory* to=transactionInventory(destination);
        if (!from || !to || from==to || !from->owner || !to->owner) return 0;
        static InvItemEntry entries[INV_SNAPSHOT_ITEMS_MAX];
        static Item* physical[INV_SNAPSHOT_ITEMS_MAX];
        bool truncated=false;
        unsigned int count=readInvItems(from,entries,physical,INV_SNAPSHOT_ITEMS_MAX,&truncated,true);
        if (truncated) return 0;
        Item* selected=0;
        for (unsigned int i=0;i<count;++i) {
            if (!physical[i] || entries[i].parentIdx ||
                entries[i].itemType!=request.itemType ||
                entries[i].quality!=request.quality || entries[i].level!=request.level ||
                strcmp(entries[i].stringID,request.stringID) ||
                strcmp(entries[i].manufacturer,request.manufacturer) ||
                strcmp(entries[i].material,request.material)) continue;
            // Admit one existing stack for the whole selection. The executor
            // still licenses exactly one unit for every native RClick call.
            if (request.quantity>0 && request.quantity<=entries[i].quantity) {
                selected=physical[i]; break;
            }
        }
        if (!selected) return 0;
        int before=-1; if (!readPlayerWallet(gw,&before)) return 0;
        if (!inventory_transaction::sourceRevisionMatches(
                request.sourceRevision,inventoryRevision(source)) || request.quotedTotal<0)
            return 0;
        const bool saved=g_invVetoSuspend; g_invVetoSuspend=true;
        int received=0;
        __try {
            // Use the real player implementation, including its price, room,
            // legality, split/merge, payment and rollback rules. NPC buyItem is
            // not a player commerce primitive.
            received=executeNativePlayerTrade(from,to,selected,
                isMerchantInventory(source),isMerchantInventory(destination),request);
        } __finally { g_invVetoSuspend=saved; }
        int after=-1; readPlayerWallet(gw,&after);
        if (wallet) *wallet=after;
        char line[224]; _snprintf(line,sizeof(line)-1,
            "[shop] HOST-COMMIT op=%u id=%u owner=%u revision=%u quote=%d wallet=%d->%d ok=%d moved=%d",
            (unsigned)request.operation,request.xferId,request.ownerId,request.sourceRevision,
            request.quotedTotal,before,after,received ? 1 : 0,received);
        line[sizeof(line)-1]=0; coop::logLine(line);
        return received;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        coop::logLine("[shop] native transaction fault; no synthetic item grant");
        return 0;
    }
}

int commitNativeExtraction(GameWorld* gw,const unsigned int source[5],
    const unsigned int destination[5],const InvXferPacket& request) {
    if (!transactionEnabled || !transactionHost || request.operation!=3) return 0;
    __try {
        Inventory* from=transactionInventory(source),*to=transactionInventory(destination);
        if (!from || !to || from==to) return 0;
        static InvItemEntry rows[INV_SNAPSHOT_ITEMS_MAX];
        static Item* physical[INV_SNAPSHOT_ITEMS_MAX]; bool truncated=false;
        unsigned int count=readInvItems(from,rows,physical,INV_SNAPSHOT_ITEMS_MAX,&truncated,true);
        if (truncated) return 0;
        for (unsigned int i=0;i<count;++i) {
            if (!physical[i] || rows[i].parentIdx || rows[i].itemType!=request.itemType ||
                rows[i].quality!=request.quality || rows[i].level!=request.level ||
                strcmp(rows[i].stringID,request.stringID) ||
                strcmp(rows[i].manufacturer,request.manufacturer) ||
                strcmp(rows[i].material,request.material)) continue;
            if (inventory_transaction::validate(3,request.sourceRevision,inventoryRevision(source),
                    request.quantity,rows[i].quantity,0,request.quotedTotal,0)!=
                    inventory_transaction::VALID) continue;
            if (!to->hasRoomForItem(physical[i]->getGameData())) return 0;
            // One existing source stack, one reversible native move: a short
            // request cannot silently consume several unrelated template matches.
            return moveItemBetweenContainers(gw,source,destination,request.stringID,
                request.itemType,request.quantity,true,0,0,physical[i],true);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return 0;
}

namespace {
typedef std::map<InventoryGUI*,InventoryGUI::InventoryTradeData,
    std::less<InventoryGUI*>,Ogre::STLAllocator<
    std::pair<InventoryGUI* const,InventoryGUI::InventoryTradeData>,
    Ogre::GeneralAllocPolicy> > NativeTradePartners;

NativeTradePartners* nativeTradePartners() {
    // KenshiLib exports the method but not its static map. Verify both native
    // RIP references agree before accessing the supported VC10 map layout.
    const unsigned char* code=reinterpret_cast<const unsigned char*>(
        KenshiLib::GetRealAddress(&InventoryGUI::clearTradePartners));
    if (!code || memcmp(code+10,"\x48\x8b\x05",3) ||
        memcmp(code+36,"\x48\x8d\x0d",3)) return 0;
    const unsigned char* head=code+17+*reinterpret_cast<const int*>(code+13);
    const unsigned char* map=code+43+*reinterpret_cast<const int*>(code+39);
    if (head!=map+8) return 0;
    NativeTradePartners* partners=reinterpret_cast<NativeTradePartners*>(
        const_cast<unsigned char*>(map));
    return partners->size()<=128 ? partners : 0;
}

class NativeTradeContext {
    NativeTradePartners* partners_;
    NativeTradePartners saved_;
    hand town_;
    InventoryGUI* source_;
    InventoryGUI* destination_;
    bool sourceMerchant_,destinationMerchant_;
    NativeTradeContext(const NativeTradeContext&);
    NativeTradeContext& operator=(const NativeTradeContext&);
    InventoryGUI* makeWindow(Inventory* inventory,bool merchant) {
        std::auto_ptr<InventoryLayout> layout(merchant ? static_cast<InventoryLayout*>(
            new TraderInventoryLayout) : inventory->owner->createInventoryLayout());
        if (!layout.get()) return 0;
        std::auto_ptr<InventoryGUI> window(merchant ? static_cast<InventoryGUI*>(
            new InventoryTraderGUI(inventory,layout.get(),inventory->owner)) :
            new InventoryGUI(inventory,layout.get(),inventory->owner));
        layout.release(); // native InventoryGUI owns its layout
        window->show(false);
        return window.release();
    }
public:
    NativeTradeContext(bool sourceMerchant,bool destinationMerchant):
        partners_(nativeTradePartners()),source_(0),destination_(0),
        sourceMerchant_(sourceMerchant),destinationMerchant_(destinationMerchant) {
        if (!partners_) return;
        Town* current=InventoryGUI::getTradingTown();
        if (current) town_=current->getHandle();
        saved_.swap(*partners_);
    }
    bool prepare(Inventory* source,Inventory* destination) {
        if (!partners_) return false;
        source_=makeWindow(source,sourceMerchant_);
        destination_=makeWindow(destination,destinationMerchant_);
        if (!source_ || !destination_) return false;
        RootObject* merchant=sourceMerchant_ ? source->owner : destination->owner;
        TownBase* location=merchant->getCurrentTownLocation();
        Town* town=location ? location->isTown() : 0;
        if (!town) return false;
        InventoryGUI::setTradingTown(town->getHandle());
        InventoryGUI::addTradePartner(source_,true,false,!sourceMerchant_,
            source->owner->getHandle());
        InventoryGUI::addTradePartner(destination_,true,false,!destinationMerchant_,
            destination->owner->getHandle());
        return true;
    }
    ~NativeTradeContext() {
        if (!partners_) return;
        InventoryGUI::clearTradePartners();
        // InventoryTraderGUI normally retires the global UI's ShopTrader.
        // These hidden windows borrow existing native stock views instead.
        if (source_ && sourceMerchant_) source_->callbackObject=0;
        if (destination_ && destinationMerchant_) destination_->callbackObject=0;
        delete source_; delete destination_;
        saved_.swap(*partners_);
        InventoryGUI::setTradingTown(town_);
    }
    bool run(Item* item) {
        if (!partners_ || !source_ || !destination_) return false;
        struct PlayerTradeAccess : InventoryGUI { using InventoryGUI::RClickAutoTrade; };
        typedef InventoryGUI::TradeResult (InventoryGUI::*TradeFn)(
            const std::string&,int,int,InventoryGUI*,bool,bool);
        static TradeFn trade=static_cast<TradeFn>(&PlayerTradeAccess::RClickAutoTrade);
        const InventoryGUI::TradeResult result=(source_->*trade)(
            item->inventorySection,item->inventoryPos.x,item->inventoryPos.y,
            destination_,true,false);
        char line[128];_snprintf(line,sizeof(line)-1,
            "[test] NATIVE_HIDDEN_PLAYER_TRADE result=%d",(int)result.value);
        line[sizeof(line)-1]=0;coop::logLine(line);
        return result.value==InventoryGUI::TradeResult::OK;
    }
};

class NativeTradePriceScope {
    NativeTradePriceContext context_;
    NativeTradePriceContext* previous_;
public:
    NativeTradePriceScope(Inventory* payer,unsigned int operation,int quote,int quantity):
        previous_(nativeTradePriceContext) {
        context_.payer=payer; context_.operation=operation; context_.quote=quote;
        context_.quantity=quantity;
        context_.actual=0; context_.seen=false; context_.allowed=false;
        nativeTradePriceContext=&context_;
    }
    ~NativeTradePriceScope() { nativeTradePriceContext=previous_; }
    bool accepted() const { return context_.seen && context_.allowed; }
};

bool nativeTradeMatches(const InvItemEntry& row,const InvXferPacket& request) {
    return row.itemType==request.itemType && row.quality==request.quality &&
        row.level==request.level && !strcmp(row.stringID,request.stringID) &&
        !strcmp(row.manufacturer,request.manufacturer) && !strcmp(row.material,request.material);
}

struct NativeTradeStock {
    unsigned int rows,units,selectedQuantity,selectedRows;
    unsigned int selectedHand[5];
    bool selectedHandKnown;
};

bool readNativeTradeStock(Inventory* inventory,const InvXferPacket& request,
    Item* selected,NativeTradeStock* stock) {
    static InvItemEntry rows[INV_SNAPSHOT_ITEMS_MAX];
    static Item* physical[INV_SNAPSHOT_ITEMS_MAX];
    bool truncated=false;
    memset(stock,0,sizeof(*stock));
    stock->rows=readInvItems(inventory,rows,physical,INV_SNAPSHOT_ITEMS_MAX,&truncated,true);
    if (truncated) return false;
    unsigned int selectedIndex=INV_SNAPSHOT_ITEMS_MAX;
    for (unsigned int i=0;i<stock->rows;++i) {
        if (nativeTradeMatches(rows[i],request)) {
            // Native auto-placement may put a purchased unit inside a worn
            // backpack. Conservation covers the complete root inventory tree.
            stock->units+=rows[i].quantity;
            if (selected && !rows[i].parentIdx && physical[i]==selected) {
                stock->selectedQuantity=rows[i].quantity; selectedIndex=i;
                // Merchant display copies need not be registered world objects.
                // Membership above licenses this read; registry resolution does not.
                stock->selectedHandKnown=readObjectHand(
                    static_cast<RootObject*>(physical[i]),stock->selectedHand);
            }
        }
    }
    if (selectedIndex!=INV_SNAPSHOT_ITEMS_MAX) {
        stock->selectedRows=1;
        for (unsigned int i=0;i<stock->rows;++i)
            if (rows[i].parentIdx==selectedIndex+1) ++stock->selectedRows;
    }
    return true;
}

// No C++ ownership lives in this SEH boundary. Even a failed native result must
// be read back before deciding what the batch actually committed.
int runNativeTradeUnit(NativeTradeContext& context,Inventory* source,Inventory* destination,
    Item* selected,const NativeTradeStock& original,unsigned int expectedQuantity,
    const InvXferPacket& request,bool* nativeOK) {
    *nativeOK=false;
    __try {
        NativeTradeStock fromBefore,toBefore,fromAfter,toAfter;
        if (!readNativeTradeStock(source,request,selected,&fromBefore) ||
            fromBefore.selectedQuantity!=expectedQuantity ||
            (original.selectedHandKnown && (!fromBefore.selectedHandKnown ||
                memcmp(original.selectedHand,fromBefore.selectedHand,sizeof(original.selectedHand)))) ||
            !inventory_transaction::nativeQuantitySupported(request.operation,1,
                fromBefore.selectedQuantity) ||
            !readNativeTradeStock(destination,request,0,&toBefore) ||
            toBefore.rows+fromBefore.selectedRows>INV_SNAPSHOT_ITEMS_MAX ||
            !destination->hasRoomForItem(selected->getGameData())) return 0;
        if (request.operation==2) {
            unsigned int root[5],owner[5];bool inInventory=false;void* physical=0;
            if (!transactionOwner(source->owner,owner) ||
                !fromBefore.selectedHandKnown ||
                !readPhysicalItemLocation(fromBefore.selectedHand,root,&inInventory,&physical) ||
                !inInventory || physical!=selected || memcmp(root,owner,sizeof(root))) return 0;
        }
        __try { *nativeOK=context.run(selected); }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            coop::logLine("[shop] native trade fault; reading back final physical unit");
        }
        // Native insertion may merge/free the moved item. Never dereference the
        // pre-call pointer again; the next unit reacquires it from source rows.
        if (!readNativeTradeStock(source,request,0,&fromAfter) ||
            !readNativeTradeStock(destination,request,0,&toAfter)) return 0;
        return inventory_transaction::nativeUnitMoved(fromBefore.units,fromAfter.units,
            toBefore.units,toAfter.units);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        coop::logLine("[shop] native unit fault; stop batch without synthetic grant");
        return 0;
    }
}

int executeNativePlayerTrade(Inventory* source,Inventory* destination,Item* item,
    bool sourceMerchant,bool destinationMerchant,const InvXferPacket& request) {
    NativeTradeStock original;
    if (!readNativeTradeStock(source,request,item,&original) || !request.quantity ||
        request.quantity>original.selectedQuantity) return 0;
    NativeTradeContext context(sourceMerchant,destinationMerchant);
    if (!context.prepare(source,destination)) return 0;
    int moved=0;
    while (moved<request.quantity) {
        // The native implementation quotes the split one-unit copy, checks the
        // destination payer, inserts it, and only then transfers both purses.
        // Never manually debit or invoke the cursor-dependent All/place paths.
        NativeTradePriceScope price(destination,request.operation,request.quotedTotal,
            request.quantity);
        bool nativeOK=false;
        const int actual=runNativeTradeUnit(context,source,destination,item,original,
            original.selectedQuantity-moved,request,&nativeOK);
        moved+=actual;
        if (!actual || !nativeOK || !price.accepted()) break;
    }
    return moved;
}

#ifdef KENSHICOOP_HARNESS
bool exerciseHiddenNativePlayerTrade(Inventory* source,Inventory* destination,
    Item* item,bool sourceMerchant,bool destinationMerchant) {
    NativeTradeContext context(sourceMerchant,destinationMerchant);
    return context.prepare(source,destination) && context.run(item);
}
#endif
}

bool exerciseNativeTransaction(GameWorld* gw,const unsigned int source[5],
    const unsigned int destination[5],const InvItemEntry& item,bool commerce) {
    __try {
        Inventory* from=transactionInventory(source),*to=transactionInventory(destination);
        if (!gw || !from || !to || from==to || !to->owner ||
            !inventoryCheckpointReady(from) || !inventoryCheckpointReady(to)) {
#ifdef KENSHICOOP_HARNESS
            coop::logLine("[test] NATIVE_TRADE_REFUSE inventory/checkpoint");
#endif
            return false;
        }
        static InvItemEntry rows[INV_SNAPSHOT_ITEMS_MAX];
        static Item* physical[INV_SNAPSHOT_ITEMS_MAX]; bool truncated=false;
        unsigned int count=readInvItems(from,rows,physical,INV_SNAPSHOT_ITEMS_MAX,&truncated,true);
        if (truncated) return false;
        for (unsigned int i=0;i<count;++i) {
            if (!physical[i] || rows[i].parentIdx || (!commerce && rows[i].equipped) ||
                (commerce ? !inventory_transaction::nativeQuantitySupported(1,item.quantity,rows[i].quantity) :
                    rows[i].quantity!=item.quantity) || rows[i].itemType!=item.itemType ||
                rows[i].quality!=item.quality || rows[i].level!=item.level ||
                strcmp(rows[i].stringID,item.stringID) ||
                strcmp(rows[i].manufacturer,item.manufacturer) ||
                strcmp(rows[i].material,item.material)) continue;
            if (!commerce)
                return moveItemBetweenContainers(gw,source,destination,item.stringID,
                    item.itemType,item.quantity,false,0,0,physical[i],true)==item.quantity;
#ifdef KENSHICOOP_HARNESS
            if (transactionHost)
                return exerciseHiddenNativePlayerTrade(from,to,physical[i],
                    isMerchantInventory(source),isMerchantInventory(destination));
#endif
            // Player right-click commerce has a different native implementation
            // from Inventory::buyItem (called by NPC shopping AI).
            struct PlayerTradeAccess : InventoryGUI {
                using InventoryGUI::RClickAutoTrade;
            };
            typedef InventoryGUI::TradeResult (InventoryGUI::*PlayerTradeFn)(
                const std::string&,int,int,InventoryGUI*,bool,bool);
            static PlayerTradeFn playerTrade=static_cast<PlayerTradeFn>(&PlayerTradeAccess::RClickAutoTrade);
            const intptr_t entry=KenshiLib::GetRealAddress(playerTrade);
            InventoryGUI* sourceWindow=from->getInventoryGUI();
            InventoryGUI* destinationWindow=to->getInventoryGUI();
            if (!entry || !sourceWindow || !destinationWindow) return false;
            const unsigned int queuedBefore=shopIntentCount;
            InventoryGUI::TradeResult result=(sourceWindow->*playerTrade)(
                physical[i]->inventorySection,physical[i]->inventoryPos.x,
                physical[i]->inventoryPos.y,destinationWindow,true,true);
            char trace[192];_snprintf(trace,sizeof(trace)-1,
                "[test] NATIVE_PLAYER_TRADE entry=%p result=%d source=%p destination=%p",
                (void*)entry,(int)result.value,sourceWindow,destinationWindow);
            trace[sizeof(trace)-1]=0;coop::logLine(trace);
            // An intercepted JOIN call deliberately refuses local mutation.
            // Submission only starts the observation leg; the scenario still
            // requires authoritative stock, actor contents and money to change.
            return result.value==InventoryGUI::TradeResult::OK ||
                shopIntentCount==queuedBefore+1;
        }
#ifdef KENSHICOOP_HARNESS
        coop::logLine("[test] NATIVE_TRADE_REFUSE exact item absent");
#endif
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        coop::logLine("[native-tx] real native action unavailable/faulted");
    }
    return false;
}

// SEH-guarded DIAGNOSTIC: log the full inventory of the object at cHand - every loose
// _allItems entry and every section (name/slot/equipFlag/containerFlag) with its items
// (type/eqFlag/sid). Lets us see exactly where a worn WEAPON lives: a section flagged
// isAnEquippedItemSection (the snapshot captures it) vs. a dedicated weapon pointer the
// section walk misses (type 0 = WEAPON, 1 = ARMOUR, 2 = ITEM).
void dumpInventory(GameWorld* gw, const unsigned int cHand[5]) {
    if (!gw) return;
    RootObject* ro = resolveObjectByHand(cHand);
    if (!ro) return;
    Inventory* inv = invOf(ro);
    if (!inv) return;
    __try {
        lektor<Item*>& all = inv->_allItems;
        unsigned int na = all.size();
        char h[120]; _snprintf(h, sizeof(h) - 1, "DUMP _allItems n=%u", na); h[sizeof(h) - 1] = '\0';
        coop::logLine(h);
        for (unsigned int i = 0; i < na; ++i) {
            Item* it = all[i]; if (!it) continue;
            GameData* gd = it->getGameData(); if (!gd) continue;
            char b[200];
            _snprintf(b, sizeof(b) - 1, "DUMP   loose type=%u eq=%d slot=%u sid='%s'",
                      (unsigned int)gd->type, it->isEquipped ? 1 : 0,
                      (unsigned int)it->slotType, gd->stringID.c_str());
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    __try {
        Item* pw = g_getPrimaryWeaponFn ? g_getPrimaryWeaponFn(inv) : 0;
        Item* sw = g_getSecondaryWeaponFn ? g_getSecondaryWeaponFn(inv) : 0;
        GameData* pgd = pw ? pw->getGameData() : 0;
        GameData* sgd = sw ? sw->getGameData() : 0;
        char b[220];
        _snprintf(b, sizeof(b) - 1, "DUMP weapons primary=%s sid='%s' secondary=%s sid='%s'",
                  pw ? "Y" : "n", pgd ? pgd->stringID.c_str() : "",
                  sw ? "Y" : "n", sgd ? sgd->stringID.c_str() : "");
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (!g_getSectionsFn) return;
    __try {
        lektor<InventorySection*>* secs = g_getSectionsFn(inv);
        unsigned int ns = secs ? secs->size() : 0;
        char h[120]; _snprintf(h, sizeof(h) - 1, "DUMP sections n=%u", ns); h[sizeof(h) - 1] = '\0';
        coop::logLine(h);
        for (unsigned int s = 0; s < ns; ++s) {
            InventorySection* sec = (*secs)[s]; if (!sec) continue;
            const Ogre::vector<InventorySection::SectionItem>::type& its = sec->items;
            unsigned int ni = (unsigned int)its.size();
            char b[220];
            _snprintf(b, sizeof(b) - 1, "DUMP sec='%s' slot=%u equip=%d ctnr=%d items=%u",
                      sec->name.c_str(), (unsigned int)sec->limitedSlot,
                      sec->isAnEquippedItemSection ? 1 : 0, sec->containerSlot ? 1 : 0, ni);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            for (unsigned int i = 0; i < ni; ++i) {
                Item* it = its[i].item; if (!it) continue;
                GameData* gd = it->getGameData(); if (!gd) continue;
                char c[200];
                _snprintf(c, sizeof(c) - 1, "DUMP     type=%u eq=%d sid='%s'",
                          (unsigned int)gd->type, it->isEquipped ? 1 : 0, gd->stringID.c_str());
                c[sizeof(c) - 1] = '\0'; coop::logLine(c);
                // A worn CONTAINER carries its own Inventory, and nothing in the character's
                // own sections describes what is inside it. Recurse one level so the log says
                // plainly where a bagged item lives - the snapshot channel walks only the
                // character's inventory, which is why such an item replicates nowhere.
                Inventory* sub = it->getInventory();
                if (!sub || sub == inv) continue;
                lektor<InventorySection*>* subSecs = g_getSectionsFn(sub);
                unsigned int nss = subSecs ? subSecs->size() : 0;
                char sh[160]; _snprintf(sh, sizeof(sh) - 1,
                    "DUMP     NESTED inventory of '%s': sections=%u",
                    gd->stringID.c_str(), nss);
                sh[sizeof(sh) - 1] = '\0'; coop::logLine(sh);
                for (unsigned int ss = 0; ss < nss; ++ss) {
                    InventorySection* s2 = (*subSecs)[ss]; if (!s2) continue;
                    const Ogre::vector<InventorySection::SectionItem>::type& i2 = s2->items;
                    char sb[220]; _snprintf(sb, sizeof(sb) - 1,
                        "DUMP       nsec='%s' equip=%d ctnr=%d items=%u", s2->name.c_str(),
                        s2->isAnEquippedItemSection ? 1 : 0, s2->containerSlot ? 1 : 0,
                        (unsigned int)i2.size());
                    sb[sizeof(sb) - 1] = '\0'; coop::logLine(sb);
                    for (unsigned int k = 0; k < (unsigned int)i2.size(); ++k) {
                        Item* n2 = i2[k].item; if (!n2) continue;
                        GameData* g2 = n2->getGameData(); if (!g2) continue;
                        char nb[200]; _snprintf(nb, sizeof(nb) - 1,
                            "DUMP         type=%u qty=%d sid='%s'", (unsigned int)g2->type,
                            n2->quantity, g2->stringID.c_str());
                        nb[sizeof(nb) - 1] = '\0'; coop::logLine(nb);
                    }
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// ---- Phase W0: world-item (ground drop) diagnostic hooks -------------------
// These let a scenario drive a DROP and enumerate nearby world items WITHOUT any UI,
// so we can characterize exactly what a Kenshi drop produces (object kind/itemType,
// position, whether getObjectsWithinSphere enumerates it, hand value) before designing
// the world-item replication channel. Mirrors addTestItemsToContainer / dumpInventory.

// SEH-guarded: drop `qty` of the LOOSE (sid,type) the object at cHand holds onto the
// ground. Finds the matching loose Item* and calls Inventory::dropItem (virtual), which
// removes it from the bag and places it as a world object. Returns the number dropped.
int dropItemFromInventory(GameWorld* gw, const unsigned int cHand[5],
                          const char* sid, unsigned int typeCat, int qty,
                          void** outLastDropped, bool allowEquipped) {
    if (outLastDropped) *outLastDropped = 0;
    if (!gw || !sid || !sid[0] || qty <= 0) return 0;
    RootObject* ro = resolveObjectByHand(cHand);
    if (!ro) return 0;
    Inventory* inv = invOf(ro);
    if (!inv) return 0;
    int dropped = 0;
    __try {
        // Re-snapshot each iteration: dropItem mutates _allItems, so a cached Item*
        // could dangle. Find the first LOOSE entry matching (sid,type) and drop it.
        for (int d = 0; d < qty; ++d) {
            const unsigned int MAXC = 64;
            InvItemEntry cur[64];
            Item* curItems[64];
            unsigned int ncur = readInvItems(inv, cur, curItems, MAXC);
            Item* victim = 0;
            // Two passes so a LOOSE copy is always preferred over a worn one: dropping the
            // worn piece when a spare lies in the bag would strip the character needlessly.
            for (unsigned int pass = 0; pass < (allowEquipped ? 2u : 1u) && !victim; ++pass) {
                for (unsigned int i = 0; i < ncur; ++i) {
                    if (pass == 0 && cur[i].equipped) continue;
                    if (cur[i].itemType != typeCat) continue;
                    if (strcmp(cur[i].stringID, sid) != 0) continue;
                    victim = curItems[i]; break;
                }
            }
            if (!victim) break;
            inv->dropItem(victim);                             // virtual: bag -> ground
            if (outLastDropped) *outLastDropped = victim;      // the now-grounded object
            ++dropped;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return dropped;
}

namespace {
// Enumerate world objects of `type` within `radius` of `center` and log one line per
// object (itemType / sid / hand / pos). Returns the count enumerated. Caller holds SEH.
unsigned int dumpWorldItemsOfType(GameWorld* gw, const Ogre::Vector3& center,
                                  float radius, itemType type, const char* label) {
    if (!g_getObjsFn) return 0;
    g_npcQuery.clear();
    g_getObjsFn(gw, &g_npcQuery, &center, radius, type, 256, 0);
    unsigned int n = g_npcQuery.size();
    char h[120];
    _snprintf(h, sizeof(h) - 1, "WORLDITEM scan type=%s(%d) n=%u r=%.1f",
              label, (int)type, n, radius);
    h[sizeof(h) - 1] = '\0'; coop::logLine(h);
    for (unsigned int i = 0; i < n; ++i) {
        RootObject* o = g_npcQuery[i]; if (!o) continue;
        unsigned int hd[5] = {0,0,0,0,0};
        readObjectHand(o, hd);
        Ogre::Vector3 p(0,0,0);
        __try { p = o->getPosition(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        GameData* gd = 0;
        __try { gd = o->getGameData(); } __except (EXCEPTION_EXECUTE_HANDLER) { gd = 0; }
        char b[260];
        _snprintf(b, sizeof(b) - 1,
            "WORLDITEM   %s sid='%s' gdtype=%u hand=%u,%u,%u,%u,%u pos=%.2f,%.2f,%.2f",
            label, gd ? gd->stringID.c_str() : "(no gd)",
            gd ? (unsigned int)gd->type : 0u,
            hd[0], hd[1], hd[2], hd[3], hd[4], p.x, p.y, p.z);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        // A screenshot cannot distinguish a loose Item from a held/equipped
        // visual. Report the actual native inventory flags alongside its hand.
        int inInventory=-1,equipped=-1,quantity=-1;
        __try {
            Item* item=reinterpret_cast<Item*>(o);
            inInventory=item->isInInventory ? 1 : 0;
            equipped=item->isEquipped ? 1 : 0;
            quantity=item->quantity;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        char identity[192]; _snprintf(identity,sizeof(identity)-1,
            "WORLDITEM IDENTITY hand=%u,%u,%u,%u,%u inventory=%d equipped=%d qty=%d",
            hd[0],hd[1],hd[2],hd[3],hd[4],inInventory,equipped,quantity);
        identity[sizeof(identity)-1]='\0'; coop::logLine(identity);
    }
    return n;
}
} // namespace

// SEH-guarded DIAGNOSTIC: enumerate WEAPON/ARMOUR/ITEM/CONTAINER world objects near the
// local leader and log each (so we can see what a dropped item became and whether the
// interest query enumerates it). The log IS the deliverable for the W0 drop_probe.
void dumpWorldItems(GameWorld* gw) {
    if (!gw || !gw->player) return;
    __try {
        if (gw->player->playerCharacters.size() == 0) return;
        Character* ld = gw->player->playerCharacters[0];
        if (!ld) return;
        Ogre::Vector3 center = ld->getPosition();
        const float r = 30.0f;
        dumpWorldItemsOfType(gw, center, r, WEAPON,    "WEAPON");
        dumpWorldItemsOfType(gw, center, r, ARMOUR,    "ARMOUR");
        dumpWorldItemsOfType(gw, center, r, ITEM,      "ITEM");
        dumpWorldItemsOfType(gw, center, r, CONTAINER, "CONTAINER");
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// SEH-guarded: count loose world items (WEAPON+ARMOUR+ITEM) within `radius` of the
// leader. Oracle cross-check that a drop actually produced an enumerable ground item.
int countWorldItemsNear(GameWorld* gw, float radius) {
    if (!gw || !gw->player || !g_getObjsFn) return 0;
    int total = 0;
    __try {
        if (gw->player->playerCharacters.size() == 0) return 0;
        Character* ld = gw->player->playerCharacters[0];
        if (!ld) return 0;
        Ogre::Vector3 center = ld->getPosition();
        const itemType kinds[] = { WEAPON, ARMOUR, ITEM };
        for (int k = 0; k < 3; ++k) {
            g_npcQuery.clear();
            g_getObjsFn(gw, &g_npcQuery, &center, radius, kinds[k], 256, 0);
            total += (int)g_npcQuery.size();
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return total; }
    return total;
}

// ---- Phase W1: world-item (ground drop) replication ------------------------
namespace {
// CONTENT fingerprint of one world item: stringID + type + qty + quality. Position is
// DELIBERATELY excluded - the join may re-ground a spawned proxy's Y slightly, so a
// pos-inclusive hash would never match host vs join. The caller tracks position
// separately (the WorldItemEntry carries x/y/z; the oracle matches pos within tolerance).
unsigned int worldItemHash(const char* sid, unsigned int type, unsigned short qty,
                           unsigned short quality) {
    unsigned int h = 2166136261u;
    if (sid) for (const char* p = sid; *p; ++p) { h ^= (unsigned char)*p; h *= 16777619u; }
    h ^= (unsigned int)type    * 2654435761u;
    h ^= (unsigned int)qty     * 40503u;
    h ^= (unsigned int)quality * 2246822519u;
    return h ? h : 1u;
}
} // namespace

unsigned int captureWorldItems(GameWorld* gw, WorldItemRaw* out, unsigned int maxOut,
                               float radius) {
    if (!gw || !gw->player || !out || maxOut == 0 || !g_getObjsFn) return 0;
    unsigned int n = 0;
    __try {
        if (gw->player->playerCharacters.size() == 0) return 0;
        Character* ld = gw->player->playerCharacters[0];
        if (!ld) return 0;
        Ogre::Vector3 center = ld->getPosition();
        // Category queries overlap. Accept only real free-floor Items and
        // deduplicate their complete registered native hands.
        const itemType kinds[] = { ITEM, WEAPON, ARMOUR };
        for (int k = 0; k < 3 && n < maxOut; ++k) {
            g_npcQuery.clear();
            g_getObjsFn(gw, &g_npcQuery, &center, radius, kinds[k], 256, 0);
            unsigned int total = g_npcQuery.size();
            for (unsigned int i = 0; i < total && n < maxOut; ++i) {
                RootObject* o = g_npcQuery[i];
                if (!o || !o->typeIsAnItem()) continue;
                Item* it = static_cast<Item*>(o);
                if (!physicalItemIsOnGround(it)) continue;
                InvItemEntry metadata;
                unsigned int hd[5] = {0,0,0,0,0};
                if (!readPhysicalItemMetadata(it,&metadata,hd)) continue;
                bool dup = false;
                for (unsigned int j = 0; j < n; ++j)
                    if (memcmp(out[j].hand,hd,sizeof(hd))==0) { dup=true; break; }
                if (dup) continue;
                Ogre::Vector3 p = it->getPosition();
                WorldItemRaw& w = out[n];
                memcpy(w.hand,hd,sizeof(hd));
                memcpy(w.stringID,metadata.stringID,sizeof(w.stringID));
                w.itemType = metadata.itemType;
                w.quantity = metadata.quantity;
                w.quality = metadata.quality;
                w.x = p.x; w.y = p.y; w.z = p.z;
                w.hash = worldItemHash(w.stringID, w.itemType, w.quantity, w.quality);
                ++n;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return n;
    }
    return n;
}

RootObject* spawnWorldItemProxy(GameWorld* gw, const char* sid, unsigned int typeCat,
                                int qty, float x, float y, float z,
                                unsigned int* outHand) {
    if (outHand) for (int i = 0; i < 5; ++i) outHand[i] = 0;
    if (!gw || !gw->theFactory || !g_createObjFn || !sid || !sid[0]) return 0;
    __try {
        GameData* tmpl = findItemTemplateImpl(gw, sid, typeCat);
        if (!tmpl) return 0;
        Ogre::Vector3 pos(x, y, z);
        Ogre::Quaternion rot = Ogre::Quaternion::IDENTITY;
        // owner=0 (unowned ground item), invisible=false (must render), no home building.
        RootObjectBase* ro = g_createObjFn(gw->theFactory, tmpl, pos, /*fromMod*/false,
                                           /*owner*/0, rot, /*cb*/0, /*container*/0,
                                           /*state*/0, /*invisible*/false, /*home*/0,
                                           /*age*/0.0f);
        if (!ro) return 0;
        // RootObjectBase is the topmost base (offset 0), so this is an address no-op
        // (same pattern as createBuilding -> RootObject*).
        RootObject* obj = reinterpret_cast<RootObject*>(ro);
        // Stacks: the factory mints a single unit; set the visible stack quantity.
        if (qty > 1) { __try { reinterpret_cast<Item*>(obj)->quantity = qty; } __except (EXCEPTION_EXECUTE_HANDLER) {} }
        // Round-trip the hand (both helpers carry their own SEH). Storing one that
        // does not resolve back to this object would be worse than storing none:
        // the caller reads an unresolvable hand as "the engine already destroyed
        // it" and drops the mapping, stranding the proxy on the ground forever.
        if (outHand) {
            unsigned int h[5] = { 0, 0, 0, 0, 0 };
            if (readObjectHand(obj, h) && resolveObjectByHand(h) == obj)
                for (int i = 0; i < 5; ++i) outHand[i] = h[i];
        }
        return obj;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

void updateWorldItemProxy(RootObject* proxy, float x, float y, float z) {
    if (!proxy) return;
    __try {
        Ogre::Vector3 pos(x, y, z);
        Ogre::Quaternion rot = Ogre::Quaternion::IDENTITY;
        // setPositionRotation is virtual on Item; the proxy IS an Item.
        reinterpret_cast<Item*>(proxy)->setPositionRotation(pos, rot, /*fixedPosition*/true);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

bool removeWorldItemProxy(GameWorld* gw, RootObject* proxy) {
    if (!gw || !proxy || !g_destroyObjFn) return false;
    __try {
        return g_destroyObjFn(gw, proxy, /*justUnloaded*/false, "coop-worlditem-cull");
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

int destroyWorldItemsNear(GameWorld* gw, float radius) {
    if (!gw || !gw->player || !g_getObjsFn || !g_destroyObjFn) return 0;
    int destroyed = 0;
    __try {
        if (gw->player->playerCharacters.size() == 0) return 0;
        Character* ld = gw->player->playerCharacters[0];
        if (!ld) return 0;
        Ogre::Vector3 center = ld->getPosition();
        // Collect distinct ground items first (the query enumerates one object under
        // multiple category filters), THEN destroy - destroying mutates the world, so we
        // must not destroy mid-enumeration of the shared scratch buffer.
        RootObject* victims[64]; unsigned int nv = 0;
        const itemType kinds[] = { ITEM, WEAPON, ARMOUR };
        for (int k = 0; k < 3 && nv < 64; ++k) {
            g_npcQuery.clear();
            g_getObjsFn(gw, &g_npcQuery, &center, radius, kinds[k], 256, 0);
            unsigned int total = g_npcQuery.size();
            for (unsigned int i = 0; i < total && nv < 64; ++i) {
                RootObject* o = g_npcQuery[i]; if (!o) continue;
                bool dup = false;
                for (unsigned int j = 0; j < nv; ++j) if (victims[j] == o) { dup = true; break; }
                if (!dup) victims[nv++] = o;
            }
        }
        for (unsigned int i = 0; i < nv; ++i) {
            if (g_destroyObjFn(gw, victims[i], /*justUnloaded*/false, "coop-worlditem-test-despawn"))
                ++destroyed;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return destroyed; }
    return destroyed;
}

// SEH-guarded DIAGNOSTIC: dump every nearby item (across ITEM/WEAPON/ARMOUR queries) so we
// can see what the engine actually reports for a UI-dropped weapon - its gd->type, its
// isInInventory flag, and distance. Used once when a drop-decrease can't be correlated to a
// ground copy, to learn WHY (not enumerated at all? enumerated but flagged in-inventory?).
void diagGroundScan(GameWorld* gw, const unsigned int cHand[5], const char* sid, float radius) {
    if (!gw || !g_getObjsFn) return;
    RootObject* ro = resolveObjectByHand(cHand);
    __try {
        // Resolve both candidate query centers: the OWNED character (what the detector uses)
        // and the player leader (what the working W1 captureWorldItems uses). Log both so we
        // can tell whether the owned-hand position is the problem.
        Ogre::Vector3 cpos(0,0,0); bool haveC = false;
        if (ro) { cpos = ro->getPosition(); haveC = true; }
        Ogre::Vector3 lpos(0,0,0); bool haveL = false;
        if (gw->player && gw->player->playerCharacters.size() > 0 && gw->player->playerCharacters[0]) {
            lpos = gw->player->playerCharacters[0]->getPosition(); haveL = true;
        }
        float dCL = (haveC && haveL)
            ? (float)sqrt((cpos.x-lpos.x)*(cpos.x-lpos.x) + (cpos.y-lpos.y)*(cpos.y-lpos.y) + (cpos.z-lpos.z)*(cpos.z-lpos.z))
            : -1.0f;
        char hb[220]; _snprintf(hb, sizeof(hb) - 1,
            "[wd] scan ownedPos=%.1f,%.1f,%.1f leaderPos=%.1f,%.1f,%.1f apart=%.1f wideR=%.0f",
            cpos.x, cpos.y, cpos.z, lpos.x, lpos.y, lpos.z, dCL, radius * 4.0f);
        hb[sizeof(hb) - 1] = '\0'; coop::logLine(hb);
        // Scan a WIDE radius around the LEADER (the center that works for W1) so a far/cursor
        // drop is still caught. Log every candidate with its distance from BOTH centers.
        Ogre::Vector3 center = haveL ? lpos : cpos;
        float wideR = radius * 4.0f;
        const itemType kinds[] = { ITEM, WEAPON, ARMOUR };
        const char* knm[] = { "ITEM", "WEAPON", "ARMOUR" };
        int logged = 0;
        for (int k = 0; k < 3; ++k) {
            g_npcQuery.clear();
            g_getObjsFn(gw, &g_npcQuery, &center, wideR, kinds[k], 256, 0);
            unsigned int n = g_npcQuery.size();
            char qb[120]; _snprintf(qb, sizeof(qb) - 1, "[wd] scan query=%s found=%u (leader-centered, R=%.0f)", knm[k], n, wideR);
            qb[sizeof(qb) - 1] = '\0'; coop::logLine(qb);
            for (unsigned int i = 0; i < n && logged < 30; ++i) {
                RootObject* o = g_npcQuery[i]; if (!o) continue;
                Item* it = reinterpret_cast<Item*>(o);
                GameData* gd = 0; __try { gd = it->getGameData(); } __except (EXCEPTION_EXECUTE_HANDLER) { gd = 0; }
                if (!gd) continue;
                int inInv = 0; __try { inInv = it->isInInventory ? 1 : 0; } __except (EXCEPTION_EXECUTE_HANDLER) {}
                Ogre::Vector3 p(0,0,0); __try { p = it->getPosition(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
                float dL = (float)sqrt((p.x-lpos.x)*(p.x-lpos.x)+(p.y-lpos.y)*(p.y-lpos.y)+(p.z-lpos.z)*(p.z-lpos.z));
                float dC = (float)sqrt((p.x-cpos.x)*(p.x-cpos.x)+(p.y-cpos.y)*(p.y-cpos.y)+(p.z-cpos.z)*(p.z-cpos.z));
                char b[240]; _snprintf(b, sizeof(b) - 1,
                    "[wd]   cand sid='%s' type=%u inInv=%d dLeader=%.1f dOwned=%.1f%s",
                    gd->stringID.c_str(), (unsigned)gd->type, inInv, dL, dC,
                    (sid && sid[0] && strcmp(gd->stringID.c_str(), sid) == 0) ? " <== MATCH" : "");
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
                ++logged;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// SEH-guarded: count FREE ground items (isInInventory==false) of (sid,type) within radius
// of the object at cHand. "Free" excludes the character's own worn/held copies, which the
// interest query also enumerates - so this isolates the item actually lying on the ground.
int countFreeGroundItemsNear(GameWorld* gw, const unsigned int cHand[5],
                             const char* sid, unsigned int typeCat, float radius) {
    if (!gw || !sid || !sid[0] || !g_getObjsFn) return 0;
    RootObject* ro = resolveObjectByHand(cHand);
    if (!ro) return 0;
    int count = 0;
    __try {
        Ogre::Vector3 center = ro->getPosition();
        // A dropped item enumerates under multiple category queries (W0 finding) - a
        // UI-dropped weapon often shows up only under ITEM, not WEAPON. Scan all three
        // and DEDUPE by object pointer so the same ground item isn't counted twice.
        const itemType kinds[] = { ITEM, WEAPON, ARMOUR };
        RootObject* seen[64]; unsigned int ns = 0;
        for (int k = 0; k < 3; ++k) {
            g_npcQuery.clear();
            g_getObjsFn(gw, &g_npcQuery, &center, radius, kinds[k], 256, 0);
            unsigned int n = g_npcQuery.size();
            for (unsigned int i = 0; i < n; ++i) {
                RootObject* o = g_npcQuery[i]; if (!o) continue;
                Item* it = reinterpret_cast<Item*>(o);   // WEAPON/ARMOUR/ITEM objects are Items
                GameData* gd = it->getGameData(); if (!gd) continue;
                if ((unsigned int)gd->type != typeCat) continue;
                if (strcmp(gd->stringID.c_str(), sid) != 0) continue;
                if (it->isInInventory) continue;          // skip the char's own worn/held copies
                bool dup = false;
                for (unsigned int j = 0; j < ns; ++j) if (seen[j] == o) { dup = true; break; }
                if (dup) continue;
                if (ns < 64) seen[ns++] = o;
                ++count;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return count;
}

// SEH-guarded SPIKE: pick up a FREE ground item of (sid,type) near cHand by RELOCATING the
// REAL object into the character's inventory (tryAddItem) - NO createItem. This is the
// conservation primitive that makes weapons work where fabrication fails: the dropped
// weapon already exists as a real object, so we just re-home it. Returns 1 on success.
int pickupWorldItemIntoInventory(GameWorld* gw, const unsigned int cHand[5],
                                 const char* sid, unsigned int typeCat, float radius) {
    if (!gw || !sid || !sid[0] || !g_getObjsFn) return 0;
    RootObject* ro = resolveObjectByHand(cHand);
    if (!ro) return 0;
    Inventory* inv = invOf(ro);
    if (!inv) return 0;
    int picked = 0;
    __try {
        Ogre::Vector3 center = ro->getPosition();
        // Scan all categories (a dropped weapon often enumerates under ITEM, not WEAPON).
        const itemType kinds[] = { ITEM, WEAPON, ARMOUR };
        for (int k = 0; k < 3 && !picked; ++k) {
            g_npcQuery.clear();
            g_getObjsFn(gw, &g_npcQuery, &center, radius, kinds[k], 256, 0);
            unsigned int n = g_npcQuery.size();
            for (unsigned int i = 0; i < n; ++i) {
                RootObject* o = g_npcQuery[i]; if (!o) continue;
                Item* it = reinterpret_cast<Item*>(o);
                GameData* gd = it->getGameData(); if (!gd) continue;
                if ((unsigned int)gd->type != typeCat) continue;
                if (strcmp(gd->stringID.c_str(), sid) != 0) continue;
                if (it->isInInventory) continue;          // only a free ground item
                if (inv->tryAddItem(it, 1)) { picked = 1; break; } // relocate real object: bag <- ground
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return picked;
}

// SEH-guarded: position of the FIRST free ground item of (sid,type) within radius of cHand.
// Fills out[3] and returns 1 if found, else 0. The drop detector uses this to put the
// dropped weapon's world position on the wire so the peer mirrors it.
int firstFreeGroundItemPos(GameWorld* gw, const unsigned int cHand[5],
                           const char* sid, unsigned int typeCat, float radius, float out[3]) {
    if (!gw || !sid || !sid[0] || !g_getObjsFn || !out) return 0;
    RootObject* ro = resolveObjectByHand(cHand);
    if (!ro) return 0;
    int found = 0;
    __try {
        Ogre::Vector3 center = ro->getPosition();
        // A UI-dropped weapon often enumerates only under the ITEM category, not WEAPON
        // (W0 finding) - scan all three and filter by gd->type so we still locate it.
        const itemType kinds[] = { ITEM, WEAPON, ARMOUR };
        for (int k = 0; k < 3 && !found; ++k) {
            g_npcQuery.clear();
            g_getObjsFn(gw, &g_npcQuery, &center, radius, kinds[k], 256, 0);
            unsigned int n = g_npcQuery.size();
            for (unsigned int i = 0; i < n; ++i) {
                RootObject* o = g_npcQuery[i]; if (!o) continue;
                Item* it = reinterpret_cast<Item*>(o);
                GameData* gd = it->getGameData(); if (!gd) continue;
                if ((unsigned int)gd->type != typeCat) continue;
                if (strcmp(gd->stringID.c_str(), sid) != 0) continue;
                if (it->isInInventory) continue;
                Ogre::Vector3 p = it->getPosition();
                out[0] = p.x; out[1] = p.y; out[2] = p.z; found = 1; break;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return found;
}

// SEH-guarded: world position of the object at hand. Fills out[3], returns 1 on success.
// The drop detector uses this as the drop position when the engine's spatial item query
// can't locate the dropped weapon on the ground (e.g. in towns) - the weapon was dropped at
// the owner's feet anyway, so the owner's position is a faithful mirror target.
int objectWorldPos(const unsigned int hand[5], float out[3]) {
    if (!out) return 0;
    RootObject* ro = resolveObjectByHand(hand);
    if (!ro) return 0;
    int ok = 0;
    __try {
        Ogre::Vector3 p = ro->getPosition();
        out[0] = p.x; out[1] = p.y; out[2] = p.z; ok = 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
    return ok;
}

// SEH-guarded (Phase W1b): handle-based liveness of a tracked ground item. Resolve the
// item's local engine hand and decide whether it is still a FREE ground item, WITHOUT
// the getObjectsWithinSphere query that fails in towns. Returns 1 (and fills out[3] with
// the current world position) if the item still exists and is NOT inside an inventory;
// returns 0 if it was destroyed (hand no longer resolves) or picked up (isInInventory).
// This replaces the "vanished from the spatial scan" cull, which false-culled town drops.
int groundObjectLiveness(RootObject* ro, float out[3], bool* outPickedUp) {
    if (outPickedUp) *outPickedUp = false;
    if (!ro) return 0;
    int live = 0;
    __try {
        if (!ro->typeIsAnItem()) return 0;
        Item* it = static_cast<Item*>(ro);
        if (it->isInInventory) {
            // Distinguishable from "destroyed": the object still reads, it has just
            // entered a bag. Only THIS case licenses a W1 claim - claiming on a
            // destroyed proxy would delete the author's item for no reason.
            if (outPickedUp) *outPickedUp = true;
            return 0;
        }
        Ogre::Vector3 p = ro->getPosition();
        if (out) { out[0] = p.x; out[1] = p.y; out[2] = p.z; }
        live = 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { live = 0; }
    return live;
}

int groundItemLiveness(const unsigned int itemHand[5], float out[3]) {
    RootObject* ro = resolveObjectByHand(itemHand);
    if (!ro) return 0; // destroyed / no longer resolvable
    return groundObjectLiveness(ro, out, 0);
}

namespace {
bool physicalItemOnGround(Item* item) {
    typedef bool (__fastcall* OnGroundFn)(const InventoryItemBase*);
    static OnGroundFn onGround=(OnGroundFn)KenshiLib::GetRealAddress(
        &InventoryItemBase::onGround);
    return onGround && item && !item->isInInventory && !item->isEquipped && onGround(item);
}
bool inventoryContainsPhysicalItem(Inventory* inventory,Item* item) {
    if (!inventory || !item) return false;
    lektor<Item*>& all=inventory->_allItems;
    for (unsigned int i=0;i<all.size();++i) if (all[i]==item) return true;
    lektor<InventorySection*>* sections=g_getSectionsFn ? g_getSectionsFn(inventory) : 0;
    for (unsigned int s=0;sections && s<sections->size();++s) {
        InventorySection* section=(*sections)[s];
        if (!section) continue;
        const Ogre::vector<InventorySection::SectionItem>::type& entries=section->items;
        for (unsigned int i=0;i<entries.size();++i) if (entries[i].item==item) return true;
    }
    return (g_getPrimaryWeaponFn && g_getPrimaryWeaponFn(inventory)==item) ||
           (g_getSecondaryWeaponFn && g_getSecondaryWeaponFn(inventory)==item);
}
}

bool physicalItemIsOnGround(Item* item) {
    if (!item) return false;
    __try {
        unsigned int hand[5];
        RootObject* object=static_cast<RootObject*>(item);
        return object->typeIsAnItem() && readObjectHand(object,hand) &&
            resolveObjectByHand(hand)==object && physicalItemOnGround(item);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool readPhysicalItemMetadata(Item* item,InvItemEntry* out,unsigned int outHand[5]) {
    if (!item || !out || !outHand) return false;
    memset(out,0,sizeof(*out));
    memset(outHand,0,5*sizeof(unsigned int));
    __try {
        RootObject* object=static_cast<RootObject*>(item);
        if (!object->typeIsAnItem() || !readObjectHand(object,outHand) ||
            resolveObjectByHand(outHand)!=object) return false;
        GameData* data=item->getGameData();
        if (!data || data->stringID.empty() ||
            data->stringID.size()>=sizeof(out->stringID) ||
            item->quantity<1 || item->quantity>65535) return false;
        strcpy(out->stringID,data->stringID.c_str());
        out->itemType=(unsigned int)data->type;
        out->quantity=(unsigned short)item->quantity;
        out->quality=qualityBucketOf(item->quality);
        out->level=gradeLevelOf(item);
        out->equipped=item->isEquipped ? 1 : 0;
        out->slot=(unsigned char)((unsigned int)item->slotType&0xffu);
        if (out->equipped) out->section=sectionNameHash(item->inventorySection.c_str());
        GameData* maker=item->manufacturerData;
        GameData* material=item->materialData;
        if ((maker && maker->stringID.size()>=sizeof(out->manufacturer)) ||
            (material && material->stringID.size()>=sizeof(out->material))) return false;
        if (maker) strcpy(out->manufacturer,maker->stringID.c_str());
        if (material) strcpy(out->material,material->stringID.c_str());
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool readPhysicalInventoryRootOwner(RootObject* inventoryOwner,unsigned int out[5]) {
    if (!inventoryOwner || !out) return false;
    memset(out,0,5*sizeof(unsigned int));
    __try {
        RootObject* visited[8];
        unsigned int count=0;
        RootObject* owner=inventoryOwner;
        while (owner && count<8) {
            for (unsigned int i=0;i<count;++i) if (visited[i]==owner) return false;
            visited[count++]=owner;
            unsigned int key[5];
            if (!readObjectHand(owner,key) || resolveObjectByHand(key)!=owner) return false;
            if (!owner->typeIsAnItem()) {
                if (!invOf(owner)) return false;
                memcpy(out,key,sizeof(key));
                return true;
            }
            Item* bag=static_cast<Item*>(owner);
            if (!bag->isInInventory) return false;
            const hand& parent=bag->_whosInventoryWeAreIn;
            unsigned int parentHand[5]={(unsigned int)parent.type,parent.container,
                parent.containerSerial,parent.index,parent.serial};
            owner=resolveObjectByHand(parentHand);
        }
        return false; // no guessed ancestor, cycle, or unbounded container traversal
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool readPhysicalItemLocation(const unsigned int itemHand[5],unsigned int inventoryOwner[5],
                              bool* inInventory,void** outItem) {
    if (inventoryOwner) memset(inventoryOwner,0,5*sizeof(unsigned int));
    if (inInventory) *inInventory=false;
    if (outItem) *outItem=0;
    if (!itemHand || !inventoryOwner || !inInventory || !outItem) return false;
    RootObject* object=resolveObjectByHand(itemHand);
    if (!object) return false; // unloading/unreadable is never a destruction verdict
    __try {
        if (!object->typeIsAnItem()) return false;
        unsigned int actual[5];
        if (!readObjectHand(object,actual) || memcmp(actual,itemHand,sizeof(actual))!=0) return false;
        Item* item=static_cast<Item*>(object);
        if (item->isInInventory) {
            const hand& owner=item->_whosInventoryWeAreIn;
            inventoryOwner[0]=(unsigned int)owner.type;
            inventoryOwner[1]=owner.container; inventoryOwner[2]=owner.containerSerial;
            inventoryOwner[3]=owner.index; inventoryOwner[4]=owner.serial;
            RootObject* container=resolveObjectByHand(inventoryOwner);
            Inventory* inventory=container ? invOf(container) : 0;
            if (!inventory) return false;
            // The native owner hand can remain stale during detachment. Only
            // actual membership licenses a positive pickup transition.
            if (!inventoryContainsPhysicalItem(inventory,item) ||
                !readPhysicalInventoryRootOwner(container,inventoryOwner)) return false;
            *inInventory=true;
        } else if (!physicalItemOnGround(item)) return false;
        *outItem=item;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool describeGroundItem(const unsigned int itemHand[5], InvItemEntry* out, void** outItem) {
    if (outItem) *outItem=0;
    if (!out || !itemHand) return false;
    RootObject* object=resolveObjectByHand(itemHand);
    if (!object) return false;
    Item* item=static_cast<Item*>(object);
    unsigned int actual[5];
    if (!readPhysicalItemMetadata(item,out,actual) ||
        memcmp(actual,itemHand,sizeof(actual))!=0 ||
        !isConservedItemType(out->itemType) || !physicalItemIsOnGround(item)) return false;
    if (outItem) *outItem=item;
    return true;
}

// SEH-guarded (Phase W3): world position of a tracked Item* (as void*). The drop detector
// reads this off the REAL just-dropped object to author the exact cursor-drop location -
// far more accurate than the owner's feet, and it never relies on the (town-unreliable)
// spatial query. Fills out[3], returns 1 on success.
int itemWorldPos(void* item, float out[3]) {
    if (!out || !item) return 0;
    int ok = 0;
    __try {
        Ogre::Vector3 p = reinterpret_cast<Item*>(item)->getPosition();
        out[0] = p.x; out[1] = p.y; out[2] = p.z; ok = 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
    return ok;
}

namespace {
void rememberScenarioPickup(GameWorld*,void*,const unsigned int before[5],const unsigned int after[5]);
bool samePhysicalItemIdentity(const InvItemEntry& a,const InvItemEntry& b) {
    return a.itemType==b.itemType && a.quality==b.quality && a.level==b.level &&
        strcmp(a.stringID,b.stringID)==0 && strcmp(a.manufacturer,b.manufacturer)==0 &&
        strcmp(a.material,b.material)==0;
}
}

// Relocate only a uniquely identified EXISTING physical item. SID alone must
// never select a spare of another grade, maker, material, or condition.
int relocateWeaponToGround(GameWorld* gw,const unsigned int ownerHand[5],
                           const InvItemEntry& identity,float x,float y,float z,void** outDropped) {
    if (outDropped) *outDropped=0;
    if (!gw || !identity.stringID[0] || !isConservedItemType(identity.itemType)) return 0;
    RootObject* owner=resolveObjectByHand(ownerHand);
    Inventory* inventory=owner ? invOf(owner) : 0;
    if (!inventory) return 0;
    InvItemEntry entries[INV_ITEMS_MAX];
    Item* items[INV_ITEMS_MAX];
    bool truncated=false;
    unsigned int count=readInvItems(inventory,entries,items,INV_ITEMS_MAX,&truncated,true);
    if (truncated || count>=INV_ITEMS_MAX) return 0; // a capped read cannot prove uniqueness
    int chosen=-1;
    unsigned int matches=0;
    for (unsigned int i=0;i<count;++i) {
        if (!items[i] || !samePhysicalItemIdentity(entries[i],identity)) continue;
        if (entries[i].equipped!=identity.equipped ||
            (identity.equipped &&
             (entries[i].slot!=identity.slot || entries[i].section!=identity.section))) continue;
        bool duplicate=false;
        for (unsigned int prior=0;prior<i;++prior)
            if (items[prior]==items[i]) { duplicate=true; break; }
        if (duplicate) continue;
        ++matches; chosen=(int)i;
    }
    if (matches!=1) return 0; // exact source context and physical uniqueness, even for one candidate
    if (chosen<0) return 0;
    Item* victim=items[chosen];
    Inventory* actualInventory=inventory;
    bool nested=entries[chosen].parentIdx!=0;
    int moved=0;
    bool vetoSaved=g_invVetoSuspend; g_invVetoSuspend=true;
    __try {
        if (nested) {
            unsigned int parent=entries[chosen].parentIdx-1;
            if (parent>=count || !items[parent]) actualInventory=0;
            else actualInventory=items[parent]->getInventory();
        }
        InvItemEntry actual;
        unsigned int actualHand[5];
        if (actualInventory && readPhysicalItemMetadata(victim,&actual,actualHand) &&
            samePhysicalItemIdentity(actual,identity)) {
            // Inventory::dropItem accepts the exact loose OR equipped Item*.
            // Do not detach an arbitrary same-SID item through unequip helpers.
            actualInventory->dropItem(victim);
            if (physicalItemOnGround(victim)) {
                unsigned int afterHand[5];
                if (readObjectHand(victim,afterHand) && resolveObjectByHand(afterHand)==victim)
                    rememberScenarioPickup(gw,victim,actualHand,afterHand);
                // Preserve this pointer even if positioning fails. The caller
                // retains the physical track and retries, never drops a second copy.
                if (outDropped) *outDropped=victim;
                Ogre::Vector3 position(x,y,z);
                Ogre::Quaternion rotation=Ogre::Quaternion::IDENTITY;
                victim->setPositionRotation(position,rotation,true);
                Ogre::Vector3 observed=victim->getPosition();
                float dx=observed.x-x,dy=observed.y-y,dz=observed.z-z;
                if (dx*dx+dy*dy+dz*dz<=0.25f) moved=1;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { moved=0; }
    g_invVetoSuspend=vetoSaved;
    if (moved && nested) {
        char line[200]; _snprintf(line,sizeof(line)-1,
            "[wd] RELOCATE-NESTED sid='%s' type=%u (found inside a carried container, "
            "not at top level)",identity.stringID,identity.itemType);
        line[sizeof(line)-1]='\0'; coop::logLine(line);
    }
    return moved;
}

// SEH-guarded (Phase W3): capture the WEAPON items the object at cHand currently holds, with
// their REAL Item* handles. The drop detector remembers these per tick so that when a weapon
// leaves the bag (drop), the prior tick's handle IS the now-grounded object - trackable for a
// later pickup without re-querying the world (the spatial query fails in towns). Fills
// sids[i] (NUL-terminated, <48) and items[i] (Item* as void*); returns the count.
unsigned int captureWeaponPtrs(GameWorld* gw, const unsigned int cHand[5],
                               char (*sids)[48], void** items, unsigned int maxOut) {
    if (!gw || !sids || !items || maxOut == 0) return 0;
    RootObject* ro = resolveObjectByHand(cHand);
    if (!ro) return 0;
    Inventory* inv = invOf(ro);
    if (!inv) return 0;
    const unsigned int MAXC = 64;
    InvItemEntry ent[64];
    Item* its[64];
    unsigned int ncur = readInvItems(inv, ent, its, MAXC);
    unsigned int n = 0;
    for (unsigned int i = 0; i < ncur && n < maxOut; ++i) {
        // Conserved gear only: the conservation channel tracks these by real Item*
        // handle. Stackable items stay on the W1 proxy stream.
        if (!isConservedItemType(ent[i].itemType)) continue;
        strncpy(sids[n], ent[i].stringID, 47); sids[n][47] = '\0';
        items[n] = its[i];
        ++n;
    }
    return n;
}

// SEH-guarded (Phase W3): re-home a tracked ground Item* back into the inventory of the
// object at targetHand (Inventory::tryAddItem of the EXISTING object - no fabrication). This
// is the pickup mirror of relocateWeaponToGround. Returns 1 on success. The caller guarantees
// `item` is a real, still-live object it dropped earlier (conservation); SEH guards a stale
// handle from crashing, but a destroyed object should never be passed.
namespace {
struct ScenarioPickupTransition {
    GameWorld* world; void* item; unsigned int generation,before[5],after[5];
};
ScenarioPickupTransition scenarioPickups[64];
unsigned int scenarioPickupCursor=0;
void rememberScenarioPickup(GameWorld* gw,void* item,const unsigned int before[5],
                            const unsigned int after[5]) {
    ScenarioPickupTransition& row=scenarioPickups[scenarioPickupCursor++%64];
    row.world=gw; row.item=item; row.generation=groundItemIdentityGeneration();
    memcpy(row.before,before,sizeof(row.before)); memcpy(row.after,after,sizeof(row.after));
}
}
bool readScenarioItemAfter(GameWorld* gw,const unsigned int before[5],unsigned int after[5]) {
    unsigned int generation=groundItemIdentityGeneration();
    for (unsigned int n=0;n<64 && n<scenarioPickupCursor;++n) {
        const ScenarioPickupTransition& first=scenarioPickups[(scenarioPickupCursor-1-n)%64];
        if (first.world!=gw || first.generation!=generation ||
            memcmp(before,first.before,sizeof(first.before))!=0) continue;
        unsigned int current[5]; memcpy(current,first.after,sizeof(current));
        for (unsigned int newer=n;newer>0;--newer) {
            const ScenarioPickupTransition& next=scenarioPickups[(scenarioPickupCursor-newer)%64];
            if (next.world!=gw || next.generation!=generation || next.item!=first.item) continue;
            if (memcmp(current,next.before,sizeof(current))!=0) return false;
            memcpy(current,next.after,sizeof(current));
        }
        if (resolveObjectByHand(current)!=first.item) return false;
        memcpy(after,current,sizeof(current)); return true;
    }
    return false;
}

int addItemPtrToInventory(GameWorld* gw, const unsigned int targetHand[5], void* item) {
    if (!gw || !item) return 0;
    RootObject* ro = resolveObjectByHand(targetHand);
    if (!ro) return 0;
    Inventory* inv = invOf(ro);
    if (!inv) return 0;
    int ok = 0;
    // Sanctioned conservation pickup (W3): re-homing a REAL tracked ground item
    // whose stale _whosInventoryWeAreIn could otherwise look cross-owner to the
    // trade veto. Suspend it so the sanctioned relocation is never refused.
    bool vetoSav = g_invVetoSuspend; g_invVetoSuspend = true;
    __try {
        Item* physical=reinterpret_cast<Item*>(item);
        InvItemEntry metadata; unsigned int before[5],expectedRoot[5];
        if (readPhysicalItemMetadata(physical,&metadata,before) &&
            physicalItemIsOnGround(physical) && readPhysicalInventoryRootOwner(ro,expectedRoot) &&
            inv->tryAddItem(physical,1)) {
            unsigned int after[5],actualRoot[5]; bool inBag=false; void* actual=0;
            if (readObjectHand(physical,after) &&
                readPhysicalItemLocation(after,actualRoot,&inBag,&actual) && inBag &&
                actual==physical && memcmp(expectedRoot,actualRoot,sizeof(actualRoot))==0) {
                rememberScenarioPickup(gw,physical,before,after);
                ok=1;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
    g_invVetoSuspend = vetoSav;
    return ok;
}

// Actual-engine scenario fixture only: one explicit additional loose native
// object. Production DROP application never calls the factory.
bool seedGearIdentityVariant(GameWorld* gw,const unsigned int ownerHand[5],
                              const InvItemEntry& identity,void** outItem) {
    if (outItem) *outItem=0;
    if (!gw || !outItem || identity.quantity!=1 || identity.equipped ||
        !isConservedItemType(identity.itemType)) return false;
    RootObject* owner=resolveObjectByHand(ownerHand);
    Inventory* inventory=owner ? invOf(owner) : 0;
    unsigned int expectedRoot[5];
    if (!inventory || !readPhysicalInventoryRootOwner(owner,expectedRoot)) return false;
    Item* fixture=fabricateItem(gw,identity.stringID,identity.itemType,identity.quality,
                               identity.manufacturer,identity.material,identity.level);
    if (!fixture) return false;
    bool valid=false;
    __try {
        if (inventory->tryAddItem(fixture,1)) {
            // tryAddItem may choose a spare weapon slot. Stage this fixture
            // through the native loose-section path, never forge worn flags.
            if (fixture->isEquipped) unequipToLoose(inventory,fixture);
            InvItemEntry actual; memset(&actual,0,sizeof(actual));
            unsigned int hand[5]={0},root[5]={0}; bool inBag=false; void* physical=0;
            bool metadataRead=readPhysicalItemMetadata(fixture,&actual,hand);
            valid=metadataRead &&
                samePhysicalItemIdentity(actual,identity) && actual.quantity==1 && !actual.equipped &&
                readPhysicalItemLocation(hand,root,&inBag,&physical) && inBag && physical==fixture &&
                memcmp(expectedRoot,root,sizeof(root))==0;
            if (!valid) {
                char b[360]; _snprintf(b,sizeof(b)-1,
                    "[scenario] WIDENT fixture-detail metadata=%d expectedLevel=%u actualLevel=%u "
                    "expectedQuality=%u actualQuality=%u equipped=%u inBag=%d sameObject=%d "
                    "sameIdentity=%d expectedRoot=%u,%u,%u,%u,%u actualRoot=%u,%u,%u,%u,%u",
                    metadataRead ? 1 : 0,(unsigned)identity.level,(unsigned)actual.level,
                    (unsigned)identity.quality,(unsigned)actual.quality,(unsigned)actual.equipped,
                    inBag ? 1 : 0,physical==fixture ? 1 : 0,
                    metadataRead && samePhysicalItemIdentity(actual,identity) ? 1 : 0,
                    expectedRoot[0],expectedRoot[1],expectedRoot[2],expectedRoot[3],expectedRoot[4],
                    root[0],root[1],root[2],root[3],root[4]);
                b[sizeof(b)-1]='\0'; coop::logLine(b);
            }
        }
        if (!valid && fixture->isInInventory) {
            const hand& holder=fixture->_whosInventoryWeAreIn;
            unsigned int key[5]={(unsigned int)holder.type,holder.container,
                holder.containerSerial,holder.index,holder.serial};
            RootObject* actualOwner=resolveObjectByHand(key);
            Inventory* actualInventory=actualOwner ? invOf(actualOwner) : 0;
            if (inventoryContainsPhysicalItem(actualInventory,fixture))
                actualInventory->removeItemAutoDestroy(fixture,1);
        } else if (!valid) removeWorldItemProxy(gw,fixture);
    } __except (EXCEPTION_EXECUTE_HANDLER) { valid=false; }
    if (!valid) {
        coop::logLine("[scenario] WIDENT fixture rejected (actual native grade/provenance/hand/membership not exact)");
        return false;
    }
    *outItem=fixture;
    return true;
}

bool pickScenarioPhysicalItem(GameWorld* gw,const unsigned int owner[5],const char* sid,
                              unsigned int type,unsigned int outHand[5],InvItemEntry* out) {
    if (!gw || !out || !outHand || !containerInventoryAvailable(owner)) return false;
    Inventory* inv=invOf(resolveObjectByHand(owner));
    InvItemEntry rows[INV_ITEMS_MAX]; Item* items[INV_ITEMS_MAX]; bool truncated=false;
    unsigned int n=readInvItems(inv,rows,items,INV_ITEMS_MAX,&truncated,true);
    if (truncated || n>=INV_ITEMS_MAX) return false;
    unsigned int matches=0;
    for (unsigned int i=0;i<n;++i) {
        if (type ? rows[i].itemType!=type : !isConservedItemType(rows[i].itemType)) continue;
        if (sid && sid[0] && strcmp(sid,rows[i].stringID)!=0) continue;
        InvItemEntry actual; unsigned int key[5],root[5]; bool inBag=false; void* physical=0;
        if (!readPhysicalItemMetadata(items[i],&actual,key) ||
            !readPhysicalItemLocation(key,root,&inBag,&physical) || !inBag ||
            physical!=items[i] || memcmp(root,owner,sizeof(root))!=0) return false;
        if (actual.quantity!=1) return false; // never call a whole-stack drop one unit
        if (++matches>1) return false;
        *out=actual; memcpy(outHand,key,sizeof(key));
    }
    return matches==1;
}

namespace {
bool scenarioBagUnits(GameWorld* gw,const unsigned int owner[5],const char* sid,
                       unsigned int type,int* units) {
    *units=0;
    if (!containerInventoryAvailable(owner)) return false;
    InvItemEntry rows[INV_ITEMS_MAX]; Item* items[INV_ITEMS_MAX]; bool truncated=false;
    unsigned int n=readInvItems(invOf(resolveObjectByHand(owner)),rows,items,INV_ITEMS_MAX,&truncated,true);
    if (truncated || n>=INV_ITEMS_MAX) return false;
    for (unsigned int i=0;i<n;++i) {
        InvItemEntry row; unsigned int key[5],root[5]; bool inBag=false; void* physical=0;
        if (!readPhysicalItemMetadata(items[i],&row,key) ||
            !readPhysicalItemLocation(key,root,&inBag,&physical) || !inBag ||
            physical!=items[i] || memcmp(root,owner,sizeof(root))!=0) return false;
        if (row.itemType==type && strcmp(row.stringID,sid)==0) *units+=row.quantity;
    }
    return true;
}
}
bool sampleScenarioItem(GameWorld* gw,const unsigned int bag[5],
                        const unsigned int source[5],const char* sid,unsigned int type,
                        const unsigned int itemHand[5],ScenarioItemSample* out) {
    if (!gw || !sid || !sid[0] || !out) return false;
    memset(out,0,sizeof(*out));
    if (!scenarioBagUnits(gw,bag,sid,type,&out->bag)) return false;
    if (source && !scenarioBagUnits(gw,source,sid,type,&out->source)) return false;
    // Game-thread only; retain a reusable census buffer instead of a large stack allocation.
    static GroundItemRow ground[4096];
    GroundItemCensusStatus status;
    unsigned int n=captureLoadedGroundItems(gw,ground,4096,&status);
    if (!status.complete) return false;
    for (unsigned int i=0;i<n;++i) {
        bool duplicate=false;
        for (unsigned int j=0;j<i;++j)
            if (memcmp(ground[j].hand,ground[i].hand,sizeof(ground[i].hand))==0) {
                duplicate=true; break;
            }
        if (duplicate) continue;
        InvItemEntry row; unsigned int key[5];
        Item* physical=reinterpret_cast<Item*>(resolveObjectByHand(ground[i].hand));
        if (!readPhysicalItemMetadata(physical,&row,key) ||
            memcmp(key,ground[i].hand,sizeof(key))!=0 || !physicalItemIsOnGround(physical))
            return false; // unclassified floor object is not proven irrelevant
        if (row.itemType==type && strcmp(row.stringID,sid)==0) out->ground+=row.quantity;
    }
    if (itemHand && (itemHand[3] || itemHand[4])) {
        unsigned int root[5],actual[5]; bool inBag=false; void* physical=0; InvItemEntry row;
        if (!readPhysicalItemLocation(itemHand,root,&inBag,&physical) ||
            !readPhysicalItemMetadata(reinterpret_cast<Item*>(physical),&row,actual) ||
            memcmp(actual,itemHand,sizeof(actual))!=0 || row.quantity!=1 ||
            row.itemType!=type || strcmp(row.stringID,sid)!=0) return false;
        memcpy(out->itemHand,actual,sizeof(actual));
        out->metadataHash=invEntryHash(row);
        if (!inBag) out->location=1;
        else if (memcmp(root,bag,sizeof(root))==0) out->location=2;
        else if (source && memcmp(root,source,sizeof(root))==0) out->location=3;
        else return false;
    }
    return true;
}

// Use the original native user mutation path, not the mirror's veto suspension.
int dropPhysicalItemByHand(GameWorld* gw,const unsigned int itemHand[5],
                           unsigned int outAfterItemHand[5]) {
    if (!gw || !itemHand || !outAfterItemHand) return 0;
    unsigned int root[5]; bool inBag=false; void* physical=0;
    if (!readPhysicalItemLocation(itemHand,root,&inBag,&physical) || !inBag) return 0;
    Item* item=reinterpret_cast<Item*>(physical);
    int dropped=0;
    __try {
        const hand& holder=item->_whosInventoryWeAreIn;
        unsigned int key[5]={(unsigned int)holder.type,holder.container,
            holder.containerSerial,holder.index,holder.serial};
        RootObject* actualOwner=resolveObjectByHand(key);
        Inventory* inventory=actualOwner ? invOf(actualOwner) : 0;
        if (inventoryContainsPhysicalItem(inventory,item)) {
            inventory->dropItem(item);
            unsigned int after[5];
            if (physicalItemIsOnGround(item) && readObjectHand(item,after)) {
                rememberScenarioPickup(gw,physical,itemHand,after);
                memcpy(outAfterItemHand,after,sizeof(after));
                dropped=1;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { dropped=0; }
    return dropped;
}

int pickupPhysicalItemByHand(GameWorld* gw,const unsigned int ownerHand[5],
                            const unsigned int itemHand[5],unsigned int outAfterItemHand[5]) {
    if (!gw || !ownerHand || !itemHand || !outAfterItemHand) return 0;
    RootObject* owner=resolveObjectByHand(ownerHand);
    Inventory* inventory=owner ? invOf(owner) : 0;
    unsigned int expectedRoot[5],unused[5]; bool inBag=false; void* physical=0;
    if (!inventory || !readPhysicalInventoryRootOwner(owner,expectedRoot) ||
        !readPhysicalItemLocation(itemHand,unused,&inBag,&physical) || inBag) return 0;
    Item* item=reinterpret_cast<Item*>(physical);
    int moved=0;
    __try {
        if (inventory->tryAddItem(item,1)) {
            unsigned int after[5],actualRoot[5]; void* actual=0;
            if (readObjectHand(item,after) &&
                readPhysicalItemLocation(after,actualRoot,&inBag,&actual) && inBag &&
                actual==physical && memcmp(actualRoot,expectedRoot,sizeof(actualRoot))==0) {
                rememberScenarioPickup(gw,physical,itemHand,after);
                memcpy(outAfterItemHand,after,sizeof(after));
                moved=1;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { moved=0; }
    return moved;
}

bool equipPhysicalItemByHand(GameWorld* gw,const unsigned int ownerHand[5],
                             const unsigned int itemHand[5]) {
    if (!gw || !ownerHand || !itemHand || !g_equipItemFn) return false;
    RootObject* owner=resolveObjectByHand(ownerHand);
    Inventory* inventory=owner ? invOf(owner) : 0;
    unsigned int expected[5],root[5]; bool inBag=false; void* physical=0;
    if (!inventory || !readPhysicalInventoryRootOwner(owner,expected) ||
        !readPhysicalItemLocation(itemHand,root,&inBag,&physical) || !inBag ||
        memcmp(root,expected,sizeof(root))!=0) return false;
    __try {
        Item* item=reinterpret_cast<Item*>(physical);
        if (!g_equipItemFn(inventory,item)) return false;
        InvItemEntry row; unsigned int after[5]; void* actual=0;
        return readPhysicalItemMetadata(item,&row,after) && row.equipped &&
            readPhysicalItemLocation(after,root,&inBag,&actual) && inBag && actual==physical &&
            memcmp(root,expected,sizeof(root))==0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool physicalWeaponIsDrawn(GameWorld* gw,const unsigned int ownerHand[5],
                           const unsigned int itemHand[5]) {
    if (!gw || !ownerHand || !itemHand || !g_getSectionsFn) return false;
    RootObject* owner=resolveObjectByHand(ownerHand);
    Inventory* inventory=owner ? invOf(owner) : 0;
    unsigned int expected[5],root[5]; bool inBag=false; void* physical=0;
    if (!inventory || !readPhysicalInventoryRootOwner(owner,expected) ||
        !readPhysicalItemLocation(itemHand,root,&inBag,&physical) || !inBag ||
        memcmp(root,expected,sizeof(root))!=0) return false;
    __try {
        Item* item=reinterpret_cast<Item*>(physical);
        InvItemEntry row; unsigned int actual[5];
        if (!readPhysicalItemMetadata(item,&row,actual) || row.itemType!=2 || !row.equipped) return false;
        bool held=(g_getPrimaryWeaponFn && g_getPrimaryWeaponFn(inventory)==item) ||
                  (g_getSecondaryWeaponFn && g_getSecondaryWeaponFn(inventory)==item);
        if (!held) return false;
        lektor<InventorySection*>* sections=g_getSectionsFn(inventory);
        if (!sections) return false;
        for (unsigned int s=0;s<sections->size();++s) {
            InventorySection* section=(*sections)[s];
            if (!section) continue;
            const Ogre::vector<InventorySection::SectionItem>::type& entries=section->items;
            for (unsigned int i=0;i<entries.size();++i) if (entries[i].item==item) return false;
        }
        return true; // actual held getter AND absent from all native sections
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// SEH-guarded: report the first EQUIPPED item worn by the object at cHand. Writes its
// template stringID + itemType + equipped count, returns 1 if any worn item exists.
// The inv_equip scenario uses this to drive equip/unequip on a KNOWN, race-compatible
// template (the gear the character already wears), so no template-hunting is needed.
int findEquippedItemKey(GameWorld* gw, const unsigned int cHand[5],
                        char* outSid, unsigned int outLen, unsigned int* outType,
                        int* outEquippedCount) {
    if (outEquippedCount) *outEquippedCount = 0;
    if (!gw) return 0;
    RootObject* ro = resolveObjectByHand(cHand);
    if (!ro) return 0;
    Inventory* inv = invOf(ro);
    if (!inv) return 0;
    const unsigned int MAXC = 64;
    InvItemEntry cur[64];
    unsigned int ncur = readInvItems(inv, cur, 0, MAXC);
    int found = 0; int eqCount = 0;
    for (unsigned int i = 0; i < ncur; ++i) {
        if (!cur[i].equipped) continue;
        ++eqCount;
        if (!found) {
            if (outSid && outLen) { strncpy(outSid, cur[i].stringID, outLen - 1); outSid[outLen - 1] = '\0'; }
            if (outType) *outType = cur[i].itemType;
            found = 1;
        }
    }
    if (outEquippedCount) *outEquippedCount = eqCount;
    return found;
}

// SEH-guarded: remove `qty` of the EQUIPPED (sid, type) worn by the object at cHand
// (the "drop/unequip armour" action). Returns the number removed. Used by inv_equip
// to prove that unequipping/removing worn gear propagates cross-client.
int removeEquippedItem(GameWorld* gw, const unsigned int cHand[5],
                       const char* sid, unsigned int typeCat, int qty) {
    if (!gw || !sid || !sid[0] || qty <= 0) return 0;
    RootObject* ro = resolveObjectByHand(cHand);
    if (!ro) return 0;
    Inventory* inv = invOf(ro);
    if (!inv) return 0;
    const unsigned int MAXC = 64;
    InvItemEntry cur[64];
    Item* curItems[64];
    unsigned int ncur = readInvItems(inv, cur, curItems, MAXC);
    return removeByKey(inv, curItems, cur, ncur, sid, typeCat, 0, 0, qty, /*wantEquipped=*/1);
}

// SEH-guarded: create and EQUIP `qty` of (sid, type) onto the object at cHand (the
// "equip armour" action). Returns the number equipped. Used by inv_equip to prove
// that equipping worn gear propagates cross-client.
int addEquippedItem(GameWorld* gw, const unsigned int cHand[5],
                    const char* sid, unsigned int typeCat, int qty) {
    if (!gw || !sid || !sid[0] || qty <= 0) return 0;
    RootObject* ro = resolveObjectByHand(cHand);
    if (!ro) return 0;
    Inventory* inv = invOf(ro);
    if (!inv) return 0;
    return createItemAndAdd(gw, inv, sid, typeCat, qty, 0, /*equip=*/true) ? qty : 0;
}

// SEH-guarded: UNEQUIP `qty` of the worn (sid,type) on the object at cHand to LOOSE
// inventory WITHOUT destroying it (move slot -> loose; the real "drag worn item into
// the bag" action). Unlike removeEquippedItem (which destroys), this PRESERVES the item
// so it can be re-equipped later. Returns how many moved. Used by inv_reequip.
int unequipItemToLoose(GameWorld* gw, const unsigned int cHand[5],
                       const char* sid, unsigned int typeCat, int qty) {
    if (!gw || !sid || !sid[0] || qty <= 0) return 0;
    RootObject* ro = resolveObjectByHand(cHand);
    if (!ro) return 0;
    Inventory* inv = invOf(ro);
    if (!inv) return 0;
    const unsigned int MAXC = 64;
    InvItemEntry cur[64]; Item* curItems[64];
    unsigned int ncur = readInvItems(inv, cur, curItems, MAXC);
    int moved = 0;
    for (unsigned int i = 0; i < ncur && moved < qty; ++i) {
        if (!curItems[i] || !cur[i].equipped || cur[i].itemType != typeCat) continue;
        if (strcmp(cur[i].stringID, sid) != 0) continue;
        if (unequipToLoose(inv, curItems[i])) ++moved;
    }
    return moved;
}

// SEH-guarded: EQUIP `qty` of the LOOSE (sid,type) the object at cHand already holds
// (move loose -> slot; the real "drag item from bag onto the paperdoll" action). Equips
// a REAL item so it persists (fabricated equips are discarded, d25). Returns how many
// equipped. Used by inv_reequip to drive the UP path on save-loaded gear.
int reequipLooseItem(GameWorld* gw, const unsigned int cHand[5],
                     const char* sid, unsigned int typeCat, int qty) {
    if (!gw || !sid || !sid[0] || qty <= 0) return 0;
    RootObject* ro = resolveObjectByHand(cHand);
    if (!ro) return 0;
    Inventory* inv = invOf(ro);
    if (!inv) return 0;
    const unsigned int MAXC = 64;
    InvItemEntry cur[64]; Item* curItems[64];
    unsigned int ncur = readInvItems(inv, cur, curItems, MAXC);
    int moved = 0;
    for (unsigned int i = 0; i < ncur && moved < qty; ++i) {
        if (!curItems[i] || cur[i].equipped || cur[i].itemType != typeCat) continue;
        if (strcmp(cur[i].stringID, sid) != 0) continue;
        if (equipExisting(inv, curItems[i])) ++moved;
    }
    return moved;
}

// The spike-451 weapon-mint recipe trace (the mkspy createItem/copyItem/Sword-ctor/
// chooseDataFromList detours + installCreateItemTraceHook, probeReplayWeaponMint,
// probeFabricateWeaponLoose, commonNovelWeaponSid) moved to EngineProbe.cpp (Phase
// 5e) - a HARNESS-ONLY TU, so the shipping Release DLL no longer carries the detour
// trampolines. diagWeaponCreate moved there too. createItemAndAdd +
// fallbackWeaponManufacturer stay here (external linkage) since inventory needs them.

// ---- Spike 401: research tech-tree store probe ------------------------------
// Real-RVA surfaces recovered from the on-disk 1.0.65 exe (string-xref +
// disassembly of the "Research already known" caller at 0x2b65d0 - the research
// UI's click handler: canResearch -> isKnown -> startResearch):
//   0x2134690  .data global whose +0x38 slot is the engine's LIVE Research*
//              (matches PlayerInterface::technology @ 0x38 - the global is the
//              PlayerInterface singleton the UI code reads)
//   0x82f300   Research::isKnown(GameData*) -> bool (special path for type 0x15,
//              else walks a vector reached through the container at this+0x10)
//   0x832fa0   Research::canResearch(GameData*, bool, bool) -> bool (the UI
//              passes false,false)
//   0x834550   Research::startResearch(GameData*) (dedupes the queue at
//              +0x38/+0x58, activates, fires the UI toast natively)
// These are ON-DISK 1.0.65 RVAs from disassembling the installed exe, so the
// only correct base is GetModuleHandle(NULL). Deriving the base from a
// KenshiLib-resolved neighbour (run 211124) crashed BOTH clients through the
// SEH guards - KenshiLib's GetRealAddress space need not be base+headerRVA
// (version remap), and a mid-instruction jump can land on __fastfail, which
// no SEH frame catches. The two bases are logged once for the evidence trail.

namespace {

const unsigned int R401_WIN = 0x800; // Research-object snapshot window (bytes)
unsigned char g_r401Snap[0x800];
bool  g_r401Have = false;
void* g_r401Ptr  = 0;

typedef bool (__fastcall* R401BoolGdFn)(void* research, GameData* gd);
typedef bool (__fastcall* R401CanFn)(void* research, GameData* gd,
                                     bool a, bool b);
typedef void (__fastcall* R401StartFn)(void* research, GameData* gd);

// SEH-guarded prologue compare - a raw-RVA call is only allowed when the live
// bytes are EXACTLY the disassembled function's prologue. A wrong base (or a
// different exe build) fails the compare instead of jumping into garbage,
// where a mid-instruction landing can hit __fastfail - unrecoverable by SEH
// (run 211124 killed both clients that way).
bool r401SigOk(unsigned __int64 addr, const unsigned char* sig,
               unsigned int n) {
    __try {
        return memcmp((const void*)addr, sig, n) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

struct R401Levers {
    R401BoolGdFn isKnown;
    R401CanFn    can;
    R401StartFn  start;
};

unsigned __int64 r401Base() {
    static unsigned __int64 base = 0;
    if (!base) base = (unsigned __int64)GetModuleHandleA(NULL);
    return base;
}

// Scan the running module's .text for a unique byte signature. The on-disk exe
// RVAs do NOT map to the live image by base+RVA (run 212346: klib's getTechLevel
// resolved to base+0x2ADE00, 0x470 past the on-disk 0x2AD990, and the on-disk
// bytes there are unrelated - the executing image differs from the file on
// disk). A prologue scan finds each function at its TRUE runtime address no
// matter the skew (each signature was verified count==1 in the file's .text).
// SEH-guarded page walk; returns 0 if not found.
unsigned __int64 r401ScanText(const unsigned char* sig, unsigned int n) {
    unsigned __int64 base = r401Base();
    if (!base || !sig || n == 0) return 0;
    __try {
        const unsigned char* p = (const unsigned char*)base;
        // PE -> optional header -> section table; find .text.
        unsigned int peOff = *(unsigned int*)(p + 0x3c);
        const unsigned char* pe = p + peOff;
        unsigned short nsec = *(unsigned short*)(pe + 6);
        unsigned short optSz = *(unsigned short*)(pe + 20);
        const unsigned char* sec = pe + 24 + optSz;
        unsigned __int64 txtVa = 0, txtSz = 0;
        for (unsigned short i = 0; i < nsec; ++i) {
            const unsigned char* s = sec + i * 40;
            if (memcmp(s, ".text", 5) == 0) {
                txtVa = *(unsigned int*)(s + 12);
                txtSz = *(unsigned int*)(s + 8);
                break;
            }
        }
        if (!txtVa || txtSz < n) return 0;
        const unsigned char* start = p + txtVa;
        const unsigned char* end = start + txtSz - n;
        unsigned char first = sig[0];
        for (const unsigned char* q = start; q <= end; ++q) {
            if (*q != first) continue;
            if (memcmp(q, sig, n) == 0) return (unsigned __int64)q;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return 0;
}

// Locate the three lever entry points once by prologue scan. All-or-nothing:
// any miss disables every raw call this spike makes.
const R401Levers* r401GetLevers() {
    static R401Levers lv = { 0, 0, 0 };
    static int state = 0; // 0 unresolved, 1 ok, -1 refused
    if (state == 0) {
        state = -1;
        // 24-byte prologues (verified unique in the 1.0.65 .text).
        static const unsigned char sigKnown[] = {
            0x48,0x89,0x54,0x24,0x10,0x53,0x48,0x83,0xEC,0x20,0x48,0x8B,
            0xDA,0x48,0x83,0xC2,0x50,0x83,0x3A,0x15,0x74,0x39,0x48,0x83 };
        static const unsigned char sigCan[] = {
            0x40,0x55,0x56,0x57,0x48,0x8D,0x6C,0x24,0xB9,0x48,0x81,0xEC,
            0xA0,0x00,0x00,0x00,0x48,0xC7,0x45,0xFF,0xFE,0xFF,0xFF,0xFF };
        static const unsigned char sigStart[] = {
            0x48,0x8B,0xC4,0x55,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x48,
            0x8D,0xA8,0x38,0xFE,0xFF,0xFF,0x48,0x81,0xEC,0xA0,0x02,0x00 };
        unsigned __int64 a = r401ScanText(sigKnown, sizeof(sigKnown));
        unsigned __int64 b = r401ScanText(sigCan, sizeof(sigCan));
        unsigned __int64 c = r401ScanText(sigStart, sizeof(sigStart));
        char msg[200];
        _snprintf(msg, sizeof(msg) - 1,
                  "[r401] lever scan isKnown=%016llx can=%016llx start=%016llx",
                  a, b, c);
        msg[sizeof(msg) - 1] = '\0'; coop::logLine(msg);
        if (a && b && c) {
            lv.isKnown = (R401BoolGdFn)a;
            lv.can     = (R401CanFn)b;
            lv.start   = (R401StartFn)c;
            state = 1;
            coop::logLine("[r401] levers located by prologue scan");
        } else {
            coop::logLine("[r401] levers REFUSED (prologue not found)");
        }
    }
    return state == 1 ? &lv : 0;
}

// The live Research* is gw->player->technology (the KenshiLib header member at
// PlayerInterface+0x38). The on-disk-RVA global chain was dropped: the running
// image is base-skewed from the file (see r401ScanText), so a base+RVA global
// read returned null (run 212346). The header member resolved to a real object.
void* r401Research(GameWorld* gw) {
    __try {
        if (gw && gw->player) return gw->player->technology;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return 0;
}

// SEH-guarded lookup of a RESEARCH GameData by sid (main-thread scratch).
GameData* r401FindResearch(GameWorld* gw, const char* sid) {
    if (!gw || !sid || !sid[0] || !g_getDataOfTypeFn) return 0;
    __try {
        g_dataScratch.clear();
        g_getDataOfTypeFn(&gw->gamedata, &g_dataScratch, RESEARCH);
        unsigned int n = g_dataScratch.size();
        for (unsigned int i = 0; i < n; ++i) {
            GameData* t = g_dataScratch[i];
            if (!t) continue;
            const char* s = t->stringID.c_str();
            if (s && strcmp(s, sid) == 0) return t;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return 0;
}

// SEH-guarded: true when p reads as a plausible GameData record (small type
// enum + short printable stringID). Bogus pointers fault into the handler or
// fail the plausibility checks - the mkspy classification trick.
bool r401GdProbe(void* p, int* outType, char* outSid, unsigned int outLen) {
    if (!p || ((ULONG_PTR)p & 0x7) || outLen < 2) return false;
    __try {
        GameData* gd = (GameData*)p;
        int t = (int)gd->type;
        if (t < 0 || t > 90) return false;
        const char* s = gd->stringID.c_str();
        if (!s || !s[0]) return false;
        unsigned int i = 0;
        for (; i < outLen - 1 && s[i]; ++i) {
            if ((unsigned char)s[i] < 0x20 || (unsigned char)s[i] > 0x7e)
                return false;
            outSid[i] = s[i];
        }
        outSid[i] = '\0';
        *outType = t;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

} // namespace

int probeResearchStore(GameWorld* gw, int phase) {
    if (!gw) return 0;
    unsigned char cur[R401_WIN];
    void* tech = r401Research(gw);
    if (!tech) return 0;
    // The interim out-of-process probing of this object CRASHED the host, so
    // every read stays in-process under its own SEH frame.
    __try {
        memcpy(cur, tech, R401_WIN);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        coop::logLine("[r401] store read FAULTED");
        return -1;
    }
    char b[240];
    if (tech != g_r401Ptr) { g_r401Ptr = tech; g_r401Have = false; }
    if (phase == 0 || !g_r401Have) {
        memcpy(g_r401Snap, cur, R401_WIN);
        g_r401Have = true;
        _snprintf(b, sizeof(b) - 1, "[r401] store ptr=%p win=0x%x",
                  tech, R401_WIN);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        for (unsigned int off = 0; off < 0x100; off += 32) {
            unsigned __int64 q[4];
            memcpy(q, cur + off, sizeof(q));
            _snprintf(b, sizeof(b) - 1,
                      "[r401] hex 0x%03x: %016llx %016llx %016llx %016llx",
                      off, q[0], q[1], q[2], q[3]);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
        for (unsigned int off = 0; off + 8 <= 0x400; off += 8) {
            void* p = 0;
            memcpy(&p, cur + off, sizeof(p));
            int t = -1; char sid[48];
            if (r401GdProbe(p, &t, sid, sizeof(sid))) {
                _snprintf(b, sizeof(b) - 1,
                          "[r401] slot 0x%03x -> GameData type=%d sid='%s'",
                          off, t, sid);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
        }
        return 1;
    }
    int nd = 0;
    for (unsigned int off = 0; off + 4 <= R401_WIN; off += 4) {
        unsigned int a, c;
        memcpy(&a, g_r401Snap + off, 4);
        memcpy(&c, cur + off, 4);
        if (a == c) continue;
        float fa, fc;
        memcpy(&fa, &a, 4); memcpy(&fc, &c, 4);
        _snprintf(b, sizeof(b) - 1,
                  "[r401] diff 0x%03x %08x->%08x (f %.5g->%.5g)",
                  off, a, c, fa, fc);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        if (++nd >= 40) { coop::logLine("[r401] diff TRUNCATED"); break; }
    }
    if (nd == 0) coop::logLine("[r401] diff none");
    memcpy(g_r401Snap, cur, R401_WIN);
    return 1;
}

int probeCurrentResearchSid(char* outSid, unsigned int outLen) {
    if (outSid && outLen) outSid[0] = '\0';
    // Unresolvable for now: ManagementScreen.h does not compile under VC100
    // (its inline ReorderableList template trips C2065), and the KenshiLib
    // header RVAs (getSingleton 0x2967F0 / singleton 0x212C428) live in
    // GetRealAddress's REMAPPED space, not at base+RVA on the 1.0.65 exe -
    // the on-disk bytes at those offsets are unrelated code (spike-401 run
    // 211124 finding). The current-research identity comes from the store
    // diff instead; a UI read can be re-added if a real on-disk anchor for
    // the ManagementScreen singleton is ever recovered.
    return 0;
}

int researchQueryBySid(GameWorld* gw, const char* sid, int* outKnown,
                       int* outCan) {
    if (outKnown) *outKnown = -1;
    if (outCan)   *outCan   = -1;
    void* res = r401Research(gw);
    const R401Levers* lv = r401GetLevers();
    if (!res || !lv) return 0;
    GameData* gd = r401FindResearch(gw, sid);
    if (!gd) return 0;
    __try {
        if (outKnown) *outKnown = lv->isKnown(res, gd) ? 1 : 0;
        // The research UI's click handler passes (gd, false, false).
        if (outCan)   *outCan   = lv->can(res, gd, false, false) ? 1 : 0;
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

int researchStartBySid(GameWorld* gw, const char* sid) {
    void* res = r401Research(gw);
    const R401Levers* lv = r401GetLevers();
    if (!res || !lv) return 0;
    GameData* gd = r401FindResearch(gw, sid);
    if (!gd) return 0;
    __try {
        lv->start(res, gd);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

int researchPickSubject(GameWorld* gw, char* outSid, unsigned int outLen) {
    if (outSid && outLen) outSid[0] = '\0';
    void* res = r401Research(gw);
    const R401Levers* lv = r401GetLevers();
    if (!gw || !res || !lv || !g_getDataOfTypeFn || !outSid || outLen < 2)
        return 0;
    __try {
        g_dataScratch.clear();
        g_getDataOfTypeFn(&gw->gamedata, &g_dataScratch, RESEARCH);
        unsigned int n = g_dataScratch.size();
        for (unsigned int i = 0; i < n; ++i) {
            GameData* t = g_dataScratch[i];
            if (!t) continue;
            if (lv->isKnown(res, t)) continue;
            if (!lv->can(res, t, false, false)) continue;
            const char* s = t->stringID.c_str();
            if (!s || !s[0]) continue;
            strncpy(outSid, s, outLen - 1);
            outSid[outLen - 1] = '\0';
            return 1;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
    return 0;
}

unsigned int probeResearchEnum(GameWorld* gw, unsigned int maxLog) {
    void* res = r401Research(gw);
    const R401Levers* lv = r401GetLevers();
    if (!gw || !res || !lv || !g_getDataOfTypeFn) return 0;
    unsigned int logged = 0, total = 0, known = 0;
    __try {
        g_dataScratch.clear();
        g_getDataOfTypeFn(&gw->gamedata, &g_dataScratch, RESEARCH);
        unsigned int n = g_dataScratch.size();
        total = n;
        char b[220];
        for (unsigned int i = 0; i < n; ++i) {
            GameData* t = g_dataScratch[i];
            if (!t) continue;
            bool k = lv->isKnown(res, t);
            if (k) ++known;
            if (logged < maxLog) {
                bool c = lv->can(res, t, false, false);
                _snprintf(b, sizeof(b) - 1,
                          "[r401] research[%u] sid='%s' name='%s' known=%d can=%d",
                          i, t->stringID.c_str(), t->name.c_str(),
                          k ? 1 : 0, c ? 1 : 0);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
                ++logged;
            }
        }
        _snprintf(b, sizeof(b) - 1, "[r401] research total=%u known=%u",
                  total, known);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        coop::logLine("[r401] enum FAULTED");
        return logged;
    }
    return total;
}

unsigned int researchEnumKnown(GameWorld* gw, char* outSids,
                               unsigned int sidCap, unsigned int maxN) {
    void* res = r401Research(gw);
    const R401Levers* lv = r401GetLevers();
    if (!gw || !res || !lv || !g_getDataOfTypeFn || !outSids || sidCap < 2)
        return 0;
    unsigned int written = 0;
    __try {
        g_dataScratch.clear();
        g_getDataOfTypeFn(&gw->gamedata, &g_dataScratch, RESEARCH);
        unsigned int n = g_dataScratch.size();
        for (unsigned int i = 0; i < n && written < maxN; ++i) {
            GameData* t = g_dataScratch[i];
            if (!t) continue;
            if (!lv->isKnown(res, t)) continue;
            const char* s = t->stringID.c_str();
            if (!s || !s[0]) continue;
            char* dst = outSids + (size_t)written * sidCap;
            strncpy(dst, s, sidCap - 1);
            dst[sidCap - 1] = '\0';
            ++written;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return written; }
    return written;
}

int probeResearchBenchRead(const unsigned int mHand[5], int* outTech,
                           float* outProg, int* outPower) {
    if (outTech)  *outTech  = -1;
    if (outProg)  *outProg  = -1.0f;
    if (outPower) *outPower = -1;
    if (!mHand) return 0;
    RootObject* ro = resolveObjectByHand(mHand);
    if (!ro) return 0;
    __try {
        Building* bd = static_cast<Building*>(ro);
        if ((int)bd->classType != (int)BCTYPE_RESEARCH) return 0;
        UseableStuff* us = static_cast<UseableStuff*>(bd);
        if (outProg)  *outProg  = us->progressBarLevel;
        if (outPower) *outPower = us->powerOn ? 1 : 0;
        if (outTech && g_machTechLvlFn)
            *outTech = g_machTechLvlFn(static_cast<ResearchBuilding*>(bd));
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

// diagWeaponCreate (the spike-451 weapon-fabrication diagnostic) moved to
// EngineProbe.cpp (Phase 5e, HARNESS-ONLY). It uses the now-external
// fallbackWeaponManufacturer + createItemAndAdd defined above.

// SEH-guarded: find a template the character at cHand can actually WEAR and equip it,
// so the inv_equip scenario has a known worn item to cycle even on a save whose squad
// members start naked. Walks ARMOUR then WEAPON templates, creating+equipping each and
// keeping the first that actually lands in a slot (it->isEquipped); non-wearable trials
// are destroyed so nothing leaks loose. Writes the winning stringID + itemType.
int seedEquippedItem(GameWorld* gw, const unsigned int cHand[5],
                     char* outSid, unsigned int outLen, unsigned int* outType) {
    if (outSid && outLen) outSid[0] = '\0';
    if (outType) *outType = 0;
    if (!gw || !gw->theFactory || !g_createItemFn || !g_handCtorFn ||
        !g_equipItemFn || !g_getDataOfTypeFn) return 0;
    RootObject* ro = resolveObjectByHand(cHand);
    if (!ro) return 0;
    Inventory* inv = invOf(ro);
    if (!inv) return 0;

    const itemType cats[2] = { ARMOUR, WEAPON };
    for (int c = 0; c < 2; ++c) {
        unsigned int n = 0;
        __try {
            g_dataScratch.clear();
            g_getDataOfTypeFn(&gw->gamedata, &g_dataScratch, cats[c]);
            n = g_dataScratch.size();
        } __except (EXCEPTION_EXECUTE_HANDLER) { n = 0; }
        unsigned int tried = 0;
        for (unsigned int i = 0; i < n && tried < 120; ++i) {
            GameData* tmpl = 0;
            __try { tmpl = g_dataScratch[i]; } __except (EXCEPTION_EXECUTE_HANDLER) { tmpl = 0; }
            if (!tmpl) continue;
            ++tried;
            int ok = 0;
            char sidbuf[48]; sidbuf[0] = '\0';
            __try {
                char buf[sizeof(hand) + 16];
                memset(buf, 0, sizeof(buf));
                hand* h = reinterpret_cast<hand*>(buf);
                g_handCtorFn(h, 0, 0, cats[c], 0, 0);
                Item* it = g_createItemFn(gw->theFactory, tmpl, h, 0, 0, -1, 0);
                if (it) {
                    if (inv->tryAddItem(it, 1)) {                  // virtual
                        bool eq = g_equipItemFn(inv, it);
                        if (eq && it->isEquipped) {
                            const char* s = tmpl->stringID.c_str();
                            strncpy(sidbuf, s ? s : "", sizeof(sidbuf) - 1);
                            sidbuf[sizeof(sidbuf) - 1] = '\0';
                            ok = sidbuf[0] ? 1 : 0;
                        } else {
                            inv->removeItemAutoDestroy(it, 1);     // not wearable here
                        }
                    }
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
            if (ok) {
                if (outSid && outLen) { strncpy(outSid, sidbuf, outLen - 1); outSid[outLen - 1] = '\0'; }
                if (outType) *outType = (unsigned int)cats[c];
                return 1;
            }
        }
    }
    return 0;
}

bool setupInventoryScene(GameWorld* gw, unsigned int outHand[5]) {
    if (outHand) { for (int i = 0; i < 5; ++i) outHand[i] = 0; }
    if (!gw || !gw->player) { coop::logLine("SETUP(inventory): no player interface"); return false; }
    unsigned int ch[5];
    if (!pickInventoryContainer(gw, ch)) {
        coop::logLine("SETUP(inventory): could not resolve leader container");
        return false;
    }
    char sid[48]; sid[0] = '\0';
    int added = addTestItemsToContainer(gw, ch, 2, sid, sizeof(sid));
    char b[220];
    _snprintf(b, sizeof(b) - 1,
        "SETUP(inventory): anchor=leader hand=%u,%u,%u,%u,%u seeded=%d x '%s'",
        ch[0], ch[1], ch[2], ch[3], ch[4], added, sid[0] ? sid : "(none)");
    b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    if (outHand) { for (int i = 0; i < 5; ++i) outHand[i] = ch[i]; }
    return true;
}

bool spawnSeatInFront(GameWorld* gw, float fwd, float side, RootObject** spawned) {
    if (spawned) *spawned = 0;
    if (!gw || !gw->theFactory || !g_createBldgFn) {
        coop::logLine("SETUP: seat spawn skipped (no factory / createBuilding fn)");
        return false;
    }
    __try {
        unsigned int nTemplates = 0;
        if (g_getDataOfTypeFn) {
            g_dataScratch.clear();
            g_getDataOfTypeFn(&gw->gamedata, &g_dataScratch, BUILDING);
            nTemplates = g_dataScratch.size();
        }
        GameData* tmpl = findSeatTemplate(gw);
        {
            char d[200];
            _snprintf(d, sizeof(d) - 1, "SETUP: BUILDING templates=%u seatTemplate='%s'",
                      nTemplates, tmpl ? tmpl->name.c_str() : "(none)");
            d[sizeof(d) - 1] = '\0';
            coop::logLine(d);
        }
        if (!tmpl) return false;
        Ogre::Vector3 pos; float yaw = 0.0f;
        if (!leaderAnchor(gw, fwd, side, &pos, &yaw)) return false;
        {
            char d[160];
            _snprintf(d, sizeof(d) - 1, "SETUP: seat reqPos=%.2f,%.2f,%.2f yaw=%.2f",
                      pos.x, pos.y, pos.z, yaw);
            d[sizeof(d) - 1] = '\0';
            coop::logLine(d);
        }
        // Face the seat back toward the leader so a seated body looks outward.
        Ogre::Quaternion rot(Ogre::Radian(yaw + 3.14159265f), Ogre::Vector3::UNIT_Y);
        // createBuilding adds the terrain height to position.y (a seat passed at the
        // absolute ground Y floats ~terrain-height up). Pass y=0 so it re-grounds.
        Ogre::Vector3 placePos(pos.x, 0.0f, pos.z);
        Building* b = g_createBldgFn(
            gw->theFactory, tmpl, placePos, /*town*/0, /*owner*/0, rot, /*cb*/0,
            /*furnitureOf*/0, /*isDoorOf*/0, /*saveState*/0, /*isIndoorsOf*/0,
            /*invisible*/false, /*completed*/true, /*isFoliage*/false,
            /*floor*/0, /*isOutsideFurniture*/false);
        if (!b) { coop::logLine("SETUP: createBuilding returned null"); return false; }
        // Building's first base is RootObject (offset 0), so this reinterpret is a
        // no-op address-wise - lets us avoid pulling in the heavy Building.h.
        RootObject* ro = reinterpret_cast<RootObject*>(b);
        Ogre::Vector3 ap = ro->getPosition(); // virtual: where it actually landed
        {
            char d[160];
            _snprintf(d, sizeof(d) - 1, "SETUP: seat actualPos=%.2f,%.2f,%.2f visible=%d",
                      ap.x, ap.y, ap.z, ro->getVisible() ? 1 : 0);
            d[sizeof(d) - 1] = '\0';
            coop::logLine(d);
        }
        if (spawned) *spawned = ro;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        coop::logLine("SETUP: createBuilding FAULTED");
        return false;
    }
}

Character* spawnNpcInFront(GameWorld* gw, float fwd, float side) {
    if (!gw || !gw->theFactory || !g_createCharFn) return 0;
    __try {
        Character* ld = (gw->player && gw->player->playerCharacters.size())
                            ? gw->player->playerCharacters[0] : 0;
        if (!ld) return 0;
        Faction* fac = ld->getFaction();
        GameData* tmpl = ld->getGameData();
        if (!fac || !tmpl) return 0;
        Ogre::Vector3 pos; float yaw = 0.0f;
        if (!leaderAnchor(gw, fwd, side, &pos, &yaw)) return 0;
        // owner=0: createRandomCharacter makes its own container; NOT enlisted in
        // the player squad, so the receiver drives it via the NPC path.
        RootObject* r = g_createCharFn(gw->theFactory, fac, pos, 0, tmpl, 0, 25.0f);
        return r ? static_cast<Character*>(r) : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// Spawn a world character in a GIVEN faction (not the player's), so it is NOT a
// squad member and flows through the host-authoritative world-NPC path. Body uses
// the leader's race template (a valid mesh); only the faction differs. SEH-guarded.
Character* spawnCharInFaction(GameWorld* gw, float fwd, float side, Faction* fac) {
    if (!gw || !gw->theFactory || !g_createCharFn || !fac) return 0;
    __try {
        Character* ld = (gw->player && gw->player->playerCharacters.size())
                            ? gw->player->playerCharacters[0] : 0;
        if (!ld) return 0;
        GameData* tmpl = ld->getGameData();
        if (!tmpl) return 0;
        Ogre::Vector3 pos; float yaw = 0.0f;
        if (!leaderAnchor(gw, fwd, side, &pos, &yaw)) return 0;
        RootObject* r = g_createCharFn(gw->theFactory, fac, pos, 0, tmpl, 0, 25.0f);
        return r ? static_cast<Character*>(r) : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}


namespace {
struct SavedGroundKey {
    unsigned int hand[5];
    bool operator<(const SavedGroundKey& other) const {
        return memcmp(hand,other.hand,sizeof(hand))<0;
    }
};
struct SavedGroundInput {
    std::string instanceID,sourceName,instanceRef,baseSID;
    bool firstTime; unsigned int generation;
    SavedGroundInput() : firstTime(false),generation(0) {}
};
typedef void (__fastcall* ItemSavedLoadFn)(Item*,GameSaveState*);
typedef void (__fastcall* ContainerSavedLoadFn)(ContainerItem*,GameSaveState*);
ItemSavedLoadFn savedItemLoadOriginal=0;
ContainerSavedLoadFn savedContainerLoadOriginal=0;
typedef RootObjectBase* (__fastcall* SavedFactoryProcessFn)(RootObjectFactory*,RootObjectFactory::CreatelistItem*);
SavedFactoryProcessFn savedFactoryProcessOriginal=0;
bool savedFactoryProcessInstalled=false;
volatile LONG savedGroundGeneration=0;
bool savedItemLoadInstalled=false;
bool savedContainerLoadInstalled=false;
CRITICAL_SECTION savedGroundLock;
bool savedGroundLockReady=false;
std::map<SavedGroundKey,SavedGroundInput> savedGroundInputs;

struct SavedGroundLockGuard {
    SavedGroundLockGuard() { EnterCriticalSection(&savedGroundLock); }
    ~SavedGroundLockGuard() { LeaveCriticalSection(&savedGroundLock); }
};

void copySavedGroundInput(GameSaveState* state,SavedGroundInput* out) {
    out->instanceID=state->instanceID;
    if (state->dataSource) out->sourceName=state->dataSource->name;
    if (state->instance) out->instanceRef=state->instance->refID;
    if (state->baseData) out->baseSID=state->baseData->stringID;
    out->firstTime=state->firstTime;
}
bool readSavedGroundInput(GameSaveState* state,SavedGroundInput* out) {
    if (!state || !out) return false;
    out->generation=groundItemIdentityGeneration();
    // Keep STL unwinding outside the SEH frame on the v100 toolchain.
    __try {
        copySavedGroundInput(state,out);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool rememberSavedGroundInput(Item* item,SavedGroundInput& input) {
    SavedGroundKey key;
    RootObject* object=static_cast<RootObject*>(item);
    if (!readObjectHand(object,key.hand) || resolveObjectByHand(key.hand)!=object) return false;
    SavedGroundLockGuard guard;
    if (input.generation!=groundItemIdentityGeneration()) return false;
    SavedGroundInput& stored=savedGroundInputs[key];
    stored.generation=input.generation;
    stored.instanceID.swap(input.instanceID);
    stored.sourceName.swap(input.sourceName);
    stored.instanceRef.swap(input.instanceRef);
    stored.baseSID.swap(input.baseSID);
    stored.firstTime=input.firstTime;
    return true;
}
void logSavedGroundBoundary(const char* boundary,RootObject* object,bool statePresent,
                            bool captured,bool registered,const SavedGroundInput& input) {
    unsigned int hand[5]={0,0,0,0,0};
    if (registered) readObjectHand(object,hand);
    char b[1024]; _snprintf(b,sizeof(b)-1,
        "[wi] SAVE-IDENTITY-BOUNDARY phase=%s generation=%u state=%d captured=%d registered=%d "
        "itemhand=%u,%u,%u,%u,%u instance='%s' source='%s' ref='%s' base='%s' firstTime=%d",
        boundary,groundItemIdentityGeneration(),statePresent ? 1 : 0,captured ? 1 : 0,
        registered ? 1 : 0,hand[0],hand[1],hand[2],hand[3],hand[4],
        input.instanceID.c_str(),input.sourceName.c_str(),input.instanceRef.c_str(),input.baseSID.c_str(),
        input.firstTime ? 1 : 0);
    b[sizeof(b)-1]='\0'; coop::logLine(b);
}
bool savedGroundRegistered(RootObject* object) {
    unsigned int hand[5]; InvItemEntry metadata;
    return object && readPhysicalItemMetadata(reinterpret_cast<Item*>(object),&metadata,hand) &&
        resolveObjectByHand(hand)==object;
}
void savedItemLoadScoped(Item* item,GameSaveState* state) {
    const bool environment=environmentItemHooksReady();
    if (environment) environmentSavedLoadEnter();
    __try { savedItemLoadOriginal(item,state); }
    __finally { if (environment) environmentSavedLoadLeave(); }
}
void savedContainerLoadScoped(ContainerItem* item,GameSaveState* state) {
    const bool environment=environmentItemHooksReady();
    if (environment) environmentSavedLoadEnter();
    __try { savedContainerLoadOriginal(item,state); }
    __finally { if (environment) environmentSavedLoadLeave(); }
}
void __fastcall savedItemLoadHook(Item* item,GameSaveState* state) {
    SavedGroundInput input;
    bool captured=readSavedGroundInput(state,&input);
    savedItemLoadScoped(item,state);
    bool registered=savedGroundRegistered(static_cast<RootObject*>(item));
    logSavedGroundBoundary("item-load",static_cast<RootObject*>(item),state!=0,captured,registered,input);
    if (captured && registered) rememberSavedGroundInput(item,input);
    if (environmentItemHooksReady()) environmentSavedItemLoaded(item,captured,input.firstTime);
}
void __fastcall savedContainerLoadHook(ContainerItem* item,GameSaveState* state) {
    SavedGroundInput input;
    bool captured=readSavedGroundInput(state,&input);
    savedContainerLoadScoped(item,state);
    bool registered=savedGroundRegistered(static_cast<RootObject*>(item));
    logSavedGroundBoundary("container-load",static_cast<RootObject*>(item),state!=0,captured,registered,input);
    if (captured && registered) rememberSavedGroundInput(item,input);
    if (environmentItemHooksReady()) environmentSavedItemLoaded(item,captured,input.firstTime);
}
bool readFactorySavedInput(RootObjectFactory::CreatelistItem* work,SavedGroundInput* input,
                           bool* statePresent,bool* itemWork) {
    *statePresent=false; *itemWork=false;
    __try {
        if (!work) return false;
        *statePresent=work->saveState!=0;
        *itemWork=work->data && (work->data->type==ITEM || work->data->type==WEAPON ||
            work->data->type==ARMOUR || work->data->type==CONTAINER);
        return readSavedGroundInput(work->saveState,input);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
RootObjectBase* processFactoryWithProceduralOrigin(RootObjectFactory* factory,
        RootObjectFactory::CreatelistItem* work) {
    proceduralFactoryWorkEnter(work);
    RootObjectBase* result=0;
    const u32 environmentOrigin=environmentFactoryWorkBegin(work);
    __try {
        result=savedFactoryProcessOriginal(factory,work);
        environmentFactoryProcessed(result,environmentOrigin);
    }
    __finally { proceduralFactoryWorkLeave(); }
    return result;
}
RootObjectBase* __fastcall savedFactoryProcessHook(RootObjectFactory* factory,RootObjectFactory::CreatelistItem* work) {
    SavedGroundInput input; bool statePresent=false,itemWork=false;
    bool captured=readFactorySavedInput(work,&input,&statePresent,&itemWork);
    // Observe the exact consumed queue occurrence, preserving native call count and return.
    RootObjectBase* result=processFactoryWithProceduralOrigin(factory,work);
    RootObject* object=reinterpret_cast<RootObject*>(result);
    bool registered=savedGroundRegistered(object);
    if (itemWork || registered) {
        logSavedGroundBoundary("factory-process",object,statePresent,captured,registered,input);
        if (captured && registered) rememberSavedGroundInput(reinterpret_cast<Item*>(object),input);
        // UNKNOWN remains UNKNOWN when registration/input is absent. Never guess a saved id.
    }
    return result;
}
void copyGroundIdentityText(char* dst,unsigned int capacity,
                           const std::string& text,bool* truncated) {
    if (text.size()>=capacity) *truncated=true;
    strncpy(dst,text.c_str(),capacity-1);
    dst[capacity-1]='\0';
}
bool readGroundItemPhysicalIdentity(const unsigned int hand[5],GroundItemIdentityRow* out) {
    RootObject* ro=resolveObjectByHand(hand);
    if (!ro) return false;
    __try {
        if (!ro->typeIsAnItem()) return false;
        if (!readObjectHand(ro,out->hand) ||
            memcmp(hand,out->hand,sizeof(out->hand))!=0) return false;
        Item* item=static_cast<Item*>(ro);
        GameData* gd=item->getGameData();
        if (!gd) return false;
        if (gd->stringID.size()>=sizeof(out->item.stringID)) out->identityTruncated=true;
        strncpy(out->item.stringID,gd->stringID.c_str(),sizeof(out->item.stringID)-1);
        out->item.itemType=(unsigned int)gd->type;
        int quantity=item->quantity;
        if (quantity<0 || quantity>65535) out->identityTruncated=true;
        out->item.quantity=(unsigned short)(quantity<0 ? 0 : (quantity>65535 ? 65535 : quantity));
        out->item.quality=qualityBucketOf(item->quality);
        out->item.level=gradeLevelOf(item);
        out->item.equipped=item->isEquipped ? 1 : 0;
        fillItemProvenance(item,out->item);
        if ((item->manufacturerData &&
             item->manufacturerData->stringID.size()>=sizeof(out->item.manufacturer)) ||
            (item->materialData &&
             item->materialData->stringID.size()>=sizeof(out->item.material)))
            out->identityTruncated=true;
        const ::hand& owner=item->properOwner;
        out->properOwner[0]=(unsigned int)owner.type;
        out->properOwner[1]=owner.container; out->properOwner[2]=owner.containerSerial;
        out->properOwner[3]=owner.index; out->properOwner[4]=owner.serial;
        const ::hand& inventory=item->_whosInventoryWeAreIn;
        out->inventoryOwner[0]=(unsigned int)inventory.type;
        out->inventoryOwner[1]=inventory.container; out->inventoryOwner[2]=inventory.containerSerial;
        out->inventoryOwner[3]=inventory.index; out->inventoryOwner[4]=inventory.serial;
        const ::hand& inside=item->isInsideBuilding;
        out->insideBuilding[0]=(unsigned int)inside.type;
        out->insideBuilding[1]=inside.container; out->insideBuilding[2]=inside.containerSerial;
        out->insideBuilding[3]=inside.index; out->insideBuilding[4]=inside.serial;
        out->condition=item->quality;
        out->charges=item->chargesLeft;
        out->fullCharge=item->originalFullChargeAmount;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
} // namespace

bool installGroundItemIdentityHooks() {
    if (!savedGroundLockReady) {
        InitializeCriticalSection(&savedGroundLock);
        savedGroundLockReady=true;
    }
    if (!savedItemLoadInstalled) {
        intptr_t address=KenshiLib::GetRealAddress(&Item::_NV_loadFromSerialise);
        if (address)
            savedItemLoadInstalled=KenshiLib::AddHook(address,(void*)&savedItemLoadHook,
                (void**)&savedItemLoadOriginal)==KenshiLib::SUCCESS;
    }
    if (!savedContainerLoadInstalled) {
        intptr_t address=KenshiLib::GetRealAddress(&ContainerItem::_NV_loadFromSerialise);
        if (address)
            savedContainerLoadInstalled=KenshiLib::AddHook(address,(void*)&savedContainerLoadHook,
                (void**)&savedContainerLoadOriginal)==KenshiLib::SUCCESS;
    }
    if (!savedFactoryProcessInstalled) {
        intptr_t address=KenshiLib::GetRealAddress(&RootObjectFactory::process);
        if (address)
            savedFactoryProcessInstalled=KenshiLib::AddHook(address,(void*)&savedFactoryProcessHook,
                (void**)&savedFactoryProcessOriginal)==KenshiLib::SUCCESS;
    }
    proceduralFactoryProcessSeamReady(savedFactoryProcessInstalled);
    char line[128]; _snprintf(line,sizeof(line)-1,
        "[wi] SAVE-IDENTITY-HOOK item=%d container=%d factoryProcess=%d generation=%u",
        savedItemLoadInstalled ? 1 : 0,savedContainerLoadInstalled ? 1 : 0,
        savedFactoryProcessInstalled ? 1 : 0,groundItemIdentityGeneration());
    line[sizeof(line)-1]='\0'; coop::logLine(line);
    return savedItemLoadInstalled && savedContainerLoadInstalled;
}

void clearGroundItemIdentityRecords() {
    scenarioPickupCursor=0;
    InterlockedIncrement(&savedGroundGeneration);
    if (!savedGroundLockReady) return;
    SavedGroundLockGuard guard;
    savedGroundInputs.clear();
}

unsigned int groundItemIdentityGeneration() {
    return (unsigned int)InterlockedCompareExchange(&savedGroundGeneration,0,0);
}

bool readGroundItemIdentity(const unsigned int hand[5],GroundItemIdentityRow* out) {
    if (!hand || !out) return false;
    memset(out,0,sizeof(*out));
    if (!readGroundItemPhysicalIdentity(hand,out)) return false;
    if (!savedGroundLockReady) return true;
    SavedGroundKey key;
    memcpy(key.hand,hand,sizeof(key.hand));
    SavedGroundLockGuard guard;
    std::map<SavedGroundKey,SavedGroundInput>::const_iterator it=savedGroundInputs.find(key);
    if (it==savedGroundInputs.end()) return true;
    const SavedGroundInput& input=it->second;
    if (input.generation!=groundItemIdentityGeneration()) return true;
    out->savedInput=true; out->firstTime=input.firstTime;
    copyGroundIdentityText(out->instanceID,sizeof(out->instanceID),input.instanceID,&out->identityTruncated);
    copyGroundIdentityText(out->sourceName,sizeof(out->sourceName),input.sourceName,&out->identityTruncated);
    copyGroundIdentityText(out->instanceRef,sizeof(out->instanceRef),input.instanceRef,&out->identityTruncated);
    copyGroundIdentityText(out->baseSID,sizeof(out->baseSID),input.baseSID,&out->identityTruncated);
    return true;
}

} // namespace engine
} // namespace coop
