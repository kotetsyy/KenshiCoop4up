#define _CRT_SECURE_NO_WARNINGS 1
#include "SteamInvite.h"
#include "../core/Config.h"
#include "../CoopLog.h"
#include "../../netproto/Wire.h"
#include <windows.h>
#include <cstdio>
#include <cstring>

namespace coop { namespace steaminvite { namespace {
typedef unsigned long long Call;
typedef int (*HandleFn)();
typedef void* (*ClientFn)();
typedef void* (*IfaceFn)(void*, int, int, const char*);
typedef void* (*UtilsIfaceFn)(void*, int, const char*);
typedef SteamId (*IdFn)(void*);
typedef bool (*LoggedFn)(void*);
typedef unsigned (*AppFn)(void*);
typedef Call (*CreateFn)(void*, int, int);
typedef Call (*JoinFn)(void*, SteamId);
typedef void (*LeaveFn)(void*, SteamId);
typedef bool (*SetDataFn)(void*, SteamId, const char*, const char*);
typedef const char* (*GetDataFn)(void*, SteamId, const char*);
typedef SteamId (*OwnerFn)(void*, SteamId);
typedef int (*RelationFn)(void*, SteamId);
typedef void (*RegisterFn)(void*, int);
typedef void (*RegisterResultFn)(void*, Call);
typedef void (*UnregisterFn)(void*);
typedef void (*UnregisterResultFn)(void*, Call);
typedef void (*PumpFn)();
#pragma pack(push, 8)
struct Created { int result; SteamId lobby; };
struct Requested { SteamId lobby, friendId; };
struct Entered { SteamId lobby; unsigned permissions; unsigned char locked; unsigned response; };
struct GameInfo { SteamId game; unsigned ip; unsigned short port, queryPort; SteamId lobby; };
#pragma pack(pop)
typedef bool (*GameFn)(void*, SteamId, GameInfo*);
const int CB_CREATED = 513, CB_ENTERED = 504, CB_REQUESTED = 533;
struct Intent { int kind; bool failed; SteamId epoch, generation, lobby, friendId; unsigned result; };
Intent g_intents[16]; unsigned g_intentCount = 0;
SteamId g_epoch = 1, g_generation = 0, g_self = 0, g_lobby = 0;
SteamId g_requestedLobby = 0, g_requestedFriend = 0;
bool g_ready = false, g_host = false, g_running = false, g_joinDelivered = false;
bool g_lobbyAttempted = false;
Call g_createCall = 0, g_joinCall = 0;
ConnectFn g_connect = 0;
void* g_mm = 0; void* g_friends = 0;
CreateFn g_create = 0; JoinFn g_join = 0; LeaveFn g_leave = 0;
SetDataFn g_set = 0; GetDataFn g_get = 0; OwnerFn g_owner = 0;
RelationFn g_relation = 0; GameFn g_game = 0;
RegisterFn g_register = 0; RegisterResultFn g_registerResult = 0;
UnregisterFn g_unregister = 0; UnregisterResultFn g_unregisterResult = 0; PumpFn g_pump = 0;

void enqueue(int kind, void* param, bool failed, Call call);
class Callback {
public:
    virtual void Run(void* p) { enqueue(id, p, false, 0); }
    virtual void Run(void* p, bool failed, Call call) { enqueue(id, p, failed, call); }
    virtual int GetCallbackSizeBytes() { return size; }
    unsigned char flags; int id; int size;
    void arm(int cb, int bytes) { flags = 0; id = cb; size = bytes; }
};
Callback g_created, g_entered, g_requested;
void enqueue(int kind, void* param, bool failed, Call call) {
    if (!g_ready || (!param && !failed) || g_intentCount == 16) return;
    if (kind == CB_REQUESTED && (g_running || g_joinCall || g_joinDelivered)) return;
    for (unsigned n = 0; n < g_intentCount; ++n)
        if (g_intents[n].kind == kind) return; // bounded one outstanding intent per callback kind
    if ((kind == CB_CREATED && (!g_createCall || call != g_createCall)) ||
        (kind == CB_ENTERED && (!g_joinCall || call != g_joinCall))) return;
    Intent i; memset(&i, 0, sizeof(i));
    i.kind = kind; i.failed = failed; i.epoch = g_epoch; i.generation = g_generation;
    if (!failed && kind == CB_CREATED) { Created* r = (Created*)param; i.lobby = r->lobby; i.result = r->result; }
    else if (!failed && kind == CB_ENTERED) { Entered* r = (Entered*)param; i.lobby = r->lobby; i.result = r->response; i.friendId = g_requestedFriend; }
    else if (!failed && kind == CB_REQUESTED) { Requested* r = (Requested*)param; i.lobby = r->lobby; i.friendId = r->friendId; }
    else if (!failed) return;
    g_intents[g_intentCount++] = i;
}
bool friendInLobby(SteamId id, SteamId lobby) {
    GameInfo info; memset(&info, 0, sizeof(info));
    return validSteamPeerId(id) && id != g_self && g_relation(g_friends, id) == 3 &&
           g_game(g_friends, id, &info) && info.game == 233860 && info.lobby == lobby;
}
bool lobbyProtocol(SteamId lobby) {
    const char* game = g_get(g_mm, lobby, "kc_game");
    if (!game || strcmp(game, "KenshiCoop")) return false;
    char protocol[16]; _snprintf(protocol, sizeof(protocol), "%u", (unsigned)PROTOCOL_VERSION);
    const char* version = g_get(g_mm, lobby, "kc_protocol");
    return version && !strcmp(version, protocol);
}
} // namespace

bool init(ConnectFn connect) {
    if (g_ready) { g_connect = connect; return true; }
    HMODULE mod = GetModuleHandleA("steam_api64.dll");
    if (!mod) return false;
#define BIND(type, name, exportName) type name = (type)GetProcAddress(mod, exportName)
    BIND(HandleFn, userHandle, "SteamAPI_GetHSteamUser");
    BIND(HandleFn, pipeHandle, "SteamAPI_GetHSteamPipe");
    BIND(ClientFn, clientFn, "SteamClient");
    BIND(IfaceFn, userFn, "SteamAPI_ISteamClient_GetISteamUser");
    BIND(UtilsIfaceFn, utilsFn, "SteamAPI_ISteamClient_GetISteamUtils");
    BIND(IfaceFn, mmFn, "SteamAPI_ISteamClient_GetISteamMatchmaking");
    BIND(IfaceFn, friendsFn, "SteamAPI_ISteamClient_GetISteamFriends");
    BIND(IdFn, selfFn, "SteamAPI_ISteamUser_GetSteamID");
    BIND(LoggedFn, loggedFn, "SteamAPI_ISteamUser_BLoggedOn");
    BIND(AppFn, appFn, "SteamAPI_ISteamUtils_GetAppID");
#undef BIND
#define EXPORT(field, type, suffix) field = (type)GetProcAddress(mod, "SteamAPI_" suffix)
    EXPORT(g_create, CreateFn, "ISteamMatchmaking_CreateLobby");
    EXPORT(g_join, JoinFn, "ISteamMatchmaking_JoinLobby");
    EXPORT(g_leave, LeaveFn, "ISteamMatchmaking_LeaveLobby");
    EXPORT(g_set, SetDataFn, "ISteamMatchmaking_SetLobbyData");
    EXPORT(g_get, GetDataFn, "ISteamMatchmaking_GetLobbyData");
    EXPORT(g_owner, OwnerFn, "ISteamMatchmaking_GetLobbyOwner");
    EXPORT(g_relation, RelationFn, "ISteamFriends_GetFriendRelationship");
    EXPORT(g_game, GameFn, "ISteamFriends_GetFriendGamePlayed");
    EXPORT(g_register, RegisterFn, "RegisterCallback");
    EXPORT(g_registerResult, RegisterResultFn, "RegisterCallResult");
    EXPORT(g_unregister, UnregisterFn, "UnregisterCallback");
    EXPORT(g_unregisterResult, UnregisterResultFn, "UnregisterCallResult");
    EXPORT(g_pump, PumpFn, "RunCallbacks");
#undef EXPORT
    if (!userHandle || !pipeHandle || !clientFn || !userFn || !utilsFn || !mmFn || !friendsFn ||
        !selfFn || !loggedFn || !appFn || !g_create || !g_join || !g_leave ||
        !g_set || !g_get || !g_owner || !g_relation || !g_game || !g_register ||
        !g_registerResult || !g_unregister || !g_unregisterResult || !g_pump) return false;
    void* client = clientFn(); int user = userHandle(), pipe = pipeHandle();
    if (!client || !user || !pipe) return false;
    void* steamUser = userFn(client, user, pipe, "SteamUser019");
    void* utils = utilsFn(client, pipe, "SteamUtils007");
    g_mm = mmFn(client, user, pipe, "SteamMatchMaking009");
    g_friends = friendsFn(client, user, pipe, "SteamFriends015");
    if (!steamUser || !utils || !g_mm || !g_friends || !loggedFn(steamUser) || appFn(utils) != 233860) return false;
    g_self = selfFn(steamUser);
    if (!validSteamPeerId(g_self)) { g_self = 0; return false; }
    g_connect = connect;
    g_created.arm(CB_CREATED, sizeof(Created));
    g_entered.arm(CB_ENTERED, sizeof(Entered));
    g_requested.arm(CB_REQUESTED, sizeof(Requested));
    g_register(&g_requested, CB_REQUESTED);
    g_ready = true;
    return true;
}
SteamId selfId() { return g_ready ? g_self : 0; }

void synchronize(bool host, bool running, SteamId generation) {
    if (!g_ready) return;
    if (g_generation != generation || g_host != host || g_running != running) {
        if (g_running && !running) g_joinDelivered = false;
        if (g_running && !running && g_lobby) {
            g_leave(g_mm, g_lobby); g_lobby = 0;
        }
        ++g_epoch; g_intentCount = 0;
        if (g_createCall) g_unregisterResult(&g_created, g_createCall);
        if (g_joinCall) g_unregisterResult(&g_entered, g_joinCall);
        g_createCall = g_joinCall = 0;
        g_requestedLobby = g_requestedFriend = 0;
        g_lobbyAttempted = false;
    }
    g_generation = generation; g_host = host; g_running = running;
    if (running && host && !g_lobby && !g_createCall && !g_lobbyAttempted) {
        g_lobbyAttempted = true;
        g_createCall = g_create(g_mm, 1, (int)MAX_PLAYERS); // friends-only host+3
        if (g_createCall) g_registerResult(&g_created, g_createCall);
    }
}
void pumpCallbacks() { if (g_ready) g_pump(); }
void processIntents() {
    if (!g_ready) return;
    // Snapshot before invoking any plugin action; callbacks only ever enqueue PODs.
    Intent pending[16]; unsigned count = g_intentCount;
    memcpy(pending, g_intents, count * sizeof(Intent)); g_intentCount = 0;
    for (unsigned n = 0; n < count; ++n) {
        const Intent& i = pending[n];
        if (i.epoch != g_epoch || i.generation != g_generation) continue;
        if (i.kind == CB_CREATED) g_createCall = 0;
        if (i.kind == CB_ENTERED) g_joinCall = 0;
        if (i.failed || !i.lobby) continue;
        if (i.kind == CB_CREATED) {
            if (!g_running || !g_host || i.result != 1 || g_owner(g_mm, i.lobby) != g_self) {
                if (i.result == 1) g_leave(g_mm, i.lobby);
                continue;
            }
            char protocol[16]; _snprintf(protocol, sizeof(protocol), "%u", (unsigned)PROTOCOL_VERSION);
            if (!g_set(g_mm, i.lobby, "kc_game", "KenshiCoop") || !g_set(g_mm, i.lobby, "kc_protocol", protocol)) {
                g_leave(g_mm, i.lobby); continue;
            }
            g_lobby = i.lobby;
        } else if (i.kind == CB_REQUESTED) {
            if (g_running || g_joinCall || !friendInLobby(i.friendId, i.lobby)) continue;
            if (g_lobby) g_leave(g_mm, g_lobby);
            g_lobby = 0; g_joinDelivered = false;
            g_requestedLobby = i.lobby; g_requestedFriend = i.friendId;
            g_joinCall = g_join(g_mm, i.lobby);
            if (g_joinCall) g_registerResult(&g_entered, g_joinCall);
        } else if (i.kind == CB_ENTERED) {
            if (g_running || g_joinDelivered || i.result != 1 || i.lobby != g_requestedLobby ||
                i.friendId != g_requestedFriend || !friendInLobby(i.friendId, i.lobby) ||
                g_owner(g_mm, i.lobby) != i.friendId || !lobbyProtocol(i.lobby)) {
                g_leave(g_mm, i.lobby); continue;
            }
            g_lobby = i.lobby; g_joinDelivered = true;
            if (g_connect) g_connect(i.friendId);
            return; // next hook synchronizes the new session before another join intent
        }
    }
}
void reset() {
    ++g_epoch; g_intentCount = 0;
    if (g_ready) {
        if (g_lobby) g_leave(g_mm, g_lobby);
        if (g_requestedLobby && g_requestedLobby != g_lobby) g_leave(g_mm, g_requestedLobby);
        if (g_createCall) g_unregisterResult(&g_created, g_createCall);
        if (g_joinCall) g_unregisterResult(&g_entered, g_joinCall);
        g_unregister(&g_requested);
    }
    g_ready = g_host = g_running = g_joinDelivered = false;
    g_lobbyAttempted = false;
    g_self = g_lobby = g_generation = g_requestedLobby = g_requestedFriend = 0;
    g_createCall = g_joinCall = 0;
    memset(g_intents, 0, sizeof(g_intents));
    g_mm = g_friends = 0; g_connect = 0;
}
} }
