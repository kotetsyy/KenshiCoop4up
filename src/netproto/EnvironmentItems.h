#ifndef COOP_ENVIRONMENT_ITEMS_WIRE_H
#define COOP_ENVIRONMENT_ITEMS_WIRE_H
#include "Wire.h"
namespace coop {
const unsigned ENVIRONMENT_ITEMS_MAX=1024;
enum EnvironmentOperation { ENV_REQUEST=1, ENV_BEGIN, ENV_ROW, ENV_END, ENV_ACK, ENV_RETIRE, ENV_SCOPE_COMMIT };
enum EnvironmentItemFlags { ENV_FIXED=1, ENV_DYNAMIC=2, ENV_VISIBLE=4, ENV_PERSISTENT=8, ENV_UNIQUE=16, ENV_TRADE=32, ENV_DEATH=64, ENV_BARE=128 };
#pragma pack(push,1)
// Anchors are native stable descriptors, never peer-local engine handles.
struct EnvironmentAnchor {
    char sid[48],instance[128];
    u32 present,type;
    u32 actorOwner,actorKey[5];
    float x,y,z;
};
struct EnvironmentItemObject {
    u64 id;
    InvItemEntry item;
    float x,y,z,rotationW,rotationX,rotationY,rotationZ;
    float condition,charges,fullCharge;
    i32 floor;
    u32 flags;
    EnvironmentAnchor parent,properOwner,persistentOwner;
};
struct EnvironmentItemPacket {
    u8 type,operation;
    u32 ownerId,targetId;
    u64 worldToken;
    u32 loadId,scopeId,epoch,count,digest;
    i32 regionX,regionZ;
    EnvironmentItemObject object;
};
#pragma pack(pop)
}
#endif
