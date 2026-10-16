#ifndef KENSHICOOP_INVENTORY_SNAPSHOT_TESTS_H
#define KENSHICOOP_INVENTORY_SNAPSHOT_TESTS_H
#include "../plugin/core/InventorySnapshotAssembler.h"
#include "../plugin/core/InventorySnapshotRevision.h"
#include <cstdio>
inline int testInventorySnapshots() {
    using namespace coop;
    int failed=0;
#define SNAP_CHECK(name, condition) do { if (!(condition)) { std::printf("FAIL inventory snapshot: %s\n",name); ++failed; } } while (0)
    InventorySnapshotRevision version;
    SNAP_CHECK("initial authority",version.accept(7,false));
    SNAP_CHECK("normal refresh is not operation permission",version.accept(7,true));
    SNAP_CHECK("duplicate purchase cannot regrant moved item",!version.accept(7,true));
    SNAP_CHECK("normal refresh remains available",version.accept(7,false));
    SNAP_CHECK("older operation cannot replace newer stock",!version.accept(6,true));
    SNAP_CHECK("legacy snapshot cannot overwrite versioned stock",!version.accept(0,false));
    SNAP_CHECK("later committed operation",version.accept(8,true));
    InventorySnapshotRevision wrapped;
    SNAP_CHECK("pre-wrap",wrapped.accept(0xffffffffu,true));
    SNAP_CHECK("revision wrap",wrapped.accept(1,true));
    SNAP_CHECK("pre-wrap replay",!wrapped.accept(0xffffffffu,true));
    std::vector<InvItemEntry> rows(513),out;
    for (unsigned i=0;i<rows.size();++i) {
        memset(&rows[i],0,sizeof(rows[i])); strcpy(rows[i].stringID,"food");
        rows[i].quantity=(u16)(i+1); rows[i].quality=10000; rows[i].level=GRADE_NA;
    }
    rows[400].parentIdx=301; // cross-frame reference wider than the old byte field
    InvSnapshotHeader h; memset(&h,0,sizeof(h)); h.type=PKT_INV_SNAPSHOT;
    h.cIndex=31; h.cSerial=42; h.revision=5;
    h.totalCount=(u32)rows.size(); h.digest=inventorySnapshotDigest(&rows[0],(unsigned)rows.size());
    InventorySnapshotAssembler stage;
    h.offset=512; h.count=1; h.flags=INV_FLAG_COMPLETE;
    SNAP_CHECK("END alone cannot expose partial stock",stage.put(h,&rows[512],out)==0 && out.empty());
    h.flags=0;
    for (unsigned offset=0;offset<512;offset+=64) {
        h.offset=offset; h.count=64;
        const int result=stage.put(h,&rows[offset],out);
        SNAP_CHECK("all rows required before commit",result==(offset==448?1:0));
        if (!offset) SNAP_CHECK("repeat chunk does not count twice",stage.put(h,&rows[0],out)==0);
    }
    SNAP_CHECK("complete merchant stock and nested identity preserved",out.size()==rows.size() &&
        !memcmp(&out[0],&rows[0],rows.size()*sizeof(InvItemEntry)));
    out.clear(); stage.clear(); h.offset=0; h.count=64; h.flags=0; h.revision=9;
    SNAP_CHECK("new revision starts",stage.put(h,&rows[0],out)==0);
    h.revision=8;
    SNAP_CHECK("stale interleaved frame rejected",stage.put(h,&rows[0],out)==-1 && out.empty());
    stage.clear(); h.revision=10; h.count=1; h.flags=INV_FLAG_COMPLETE;
    SNAP_CHECK("false completion boundary rejected",stage.put(h,&rows[0],out)==-1);
    h.totalCount=1; h.digest=0;
    SNAP_CHECK("wrong complete digest rejected",stage.put(h,&rows[0],out)==-1 && out.empty());
    stage.clear(); h.totalCount=0; h.count=0; h.digest=inventorySnapshotDigest(0,0);
    SNAP_CHECK("explicit complete empty stock",stage.put(h,0,out)==1 && out.empty());
    stage.clear(); h.flags=0; h.count=1; h.totalCount=1; h.digest=inventorySnapshotDigest(&rows[0],1);
    SNAP_CHECK("missing END remains pending",stage.put(h,&rows[0],out)==0);
    stage.retire(0); h.flags=INV_FLAG_COMPLETE;
    SNAP_CHECK("retired peer starts independent state",stage.put(h,&rows[0],out)==1);
    stage.clear(); out.clear();
    rows[0].parentIdx=401; rows[400].parentIdx=1;
    h.revision=11; h.totalCount=(u32)rows.size(); h.digest=inventorySnapshotDigest(&rows[0],(unsigned)rows.size());
    for (unsigned offset=0;offset<rows.size();offset+=64) {
        h.offset=offset; h.count=(u16)((rows.size()-offset)<64?rows.size()-offset:64);
        h.flags=offset+h.count==rows.size()?INV_FLAG_COMPLETE:0;
        SNAP_CHECK("cross-frame parent cycle cannot enter game",stage.put(h,&rows[offset],out)==(h.flags?-1:0));
    }
    SNAP_CHECK("invalid parent tree exposes no snapshot",out.empty());
    stage.clear(); h.offset=0; h.totalCount=INV_SNAPSHOT_ITEMS_MAX+1; h.count=1;
    SNAP_CHECK("oversize rejects before staging",stage.put(h,&rows[0],out)==-1);
#undef SNAP_CHECK
    return failed;
}
#endif
