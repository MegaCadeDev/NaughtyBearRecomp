// Xbox networking (xnet / Winsock) for Naughty Bear, as an always-offline console.
//
// The game initialises networking at startup and aborts if that fails, so startup and
// cleanup succeed. Everything that would touch the network fails the way an Xbox with no
// connection does: sockets report WSAENETDOWN, the title address is unavailable, and
// lookups fail. Unleashed Recompiled leaves these as stubs.
//
// Every NetDll_* export takes the caller type (XNCALLER_TYPE) as its first argument.

#include <stdafx.h>
#include <kernel/function.h>
#include <kernel/memory.h>
#include <random>

namespace
{
    constexpr uint32_t SOCKET_ERROR = 0xFFFFFFFF;
    constexpr uint32_t INVALID_SOCKET = 0xFFFFFFFF;
    constexpr uint32_t WSAENETDOWN = 10050;
    constexpr uint32_t WSA_WAIT_FAILED = 0xFFFFFFFF;
    constexpr uint32_t XNET_GET_XNADDR_NONE = 0x00000001;
    constexpr uint32_t XNET_CONNECT_STATUS_LOST = 0x00000003;

    thread_local uint32_t g_wsaLastError = 0;

    uint32_t NetworkDown()
    {
        g_wsaLastError = WSAENETDOWN;
        return SOCKET_ERROR;
    }
}

uint32_t NetDll_XNetStartup(uint32_t caller, void* params)
{
    return 0;
}

uint32_t NetDll_XNetCleanup(uint32_t caller)
{
    return 0;
}

uint32_t NetDll_WSAStartup(uint32_t caller, uint32_t versionRequested, uint8_t* wsaData)
{
    if (wsaData != nullptr)
    {
        // WSADATA: wVersion, wHighVersion, then descriptions we leave empty.
        memset(wsaData, 0, 0x190);
        *(be<uint16_t>*)wsaData = uint16_t(versionRequested);
        *(be<uint16_t>*)(wsaData + 2) = 0x0202;
    }
    return 0;
}

uint32_t NetDll_WSACleanup(uint32_t caller)
{
    return 0;
}

uint32_t NetDll_WSAGetLastError()
{
    return g_wsaLastError;
}

uint32_t NetDll_socket(uint32_t caller, uint32_t af, uint32_t type, uint32_t protocol)
{
    g_wsaLastError = WSAENETDOWN;
    return INVALID_SOCKET;
}

uint32_t NetDll_closesocket(uint32_t caller, uint32_t socket) { return NetworkDown(); }
uint32_t NetDll_shutdown(uint32_t caller, uint32_t socket, uint32_t how) { return NetworkDown(); }
uint32_t NetDll_setsockopt(uint32_t caller, uint32_t socket) { return NetworkDown(); }
uint32_t NetDll_ioctlsocket(uint32_t caller, uint32_t socket) { return NetworkDown(); }
uint32_t NetDll_getsockname(uint32_t caller, uint32_t socket) { return NetworkDown(); }
uint32_t NetDll_bind(uint32_t caller, uint32_t socket) { return NetworkDown(); }
uint32_t NetDll_connect(uint32_t caller, uint32_t socket) { return NetworkDown(); }
uint32_t NetDll_listen(uint32_t caller, uint32_t socket) { return NetworkDown(); }
uint32_t NetDll_accept(uint32_t caller, uint32_t socket) { g_wsaLastError = WSAENETDOWN; return INVALID_SOCKET; }
uint32_t NetDll_select(uint32_t caller) { return NetworkDown(); }
uint32_t NetDll_recv(uint32_t caller, uint32_t socket) { return NetworkDown(); }
uint32_t NetDll_recvfrom(uint32_t caller, uint32_t socket) { return NetworkDown(); }
uint32_t NetDll_send(uint32_t caller, uint32_t socket) { return NetworkDown(); }
uint32_t NetDll_sendto(uint32_t caller, uint32_t socket) { return NetworkDown(); }
uint32_t NetDll_WSARecvFrom(uint32_t caller, uint32_t socket) { return NetworkDown(); }
uint32_t NetDll_WSASendTo(uint32_t caller, uint32_t socket) { return NetworkDown(); }
uint32_t NetDll_WSAGetOverlappedResult(uint32_t caller, uint32_t socket) { g_wsaLastError = WSAENETDOWN; return 0; }
uint32_t NetDll___WSAFDIsSet(uint32_t socket, void* set) { return 0; }

