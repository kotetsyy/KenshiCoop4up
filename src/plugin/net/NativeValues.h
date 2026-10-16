#ifndef KENSHICOOP_NATIVE_VALUES_H
#define KENSHICOOP_NATIVE_VALUES_H
#include "../../netproto/Wire.h"
#include <string.h>
#include <stddef.h>
namespace coop {
inline bool nativeFinite(f32 v) { u32 b;memcpy(&b,&v,sizeof(b));return (b&0x7f800000u)!=0x7f800000u; }
inline bool nativeFinite(f64 v) { u64 b;memcpy(&b,&v,sizeof(b));return (b&0x7ff0000000000000ULL)!=0x7ff0000000000000ULL; }
template<class T> inline bool nativeValueValid(const T&) {return true;}
inline bool nativeValueValid(const ProceduralObject& v) {
    if(!v.id || v.category!=PROC_CATEGORY_CAMP_BED || !v.sid[0] ||
       !memchr(v.sid,0,sizeof(v.sid)) || !memchr(v.ownerSid,0,sizeof(v.ownerSid)) ||
       !memchr(v.townSid,0,sizeof(v.townSid)) || !memchr(v.insideSid,0,sizeof(v.insideSid)))return false;
    if(v.flags & ~(PROC_OWNER|PROC_TOWN|PROC_INSIDE|PROC_OUTSIDE))return false;
    if(v.stateFlags & ~(PROC_COMPLETE|PROC_PAUSED|PROC_DISMANTLED|PROC_NEST))return false;
    if(((v.flags&PROC_OWNER)!=0)!=(v.ownerSid[0]!=0) ||
       ((v.flags&PROC_TOWN)!=0)!=(v.townSid[0]!=0) ||
       ((v.flags&PROC_INSIDE)!=0)!=(v.insideSid[0]!=0))return false;
    if(!nativeFinite(v.rotationW) || !nativeFinite(v.rotationX) ||
       !nativeFinite(v.rotationY) || !nativeFinite(v.rotationZ) ||
       (v.rotationW==0 && v.rotationX==0 && v.rotationY==0 && v.rotationZ==0))return false;
    return nativeFinite(v.x) && nativeFinite(v.y) && nativeFinite(v.z) &&
        nativeFinite(v.heading) && nativeFinite(v.townX) && nativeFinite(v.townY) &&
        nativeFinite(v.townZ) && nativeFinite(v.insideX) && nativeFinite(v.insideY) &&
        nativeFinite(v.insideZ) && nativeFinite(v.constructionProgress) &&
        v.constructionProgress>=0 && v.constructionProgress<=1;
}
inline bool nativeValueValid(const ProceduralPacket& v) {
    if(!v.worldToken || !v.loadId || v.count>PROCEDURAL_OBJECTS_MAX ||
       v.operation<PROC_REQUEST || v.operation>PROC_SCOPE_COMMIT)return false;
    if(v.operation==PROC_REQUEST || v.operation==PROC_RETIRE)
        return !v.epoch && !v.count && !v.digest;
    if(v.operation==PROC_SCOPE_COMMIT)
        return !v.epoch && v.count<=256 && !v.digest && !v.regionX && !v.regionZ;
    if(!v.epoch)return false;
    return v.operation!=PROC_ROW || (v.count && nativeValueValid(v.object));
}
inline bool nativeValueValid(const EventPacket& v) {
    if(!nativeFinite(v.arg))return false;
    if(v.poseValid>1)return false;
    if(v.poseValid && (!nativeFinite(v.x) || !nativeFinite(v.y) ||
       !nativeFinite(v.z) || !nativeFinite(v.heading)))return false;
    return true;
}
inline bool nativeValueValid(const EntityState& v) {
    if(!nativeFinite(v.x))return false;
    if(!nativeFinite(v.y))return false;
    if(!nativeFinite(v.z))return false;
    if(!nativeFinite(v.heading))return false;
    if(!nativeFinite(v.cSpeed))return false;
    if(!nativeFinite(v.cMotionX))return false;
    if(!nativeFinite(v.cMotionY))return false;
    if(!nativeFinite(v.cMotionZ))return false;
    if(v.cMoving>1)return false;
    if((v.bodyState&0xf000u) || bodyProne(v.bodyState)>PRONE_KO)return false;
    return true;
}
inline bool nativeValueValid(const InvItemEntry& v) {
    if(!memchr(v.stringID,0,sizeof(v.stringID)))return false;
    if(!memchr(v.manufacturer,0,sizeof(v.manufacturer)))return false;
    if(!memchr(v.material,0,sizeof(v.material)))return false;
    if(v.equipped>1 || v.locked>1)return false;
    return true;
}
inline bool nativeValueValid(const WorldItemEntry& v) {
    if(!memchr(v.stringID,0,sizeof(v.stringID)))return false;
    if(!nativeFinite(v.x))return false;
    if(!nativeFinite(v.y))return false;
    if(!nativeFinite(v.z))return false;
    return true;
}
inline bool nativeValueValid(const WorldDropPacket& v) {
    if(!memchr(v.stringID,0,sizeof(v.stringID)))return false;
    if(!memchr(v.manufacturer,0,sizeof(v.manufacturer)))return false;
    if(!memchr(v.material,0,sizeof(v.material)))return false;
    if(v.sourceEquipped>1)return false;
    if(!nativeFinite(v.x))return false;
    if(!nativeFinite(v.y))return false;
    if(!nativeFinite(v.z))return false;
    return true;
}
inline bool nativeValueValid(const WorldPickupPacket& v) {
    if(!memchr(v.stringID,0,sizeof(v.stringID)))return false;
    if(v.refDropId && v.refDropOwnerId>=MAX_PLAYERS)return false;
    return true;
}
inline bool nativeValueValid(const InvXferPacket& v) {
    if(!memchr(v.stringID,0,sizeof(v.stringID)))return false;
    if(!memchr(v.manufacturer,0,sizeof(v.manufacturer)))return false;
    if(!memchr(v.material,0,sizeof(v.material)))return false;
    if(v.operation>INV_OP_EXTRACT)return false;
    if(v.operation!=INV_OP_TRANSFER && (!v.xferId || !v.quantity || !v.sourceRevision || !v.stringID[0]))return false;
    if((v.operation==INV_OP_BUY || v.operation==INV_OP_SELL) &&
       (v.quotedTotal<0 || !v.actorRevision))return false;
    return true;
}
inline bool nativeValueValid(const InvXferAckPacket& v) {
    if(v.xferOwnerId>=MAX_PLAYERS || v.verdict>XFER_ACK_PARTIAL || v.applied>v.requested)return false;
    if(v.operation>INV_OP_EXTRACT || v.wallet< -1)return false;
    return true;
}
inline bool nativeValueValid(const MedPartEntry& v) {
    if(!nativeFinite(v.flesh))return false;
    if(!nativeFinite(v.fleshStun))return false;
    if(!nativeFinite(v.bandaging))return false;
    if(!nativeFinite(v.juryRig))return false;
    if(v.used>1)return false;
    return true;
}
inline bool nativeValueValid(const MedicalPacket& v) {
    if(!nativeFinite(v.blood))return false;
    if(!nativeFinite(v.bleedRate))return false;
    if(!nativeFinite(v.hunger))return false;
    if(!nativeFinite(v.fed))return false;
    for(unsigned i=0;i<4;++i)if(!memchr(v.limbSid[i],0,sizeof(v.limbSid[i])))return false;
    if(v.nParts>MED_PARTS_MAX)return false;
    for(unsigned i=0;i<v.nParts;++i)if(!nativeValueValid(v.parts[i]))return false;
    for(unsigned i=0;i<4;++i)if(v.limbState[i]>LIMB_WIRE_CRUSHED && v.limbState[i]!=LIMB_STATE_UNKNOWN)return false;
    return true;
}
inline bool nativeValueValid(const TreatmentPacket& v) {
    for(unsigned i=0;i<MED_PARTS_MAX;++i)if(!nativeFinite(v.partBand[i]))return false;
    return true;
}
inline bool nativeValueValid(const CombatHitPacket& v) {
    if(!nativeFinite(v.flesh))return false;
    if(!nativeFinite(v.blood))return false;
    if(v.flesh<0 || v.blood<0)return false;
    return true;
}
inline bool nativeValueValid(const SpeedPacket& v) {
    if(!nativeFinite(v.speed))return false;
    if(v.flags&~(SPEED_PAUSED|SPEED_IN_COMBAT|SPEED_COMMAND))return false;
    if(v.type==PKT_SPEED_SET && (v.flags&SPEED_COMMAND))return false;
    return true;
}
inline bool nativeValueValid(const StatsPacket& v) {
    for(unsigned i=0;i<STATS_SLOT_MAX;++i)if(!nativeFinite(v.stats[i]))return false;
    if(!nativeFinite(v.xp))return false;
    if(!nativeFinite(v.freeAttributePoints))return false;
    if(v.nStats>STATS_SLOT_MAX)return false;
    return true;
}
inline bool nativeValueValid(const FactionPacket& v) {
    if(!memchr(v.sid,0,sizeof(v.sid)))return false;
    if(!nativeFinite(v.relation))return false;
    return true;
}
inline bool nativeValueValid(const TimePacket& v) {
    if(!nativeFinite(v.gameHours))return false;
    return true;
}
inline bool nativeValueValid(const DoorPacket& v) {
    if(v.open>1 || v.locked>1)return false;
    if(v.doorIndex< -1)return false;
    if(v.doorIndex>=0 && !v.parentHand[3] && !v.parentHand[4])return false;
    return true;
}
inline bool nativeValueValid(const DeedPacket& v) {
    if(!memchr(v.ownerSid,0,sizeof(v.ownerSid)))return false;
    if(v.owned>1)return false;
    return true;
}
inline bool nativeValueValid(const RosterPacket& v) {
    for(unsigned i=0;i<MAX_PLAYERS;++i)if(!memchr(v.name[i],0,sizeof(v.name[i])))return false;
    return true;
}
inline bool nativeValueValid(const FixturePacket& v) {
    if(!nativeFinite(v.x))return false;
    if(!nativeFinite(v.y))return false;
    if(!nativeFinite(v.z))return false;
    if(!memchr(v.sid,0,sizeof(v.sid)) || !memchr(v.ownerSid,0,sizeof(v.ownerSid)) ||
       !memchr(v.townSid,0,sizeof(v.townSid)) || !memchr(v.insideSid,0,sizeof(v.insideSid)))return false;
    if(!nativeFinite(v.heading) || !nativeFinite(v.townX) || !nativeFinite(v.townY) || !nativeFinite(v.townZ) ||
       !nativeFinite(v.insideX) || !nativeFinite(v.insideY) || !nativeFinite(v.insideZ))return false;
    if(v.poseFlags & ~(POSE_FIXTURE_VALID | POSE_FIXTURE_OWNER_PRESENT | POSE_FIXTURE_OUTSIDE))return false;
    if(!v.poseFlags) return (!v.poseActor.resolvable() || v.sid[0]) &&
        !v.ownerSid[0] && !v.town.resolvable() && !v.townSid[0] &&
        !v.inside.resolvable() && !v.insideSid[0];
    if(!(v.poseFlags & POSE_FIXTURE_VALID) || !v.poseActor.resolvable() || !v.sid[0])return false;
    if(((v.poseFlags & POSE_FIXTURE_OWNER_PRESENT)!=0) != (v.ownerSid[0]!=0))return false;
    return v.town.resolvable() == (v.townSid[0]!=0) && v.inside.resolvable() == (v.insideSid[0]!=0);
}
inline bool nativeValueValid(const BuildPlacePacket& v) {
    if(!memchr(v.sid,0,sizeof(v.sid)))return false;
    if(!nativeFinite(v.x))return false;
    if(!nativeFinite(v.y))return false;
    if(!nativeFinite(v.z))return false;
    if(!nativeFinite(v.yaw))return false;
    if(v.fromUi>1)return false;
    return true;
}
inline bool nativeValueValid(const BuildStatePacket& v) {
    if(!nativeFinite(v.progress))return false;
    if(v.complete>1)return false;
    return true;
}
inline bool nativeValueValid(const BuildDoorPacket& v) {
    if(v.open>1 || v.locked>1)return false;
    return true;
}
inline bool nativeValueValid(const StealthSeerEntry& v) {
    if(!nativeFinite(v.prog))return false;
    if(v.see>2)return false;
    return true;
}
inline bool nativeValueValid(const StealthPacket& v) {
    if(v.unseen>2 || v.nSeers>STEALTH_SEER_MAX)return false;
    for(unsigned i=0;i<v.nSeers;++i)if(!nativeValueValid(v.seers[i]))return false;
    return true;
}
inline bool nativeValueValid(const SpawnReqPacket& v) {
    return v.targetOwner<MAX_PLAYERS && v.targetOwner!=v.ownerId;
}
inline bool nativeValueValid(const SpawnInfoPacket& v) {
    if(v.requestorId>=MAX_PLAYERS || v.requestorId==v.ownerId)return false;
    if(!memchr(v.charSid,0,sizeof(v.charSid)))return false;
    if(!memchr(v.facSid,0,sizeof(v.facSid)))return false;
    if(!nativeFinite(v.x))return false;
    if(!nativeFinite(v.y))return false;
    if(!nativeFinite(v.z))return false;
    if(!nativeFinite(v.heading))return false;
    if(!nativeFinite(v.age))return false;
    if(!memchr(v.shopSquadSid,0,sizeof(v.shopSquadSid)))return false;
    if(v.found>1 || v.dead>2)return false;
    return true;
}
inline bool nativeValueValid(const SaveReqPacket& v) {
    if(!memchr(v.name,0,sizeof(v.name)))return false;
    return true;
}
inline bool nativeValueValid(const SaveBeginPacket& v) {
    if(!memchr(v.name,0,sizeof(v.name)))return false;
    return true;
}
inline bool nativeValueValid(const SaveAckPacket& v) {
    if(v.ok>1)return false;
    return true;
}
inline bool nativeValueValid(const LoadGoPacket& v) {
    if(!memchr(v.name,0,sizeof(v.name)))return false;
    if(!v.worldToken || !v.loadId)return false;
    return true;
}
inline bool nativeValueValid(const LoadReqPacket& v) {
    if(!memchr(v.name,0,sizeof(v.name)))return false;
    return true;
}
inline bool nativeValueValid(const LoadNackPacket& v) {
    if(!memchr(v.name,0,sizeof(v.name)))return false;
    return true;
}
inline bool nativeValueValid(const WorldReadyPacket& v) {
    if(!v.worldToken || !v.loadId)return false;
    return true;
}
inline bool nativeValueValid(const ProdPacket& v) {
    if(!nativeFinite(v.outAmount))return false;
    if(!memchr(v.outSid,0,sizeof(v.outSid)))return false;
    for(unsigned i=0;i<2;++i)if(!nativeFinite(v.inAmount[i]))return false;
    if(!nativeFinite(v.grown))return false;
    if(!nativeFinite(v.died))return false;
    if(!nativeFinite(v.growStart))return false;
    if(!nativeFinite(v.harvested))return false;
    if(v.keyKind>1 || v.powerOn < -1 || v.powerOn>1)return false;
    return true;
}
inline bool nativeValueValid(const ResearchPacket& v) {
    if(!memchr(v.sid,0,sizeof(v.sid)))return false;
    return true;
}
inline bool nativeValueValid(const CamHintPacket& v) {
    if(!nativeFinite(v.x))return false;
    if(!nativeFinite(v.y))return false;
    if(!nativeFinite(v.z))return false;
    return true;
}
inline bool nativeInventoryValid(const void* p,unsigned count) {
    if (count>INV_SNAPSHOT_ITEMS_MAX || (count && !p)) return false;
    const u8* rows=(const u8*)p;
    u8 state[INV_SNAPSHOT_ITEMS_MAX];
    memset(state,0,count);
    for(unsigned i=0;i<count;++i) {
        InvItemEntry v;memcpy(&v,rows+i*sizeof(v),sizeof(v));
        if(!nativeValueValid(v) || v.parentIdx>count)return false;
    }
    // Each row has one parent. Colour each path once; no recursion, allocations
    // or repeated copies of the full item record on deep nested inventories.
    for(unsigned i=0;i<count;++i) {
        unsigned at=i;
        while(at<count && !state[at]) {
            state[at]=1;
            u16 parent;
            memcpy(&parent,rows+at*sizeof(InvItemEntry)+offsetof(InvItemEntry,parentIdx),sizeof(parent));
            at=parent ? parent-1 : count;
        }
        if(at<count && state[at]==1)return false;
        at=i;
        while(at<count && state[at]==1) {
            state[at]=2;
            u16 parent;
            memcpy(&parent,rows+at*sizeof(InvItemEntry)+offsetof(InvItemEntry,parentIdx),sizeof(parent));
            at=parent ? parent-1 : count;
        }
    }
    return true;
}
} // namespace coop
#endif
