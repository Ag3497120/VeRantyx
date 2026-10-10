// In-process loopback stand-in for Steam's networking interfaces
// (ISteamNetworkingSockets013 / ISteamNetworkingUtils004), for Source 2
// guests running without a Steam client.
//
// Source 2 connects the single-player client to its own listen server
// through ISteamNetworkingSockets socket pairs; on a Linux PC that interface
// comes from the Steam client. There is no Steam client here, so this serves
// the loopback subset in-process: socket pairs, reliable/unreliable messages
// (delivered in order, never dropped), poll groups, connection info and status
// callbacks. Anything that would touch the real network or Steam's servers
// (IP/P2P listen and connect, relay, authentication tickets, certificates)
// fails cleanly. Nothing here concerns ownership or licensing.
//
// The vtable layout is Valve's open-source GameNetworkingSockets headers
// (steam/, BSD-3): for slots 0-46 they match the SteamNetworkingSockets013 that
// Alyx's libsteam_api flat API dispatches through (verified slot by slot).
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include "steam/isteamnetworkingsockets.h"
#include "steam/isteamnetworkingutils.h"

// Declared (non-virtual, protected) by Valve's headers and never defined there.
ISteamNetworkingSockets::~ISteamNetworkingSockets() {}
ISteamNetworkingUtils::~ISteamNetworkingUtils() {}

