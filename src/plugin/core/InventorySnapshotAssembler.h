#ifndef KENSHICOOP_INVENTORY_SNAPSHOT_ASSEMBLER_H
#define KENSHICOOP_INVENTORY_SNAPSHOT_ASSEMBLER_H
#include "../../netproto/Wire.h"
#include "../net/NativeValues.h"
#include <map>
#include <vector>
#include <cstring>

namespace coop {
inline u32 inventorySnapshotDigest(const InvItemEntry* rows,unsigned int count) {
    u32 hash=2166136261u;
    const unsigned char* bytes=reinterpret_cast<const unsigned char*>(rows);
    const size_t size=(size_t)count*sizeof(InvItemEntry);
    for (size_t i=0;i<size;++i) { hash^=bytes[i]; hash*=16777619u; }
    return hash;
}
class InventorySnapshotAssembler {
    struct Key {
        u32 value[7];
        explicit Key(const InvSnapshotHeader& h) {
            value[0]=h.ownerId; value[1]=h.keyKind; value[2]=h.cType;
            value[3]=h.cContainer; value[4]=h.cContainerSerial;
            value[5]=h.cIndex; value[6]=h.cSerial;
        }
        bool operator<(const Key& other) const {
            for (unsigned i=0;i<7;++i)
                if (value[i]!=other.value[i]) return value[i]<other.value[i];
            return false;
        }
    };
    struct Pending {
        u32 revision,total,digest,received;
        u8 flags;
        bool ended;
        std::vector<InvItemEntry> rows;
        std::vector<u8> present;
        Pending() : revision(0),total(0),digest(0),received(0),flags(0),ended(false) {}
        void begin(const InvSnapshotHeader& h) {
            revision=h.revision; total=h.totalCount; digest=h.digest; received=0;
            flags=h.flags & ~INV_FLAG_COMPLETE; ended=false;
            rows.resize(total); present.assign(total,0);
        }
    };
    typedef std::map<Key,Pending> PendingMap;
    PendingMap pending_;
public:
    void clear() { pending_.clear(); }
    void retire(u32 owner) {
        for (PendingMap::iterator it=pending_.begin();it!=pending_.end();)
            if (it->first.value[0]==owner) pending_.erase(it++); else ++it;
    }
    bool hasCapacity() const { return pending_.size()<128; }
    // -1 rejects inconsistent framing, 0 waits, 1 swaps one verified full set out.
    int put(const InvSnapshotHeader& h,const InvItemEntry* rows,
            std::vector<InvItemEntry>& complete) {
        if (h.count>INV_ITEMS_MAX || h.totalCount>INV_SNAPSHOT_ITEMS_MAX ||
            h.offset>h.totalCount || h.count>h.totalCount-h.offset ||
            (h.count && !rows) ||
            ((h.flags & INV_FLAG_COMPLETE) && h.offset+h.count!=h.totalCount)) return -1;
        const Key key(h);
        PendingMap::iterator it=pending_.find(key);
        if (it==pending_.end()) {
            if (!hasCapacity()) return -1;
            it=pending_.insert(std::make_pair(key,Pending())).first;
            it->second.begin(h);
        } else if (it->second.revision!=h.revision) {
            const u32 old=it->second.revision;
            if (old && (!h.revision || h.revision-old>=0x80000000u)) return -1;
            it->second.begin(h);
        }
        Pending& p=it->second;
        if (p.total!=h.totalCount || p.digest!=h.digest ||
            p.flags!=(h.flags & ~INV_FLAG_COMPLETE)) return -1;
        for (u32 i=0;i<h.count;++i) {
            const u32 index=h.offset+i;
            if (!nativeValueValid(rows[i]) || rows[i].parentIdx>p.total ||
                rows[i].parentIdx==index+1) { pending_.erase(it); return -1; }
            if (p.present[index]) {
                if (memcmp(&p.rows[index],&rows[i],sizeof(InvItemEntry))) {
                    pending_.erase(it); return -1;
                }
            } else {
                p.rows[index]=rows[i]; p.present[index]=1; ++p.received;
            }
        }
        if (h.flags & INV_FLAG_COMPLETE) p.ended=true;
        if (!p.ended || p.received!=p.total) return 0;
        const InvItemEntry* first=p.rows.empty()?0:&p.rows[0];
        if (inventorySnapshotDigest(first,p.total)!=p.digest ||
            !nativeInventoryValid(first,p.total)) { pending_.erase(it); return -1; }
        complete.swap(p.rows); pending_.erase(it); return 1;
    }
};
}
#endif