uint32_t NetDll_inet_addr(const char* address)
{
    return 0xFFFFFFFF; // INADDR_NONE
}

// Events are never signalled since no socket operation ever starts.
uint32_t NetDll_WSACreateEvent() { return 0; } // WSA_INVALID_EVENT
uint32_t NetDll_WSACloseEvent(uint32_t event) { return 1; }
uint32_t NetDll_WSAResetEvent(uint32_t event) { return 1; }
uint32_t NetDll_WSAWaitForMultipleEvents(uint32_t count) { g_wsaLastError = WSAENETDOWN; return WSA_WAIT_FAILED; }

uint32_t NetDll_XNetGetTitleXnAddr(uint32_t caller, uint8_t* xnAddr)
{
    if (xnAddr != nullptr)
        memset(xnAddr, 0, 36); // XNADDR
    return XNET_GET_XNADDR_NONE;
}

uint32_t NetDll_XNetGetConnectStatus(uint32_t caller, uint32_t inAddr) { return XNET_CONNECT_STATUS_LOST; }
uint32_t NetDll_XNetConnect(uint32_t caller, uint32_t inAddr) { return WSAENETDOWN; }
uint32_t NetDll_XNetGetOpt(uint32_t caller) { return WSAENETDOWN; }
uint32_t NetDll_XNetSetSystemLinkPort(uint32_t caller, uint32_t port) { return 0; }
uint32_t NetDll_XNetDnsLookup(uint32_t caller) { return WSAENETDOWN; }
uint32_t NetDll_XNetDnsRelease(uint32_t caller) { return 0; }
uint32_t NetDll_XNetQosListen(uint32_t caller) { return WSAENETDOWN; }
uint32_t NetDll_XNetQosLookup(uint32_t caller) { return WSAENETDOWN; }
uint32_t NetDll_XNetQosRelease(uint32_t caller) { return 0; }
uint32_t NetDll_XNetCreateKey(uint32_t caller) { return WSAENETDOWN; }
uint32_t NetDll_XNetRegisterKey(uint32_t caller) { return WSAENETDOWN; }
uint32_t NetDll_XNetReplaceKey(uint32_t caller) { return WSAENETDOWN; }
uint32_t NetDll_XNetUnregisterKey(uint32_t caller) { return 0; }
uint32_t NetDll_XNetUnregisterInAddr(uint32_t caller, uint32_t inAddr) { return 0; }
uint32_t NetDll_XNetInAddrToXnAddr(uint32_t caller) { return WSAENETDOWN; }
uint32_t NetDll_XNetXnAddrToInAddr(uint32_t caller) { return WSAENETDOWN; }
uint32_t NetDll_XNetInAddrToString(uint32_t caller) { return WSAENETDOWN; }

uint32_t NetDll_XNetRandom(uint32_t caller, uint8_t* buffer, uint32_t length)
{
    static std::mt19937 rng{ std::random_device{}() };
    for (uint32_t i = 0; i < length; i++)
        buffer[i] = uint8_t(rng());
    return 0;
}

