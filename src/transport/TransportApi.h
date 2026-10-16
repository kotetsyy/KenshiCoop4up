#ifndef KENSHICOOP_TRANSPORT_API_H
#define KENSHICOOP_TRANSPORT_API_H

/* C ABI only. Core/UI remain VC2010; providers own their contexts and messages.
   All functions are __cdecl. No exceptions, STL or cross-CRT frees cross here.
   API calls for a context are confined to its owning network thread. */
#include <stddef.h>
#include <stdint.h>

#define COOP_NET_ABI_VERSION 2u
#define COOP_NET_MAX_MESSAGE (512u * 1024u)
/* Pinned GNS silently makes larger "unreliable" messages reliable. */
#define COOP_NET_MAX_STATE_MESSAGE 16500u
#define COOP_NET_ERROR_CAPACITY 256u
#define COOP_NET_INVITE_CAPACITY 4096u
#define COOP_NET_MAX_PEERS 3u

#if defined(_WIN32)
#define COOP_NET_CALL __cdecl
#else
#define COOP_NET_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

#pragma pack(push, 8)

typedef enum CoopNetMode {
    COOP_NET_STEAM = 0,
    COOP_NET_UDP = 1
} CoopNetMode;

typedef enum CoopNetTraffic {
    GAME_RELIABLE = 0,
    GAME_STATE = 1,
    SAVE_BULK = 2
} CoopNetTraffic;

typedef enum CoopNetResult {
    COOP_NET_OK = 0,
    COOP_NET_BACKPRESSURE = 1,
    COOP_NET_INVALID = 2,
    COOP_NET_CLOSED = 3,
    COOP_NET_UNAVAILABLE = 4,
    COOP_NET_AUTH_FAILED = 5,
    COOP_NET_INTERNAL = 6
} CoopNetResult;

typedef enum CoopNetEventType {
    COOP_NET_EVENT_NONE = 0,
    COOP_NET_EVENT_CONNECTING = 1,
    COOP_NET_EVENT_FINDING_ROUTE = 2,
    COOP_NET_EVENT_CONNECTED = 3,
    COOP_NET_EVENT_DISCONNECTED = 4,
    COOP_NET_EVENT_MESSAGE = 5,
    COOP_NET_EVENT_ERROR = 6
} CoopNetEventType;

typedef struct CoopNetConfig {
    uint32_t size;
    uint32_t mode;
    uint32_t host;
    uint32_t port;
    uint64_t generation;
    uint64_t remoteSteamId;
    uint32_t connectTimeoutMs;
    uint32_t connectedTimeoutMs;
    uint32_t sendBufferBytes;
    uint32_t bulkQueueBytes;
    uint32_t maxPeers;
    /* UTF-8, borrowed for start only. UDP address is numeric IPv4/IPv6;
       profilePath is a private local host credential, never a shipped asset.
       joinInvite is an out-of-band per-client credential; never log its bytes. */
    const char* address;
    const char* profilePath;
    const char* joinInvite;
} CoopNetConfig;

typedef struct CoopNetEvent {
    uint32_t size;
    uint32_t type;
    uint64_t generation;
    uint32_t connection;
    uint32_t traffic;
    uint64_t remoteSteamId;
    int32_t reason;
    uint32_t dataSize;
    const void* data;
    /* Opaque provider-owned message. Release exactly once before destroy. */
    void* message;
    char detail[COOP_NET_ERROR_CAPACITY];
} CoopNetEvent;

typedef struct CoopNetPeerStats {
    uint32_t size;
    uint32_t connection;
    int32_t rttMs;
    uint32_t pendingReliableBytes;
    uint32_t pendingStateBytes;
    uint32_t pendingBulkBytes;
    uint32_t unackedReliableBytes;
    uint32_t state;
    float inboundBytesPerSec;
    float outboundBytesPerSec;
    float localQuality;
    float remoteQuality;
} CoopNetPeerStats;

typedef struct CoopNetStatus {
    uint32_t size;
    uint32_t mode;
    uint64_t generation;
    uint64_t selfSteamId;
    uint32_t listening;
    uint32_t connections;
    int32_t authentication;
    int32_t relayAvailability;
    char detail[COOP_NET_ERROR_CAPACITY];
} CoopNetStatus;

typedef struct CoopNetApi {
    uint32_t version;
    uint32_t size;
    void* (COOP_NET_CALL *create)(char* error, uint32_t errorCapacity);
    /* start is synchronous only for local initialization, not connection.
       Mode mismatch fails before initializing the wrong provider. */
    int32_t (COOP_NET_CALL *start)(void* context, const CoopNetConfig* config,
                                 char* error, uint32_t errorCapacity);
    /* poll returns one event. Zero = no event; negative = provider error.
       Receive data remains valid only until releaseEvent. */
    int32_t (COOP_NET_CALL *poll)(void* context, CoopNetEvent* event);
    /* send borrows bytes for this call, copies into Valve-owned storage.
       BACKPRESSURE consumes nothing; reliable caller must retain/retry or
       fail the session explicitly. GAME_STATE may be dropped/coalesced. */
    int32_t (COOP_NET_CALL *send)(void* context, uint32_t connection,
                                uint32_t traffic, const void* data, uint32_t size);
    void (COOP_NET_CALL *releaseEvent)(void* context, CoopNetEvent* event);
    int32_t (COOP_NET_CALL *disconnect)(void* context, uint32_t connection,
                                      int32_t reason, const char* detail);
    void (COOP_NET_CALL *stop)(void* context);
    void (COOP_NET_CALL *destroy)(void* context);
    int32_t (COOP_NET_CALL *status)(void* context, CoopNetStatus* status);
    int32_t (COOP_NET_CALL *peerStats)(void* context, uint32_t connection,
                                     CoopNetPeerStats* stats);
    /* UDP only. Private host profile provision and per-client invite issue
       use Valve's certificate/crypto implementation, not a custom cipher. */
    int32_t (COOP_NET_CALL *prepareHost)(const char* profilePath,
                                      char* error, uint32_t errorCapacity);
    int32_t (COOP_NET_CALL *makeInvite)(const char* profilePath, char* invite,
                                     uint32_t inviteCapacity, char* error,
                                     uint32_t errorCapacity);
    /* Steam only, MAIN GAME THREAD ONLY. The shared Steam interface may
       dispatch callbacks owned by the game. Never call this from the worker.
       No context parameter: backend pointer publication/lifetime is owned
       inside the provider. UDP explicitly rejects this operation. */
    int32_t (COOP_NET_CALL *pumpMainThread)(void);
} CoopNetApi;

#pragma pack(pop)

typedef int32_t (COOP_NET_CALL *CoopNetGetApiFn)(uint32_t version,
                                               uint32_t tableSize,
                                               CoopNetApi* table);

#ifdef __cplusplus
}
#endif

#endif