namespace {

std::recursive_mutex g_lock;
bool g_trace = getenv("KL_TRACE_STEAMNET") != nullptr;
#define TR(...) do { if (g_trace) fprintf(stderr, "[steamnet] " __VA_ARGS__); } while (0)

SteamNetworkingMicroseconds now_us() {
    using namespace std::chrono;
    return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count() + 1000000;
}

struct Conn {
    HSteamNetConnection peer = k_HSteamNetConnection_Invalid;
    ESteamNetworkingConnectionState state = k_ESteamNetworkingConnectionState_Connected;
    int64 userData = -1;
    std::string name;
    HSteamNetPollGroup group = k_HSteamNetPollGroup_Invalid;
    std::deque<SteamNetworkingMessage_t *> inbox;
    int64 nextMsgNum = 1;
    SteamNetworkingIdentity peerIdentity;
    int endReason = 0;
};
std::map<HSteamNetConnection, Conn> g_conns;
std::map<HSteamNetPollGroup, std::deque<SteamNetworkingMessage_t *>> g_groups;
uint32 g_nextHandle = 0x1000;
FnSteamNetConnectionStatusChanged g_statusCb = nullptr;
struct PendingCb { SteamNetConnectionStatusChangedCallback_t cb; };
std::deque<PendingCb> g_pending;

// Messages are plain malloc'd structs (the type's destructor is protected;
// the API only ever frees them through m_pfnRelease).
void release_msg(SteamNetworkingMessage_t *m) {
    if (m->m_pfnFreeData) m->m_pfnFreeData(m);
    free(m);
}
void free_data(SteamNetworkingMessage_t *m) { free(m->m_pData); }

SteamNetworkingMessage_t *new_msg(int cb) {
    auto *m = (SteamNetworkingMessage_t *)calloc(1, sizeof(SteamNetworkingMessage_t));
    m->m_pData = cb > 0 ? malloc((size_t)cb) : nullptr;
    m->m_cbSize = cb;
    m->m_pfnFreeData = free_data;
    m->m_pfnRelease = release_msg;
    return m;
}

void fill_info(HSteamNetConnection h, const Conn &c, SteamNetConnectionInfo_t *info) {
    memset((void *)info, 0, sizeof *info);
    info->m_identityRemote = c.peerIdentity;
    info->m_nUserData = c.userData;
    info->m_eState = c.state;
    info->m_eEndReason = c.endReason;
    info->m_addrRemote.SetIPv6LocalHost();
    info->m_nFlags = k_nSteamNetworkConnectionInfoFlags_Fast | k_nSteamNetworkConnectionInfoFlags_LoopbackBuffers;
    snprintf(info->m_szConnectionDescription, sizeof info->m_szConnectionDescription,
             "#%u loopback%s%s", h, c.name.empty() ? "" : " ", c.name.c_str());
}

void post_status(HSteamNetConnection h, ESteamNetworkingConnectionState old) {
    auto it = g_conns.find(h);
    if (it == g_conns.end() || !g_statusCb) return;
    PendingCb p;
    memset((void *)&p, 0, sizeof p);
    p.cb.m_hConn = h;
    fill_info(h, it->second, &p.cb.m_info);
    p.cb.m_eOldState = old;
    g_pending.push_back(p);
}

void deliver(HSteamNetConnection from, const void *data, uint32 cb, int flags, int64 *outNum, int64 userData, uint16 lane) {
    auto s = g_conns.find(from);
    if (s == g_conns.end()) return;
    auto d = g_conns.find(s->second.peer);
    if (d == g_conns.end()) return;
    SteamNetworkingMessage_t *m = new_msg((int)cb);
    if (cb) memcpy(m->m_pData, data, cb);
    m->m_conn = d->first;
    m->m_identityPeer = d->second.peerIdentity;
    m->m_nConnUserData = d->second.userData;
    m->m_usecTimeReceived = now_us();
    m->m_nMessageNumber = s->second.nextMsgNum++;
    m->m_nFlags = flags;
    m->m_nUserData = userData;
    m->m_idxLane = lane;
    if (outNum) *outNum = m->m_nMessageNumber;
    if (d->second.group != k_HSteamNetPollGroup_Invalid) g_groups[d->second.group].push_back(m);
    else d->second.inbox.push_back(m);
}

int pop(std::deque<SteamNetworkingMessage_t *> &q, SteamNetworkingMessage_t **out, int max) {
    int n = 0;
    while (n < max && !q.empty()) { out[n++] = q.front(); q.pop_front(); }
    return n;
}

class Sockets final : public ISteamNetworkingSockets {
public:
    HSteamListenSocket CreateListenSocketIP(const SteamNetworkingIPAddr &, int, const SteamNetworkingConfigValue_t *) override { TR("CreateListenSocketIP (refused)\n"); return k_HSteamListenSocket_Invalid; }
    HSteamNetConnection ConnectByIPAddress(const SteamNetworkingIPAddr &, int, const SteamNetworkingConfigValue_t *) override { TR("ConnectByIPAddress (refused)\n"); return k_HSteamNetConnection_Invalid; }
    HSteamListenSocket CreateListenSocketP2P(int, int, const SteamNetworkingConfigValue_t *) override { TR("CreateListenSocketP2P (refused)\n"); return k_HSteamListenSocket_Invalid; }
    HSteamNetConnection ConnectP2P(const SteamNetworkingIdentity &, int, int, const SteamNetworkingConfigValue_t *) override { TR("ConnectP2P (refused)\n"); return k_HSteamNetConnection_Invalid; }
    EResult AcceptConnection(HSteamNetConnection) override { return k_EResultInvalidParam; }
    bool CloseConnection(HSteamNetConnection h, int reason, const char *dbg, bool) override {
        std::lock_guard<std::recursive_mutex> l(g_lock);
        TR("CloseConnection %u reason %d %s\n", h, reason, dbg ? dbg : "");
        auto it = g_conns.find(h);
        if (it == g_conns.end()) return false;
        HSteamNetConnection peer = it->second.peer;
        for (auto *m : it->second.inbox) m->Release();
        g_conns.erase(it);
        auto p = g_conns.find(peer);
        if (p != g_conns.end() && p->second.state == k_ESteamNetworkingConnectionState_Connected) {
            p->second.state = k_ESteamNetworkingConnectionState_ClosedByPeer;
            p->second.endReason = reason;
            p->second.peer = k_HSteamNetConnection_Invalid;
            post_status(peer, k_ESteamNetworkingConnectionState_Connected);
        }
        return true;
    }
    bool CloseListenSocket(HSteamListenSocket) override { return false; }
    bool SetConnectionUserData(HSteamNetConnection h, int64 v) override {
        std::lock_guard<std::recursive_mutex> l(g_lock);
        auto it = g_conns.find(h);
        if (it == g_conns.end()) return false;
        it->second.userData = v;
        return true;
    }
    int64 GetConnectionUserData(HSteamNetConnection h) override {
        std::lock_guard<std::recursive_mutex> l(g_lock);
        auto it = g_conns.find(h);
        return it == g_conns.end() ? -1 : it->second.userData;
    }
    void SetConnectionName(HSteamNetConnection h, const char *n) override {
        std::lock_guard<std::recursive_mutex> l(g_lock);
        auto it = g_conns.find(h);
        if (it != g_conns.end()) it->second.name = n ? n : "";
    }
    bool GetConnectionName(HSteamNetConnection h, char *n, int max) override {
        std::lock_guard<std::recursive_mutex> l(g_lock);
        auto it = g_conns.find(h);
        if (it == g_conns.end() || max <= 0) return false;
        snprintf(n, (size_t)max, "%s", it->second.name.c_str());
        return true;
    }
    EResult SendMessageToConnection(HSteamNetConnection h, const void *d, uint32 cb, int flags, int64 *out) override {
        std::lock_guard<std::recursive_mutex> l(g_lock);
        auto it = g_conns.find(h);
        if (it == g_conns.end()) return k_EResultInvalidParam;
        if (it->second.state != k_ESteamNetworkingConnectionState_Connected) return k_EResultNoConnection;
        deliver(h, d, cb, flags, out, 0, 0);
        return k_EResultOK;
    }
    void SendMessages(int n, SteamNetworkingMessage_t **msgs, int64 *out, bool deleteFailed) override {
        std::lock_guard<std::recursive_mutex> l(g_lock);
        for (int i = 0; i < n; i++) {
            SteamNetworkingMessage_t *m = msgs[i];
            auto it = g_conns.find(m->m_conn);
            int64 r = -k_EResultInvalidParam;
            if (it != g_conns.end() && it->second.state == k_ESteamNetworkingConnectionState_Connected) {
                int64 num = 0;
                deliver(m->m_conn, m->m_pData, (uint32)m->m_cbSize, m->m_nFlags, &num, m->m_nUserData, m->m_idxLane);
                r = num;
            } else if (it != g_conns.end()) r = -k_EResultNoConnection;
            if (out) out[i] = r;
            if (r >= 0 || deleteFailed) m->Release();
        }
    }
    EResult FlushMessagesOnConnection(HSteamNetConnection) override { return k_EResultOK; }
    int ReceiveMessagesOnConnection(HSteamNetConnection h, SteamNetworkingMessage_t **out, int max) override {
        std::lock_guard<std::recursive_mutex> l(g_lock);
        auto it = g_conns.find(h);
        return it == g_conns.end() ? -1 : pop(it->second.inbox, out, max);
    }
    bool GetConnectionInfo(HSteamNetConnection h, SteamNetConnectionInfo_t *info) override {
        std::lock_guard<std::recursive_mutex> l(g_lock);
        auto it = g_conns.find(h);
        if (it == g_conns.end()) return false;
        if (info) fill_info(h, it->second, info);
        return true;
    }
    EResult GetConnectionRealTimeStatus(HSteamNetConnection h, SteamNetConnectionRealTimeStatus_t *st, int nLanes, SteamNetConnectionRealTimeLaneStatus_t *lanes) override {
        std::lock_guard<std::recursive_mutex> l(g_lock);
        auto it = g_conns.find(h);
        if (it == g_conns.end()) return k_EResultNoConnection;
        if (st) {
            memset((void *)st, 0, sizeof *st);
            st->m_eState = it->second.state;
            st->m_flConnectionQualityLocal = st->m_flConnectionQualityRemote = 1.0f;
            st->m_flOutPacketsPerSec = st->m_flInPacketsPerSec = 0;
            st->m_nSendRateBytesPerSecond = 1 << 30;
        }
        for (int i = 0; lanes && i < nLanes; i++) memset((void *)&lanes[i], 0, sizeof lanes[i]);
        return k_EResultOK;
    }
    int GetDetailedConnectionStatus(HSteamNetConnection h, char *buf, int cb) override {
        if (buf && cb > 0) snprintf(buf, (size_t)cb, "loopback connection %u\n", h);
        return 0;
    }
    bool GetListenSocketAddress(HSteamListenSocket, SteamNetworkingIPAddr *) override { return false; }
    bool CreateSocketPair(HSteamNetConnection *a, HSteamNetConnection *b, bool useNet, const SteamNetworkingIdentity *id1, const SteamNetworkingIdentity *id2) override {
        std::lock_guard<std::recursive_mutex> l(g_lock);
        HSteamNetConnection h1 = g_nextHandle++, h2 = g_nextHandle++;
        Conn c1, c2;
        c1.peer = h2; c2.peer = h1;
        // Each end's remote identity is the OTHER end's local identity.
        if (id2) c1.peerIdentity = *id2; else c1.peerIdentity.SetLocalHost();
        if (id1) c2.peerIdentity = *id1; else c2.peerIdentity.SetLocalHost();
        g_conns[h1] = c1; g_conns[h2] = c2;
        *a = h1; *b = h2;
        TR("CreateSocketPair -> %u <-> %u (network loopback requested: %d)\n", h1, h2, useNet);
        return true;
    }
    EResult ConfigureConnectionLanes(HSteamNetConnection, int, const int *, const uint16 *) override { return k_EResultOK; }
    bool GetIdentity(SteamNetworkingIdentity *id) override { if (id) id->SetLocalHost(); return true; }
    ESteamNetworkingAvailability InitAuthentication() override { return k_ESteamNetworkingAvailability_CannotTry; }
    ESteamNetworkingAvailability GetAuthenticationStatus(SteamNetAuthenticationStatus_t *d) override {
        if (d) { memset((void *)d, 0, sizeof *d); d->m_eAvail = k_ESteamNetworkingAvailability_CannotTry; snprintf(d->m_debugMsg, sizeof d->m_debugMsg, "no Steam client (loopback only)"); }
        return k_ESteamNetworkingAvailability_CannotTry;
    }
    HSteamNetPollGroup CreatePollGroup() override {
        std::lock_guard<std::recursive_mutex> l(g_lock);
        HSteamNetPollGroup g = g_nextHandle++;
        g_groups[g];
        TR("CreatePollGroup -> %u\n", g);
        return g;
    }
    bool DestroyPollGroup(HSteamNetPollGroup g) override {
        std::lock_guard<std::recursive_mutex> l(g_lock);
        auto it = g_groups.find(g);
        if (it == g_groups.end()) return false;
        for (auto *m : it->second) m->Release();
        g_groups.erase(it);
        for (auto &c : g_conns) if (c.second.group == g) c.second.group = k_HSteamNetPollGroup_Invalid;
        return true;
    }
    bool SetConnectionPollGroup(HSteamNetConnection h, HSteamNetPollGroup g) override {
        std::lock_guard<std::recursive_mutex> l(g_lock);
        auto it = g_conns.find(h);
        if (it == g_conns.end()) return false;
        if (g != k_HSteamNetPollGroup_Invalid && g_groups.find(g) == g_groups.end()) return false;
        it->second.group = g;
        if (g != k_HSteamNetPollGroup_Invalid) {           // pending messages follow the connection
            for (auto *m : it->second.inbox) g_groups[g].push_back(m);
            it->second.inbox.clear();
        }
        return true;
    }
    int ReceiveMessagesOnPollGroup(HSteamNetPollGroup g, SteamNetworkingMessage_t **out, int max) override {
        std::lock_guard<std::recursive_mutex> l(g_lock);
        auto it = g_groups.find(g);
        return it == g_groups.end() ? -1 : pop(it->second, out, max);
    }
    bool ReceivedRelayAuthTicket(const void *, int, SteamDatagramRelayAuthTicket *) override { return false; }
    int FindRelayAuthTicketForServer(const SteamNetworkingIdentity &, int, SteamDatagramRelayAuthTicket *) override { return 0; }
    HSteamNetConnection ConnectToHostedDedicatedServer(const SteamNetworkingIdentity &, int, int, const SteamNetworkingConfigValue_t *) override { return k_HSteamNetConnection_Invalid; }
    uint16 GetHostedDedicatedServerPort() override { return 0; }
    SteamNetworkingPOPID GetHostedDedicatedServerPOPID() override { return 0; }
    EResult GetHostedDedicatedServerAddress(SteamDatagramHostedAddress *) override { return k_EResultFail; }
    HSteamListenSocket CreateHostedDedicatedServerListenSocket(int, int, const SteamNetworkingConfigValue_t *) override { return k_HSteamListenSocket_Invalid; }
    EResult GetGameCoordinatorServerLogin(SteamDatagramGameCoordinatorServerLogin *, int *, void *) override { return k_EResultFail; }
    HSteamNetConnection ConnectP2PCustomSignaling(ISteamNetworkingConnectionSignaling *, const SteamNetworkingIdentity *, int, int, const SteamNetworkingConfigValue_t *) override { return k_HSteamNetConnection_Invalid; }
    bool ReceivedP2PCustomSignal(const void *, int, ISteamNetworkingSignalingRecvContext *) override { return false; }
    bool GetCertificateRequest(int *, void *, SteamNetworkingErrMsg &) override { return false; }
    bool SetCertificate(const void *, int, SteamNetworkingErrMsg &) override { return false; }
    void ResetIdentity(const SteamNetworkingIdentity *) override {}
    void RunCallbacks() override {
        std::deque<PendingCb> q;
        FnSteamNetConnectionStatusChanged cb;
        { std::lock_guard<std::recursive_mutex> l(g_lock); q.swap(g_pending); cb = g_statusCb; }
        for (auto &p : q) if (cb) cb(&p.cb);
    }
    bool BeginAsyncRequestFakeIP(int) override { return false; }
    void GetFakeIP(int, SteamNetworkingFakeIPResult_t *) override {}
    HSteamListenSocket CreateListenSocketP2PFakeIP(int, int, const SteamNetworkingConfigValue_t *) override { return k_HSteamListenSocket_Invalid; }
    EResult GetRemoteFakeIPForConnection(HSteamNetConnection, SteamNetworkingIPAddr *) override { return k_EResultFail; }
    ISteamNetworkingFakeUDPPort *CreateFakeUDPPort(int) override { return nullptr; }
};

class Utils final : public ISteamNetworkingUtils {
public:
    SteamNetworkingMessage_t *AllocateMessage(int cb) override { return new_msg(cb); }
    ESteamNetworkingAvailability GetRelayNetworkStatus(SteamRelayNetworkStatus_t *d) override {
        if (d) memset((void *)d, 0, sizeof *d);
        return k_ESteamNetworkingAvailability_CannotTry;
    }
    float GetLocalPingLocation(SteamNetworkPingLocation_t &) override { return -1.0f; }
    int EstimatePingTimeBetweenTwoLocations(const SteamNetworkPingLocation_t &, const SteamNetworkPingLocation_t &) override { return k_nSteamNetworkingPing_Failed; }
    int EstimatePingTimeFromLocalHost(const SteamNetworkPingLocation_t &) override { return k_nSteamNetworkingPing_Failed; }
    void ConvertPingLocationToString(const SteamNetworkPingLocation_t &, char *s, int n) override { if (s && n) *s = 0; }
    bool ParsePingLocationString(const char *, SteamNetworkPingLocation_t &) override { return false; }
    bool CheckPingDataUpToDate(float) override { return true; }
    int GetPingToDataCenter(SteamNetworkingPOPID, SteamNetworkingPOPID *) override { return k_nSteamNetworkingPing_Failed; }
    int GetDirectPingToPOP(SteamNetworkingPOPID) override { return k_nSteamNetworkingPing_Failed; }
    int GetPOPCount() override { return 0; }
    int GetPOPList(SteamNetworkingPOPID *, int) override { return 0; }
    SteamNetworkingMicroseconds GetLocalTimestamp() override { return now_us(); }
    void SetDebugOutputFunction(ESteamNetworkingSocketsDebugOutputType, FSteamNetworkingSocketsDebugOutput) override {}
    ESteamNetworkingFakeIPType GetIPv4FakeIPType(uint32) override { return k_ESteamNetworkingFakeIPType_NotFake; }
    EResult GetRealIdentityForFakeIP(const SteamNetworkingIPAddr &, SteamNetworkingIdentity *) override { return k_EResultFail; }
    bool SetConfigValue(ESteamNetworkingConfigValue v, ESteamNetworkingConfigScope scope, intptr_t obj, ESteamNetworkingConfigDataType type, const void *arg) override {
        TR("SetConfigValue %d scope %d type %d\n", (int)v, (int)scope, (int)type);
        if (v == k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged && scope == k_ESteamNetworkingConfig_Global) {
            std::lock_guard<std::recursive_mutex> l(g_lock);
            g_statusCb = arg ? *(FnSteamNetConnectionStatusChanged const *)arg : nullptr;
        }
        (void)obj;
        return true;
    }
    ESteamNetworkingGetConfigValueResult GetConfigValue(ESteamNetworkingConfigValue v, ESteamNetworkingConfigScope, intptr_t, ESteamNetworkingConfigDataType *outType, void *result, size_t *cb) override {
        TR("GetConfigValue %d\n", (int)v);
        if (outType) *outType = k_ESteamNetworkingConfig_Int32;
        if (cb && *cb >= 4 && result) { memset(result, 0, 4); *cb = 4; return k_ESteamNetworkingGetConfigValue_OKInherited; }
        if (cb) *cb = 4;
        return k_ESteamNetworkingGetConfigValue_BufferTooSmall;
    }
    const char *GetConfigValueInfo(ESteamNetworkingConfigValue, ESteamNetworkingConfigDataType *, ESteamNetworkingConfigScope *) override { return nullptr; }
    ESteamNetworkingConfigValue IterateGenericEditableConfigValues(ESteamNetworkingConfigValue, bool) override { return k_ESteamNetworkingConfig_Invalid; }
    void SteamNetworkingIPAddr_ToString(const SteamNetworkingIPAddr &a, char *buf, size_t cb, bool withPort) override {
        if (!buf || !cb) return;
        if (a.IsLocalHost()) snprintf(buf, cb, withPort ? "[::1]:%u" : "[::1]", a.m_port);
        else snprintf(buf, cb, withPort ? "%u.%u.%u.%u:%u" : "%u.%u.%u.%u", a.m_ipv4.m_ip[0], a.m_ipv4.m_ip[1], a.m_ipv4.m_ip[2], a.m_ipv4.m_ip[3], a.m_port);
    }
    bool SteamNetworkingIPAddr_ParseString(SteamNetworkingIPAddr *, const char *) override { return false; }
    ESteamNetworkingFakeIPType SteamNetworkingIPAddr_GetFakeIPType(const SteamNetworkingIPAddr &) override { return k_ESteamNetworkingFakeIPType_NotFake; }
    void SteamNetworkingIdentity_ToString(const SteamNetworkingIdentity &id, char *buf, size_t cb) override {
        if (!buf || !cb) return;
        if (id.IsLocalHost()) snprintf(buf, cb, "ip:localhost");
        else snprintf(buf, cb, "identity:%d", (int)id.m_eType);
    }
    bool SteamNetworkingIdentity_ParseString(SteamNetworkingIdentity *id, const char *s) override {
        if (id && s && !strcmp(s, "ip:localhost")) { id->SetLocalHost(); return true; }
        return false;
    }
};

Sockets g_sockets;
Utils g_utils;

}  // namespace

