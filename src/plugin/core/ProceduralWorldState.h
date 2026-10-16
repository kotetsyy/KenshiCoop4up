#ifndef KENSHICOOP_PROCEDURAL_WORLD_STATE_H
#define KENSHICOOP_PROCEDURAL_WORLD_STATE_H

#include "../../netproto/Wire.h"
#include "../../netproto/ContentHash.h"
#include <vector>
#include <cmath>
#include <cfloat>
#include <cstring>
#include <set>
#include <utility>

namespace coop { namespace procedural {
// BED is enabled wherever the native loaded-region registry supplies a complete
// census. Coordinates are not policy; provenance and category remain mandatory.
inline bool managedBedRegion(i32,i32) { return true; }
namespace detail {
inline bool finite(float x) { return x >= -FLT_MAX && x <= FLT_MAX; }
inline bool text(const char* s) { return std::memchr(s, 0, 48) != 0; }
inline bool parent(unsigned flags, unsigned flag, const char* sid) {
    return text(sid) && (((flags & flag) != 0) == (sid[0] != 0));
}
// Quantize into explicit integers, never hash floating representation or padding.
// The bound keeps every conversion inside signed 64-bit range on v100.
inline bool coordinate(float x) { return finite(x) && x >= -9.0e15f && x <= 9.0e15f; }
inline signed __int64 quantize(double x, double scale) {
    double n = x * scale;
    return (signed __int64)(n < 0 ? std::ceil(n - 0.5) : std::floor(n + 0.5));
}
inline bool rotationValid(const ProceduralObject& o) {
    return finite(o.rotationW) && finite(o.rotationX) && finite(o.rotationY) && finite(o.rotationZ) &&
        (o.rotationW != 0 || o.rotationX != 0 || o.rotationY != 0 || o.rotationZ != 0);
}
// Valid nonzero float components cannot overflow/underflow this double norm.
// Canonical order is W,X,Y,Z; select sign AFTER quantizing to handle near pi.
inline void rotation(const ProceduralObject& o, signed __int64 (&q)[4]) {
    const double w = o.rotationW, x = o.rotationX, y = o.rotationY, z = o.rotationZ;
    const double norm = std::sqrt(w*w + x*x + y*y + z*z);
    q[0] = quantize(w/norm,10000); q[1] = quantize(x/norm,10000);
    q[2] = quantize(y/norm,10000); q[3] = quantize(z/norm,10000);
    for (unsigned i = 0; i < 4; ++i) {
        if (!q[i]) continue;
        if (q[i] < 0) for (unsigned j = 0; j < 4; ++j) q[j] = -q[j];
        break;
    }
}
inline signed __int64 angle(float x) {
    const double turn = 6.28318530717958647692;
    double n = std::fmod((double)x, turn); if (n < 0) n += turn;
    signed __int64 q = quantize(n, 10000.0);
    return q >= 62832 ? 0 : q;
}
inline u32 integer(u32 h, u64 n) {
    for (unsigned i = 0; i < 8; ++i) { const u8 byte = (u8)(n & 255); h = fnv1aUpdate(h,&byte,1); n >>= 8; }
    return h;
}
inline u32 string(u32 h, const char* s) {
    for (unsigned i = 0; i < 48; ++i) { h = fnv1aUpdate(h,s+i,1); if (!s[i]) break; }
    return h;
}
inline u32 fields(u32 h, const ProceduralObject& o) {
    h = integer(h,o.category); h = integer(h,o.flags); h = string(h,o.sid);
    h = integer(h,(u64)quantize(o.x,100)); h = integer(h,(u64)quantize(o.y,100)); h = integer(h,(u64)quantize(o.z,100));
    h = integer(h,(u64)angle(o.heading));
    signed __int64 q[4]; rotation(o,q);
    for (unsigned i = 0; i < 4; ++i) h = integer(h,(u64)q[i]);
    h = integer(h,(u64)(signed __int64)o.floor);
    h = integer(h,o.buildingFunction); h = integer(h,o.stateFlags); h = integer(h,(u64)quantize(o.constructionProgress,10000));
    h = string(h,o.ownerSid); h = string(h,o.townSid); h = string(h,o.insideSid);
    h = integer(h,(u64)quantize(o.townX,100)); h = integer(h,(u64)quantize(o.townY,100)); h = integer(h,(u64)quantize(o.townZ,100));
    h = integer(h,(u64)quantize(o.insideX,100)); h = integer(h,(u64)quantize(o.insideY,100)); h = integer(h,(u64)quantize(o.insideZ,100));
    return h;
}
inline bool validDescriptorFields(const ProceduralObject& o) {
    return o.category == PROC_CATEGORY_CAMP_BED && text(o.sid) && o.sid[0] &&
        !(o.flags & ~(PROC_OWNER|PROC_TOWN|PROC_INSIDE|PROC_OUTSIDE)) &&
        !(o.stateFlags & ~(PROC_COMPLETE|PROC_PAUSED|PROC_DISMANTLED|PROC_NEST)) &&
        parent(o.flags,PROC_OWNER,o.ownerSid) && parent(o.flags,PROC_TOWN,o.townSid) &&
        parent(o.flags,PROC_INSIDE,o.insideSid) &&
        coordinate(o.x) && coordinate(o.y) && coordinate(o.z) && finite(o.heading) &&
        rotationValid(o) &&
        coordinate(o.townX) && coordinate(o.townY) && coordinate(o.townZ) &&
        coordinate(o.insideX) && coordinate(o.insideY) && coordinate(o.insideZ) &&
        finite(o.constructionProgress) && o.constructionProgress >= 0;
}
inline bool sameDescriptorFields(const ProceduralObject& a, const ProceduralObject& b) {
    signed __int64 aq[4], bq[4]; rotation(a,aq); rotation(b,bq);
    for (unsigned i = 0; i < 4; ++i) if (aq[i] != bq[i]) return false;
    return a.category == b.category && a.flags == b.flags && std::strcmp(a.sid,b.sid) == 0 &&
        quantize(a.x,100) == quantize(b.x,100) &&
        quantize(a.y,100) == quantize(b.y,100) &&
        quantize(a.z,100) == quantize(b.z,100) && angle(a.heading) == angle(b.heading) &&
        a.floor == b.floor && a.buildingFunction == b.buildingFunction && a.stateFlags == b.stateFlags &&
        quantize(a.constructionProgress,10000) == quantize(b.constructionProgress,10000) &&
        std::strcmp(a.ownerSid,b.ownerSid) == 0 && std::strcmp(a.townSid,b.townSid) == 0 && std::strcmp(a.insideSid,b.insideSid) == 0 &&
        quantize(a.townX,100) == quantize(b.townX,100) &&
        quantize(a.townY,100) == quantize(b.townY,100) &&
        quantize(a.townZ,100) == quantize(b.townZ,100) &&
        quantize(a.insideX,100) == quantize(b.insideX,100) &&
        quantize(a.insideY,100) == quantize(b.insideY,100) &&
        quantize(a.insideZ,100) == quantize(b.insideZ,100);
}
}

inline bool validDescriptor(const ProceduralObject& o) {
    return detail::validDescriptorFields(o) && o.constructionProgress <= 1;
}

inline bool validObject(const ProceduralObject& o) {
    return o.id != 0 && validDescriptor(o);
}

inline bool sameObject(const ProceduralObject& a, const ProceduralObject& b) {
    return validDescriptor(a) && validDescriptor(b) && detail::sameDescriptorFields(a,b);
}
// Mutable construction state changes the snapshot revision, not placement ID.
inline bool samePlacement(const ProceduralObject& a,const ProceduralObject& b) {
    if (!validDescriptor(a) || !validDescriptor(b)) return false;
    ProceduralObject left=a,right=b;
    left.stateFlags=right.stateFlags=0;
    left.constructionProgress=right.constructionProgress=0;
    return detail::sameDescriptorFields(left,right);
}

// Protected saved beds are native state, not managed Camp Bed wire payloads.
// Keep their raw progress (observed as 5) while retaining the same full geometry
// and state comparison. Bound it before the signed 64-bit canonical conversion.
inline bool sameNativeState(const ProceduralObject& a, const ProceduralObject& b) {
    return detail::validDescriptorFields(a) && detail::validDescriptorFields(b) &&
        a.constructionProgress <= 9.0e14f && b.constructionProgress <= 9.0e14f &&
        detail::sameDescriptorFields(a,b);
}

inline u32 snapshotDigest(const std::vector<ProceduralObject>& objects) {
    if (objects.size() > PROCEDURAL_OBJECTS_MAX) return 0;
    unsigned indices[PROCEDURAL_OBJECTS_MAX];
    for (unsigned i = 0; i < objects.size(); ++i) {
        if (!validObject(objects[i])) return 0;
        unsigned j = i;
        while (j && objects[indices[j-1]].id > objects[i].id) { indices[j] = indices[j-1]; --j; }
        indices[j] = i;
    }
    u32 h = detail::integer(fnv1aInit(),(u64)objects.size());
    for (unsigned i = 0; i < objects.size(); ++i) {
        const ProceduralObject& o = objects[indices[i]];
        if (i && objects[indices[i-1]].id == o.id) return 0;
        h = detail::fields(detail::integer(h,o.id),o);
    }
    return h ? h : 1;
}

// A wire ACK/commit cannot outlive the native census that justified it. Native
// enqueue/birth/reset hooks advance the revision before the next sync scan.
class NativeCensusFence {
public:
    NativeCensusFence():revision_(0),complete_(false) {}
    void reset() { revision_=0; complete_=false; }
    void begin(u64 revision) { revision_=revision; complete_=false; }
    void finish(bool complete,u64 revision) { complete_=complete && revision_==revision; }
    bool current(u64 revision) const { return complete_ && revision_==revision; }
private:
    u64 revision_;
    bool complete_;
};

// Reliable-lane membership barrier. Duplicate requests/retirements are no-ops;
// only a changed current set invalidates its commit. Packet fencing is explicit.
class PeerScope {
public:
    PeerScope():world_(0),load_(0),committed_(false) {}
    void reset(u64 world,u32 load) {
        world_=world; load_=load; committed_=false; regions_.clear();
    }
    bool accept(const ProceduralPacket& p) {
        if (!world_ || p.type!=PKT_PROCEDURAL || p.worldToken!=world_ || p.loadId!=load_) return false;
        const std::pair<i32,i32> key(p.regionX,p.regionZ);
        if (p.operation==PROC_REQUEST) {
            if (regions_.find(key)==regions_.end()) {
                if (regions_.size()>=256) return false;
                regions_.insert(key); committed_=false;
            }
        } else if (p.operation==PROC_RETIRE) {
            if (regions_.erase(key)) committed_=false;
        } else if (p.operation==PROC_SCOPE_COMMIT) {
            if (p.count!=regions_.size()) return false;
            committed_=true;
        } else return false;
        return true;
    }
    bool committed() const { return committed_; }
private:
    u64 world_;
    u32 load_;
    bool committed_;
    std::set<std::pair<i32,i32> > regions_;
};

class RegionSnapshot {
public:
    RegionSnapshot() { reset(0,0,0,0); }
    void reset(u64 world, u32 load, i32 rx, i32 rz) {
        world_ = world; load_ = load; rx_ = rx; rz_ = rz; epoch_ = count_ = digest_ = 0;
        begun_ = invalid_ = complete_ = acknowledged_ = false; objects_.clear();
    }
    bool accept(const ProceduralPacket& p) {
        // Unrelated/stale traffic must not corrupt a newer valid assembly.
        if (!world_ || p.type != PKT_PROCEDURAL || p.worldToken != world_ || p.loadId != load_ ||
            p.regionX != rx_ || p.regionZ != rz_ || !p.epoch || p.epoch < epoch_) return false;
        if (p.operation != PROC_BEGIN && p.operation != PROC_ROW && p.operation != PROC_END) return false;
        if (p.operation == PROC_BEGIN && p.epoch > epoch_) {
            epoch_ = p.epoch; count_ = p.count; digest_ = p.digest; objects_.clear();
            begun_ = true; invalid_ = complete_ = acknowledged_ = false;
        }
        if (!begun_ || p.epoch != epoch_) return false;
        if (invalid_) return false;
        if (p.count > PROCEDURAL_OBJECTS_MAX || !p.digest || p.count != count_ || p.digest != digest_) return invalidate();
        if (p.operation == PROC_BEGIN) { objects_.reserve(count_); return true; }
        if (p.operation == PROC_ROW) {
            if (!validObject(p.object)) return invalidate();
            for (unsigned i = 0; i < objects_.size(); ++i) {
                if (objects_[i].id == p.object.id) return sameObject(objects_[i],p.object) ? true : invalidate();
            }
            if (complete_ || objects_.size() >= count_) return invalidate();
            objects_.push_back(p.object); return true;
        }
        // A premature END cannot make partial data ready. A retransmitted END
        // may finish once the missing reliable rows have arrived.
        if (objects_.size() != count_) return false;
        if (snapshotDigest(objects_) != digest_) return invalidate();
        // Reliable queue acceptance is not remote processing. An exact completed
        // END retry asks the coordinator to queue the exact ACK again.
        complete_ = true; acknowledged_ = false; return true;
    }
    bool complete() const { return complete_ && !invalid_; }
    bool acknowledged() const { return acknowledged_ && complete(); }
    void markAcknowledged() { if (complete()) acknowledged_ = true; }
    u32 epoch() const { return epoch_; }
    u32 digest() const { return digest_; }
    u32 count() const { return count_; }
    const std::vector<ProceduralObject>& objects() const { return objects_; }
private:
    bool invalidate() { invalid_ = true; complete_ = acknowledged_ = false; return false; }
    u64 world_; u32 load_; i32 rx_,rz_; u32 epoch_,count_,digest_;
    bool begun_,invalid_,complete_,acknowledged_;
    std::vector<ProceduralObject> objects_;
};

} }
#endif
