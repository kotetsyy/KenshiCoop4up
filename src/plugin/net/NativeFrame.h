#ifndef KENSHICOOP_NATIVE_FRAME_H
#define KENSHICOOP_NATIVE_FRAME_H
#include "../../netproto/Wire.h"
#include "../../transport/TransportApi.h"
#include "NativeValues.h"
#include "../core/EnvironmentItemsState.h"
#include <string.h>
namespace coop {
#pragma pack(push,1)
struct NativeFrame {
    u32 magic;
    u16 version;
    u8 lane;
    u8 reserved;
    u64 incarnation; // authenticated sending connection's process/session incarnation
    u64 session;     // negotiated host session; HELLO alone uses zero
    u64 world;       // host-coordinated world, zero for session controls
    u64 sequence;    // per sending connection/lane, replay protected
    u64 originIncarnation;
    u64 originSequence; // preserved by host relay for per-entity chronology
    u32 originEpoch; // sender capture epoch; retained unchanged by host relay
    u32 captureMs; // captured with origin sequence and local publication epoch
    u32 origin;
    u32 payloadBytes;
};
#pragma pack(pop)
const u32 NATIVE_FRAME_MAGIC = 0x32434e4bu;
struct NativeReplay {
    u64 newest;
    NativeReplay():newest(0) {}
    bool accept(u64 n) { if (!n || n<=newest) return false; newest=n; return true; }
};
inline bool nativeSessionType(u8 t) {
    switch(t) {
    case PKT_HELLO: case PKT_WELCOME: case PKT_PLAYER_ROSTER:
    case PKT_SAVE_REQ: case PKT_SAVE_BEGIN: case PKT_SAVE_FILE: case PKT_SAVE_DONE: case PKT_SAVE_ACK:
    case PKT_LOAD_GO: case PKT_LOAD_REQ: case PKT_LOAD_NACK: case PKT_WORLD_READY: case PKT_SESSION_ACK:
    case PKT_TIME: case PKT_TIME_PING: case PKT_TIME_PONG:
    case PKT_PROCEDURAL: case PKT_ENVIRONMENT_ITEMS:
#ifdef KENSHICOOP_NET_DIAG
    case PKT_DEBUG_LOG_FILE:
#endif
        return true;
    default: return false;
    }
}
inline unsigned nativeLane(u8 t) {
    switch(t) {
    case PKT_ENTITY_BATCH: case PKT_STEALTH: case PKT_CAM_HINT: case PKT_TIME_PING: case PKT_TIME_PONG: return GAME_STATE;
    case PKT_SAVE_REQ: case PKT_SAVE_BEGIN: case PKT_SAVE_FILE: case PKT_SAVE_DONE: case PKT_SAVE_ACK:
    case PKT_LOAD_GO: case PKT_LOAD_REQ: case PKT_LOAD_NACK:
#ifdef KENSHICOOP_NET_DIAG
    case PKT_DEBUG_LOG_FILE:
#endif
        return SAVE_BULK;
    default: return GAME_RELIABLE;
    }
}
inline bool nativeDirection(u8 t,bool receiverHost,u32 origin) {
    switch(t) {
    case PKT_HELLO: case PKT_SPEED_REQ: case PKT_COMBAT_HIT: case PKT_MONEY_DELTA:
    case PKT_SAVE_REQ: case PKT_SAVE_ACK: case PKT_LOAD_REQ: case PKT_LOAD_NACK: case PKT_WORLD_READY: case PKT_SESSION_ACK: case PKT_TIME_PING:
#ifdef KENSHICOOP_NET_DIAG
    case PKT_DEBUG_LOG_FILE:
#endif
        return receiverHost && origin>0 && origin<=MAX_JOINS;
    case PKT_DOOR:
        return receiverHost ? origin>0 && origin<=MAX_JOINS : origin==0;
    case PKT_WELCOME: case PKT_PLAYER_ROSTER: case PKT_SPEED_SET: case PKT_MONEY: case PKT_PROD:
    case PKT_RESEARCH: case PKT_SAVE_BEGIN: case PKT_SAVE_FILE:
    case PKT_SAVE_DONE: case PKT_LOAD_GO: case PKT_TIME_PONG: case PKT_STEALTH:
        return !receiverHost && origin==0;
    default: return origin<MAX_PLAYERS;
    }
}
template<class T> inline bool nativeRead(const void* p,unsigned n,T& out) {
    if (!p || n<sizeof(T)) return false; memcpy(&out,p,sizeof(T)); return nativeValueValid(out);
}
template<class T> inline bool nativeRowsValid(const void* p,unsigned count) {
    for(unsigned i=0;i<count;++i) {
        T v; memcpy(&v,(const u8*)p+i*sizeof(T),sizeof(T));
        if(!nativeValueValid(v))return false;
    }
    return true;
}
inline bool nativeCensusPositionsValid(const void* p,unsigned count) {
    for(unsigned i=0;i<count*3;++i) {
        f32 v;memcpy(&v,(const u8*)p+i*sizeof(v),sizeof(v));
        if(!nativeFinite(v))return false;
    }
    return true;
}
inline bool nativePacketValid(const void* p,unsigned n,unsigned lane,bool receiverHost,u32 origin) {
    if (!p || !n || origin>=MAX_PLAYERS) return false;
    const u8 t=*(const u8*)p;
    if (lane!=nativeLane(t) || !nativeDirection(t,receiverHost,origin)) return false;
#define FIXED(TAG,T) case TAG: { T v; return n==sizeof(v) && nativeRead(p,n,v) && v.ownerId==origin; }
    switch(t) {
    case PKT_HELLO: { HelloPacket v; return nativeRead(p,n,v) && v.nameLen<=HELLO_NAME_MAX && n==sizeof(v)+v.nameLen; }
    case PKT_PROCEDURAL: {
        ProceduralPacket v;
        if(n!=sizeof(v) || !nativeRead(p,n,v) || v.ownerId!=origin)return false;
        const bool request=v.operation==PROC_REQUEST || v.operation==PROC_ACK ||
            v.operation==PROC_RETIRE || v.operation==PROC_SCOPE_COMMIT;
        return request ? receiverHost && origin>0 && origin<=MAX_JOINS :
            !receiverHost && origin==0;
    }
    case PKT_ENVIRONMENT_ITEMS: {
        EnvironmentItemPacket v;
        if (n!=sizeof(v) || !nativeRead(p,n,v) || v.ownerId!=origin ||
            !v.worldToken || !v.loadId || v.count>ENVIRONMENT_ITEMS_MAX ||
            v.targetId>=MAX_PLAYERS || v.regionX < -4096 || v.regionX > 4096 ||
            v.regionZ < -4096 || v.regionZ > 4096) return false;
        const bool request=v.operation==ENV_REQUEST || v.operation==ENV_ACK ||
            v.operation==ENV_RETIRE || v.operation==ENV_SCOPE_COMMIT;
        if (request ? (!receiverHost || !origin || v.targetId!=0) :
            (receiverHost || origin!=0 || !v.targetId)) return false;
        if (v.operation==ENV_SCOPE_COMMIT)
            return !v.scopeId && !v.epoch && !v.digest && !v.regionX && !v.regionZ;
        if (!v.scopeId) return false;
        if (v.operation==ENV_REQUEST || v.operation==ENV_RETIRE)
            return !v.epoch && !v.count && !v.digest;
        if (v.operation!=ENV_BEGIN && v.operation!=ENV_ROW &&
            v.operation!=ENV_END && v.operation!=ENV_ACK) return false;
        if (!v.epoch || !v.digest) return false;
        return v.operation!=ENV_ROW || (v.count && environment::valid(v.object) &&
            nativeValueValid(v.object.item));
    }
    case PKT_WELCOME: { WelcomePacket v; if (!nativeRead(p,n,v) || v.playerId<1 || v.playerId>MAX_JOINS) return false;
        return n==sizeof(v) || (n>sizeof(v) && ((const u8*)p)[sizeof(v)]<=HELLO_NAME_MAX && n==sizeof(v)+1+((const u8*)p)[sizeof(v)]); }
    case PKT_ENTITY_BATCH: { EntityBatchHeader v; return nativeRead(p,n,v) && v.ownerId==origin && v.count>0 && v.count<=ENTITY_BATCH_MAX && n==sizeof(v)+v.count*sizeof(EntityState) && nativeRowsValid<EntityState>((const u8*)p+sizeof(v),v.count); }
    case PKT_EVENT: { EventPacket v; return n==sizeof(v) && nativeRead(p,n,v) && v.ownerId==origin && v.event>EVT_NONE && v.event<=EVT_SQUAD_MOVE; }
    case PKT_INV_SNAPSHOT: {
        InvSnapshotHeader v;
        if (!nativeRead(p,n,v) || v.ownerId!=origin || v.keyKind>1 ||
            v.count>INV_ITEMS_MAX || (v.flags & ~(INV_FLAG_TRUNCATED|INV_FLAG_TRANSACTION|
                INV_FLAG_CHECKPOINT|INV_FLAG_CHECKPOINT_ACK|INV_FLAG_COMPLETE)) ||
            n!=sizeof(v)+v.count*sizeof(InvItemEntry)) return false;
        if (v.flags & INV_FLAG_CHECKPOINT_ACK)
            return receiverHost && origin>0 && v.flags==INV_FLAG_CHECKPOINT_ACK &&
                v.revision && !v.count && !v.offset && !v.totalCount && !v.digest;
        if ((v.flags & INV_FLAG_TRANSACTION) && (receiverHost || origin!=0 || !v.revision)) return false;
        if ((v.flags & INV_FLAG_CHECKPOINT) && (!(v.flags & INV_FLAG_TRANSACTION) ||
            (v.flags & INV_FLAG_TRUNCATED))) return false;
        if (v.totalCount>INV_SNAPSHOT_ITEMS_MAX || v.offset>v.totalCount ||
            v.count>v.totalCount-v.offset ||
            ((v.flags & INV_FLAG_COMPLETE) && v.offset+v.count!=v.totalCount) ||
            (!v.count && !(v.flags & INV_FLAG_COMPLETE))) return false;
        for (unsigned i=0;i<v.count;++i) {
            InvItemEntry entry; memcpy(&entry,(const u8*)p+sizeof(v)+i*sizeof(entry),sizeof(entry));
            if (!nativeValueValid(entry) || entry.parentIdx>v.totalCount ||
                entry.parentIdx==v.offset+i+1) return false;
        }
        return true;
    }
    case PKT_WORLD_ITEM: { WorldItemSnapshotHeader v; return nativeRead(p,n,v) && v.ownerId==origin && v.count<=WORLD_ITEMS_MAX && n==sizeof(v)+v.count*sizeof(WorldItemEntry) && nativeRowsValid<WorldItemEntry>((const u8*)p+sizeof(v),v.count); }
    case PKT_WORLD_ITEM_REMOVE: { WorldItemRemoveHeader v; return nativeRead(p,n,v) && v.ownerId==origin && n==sizeof(v)+v.count*sizeof(u32); }
    case PKT_WORLD_ITEM_CLAIM: { WorldItemClaimHeader v; return nativeRead(p,n,v) && v.ownerId==origin && v.authorId<MAX_PLAYERS && n==sizeof(v)+v.count*sizeof(u32); }
    case PKT_NPC_CENSUS: { NpcCensusHeader v; return nativeRead(p,n,v) && v.ownerId==origin && v.count<=NPC_CENSUS_MAX && n==sizeof(v)+v.count*(5*sizeof(u32)+3*sizeof(f32)) && nativeCensusPositionsValid((const u8*)p+sizeof(v)+v.count*5*sizeof(u32),v.count); }
    case PKT_INV_XFER: {
        InvXferPacket v;
        if (!nativeRead(p,n,v) || v.ownerId!=origin ||
            (v.operation!=INV_OP_TRANSFER && (!receiverHost || origin==0))) return false;
        return v.contentsCount<=INV_ITEMS_MAX && v.contentsPresent<=1 &&
            (v.contentsPresent || !v.contentsCount) &&
            n==sizeof(v)+v.contentsCount*sizeof(InvItemEntry) &&
            nativeInventoryValid((const u8*)p+sizeof(v),v.contentsCount);
    }
    case PKT_INV_XFER_ACK: {
        InvXferAckPacket v;
        if (!nativeRead(p,n,v) || v.ownerId!=origin ||
            (v.operation!=INV_OP_TRANSFER && (receiverHost || origin!=0))) return false;
        return v.contentsCount<=INV_ITEMS_MAX && v.contentsPresent<=1 &&
            (v.contentsPresent || !v.contentsCount) &&
            n==sizeof(v)+v.contentsCount*sizeof(InvItemEntry) &&
            nativeInventoryValid((const u8*)p+sizeof(v),v.contentsCount);
    }
    case PKT_SAVE_FILE: { SaveFileHeader v; if (!nativeRead(p,n,v) || v.ownerId!=origin || !v.pathLen || v.pathLen>SAVE_PATH_MAX || v.dataLen>SAVE_CHUNK_MAX || n!=sizeof(v)+v.pathLen+v.dataLen || v.offset>0xffffffffu-v.dataLen) return false;
        const char* path=(const char*)p+sizeof(v); if (path[0]=='/' || path[0]=='\\') return false;
        unsigned component=0;
        for(unsigned i=0;i<=v.pathLen;++i) {
            if(i<v.pathLen && (!path[i] || path[i]==':'))return false;
            if(i==v.pathLen || path[i]=='/' || path[i]=='\\') {
                if(i==component || (i-component==2 && path[component]=='.' && path[component+1]=='.'))return false;
                component=i+1;
            }
        }
        return true; }
    case PKT_SAVE_DONE: { SaveDoneHeader v; return nativeRead(p,n,v) && v.ownerId==origin && n==sizeof(v)+v.fileCount*sizeof(u32); }
    case PKT_PLAYER_ROSTER: { RosterPacket v; return nativeRead(p,n,v) && v.ownerId==0 && n==sizeof(v)+1+MAX_PLAYERS*sizeof(u64) && !(((const u8*)p)[sizeof(v)] & ~((1u<<MAX_PLAYERS)-1u)); }
    case PKT_STEALTH: { StealthPacket v; return n==sizeof(v) && nativeRead(p,n,v) && v.ownerId==origin && v.nSeers<=STEALTH_SEER_MAX && v.unseen<=2; }
    case PKT_TIME_PING: return n==sizeof(TimePingPacket);
    case PKT_TIME_PONG: return n==sizeof(TimePongPacket);
#ifdef KENSHICOOP_NET_DIAG
    case PKT_DEBUG_LOG_FILE: { DebugLogFileHeader v; return nativeRead(p,n,v) && v.byteCount>0 && v.byteCount<=DEBUG_LOG_CHUNK_MAX && n==sizeof(v)+v.byteCount; }
#endif
    FIXED(PKT_WORLD_DROP,WorldDropPacket) FIXED(PKT_WORLD_PICKUP,WorldPickupPacket)
    FIXED(PKT_MEDICAL,MedicalPacket) FIXED(PKT_TREATMENT,TreatmentPacket) FIXED(PKT_COMBAT_HIT,CombatHitPacket)
    FIXED(PKT_SPEED_REQ,SpeedPacket) FIXED(PKT_SPEED_SET,SpeedPacket) FIXED(PKT_STATS,StatsPacket)
    FIXED(PKT_MONEY,MoneyPacket) FIXED(PKT_MONEY_DELTA,MoneyDeltaPacket) FIXED(PKT_FACTION,FactionPacket)
    FIXED(PKT_TIME,TimePacket) FIXED(PKT_DOOR,DoorPacket) FIXED(PKT_PROD,ProdPacket) FIXED(PKT_RESEARCH,ResearchPacket)
    FIXED(PKT_DEED,DeedPacket) FIXED(PKT_FIXTURE,FixturePacket) FIXED(PKT_BUILD_PLACE,BuildPlacePacket)
    FIXED(PKT_BUILD_STATE,BuildStatePacket) FIXED(PKT_BUILD_DOOR,BuildDoorPacket) FIXED(PKT_BUILD_REMOVE,BuildRemovePacket)
    FIXED(PKT_SPAWN_REQ,SpawnReqPacket) FIXED(PKT_SPAWN_INFO,SpawnInfoPacket) FIXED(PKT_SAVE_REQ,SaveReqPacket)
    FIXED(PKT_SAVE_BEGIN,SaveBeginPacket) FIXED(PKT_SAVE_ACK,SaveAckPacket) FIXED(PKT_LOAD_GO,LoadGoPacket)
    FIXED(PKT_LOAD_REQ,LoadReqPacket) FIXED(PKT_LOAD_NACK,LoadNackPacket) FIXED(PKT_WORLD_READY,WorldReadyPacket)
    FIXED(PKT_SESSION_ACK,SessionAckPacket)
    FIXED(PKT_CAM_HINT,CamHintPacket) FIXED(PKT_CELL_CLAIM,CellClaimPacket)
    default: return false;
    }
#undef FIXED
}
}
#endif