extern "C" void *kl_steamnet_interface(const char *version) {
    if (!version) return nullptr;
    if (!strncmp(version, "SteamNetworkingSockets0", 23)) { TR("serving %s\n", version); return &g_sockets; }
    if (!strncmp(version, "SteamNetworkingUtils0", 21)) { TR("serving %s\n", version); return &g_utils; }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Inert placeholders for ISteamClient / ISteamUser / ISteamUtils.
//
// Source 2's InitSteamNetworking refuses to use ISteamNetworkingSockets unless
// these three are non-NULL. Each placeholder answers every method with 0 /
// false / null (BLoggedOn is false, the Steam ID invalid) and logs the first
// call to each slot. ISteamApps and every ownership or licence query stay NULL,
// exactly as with Steam closed.
extern "C" char kl_steam_null_slots[];
namespace {
struct NullIface { void **vptr; const char *name; unsigned char seen[256]; };
void *g_null_vtbl[256];
NullIface g_null_client, g_null_user, g_null_utils;
void null_init() {
    static bool done;
    if (done) return;
    for (int i = 0; i < 256; i++) g_null_vtbl[i] = kl_steam_null_slots + i * 8;
    g_null_client = { g_null_vtbl, "SteamClient", {} };
    g_null_user = { g_null_vtbl, "SteamUser", {} };
    g_null_utils = { g_null_vtbl, "SteamUtils", {} };
    done = true;
}
}  // namespace

extern "C" void kl_steam_null_called(void *self, unsigned slot) {
    NullIface *o = (NullIface *)self;
    if (slot < 256 && !o->seen[slot]) {
        o->seen[slot] = 1;
        fprintf(stderr, "[steam] placeholder %s: vtable slot %u called -> 0\n", o->name, slot);
    }
}

extern "C" void *kl_steam_placeholder(const char *version) {
    if (!version) return nullptr;
    null_init();
    if (!strncmp(version, "SteamClient0", 12)) return &g_null_client;
    if (!strncmp(version, "SteamUser0", 10)) return &g_null_user;
    if (!strncmp(version, "SteamUtils0", 11)) return &g_null_utils;
    return nullptr;
}