GUEST_FUNCTION_HOOK(__imp__NetDll_XNetStartup, NetDll_XNetStartup);
GUEST_FUNCTION_HOOK(__imp__NetDll_XNetCleanup, NetDll_XNetCleanup);
GUEST_FUNCTION_HOOK(__imp__NetDll_WSAStartup, NetDll_WSAStartup);
GUEST_FUNCTION_HOOK(__imp__NetDll_WSACleanup, NetDll_WSACleanup);
GUEST_FUNCTION_HOOK(__imp__NetDll_WSAGetLastError, NetDll_WSAGetLastError);
GUEST_FUNCTION_HOOK(__imp__NetDll_socket, NetDll_socket);
GUEST_FUNCTION_HOOK(__imp__NetDll_closesocket, NetDll_closesocket);
GUEST_FUNCTION_HOOK(__imp__NetDll_shutdown, NetDll_shutdown);
GUEST_FUNCTION_HOOK(__imp__NetDll_setsockopt, NetDll_setsockopt);
GUEST_FUNCTION_HOOK(__imp__NetDll_ioctlsocket, NetDll_ioctlsocket);
GUEST_FUNCTION_HOOK(__imp__NetDll_getsockname, NetDll_getsockname);
GUEST_FUNCTION_HOOK(__imp__NetDll_bind, NetDll_bind);
GUEST_FUNCTION_HOOK(__imp__NetDll_connect, NetDll_connect);
GUEST_FUNCTION_HOOK(__imp__NetDll_listen, NetDll_listen);
GUEST_FUNCTION_HOOK(__imp__NetDll_accept, NetDll_accept);
GUEST_FUNCTION_HOOK(__imp__NetDll_select, NetDll_select);
GUEST_FUNCTION_HOOK(__imp__NetDll_recv, NetDll_recv);
GUEST_FUNCTION_HOOK(__imp__NetDll_recvfrom, NetDll_recvfrom);
GUEST_FUNCTION_HOOK(__imp__NetDll_send, NetDll_send);
GUEST_FUNCTION_HOOK(__imp__NetDll_sendto, NetDll_sendto);
GUEST_FUNCTION_HOOK(__imp__NetDll_WSARecvFrom, NetDll_WSARecvFrom);
GUEST_FUNCTION_HOOK(__imp__NetDll_WSASendTo, NetDll_WSASendTo);
GUEST_FUNCTION_HOOK(__imp__NetDll_WSAGetOverlappedResult, NetDll_WSAGetOverlappedResult);
GUEST_FUNCTION_HOOK(__imp__NetDll___WSAFDIsSet, NetDll___WSAFDIsSet);
GUEST_FUNCTION_HOOK(__imp__NetDll_inet_addr, NetDll_inet_addr);
GUEST_FUNCTION_HOOK(__imp__NetDll_WSACreateEvent, NetDll_WSACreateEvent);
GUEST_FUNCTION_HOOK(__imp__NetDll_WSACloseEvent, NetDll_WSACloseEvent);
GUEST_FUNCTION_HOOK(__imp__NetDll_WSAResetEvent, NetDll_WSAResetEvent);
GUEST_FUNCTION_HOOK(__imp__NetDll_WSAWaitForMultipleEvents, NetDll_WSAWaitForMultipleEvents);
GUEST_FUNCTION_HOOK(__imp__NetDll_XNetGetTitleXnAddr, NetDll_XNetGetTitleXnAddr);
GUEST_FUNCTION_HOOK(__imp__NetDll_XNetGetConnectStatus, NetDll_XNetGetConnectStatus);
GUEST_FUNCTION_HOOK(__imp__NetDll_XNetConnect, NetDll_XNetConnect);
GUEST_FUNCTION_HOOK(__imp__NetDll_XNetGetOpt, NetDll_XNetGetOpt);
GUEST_FUNCTION_HOOK(__imp__NetDll_XNetSetSystemLinkPort, NetDll_XNetSetSystemLinkPort);
GUEST_FUNCTION_HOOK(__imp__NetDll_XNetDnsLookup, NetDll_XNetDnsLookup);
GUEST_FUNCTION_HOOK(__imp__NetDll_XNetDnsRelease, NetDll_XNetDnsRelease);
GUEST_FUNCTION_HOOK(__imp__NetDll_XNetQosListen, NetDll_XNetQosListen);
GUEST_FUNCTION_HOOK(__imp__NetDll_XNetQosLookup, NetDll_XNetQosLookup);
GUEST_FUNCTION_HOOK(__imp__NetDll_XNetQosRelease, NetDll_XNetQosRelease);
GUEST_FUNCTION_HOOK(__imp__NetDll_XNetCreateKey, NetDll_XNetCreateKey);
GUEST_FUNCTION_HOOK(__imp__NetDll_XNetRegisterKey, NetDll_XNetRegisterKey);
GUEST_FUNCTION_HOOK(__imp__NetDll_XNetReplaceKey, NetDll_XNetReplaceKey);
GUEST_FUNCTION_HOOK(__imp__NetDll_XNetUnregisterKey, NetDll_XNetUnregisterKey);
GUEST_FUNCTION_HOOK(__imp__NetDll_XNetUnregisterInAddr, NetDll_XNetUnregisterInAddr);
GUEST_FUNCTION_HOOK(__imp__NetDll_XNetInAddrToXnAddr, NetDll_XNetInAddrToXnAddr);
GUEST_FUNCTION_HOOK(__imp__NetDll_XNetXnAddrToInAddr, NetDll_XNetXnAddrToInAddr);
GUEST_FUNCTION_HOOK(__imp__NetDll_XNetInAddrToString, NetDll_XNetInAddrToString);
GUEST_FUNCTION_HOOK(__imp__NetDll_XNetRandom, NetDll_XNetRandom);
