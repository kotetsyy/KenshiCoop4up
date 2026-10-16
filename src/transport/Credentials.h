#ifndef KENSHICOOP_CREDENTIALS_H
#define KENSHICOOP_CREDENTIALS_H
#include "TransportApi.h"
#include <string>
class ISteamNetworkingSockets;
namespace coopnet {
// Only the UDP DLL compiles this file; no private keys cross the C ABI except
// the explicitly requested, out-of-band client invitation.
struct UdpCredential {
    std::string root;
    std::string leaf;
    std::string hostIdentity;
    std::string clientPrefix;
    std::string localIdentity;
    ~UdpCredential();
};
bool loadUdpCredential(const CoopNetConfig& config, UdpCredential& credential);
bool installUdpRoot(const UdpCredential& credential);
bool installUdpCertificate(ISteamNetworkingSockets* sockets, const UdpCredential& credential);
void clearUdpRoots();
int32_t provisionUdpHost(const char* path, char* error, uint32_t capacity);
int32_t issueUdpInvite(const char* path, char* invite, uint32_t inviteCapacity,
                       char* error, uint32_t errorCapacity);
}
#endif
