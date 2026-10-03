// Shared Windows x64 C ABI. No STL, C++ ownership, engine types or network hooks.
#ifndef KENSHICOOP_UI_API_H
#define KENSHICOOP_UI_API_H

#define COOP_UI_API_VERSION 1u
#define COOP_UI_CALL __cdecl

// Do not inherit packing from an engine/wire header included by a consumer.
#pragma pack(push, 8)

#ifdef __cplusplus
extern "C" {
#endif

typedef enum CoopUiNetworkPhase {
    COOP_OFFLINE, COOP_STARTING, COOP_HOSTING, COOP_CONNECTING,
    COOP_HANDSHAKING, COOP_CONNECTED, COOP_RECONNECTING, COOP_FAILED
} CoopUiNetworkPhase;
typedef enum CoopUiWorldPhase {
    COOP_WORLD_NONE, COOP_WORLD_WAITING, COOP_WORLD_PREPARING,
    COOP_WORLD_RECEIVING, COOP_WORLD_LOADING, COOP_WORLD_READY, COOP_WORLD_FAILED
} CoopUiWorldPhase;

typedef struct CoopUiPlayer {
    unsigned int id;
    char name[64];
    int local;
    int worldReadyKnown;
    int worldReady;
} CoopUiPlayer;

// Borrowed UTF-8 strings are valid only during tick(). The UI copies what it
// retains. All fields are initialised by the core. Unknown facts stay unknown.
typedef struct CoopUiSnapshot {
    unsigned int structSize;
    unsigned long long selfSteamId;
    unsigned long long peerSteamId;
    int running, peerPresent, isHost;
    int transportSel;              // 0 Steam, 1 UDP: requested, not actual
    const char* udpIp;
    int udpPort;
    const char* playerName;
    const char* detail;
    const char* transferDetail;
    const char* updateDetail;
    const char* versionText;
    int phase, worldPhase;
    int busy;
    int activeTransport;           // -1 inactive, 0 Steam, 1 UDP
    const char* errorDetail;
    const char* diagnosticsDetail;
    unsigned long long receivedBytes, totalBytes;
    int playerCount;
    CoopUiPlayer players[4];
} CoopUiSnapshot;

typedef enum CoopUiCommandKind {
    COOP_UI_NONE, COOP_UI_REMEMBER, COOP_UI_CONNECT, COOP_UI_DISCONNECT
} CoopUiCommandKind;

typedef struct CoopUiSettings {
    int isHost;
    int useSteam;
    unsigned long long hostSteamId;
    int udpPort;
    char udpIp[256];
    char playerName[64];
} CoopUiSettings;

// Owned POD output. GUI callbacks only queue it; the core executes it after
// tick() returns, on the game's main thread. CONNECT includes current settings.
typedef struct CoopUiCommand {
    unsigned int structSize;
    int kind;
    CoopUiSettings settings;
} CoopUiCommand;

typedef struct CoopUiHost {
    unsigned int structSize;
    unsigned int apiVersion;
    void (COOP_UI_CALL *log)(const char* utf8, int error);
} CoopUiHost;

typedef struct CoopUiApi {
    unsigned int structSize;
    unsigned int apiVersion;
    int (COOP_UI_CALL *initialize)(const CoopUiHost* host);
    void (COOP_UI_CALL *tick)(const CoopUiSnapshot* state, CoopUiCommand* command);
    void (COOP_UI_CALL *shutdown)(void); // main thread; hide only, never disconnect
} CoopUiApi;

typedef int (COOP_UI_CALL *CoopUiGetApiFn)(unsigned int requestedVersion,
                                        unsigned int apiSize, CoopUiApi* api);
// Provider exports exactly "KenshiCoopUI_GetApi" with extern C linkage.

#pragma pack(pop)

#ifdef __cplusplus
}
#endif
#endif
