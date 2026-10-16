// Numeric direct-UDP endpoint grammar shared by core and F2.
// IPv4[:port], bare IPv6, [IPv6][:port], or address followed by a port.
// DNS names and trailing tokens are rejected; output is unchanged on failure.

#ifndef COOP_UDP_ENDPOINT_H
#define COOP_UDP_ENDPOINT_H

#include <string>

// The stable Winsock parser needs no WSAStartup or socket. Forward declaration
// avoids pulling conflicting Winsock headers into the legacy engine headers.
extern "C" __declspec(dllimport) int __stdcall inet_pton(
    int family, const char* address, void* output);
#pragma comment(lib, "ws2_32.lib")

namespace coop {

inline bool udpEndpointSpace(char ch) {
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
}

inline bool parseUdpEndpoint(const std::string& text, std::string& ip, int& port) {
    if (text.empty() || text.size() > 160) return false;
    size_t a = 0, b = text.size();
    while (a < b && udpEndpointSpace(text[a])) ++a;
    while (b > a && udpEndpointSpace(text[b - 1])) --b;
    if (a == b) return false;

    size_t hostStart = a, hostEnd = b, tail = b;
    size_t portStart = b;
    bool explicitPort = false, bracketed = text[a] == '[';
    if (bracketed) {
        hostStart = a + 1;
        hostEnd = text.find(']', hostStart);
        if (hostEnd == std::string::npos || hostEnd >= b) return false;
        tail = hostEnd + 1;
        if (tail < b && text[tail] == ':') {
            explicitPort = true;
            portStart = ++tail;
        } else if (tail < b && !udpEndpointSpace(text[tail])) {
            return false;
        }
    } else {
        hostEnd = a;
        unsigned colons = 0;
        size_t colon = b;
        while (hostEnd < b && !udpEndpointSpace(text[hostEnd])) {
            if (text[hostEnd] == ':') { ++colons; colon = hostEnd; }
            ++hostEnd;
        }
        tail = hostEnd;
        // A bare IPv6 suffix is an address group, never an implicit port.
        if (colons == 1) {
            explicitPort = true;
            portStart = colon + 1;
            hostEnd = colon;
        }
    }

    size_t portEnd = b;
    if (!explicitPort && tail < b) {
        while (tail < b && udpEndpointSpace(text[tail])) ++tail;
        if (tail < b) { explicitPort = true; portStart = tail; }
    } else if (explicitPort && tail < b) {
        // For IPv4:port the remaining whitespace is trailing-only.
        while (tail < b && udpEndpointSpace(text[tail])) ++tail;
        if (tail != b && tail != portStart) return false;
    }
    if (explicitPort) {
        portEnd = portStart;
        while (portEnd < b && !udpEndpointSpace(text[portEnd])) ++portEnd;
        size_t after = portEnd;
        while (after < b && udpEndpointSpace(text[after])) ++after;
        if (after != b || portStart == portEnd || portEnd - portStart > 5)
            return false;
    }

    const size_t hostSize = hostEnd - hostStart;
    if (!hostSize || hostSize > 128) return false;
    char numeric[129];
    for (size_t i = 0; i < hostSize; ++i) {
        const char ch = text[hostStart + i];
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') ||
              (ch >= 'A' && ch <= 'F') || ch == ':' || ch == '.'))
            return false;
        numeric[i] = ch;
    }
    numeric[hostSize] = '\0';
    unsigned char address[16];
    const int windowsAfInet = 2, windowsAfInet6 = 23;
    if (bracketed ? inet_pton(windowsAfInet6, numeric, address) != 1 :
        (inet_pton(windowsAfInet, numeric, address) != 1 &&
         inet_pton(windowsAfInet6, numeric, address) != 1))
        return false;

    int parsedPort = port;
    if (explicitPort) {
        parsedPort = 0;
        for (size_t i = portStart; i < portEnd; ++i) {
            if (text[i] < '0' || text[i] > '9') return false;
            parsedPort = parsedPort * 10 + text[i] - '0';
        }
        if (parsedPort < 1 || parsedPort > 65535) return false;
    }
    ip.assign(numeric, hostSize);
    port = parsedPort;
    return true;
}

} // namespace coop

#endif // COOP_UDP_ENDPOINT_H
