#ifndef COOP_ENVIRONMENT_ITEMS_STATE_H
#define COOP_ENVIRONMENT_ITEMS_STATE_H
#include "../../netproto/EnvironmentItems.h"
#include "ProceduralWorldState.h"
#include <map>
#include <cstring>
namespace coop { namespace environment {
// Native completeness and reconciliation must refer to the same current journal,
// not merely to cached ready bits or a complete transferred snapshot.
inline bool reconciledCensusCurrent(bool complete,bool reconciled,
        const procedural::NativeCensusFence& census,u64 revision) {
    return complete && reconciled && census.current(revision);
}
inline bool anchorValid(const EnvironmentAnchor& a) {
    return std::memchr(a.sid,0,sizeof(a.sid)) && std::memchr(a.instance,0,sizeof(a.instance)) &&
        procedural::detail::coordinate(a.x) && procedural::detail::coordinate(a.y) && procedural::detail::coordinate(a.z) &&
        a.present<=1 && (!a.present || a.sid[0]) &&
        (!a.present || (a.type==0 && !(a.actorOwner|a.actorKey[0]|a.actorKey[1]|a.actorKey[2]|a.actorKey[3]|a.actorKey[4])) ||
         (a.type==1 && a.actorOwner<MAX_PLAYERS && a.actorKey[0]==1 && (a.actorKey[3] || a.actorKey[4])));
}
inline bool valid(const EnvironmentItemObject& o) {
    const double q=(double)o.rotationW*o.rotationW+(double)o.rotationX*o.rotationX+
        (double)o.rotationY*o.rotationY+(double)o.rotationZ*o.rotationZ;
    return o.id && o.item.quantity && !o.item.equipped && !o.item.parentIdx && !o.item.locked &&
        (o.item.itemType==2 || o.item.itemType==3 || o.item.itemType==4 || o.item.itemType==46) &&
        procedural::detail::text(o.item.stringID) && o.item.stringID[0] &&
        procedural::detail::text(o.item.manufacturer) && procedural::detail::text(o.item.material) &&
        procedural::detail::coordinate(o.x) && procedural::detail::coordinate(o.y) && procedural::detail::coordinate(o.z) &&
        procedural::detail::finite(o.rotationW) && procedural::detail::finite(o.rotationX) &&
        procedural::detail::finite(o.rotationY) && procedural::detail::finite(o.rotationZ) && q>0.99 && q<1.01 &&
        procedural::detail::finite(o.condition) && o.condition>=0 && o.condition<=100 &&
        procedural::detail::finite(o.charges) && procedural::detail::finite(o.fullCharge) &&
        std::fabs(o.charges)<9.0e14f && std::fabs(o.fullCharge)<9.0e14f && !(o.flags&~255u) &&
        anchorValid(o.parent) && anchorValid(o.properOwner) && anchorValid(o.persistentOwner);
}
inline bool same(const EnvironmentItemObject& a,const EnvironmentItemObject& b) {
    return std::memcmp(&a.item,&b.item,sizeof(a)-sizeof(a.id))==0;
}
inline bool sameAnchor(const EnvironmentAnchor& a,const EnvironmentAnchor& b) {
    return a.present==b.present && a.type==b.type && a.actorOwner==b.actorOwner &&
        !std::memcmp(a.actorKey,b.actorKey,sizeof(a.actorKey)) && !std::strcmp(a.sid,b.sid) && !std::strcmp(a.instance,b.instance) &&
        std::fabs(a.x-b.x)<0.01f && std::fabs(a.y-b.y)<0.01f && std::fabs(a.z-b.z)<0.01f;
}
inline bool sameNative(const EnvironmentItemObject& a,const EnvironmentItemObject& b) {
    if (std::memcmp(&a.item,&b.item,sizeof(a.item)) || a.flags!=b.flags || a.floor!=b.floor ||
        !sameAnchor(a.parent,b.parent) || !sameAnchor(a.properOwner,b.properOwner) || !sameAnchor(a.persistentOwner,b.persistentOwner)) return false;
    return std::fabs(a.x-b.x)<0.01f && std::fabs(a.y-b.y)<0.01f && std::fabs(a.z-b.z)<0.01f &&
        std::fabs(a.rotationW-b.rotationW)<0.0001f && std::fabs(a.rotationX-b.rotationX)<0.0001f &&
        std::fabs(a.rotationY-b.rotationY)<0.0001f && std::fabs(a.rotationZ-b.rotationZ)<0.0001f &&
        std::fabs(a.condition-b.condition)<0.001f && std::fabs(a.charges-b.charges)<0.001f && std::fabs(a.fullCharge-b.fullCharge)<0.001f;
}
inline float rounded(float value,double scale) { return (float)(procedural::detail::quantize(value,scale)/scale); }
inline void canonicalAnchor(EnvironmentAnchor& a) {
    a.x=rounded(a.x,100);a.y=rounded(a.y,100);a.z=rounded(a.z,100);
}
inline u32 nativeFingerprint(const EnvironmentItemObject& value) {
    EnvironmentItemObject o=value;o.id=1;
    o.x=rounded(o.x,100);o.y=rounded(o.y,100);o.z=rounded(o.z,100);
    o.rotationW=rounded(o.rotationW,10000);o.rotationX=rounded(o.rotationX,10000);
    o.rotationY=rounded(o.rotationY,10000);o.rotationZ=rounded(o.rotationZ,10000);
    o.condition=rounded(o.condition,1000);o.charges=rounded(o.charges,1000);o.fullCharge=rounded(o.fullCharge,1000);
    canonicalAnchor(o.parent);canonicalAnchor(o.properOwner);canonicalAnchor(o.persistentOwner);
    return fnv1aUpdate(fnv1aInit(),&o,sizeof(o));
}
typedef std::map<u64,EnvironmentItemObject> Objects;
inline u32 digest(const Objects& rows) {
    if (rows.size()>ENVIRONMENT_ITEMS_MAX) return 0;
    u32 h=procedural::detail::integer(fnv1aInit(),rows.size());
    for (Objects::const_iterator it=rows.begin();it!=rows.end();++it) {
        if (it->first!=it->second.id || !valid(it->second)) return 0;
        // Packed fixed-width wire payload: snapshot digest validates the exact
        // transmitted bytes, not a recomputed native floating-point approximation.
        h=fnv1aUpdate(h,&it->second,sizeof(it->second));
    }
    return h?h:1;
}
class Snapshot {
public:
    Snapshot():world_(0),load_(0),scope_(0),rx_(0),rz_(0),epoch_(0),count_(0),digest_(0),begun_(false),bad_(false),complete_(false) {}
    void reset(u64 w,u32 l,i32 x,i32 z,u32 scope) { *this=Snapshot();world_=w;load_=l;scope_=scope;rx_=x;rz_=z; }
    bool accept(const EnvironmentItemPacket& p) {
        if (!world_ || !scope_ || p.worldToken!=world_ || p.loadId!=load_ || p.scopeId!=scope_ || p.regionX!=rx_ || p.regionZ!=rz_ || !p.epoch || p.epoch<epoch_) return false;
        if (p.operation!=ENV_BEGIN && p.operation!=ENV_ROW && p.operation!=ENV_END) return false;
        if (p.operation==ENV_BEGIN && p.epoch>epoch_) {
            epoch_=p.epoch;count_=p.count;digest_=p.digest;rows_.clear();begun_=true;bad_=complete_=false;
        }
        if (!begun_ || bad_ || p.epoch!=epoch_) return false;
        if (p.count>ENVIRONMENT_ITEMS_MAX || p.count!=count_ || !p.digest || p.digest!=digest_) return invalidate();
        if (p.operation==ENV_BEGIN) return true;
        if (p.operation==ENV_ROW) {
            if (!valid(p.object)) return invalidate();
            Objects::const_iterator it=rows_.find(p.object.id);
            if (it!=rows_.end()) return same(it->second,p.object) ? true : invalidate();
            if (complete_ || rows_.size()>=count_) return invalidate();
            rows_[p.object.id]=p.object; return true;
        }
        if (rows_.size()!=count_) return false;
        if (digest(rows_)!=digest_) return invalidate();
        complete_=true;return true;
    }
    bool complete() const { return complete_ && !bad_; }
    u32 epoch() const { return epoch_; }
    u32 checksum() const { return digest_; }
    const Objects& rows() const { return rows_; }
private:
    bool invalidate() { bad_=true;complete_=false;return false; }
    u64 world_;u32 load_,scope_;i32 rx_,rz_;u32 epoch_,count_,digest_;
    bool begun_,bad_,complete_;Objects rows_;
};
} }
#endif
