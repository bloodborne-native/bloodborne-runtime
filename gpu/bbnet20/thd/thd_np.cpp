// SPDX-License-Identifier: GPL-3.0-or-later
// The Hunter's Dream backend: sceNpSignaling over the peer table, and the NP manager, auth,
// lookup, score and WebAPI imports answered here (The Hunter's Dream has no PSN behind it).
// Adapted from droogie/bbhost src/hle/np_signaling.cpp and src/hle/system.cpp (566184a; the
// signaling handshake from v0.2.31, PR #217), GPL-3.0-or-later.
#include "thd_internal.h"
#include "thd_score.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <random>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "core/libraries/libs.h"

namespace BbThd {
namespace {

// ---- sceNpSignaling ---------------------------------------------------------------------------------
// The connection handshake of shadPS4 (src/core/libraries/np/signaling_handler.cpp) between two
// Hunter's Dream clients, as bbhost 0.2.31 does it (src/hle/np_signaling.cpp, PR #217): the server
// pairs co-op, invasions and PvP only with clients that speak it.
//
// The game's SocketState FSM is the only caller: state 4 activates a connection to a peer's NpId and
// expects ESTABLISHED, state 5 reads the status and expects 2 with the peer's address and port. The
// events are the ones the game's handler acts on (the co-op contract): 0x01 established, 0x0c mutual
// activated, 0x00 dead; 0x0b peer deactivated; 0x0a peer activated.
//
// Two layers, as shadPS4:
//   transport   one per other player: the link to that player and its handshake state (Inactive,
//               SendingOffer, WaitOffer, SendingAccept, WaitAccept, ConnCheck, Established)
//   connection  what the game holds (conn id, per context and player); its status is its
//               transport's: Established 2, Inactive 0, else 1
//
// The messages are kSignalMagic datagrams on the P2P port (thd_net.cpp):
//   control    ActivationRequest, ActivationAck, Established, Close
//   handshake  Offer, Accept, Check, CheckAck
//
// activate          ActivationRequest, and Offer (resent every 500 ms; 30 s without getting to
//                   Established: DEAD, timeout)
// ActivationRequest the peer's game activated: its transport made (or kept), a connection for it if
//                   the game has none (PEER_ACTIVATED once established), answered ActivationAck.
//                   Establishes nothing unless the transport already is.
// Offer             answered Accept, only if we have a transport for the sender; Accept -> Check;
//                   Check -> CheckAck
// CheckAck          the transport is Established: control Established to the peer, then
//                   ESTABLISHED (connections the game activated), MUTUAL_ACTIVATED (where the peer
//                   activated too), PEER_ACTIVATED (connections only the peer activated)
// deactivate        Close(deactivate) to the peer when established, then DEAD (terminated by
//                   myself); the connection is gone after
// Close             PEER_DEACTIVATED when it is a deactivate and the transport is established,
//                   else DEAD (terminated by peer)

constexpr int kSigErrInvalidArg = static_cast<int>(0x80552715);
constexpr int kSigErrConnNotFound = static_cast<int>(0x8055270E);
constexpr int kSigErrTerminatedByPeer = static_cast<int>(0x80552710);
constexpr int kSigErrTimeout = static_cast<int>(0x80552711);
constexpr int kSigErrTerminatedByMyself = static_cast<int>(0x80552718);
constexpr std::uint16_t kEvDead = 0x00, kEvEstablished = 0x01, kEvPeerActivated = 0x0a,
                        kEvPeerDeactivated = 0x0b, kEvMutualActivated = 0x0c;

// The datagram: magic, kind, reason, the sender's and the receiver's online ids, a nonce
// (Check / CheckAck).
enum Kind : std::uint8_t {
    kActivationRequest = 0x01,
    kActivationAck = 0x02,
    kClose = 0x03,
    kEstablishedMsg = 0x04,
    kOffer = 0x11,
    kAccept = 0x12,
    kCheck = 0x13,
    kCheckAck = 0x14,
};
enum Reason : std::uint8_t { kDeactivate = 1, kTerminate = 2 };
constexpr std::size_t kIdLen = 16;
constexpr std::size_t kFromAt = sizeof(kSignalMagic) + 2;
constexpr std::size_t kToAt = kFromAt + kIdLen;
constexpr std::size_t kNonceAt = kToAt + kIdLen;
constexpr std::size_t kPacketLen = kNonceAt + 8;
constexpr int kStepMs = 500;
constexpr int kGiveUpMs = 30000;
// Matching2's ESTABLISHED waits this long after the transport is up (shadPS4's
// kMatching2EstablishDelay), and its DEAD on a timeout.
constexpr int kM2EstablishDelayMs = 1000;
constexpr int kM2ErrTimeout = static_cast<int>(0x80550E11);
constexpr int kM2ErrTerminatedByPeer = static_cast<int>(0x80550E10);
constexpr int kM2ErrTerminatedByMyself = static_cast<int>(0x80550E18);

// Matching2's DEAD for that player when its connection closes, after NpSignaling's own DEAD on the
// same queue (shadPS4's CloseConnectionAndDispatchDead -> QueueMatching2DeadForTransport).
void queue_m2_dead(const std::string& who, int error) {
    DispatchAfter(0, Prio::Signaling, [who, error] { M2TransportDead(who, error); });
}

enum class State { Inactive, SendingOffer, WaitOffer, SendingAccept, WaitAccept, ConnCheck, Established };

struct SigCtx {
    void* cb = nullptr;
    void* arg = nullptr;
};
struct Transport {
    std::string online_id;
    std::uint16_t member_id = 0;  // the peer table's entry, 0 until known
    std::uint32_t addr = 0;       // network order
    std::uint16_t port = 0;       // host order
    State state = State::SendingOffer;
    bool peer_activated = false;
    bool peer_established = false;
    bool established_fired = false;
    bool stepping = false;  // the resend timer is running
    int waited_ms = 0;      // handshake time so far
};
struct Conn {
    unsigned ctx = 0;
    std::string online_id;
    unsigned transport = 0;
    std::uint16_t member_id = 0;
    bool locally_activated = false;  // the game activated it
    bool peer_activated = false;     // the peer's game activated its own (its ActivationRequest)
    bool established_fired = false;
    bool mutual_fired = false;
    bool peer_activated_fired = false;
    bool dead = false;
    bool start_handshake = false;  // waiting for the peer's address, then handshake
    int resolve_waited_ms = 0;
};
using Event = std::tuple<unsigned, unsigned, std::uint16_t, int>;  // ctx, conn, event, error

std::mutex g_sig_mu;
std::map<unsigned, SigCtx> g_sig_ctx;
std::map<unsigned, Transport> g_tr;  // by transport id
std::map<unsigned, Conn> g_conn;     // by connection id
unsigned g_next_sig_ctx = 1;
unsigned g_next_tr = 1;
unsigned g_conn_seed = 0;  // 0 until the first connection: then a random start

// The next free connection id, as shadPS4's AllocateConnectionIdLocked: a random start in 1..0x7fff,
// then counting up and wrapping, so two games do not hand out the same ids.
unsigned alloc_conn_id_locked() {
    if (g_conn_seed == 0) {
        std::random_device rd;
        g_conn_seed = (static_cast<unsigned>(rd()) / 0x4001u) & 0x7fffu;
        if (g_conn_seed == 0) g_conn_seed = 1;
    }
    for (int tried = 0; tried <= 0xffff; ++tried) {
        const unsigned candidate = g_conn_seed++;
        if (g_conn_seed == 0 || g_conn_seed > 0x7fff) g_conn_seed = 1;
        if (candidate != 0 && g_conn.find(candidate) == g_conn.end()) return candidate;
    }
    return 0;
}

std::uint64_t sig_now_us() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

const char* kind_name(std::uint8_t k) {
    switch (k) {
    case kActivationRequest: return "ActivationRequest";
    case kActivationAck: return "ActivationAck";
    case kClose: return "Close";
    case kEstablishedMsg: return "Established";
    case kOffer: return "Offer";
    case kAccept: return "Accept";
    case kCheck: return "Check";
    case kCheckAck: return "CheckAck";
    default: return "?";
    }
}

int status_of(const Transport& t) {
    return t.state == State::Established ? 2 : t.state == State::Inactive ? 0 : 1;
}

// (ctxId, connId, event, errorCode, arg), on the dispatcher. A connection that died before its
// event went out gets only DEAD.
void sig_fire(unsigned ctx, unsigned conn, std::uint16_t event, int error) {
    SigCtx c;
    {
        std::lock_guard lk(g_sig_mu);
        const auto it = g_sig_ctx.find(ctx);
        if (it == g_sig_ctx.end() || !it->second.cb) return;
        if (event != kEvDead) {
            const auto ci = g_conn.find(conn);
            if (ci == g_conn.end() || ci->second.dead) {
                Log("np: conn %u is dead: event 0x%x not delivered", conn, event);
                return;
            }
        }
        c = it->second;
    }
    if (Trace()) Log("np: NpSignaling event 0x%x conn %u error 0x%x", event, conn, static_cast<unsigned>(error));
    CallGuest(c.cb, static_cast<std::int64_t>(ctx), static_cast<std::int64_t>(conn), static_cast<std::int64_t>(event),
              static_cast<std::int64_t>(error), c.arg);
}

void fire_all(const std::vector<Event>& events) {
    for (const auto& [ctx, conn, event, error] : events) sig_fire(ctx, conn, event, error);
}

void queue_all(std::vector<Event> events) {
    if (events.empty()) return;
    DispatchAfter(0, Prio::Signaling, [events] { fire_all(events); });
}

// A live (not Inactive) transport to that player, or 0.
unsigned find_transport_locked(const std::string& online_id) {
    for (const auto& [id, t] : g_tr) {
        if (t.state != State::Inactive && t.online_id == online_id) return id;
    }
    return 0;
}

// The player's live transport, or a new one sending Offer. Inactive ones with no connection left are
// dropped.
unsigned ensure_transport_locked(const std::string& online_id, std::uint32_t addr, std::uint16_t port) {
    if (unsigned id = find_transport_locked(online_id)) {
        Transport& t = g_tr[id];
        if (addr && port) {
            t.addr = addr;
            t.port = port;
        }
        return id;
    }
    for (auto it = g_tr.begin(); it != g_tr.end();) {
        bool used = false;
        for (const auto& [cid, c] : g_conn) used |= c.transport == it->first && !c.dead;
        it = it->second.state == State::Inactive && !used ? g_tr.erase(it) : std::next(it);
    }
    const unsigned id = g_next_tr++;
    Transport t;
    t.online_id = online_id;
    t.addr = addr;
    t.port = port;
    Peer p;
    if (PeersFindOnline(online_id, &p)) {
        t.member_id = p.member_id;
        if (!t.addr && p.addr && p.port) {
            t.addr = p.addr;
            t.port = p.port;
        }
    }
    g_tr[id] = t;
    return id;
}

// The game's live connection in this context to that player, or 0.
unsigned find_conn_locked(const std::string& online_id, unsigned ctx = 0) {
    for (const auto& [id, c] : g_conn) {
        if (!c.dead && c.online_id == online_id && (!ctx || c.ctx == ctx)) return id;
    }
    return 0;
}

// The peer's current address from the room's peer table, if it is there.
void refresh_addr_locked(Transport& t) {
    Peer p;
    if (t.member_id && PeersGet(t.member_id, &p) && p.addr && p.port) {
        t.addr = p.addr;
        t.port = p.port;
    }
}

void send_locked(Transport& t, Kind kind, Reason reason = kDeactivate, std::uint64_t nonce = 0) {
    refresh_addr_locked(t);
    if (!t.addr || !t.port) return;
    std::uint8_t pkt[kPacketLen] = {};
    std::memcpy(pkt, kSignalMagic, sizeof(kSignalMagic));
    pkt[4] = kind;
    pkt[5] = reason;
    const std::string me = OnlineId();
    std::memcpy(pkt + kFromAt, me.data(), std::min(me.size(), kIdLen));
    std::memcpy(pkt + kToAt, t.online_id.data(), std::min(t.online_id.size(), kIdLen));
    std::memcpy(pkt + kNonceAt, &nonce, sizeof(nonce));
    P2pSignal(t.addr, t.port, pkt, sizeof(pkt));
}

// The peer table's view of the game's connections to that player.
void set_peer_status_locked(unsigned tid, int status) {
    for (const auto& [id, c] : g_conn) {
        if (c.transport == tid && !c.dead && c.locally_activated && c.member_id) PeersSetConn(c.member_id, id, status);
    }
}

// The handshake step this transport is at, sent (shadPS4's StartTransportHandshakeInitiator).
void send_handshake_locked(Transport& t) {
    Kind kind = kOffer;
    std::uint64_t nonce = 0;
    switch (t.state) {
    case State::SendingAccept: kind = kAccept; break;
    case State::ConnCheck:
        kind = kCheck;
        nonce = sig_now_us();
        break;
    case State::Established: return;
    case State::WaitOffer:
    case State::Inactive: t.state = State::SendingOffer; [[fallthrough]];
    default: kind = kOffer; break;
    }
    send_locked(t, kind, kDeactivate, nonce);
}

void close_transport_dead_locked(unsigned tid, int error, std::vector<Event>& events);
void step(unsigned tid);

// The resend timer for a transport still in its handshake.
void start_stepping_locked(unsigned tid) {
    Transport& t = g_tr[tid];
    if (t.stepping) return;
    t.stepping = true;
    DispatchAfter(kStepMs, Prio::Signaling, [tid] { step(tid); });
}

// Every 500 ms until Established: the current step again; DEAD after 30 s.
void step(unsigned tid) {
    std::vector<Event> events;
    {
        std::lock_guard lk(g_sig_mu);
        const auto it = g_tr.find(tid);
        if (it == g_tr.end()) return;
        Transport& t = it->second;
        if (t.state == State::Established || t.state == State::Inactive) {
            t.stepping = false;
            return;
        }
        if (t.addr && t.port) {
            if (t.waited_ms >= kGiveUpMs) {
                Log("np: transport %u to %s: handshake not done in %d s: DEAD (timeout)", tid, t.online_id.c_str(),
                    kGiveUpMs / 1000);
                const std::string who = t.online_id;
                close_transport_dead_locked(tid, kSigErrTimeout, events);  // erases the transport
                DispatchAfter(0, Prio::Signaling, [who] { M2TransportDead(who, kM2ErrTimeout); });
            } else {
                send_handshake_locked(t);
                t.waited_ms += kStepMs;
                DispatchAfter(kStepMs, Prio::Signaling, [tid] { step(tid); });
            }
        } else {
            DispatchAfter(kStepMs, Prio::Signaling, [tid] { step(tid); });
        }
    }
    fire_all(events);
}

// The transport is up: control Established to the peer, and each connection on it its events, once
// each (shadPS4's EstablishConnection).
void establish_locked(unsigned tid, bool peer_activated_hint, std::vector<Event>& events) {
    Transport& t = g_tr[tid];
    if (peer_activated_hint) t.peer_activated = true;
    if (!t.established_fired) {
        t.state = State::Established;
        t.established_fired = true;
        send_locked(t, kEstablishedMsg);
        set_peer_status_locked(tid, 2);
        const std::string who = t.online_id;
        DispatchAfter(kM2EstablishDelayMs, Prio::Signaling, [who] { M2TransportUp(who); });
    }
    for (auto& [id, c] : g_conn) {
        if (c.transport != tid || c.dead) continue;
        if (c.peer_activated && !c.locally_activated && !c.peer_activated_fired) {
            c.peer_activated_fired = true;
            events.emplace_back(c.ctx, id, kEvPeerActivated, 0);
        }
        if (c.locally_activated && !c.established_fired) {
            c.established_fired = true;
            events.emplace_back(c.ctx, id, kEvEstablished, 0);
        }
        if (c.locally_activated && c.peer_activated && !c.mutual_fired) {
            c.mutual_fired = true;
            events.emplace_back(c.ctx, id, kEvMutualActivated, 0);
        }
    }
}

// One connection closed: its transport Inactive, DEAD, and the connection gone.
void close_conn_dead_locked(unsigned conn_id, int error, std::vector<Event>& events) {
    const auto it = g_conn.find(conn_id);
    if (it == g_conn.end() || it->second.dead) return;
    Conn& c = it->second;
    const auto tr = g_tr.find(c.transport);
    if (tr != g_tr.end()) tr->second.state = State::Inactive;
    c.dead = true;
    if (c.member_id) PeersSetConn(c.member_id, conn_id, 0);
    events.emplace_back(c.ctx, conn_id, kEvDead, error);
    g_conn.erase(it);
}

// The whole transport closed: every connection on it DEAD and gone.
void close_transport_dead_locked(unsigned tid, int error, std::vector<Event>& events) {
    const auto tr = g_tr.find(tid);
    if (tr == g_tr.end() || tr->second.state == State::Inactive) return;
    tr->second.state = State::Inactive;
    std::vector<unsigned> conns;
    for (const auto& [id, c] : g_conn) {
        if (c.transport == tid && !c.dead) conns.push_back(id);
    }
    for (unsigned id : conns) {
        Conn& c = g_conn[id];
        c.dead = true;
        if (c.member_id) PeersSetConn(c.member_id, id, 0);
        events.emplace_back(c.ctx, id, kEvDead, error);
        g_conn.erase(id);
    }
    g_tr.erase(tr);
}

// ActivationRequest out, then the handshake, once the peer's address is known (shadPS4's
// ProcessPendingActivations): the room's peer table, else the server. Retried every 500 ms; DEAD
// (timeout) after 30 s without one.
void activation(unsigned conn_id) {
    std::string peer;
    {
        std::lock_guard lk(g_sig_mu);
        const auto it = g_conn.find(conn_id);
        if (it == g_conn.end() || it->second.dead) return;
        Transport& t = g_tr[it->second.transport];
        refresh_addr_locked(t);
        if (!t.addr || !t.port) {
            Peer p;
            if (PeersFindOnline(t.online_id, &p) && p.addr && p.port) {
                t.member_id = p.member_id;
                it->second.member_id = p.member_id;
                t.addr = p.addr;
                t.port = p.port;
            }
        }
        if (!t.addr || !t.port) peer = t.online_id;
    }
    if (!peer.empty()) {
        // Not a room member yet (the host activates its connection to the guest it summons before
        // the guest joins): the server has its address from its context_start.
        std::uint32_t addr = 0;
        std::uint16_t port = 0;
        std::string error;
        const bool ok = ServerSignalingResolve(peer, &addr, &port, error);
        std::vector<Event> events;
        {
            std::lock_guard lk(g_sig_mu);
            const auto it = g_conn.find(conn_id);
            if (it == g_conn.end() || it->second.dead) return;
            Conn& c = it->second;
            Transport& t = g_tr[c.transport];
            if (ok && !t.addr) {
                const Peer p = PeersProvisional(peer, addr, port);
                t.member_id = p.member_id;
                c.member_id = p.member_id;
                t.addr = addr;
                t.port = port;
                Log("np: conn %u to %s resolved %u.%u.%u.%u:%u", conn_id, peer.c_str(), addr & 0xff, (addr >> 8) & 0xff,
                    (addr >> 16) & 0xff, addr >> 24, port);
            } else if (!t.addr) {
                c.resolve_waited_ms += kStepMs;
                if (c.resolve_waited_ms >= kGiveUpMs) {
                    Log("np: conn %u to %s: no address in %d s (%s): DEAD (timeout)", conn_id, peer.c_str(),
                        kGiveUpMs / 1000, error.c_str());
                    close_conn_dead_locked(conn_id, kSigErrTimeout, events);
                } else {
                    DispatchAfter(kStepMs, Prio::Signaling, [conn_id] { activation(conn_id); });
                }
            }
        }
        fire_all(events);
        if (!ok) return;
    }
    std::vector<Event> events;
    {
        std::lock_guard lk(g_sig_mu);
        const auto it = g_conn.find(conn_id);
        if (it == g_conn.end() || it->second.dead) return;
        Conn& c = it->second;
        const unsigned tid = c.transport;
        Transport& t = g_tr[tid];
        if (!t.addr || !t.port) return;
        if (c.member_id) PeersSetConn(c.member_id, conn_id, status_of(t));
        send_locked(t, kActivationRequest);
        if (c.start_handshake) {
            c.start_handshake = false;
            send_handshake_locked(t);
            start_stepping_locked(tid);
        } else if (t.state == State::Established) {
            establish_locked(tid, false, events);
        }
    }
    fire_all(events);
}

s32 PS4_SYSV_ABI sig_init(u64, int, int, u64) { return 0; }
s32 PS4_SYSV_ABI sig_term() {
    std::lock_guard lk(g_sig_mu);
    g_sig_ctx.clear();
    g_conn.clear();
    g_tr.clear();
    return 0;
}
// (const SceNpId*, callback, arg, ctxId*)
s32 PS4_SYSV_ABI sig_create_ctx(const void*, void* cb, void* arg, unsigned* ctx) {
    if (!ctx) return kSigErrInvalidArg;
    std::lock_guard lk(g_sig_mu);
    const unsigned id = g_next_sig_ctx++;
    g_sig_ctx[id] = SigCtx{cb, arg};
    *ctx = id;
    return 0;
}
s32 PS4_SYSV_ABI sig_delete_ctx(unsigned ctx) {
    std::lock_guard lk(g_sig_mu);
    g_sig_ctx.erase(ctx);
    for (auto it = g_conn.begin(); it != g_conn.end();) it = it->second.ctx == ctx ? g_conn.erase(it) : std::next(it);
    return 0;
}

// (ctxId, const SceNpId* peer, connId*), as shadPS4's ActivateSig1: one live connection per context
// and player; a new or re-activated one sends ActivationRequest and starts the handshake, unless its
// transport is already up (then ESTABLISHED at once).
s32 PS4_SYSV_ABI sig_activate(unsigned ctx, const void* npid, unsigned* conn_out) {
    if (!conn_out) return kSigErrInvalidArg;
    const std::string peer = NpIdText(npid);
    std::vector<Event> events;
    unsigned id = 0;
    bool queue = false;
    {
        std::lock_guard lk(g_sig_mu);
        id = find_conn_locked(peer, ctx);
        const unsigned tid = ensure_transport_locked(peer, 0, 0);
        Transport& t = g_tr[tid];
        if (!id) {
            id = alloc_conn_id_locked();
            Conn c;
            c.ctx = ctx;
            c.online_id = peer;
            c.transport = tid;
            c.member_id = t.member_id;
            c.locally_activated = true;
            c.start_handshake = t.state != State::Established;
            g_conn[id] = c;
            queue = true;
        } else {
            Conn& c = g_conn[id];
            const bool was_local = c.locally_activated;
            c.transport = tid;
            c.locally_activated = true;
            if (!c.member_id) c.member_id = t.member_id;
            if (t.state != State::Established) {
                if (t.state == State::Inactive || t.state == State::WaitOffer) t.state = State::SendingOffer;
                c.start_handshake = true;
            }
            queue = !was_local;
        }
        Log("sceNpSignalingActivateConnection %s -> conn %u%s", peer.c_str(), id,
            t.state == State::Established ? " (transport already up)" : "");
        if (t.state == State::Established) establish_locked(tid, false, events);
    }
    *conn_out = id;
    if (queue) DispatchAfter(0, Prio::Signaling, [id] { activation(id); });
    queue_all(std::move(events));
    return 0;
}

// As shadPS4's DeactivateSig1: Close(deactivate) to the peer when the transport is up, then DEAD, and
// the connection is gone (a second deactivate is CONN_NOT_FOUND). Matching2's DEAD for that player
// is kept for its LeaveRoom.
s32 PS4_SYSV_ABI sig_deactivate(unsigned ctx, unsigned conn) {
    std::string peer;
    std::vector<Event> events;
    {
        std::lock_guard lk(g_sig_mu);
        const auto it = g_conn.find(conn);
        if (it == g_conn.end() || it->second.ctx != ctx) {
            Log("sceNpSignalingDeactivateConnection conn %u: not found", conn);
            return kSigErrConnNotFound;
        }
        Conn& c = it->second;
        Log("sceNpSignalingDeactivateConnection conn %u (%s)", conn, c.online_id.c_str());
        peer = c.online_id;
        const auto tr = g_tr.find(c.transport);
        if (tr != g_tr.end() && tr->second.state == State::Established && !c.dead) send_locked(tr->second, kClose, kDeactivate);
        close_conn_dead_locked(conn, kSigErrTerminatedByMyself, events);
    }
    M2PeerClosed(peer);
    queue_all(std::move(events));
    queue_m2_dead(peer, kM2ErrTerminatedByMyself);
    return 0;
}

// (ctxId, connId, status*, SceNetInAddr*, SceInPort_t*): 0 inactive, 1 pending, 2 active - the
// connection's transport's.
s32 PS4_SYSV_ABI sig_status(unsigned ctx, unsigned conn, int* st, std::uint32_t* addr, std::uint16_t* port) {
    int status = 0;
    std::uint32_t a = 0;
    std::uint16_t p = 0;
    {
        std::lock_guard lk(g_sig_mu);
        const auto it = g_conn.find(conn);
        if (it != g_conn.end() && it->second.ctx == ctx && !it->second.dead) {
            const auto tr = g_tr.find(it->second.transport);
            if (tr != g_tr.end()) {
                Transport& t = tr->second;
                status = status_of(t);
                if (status == 2) {
                    refresh_addr_locked(t);
                    a = t.addr;
                    p = static_cast<std::uint16_t>((t.port << 8) | (t.port >> 8));
                }
            }
        }
    }
    if (st) *st = status;
    if (addr) *addr = a;
    if (port) *port = p;
    if (Trace()) Log("sceNpSignalingGetConnectionStatus conn %u -> %d", conn, status);
    return 0;
}

// ---- NP manager: signed in as the account, no PSN behind it -----------------------------------------

constexpr int kNpErrInvalidArg = static_cast<int>(0x80550003);
std::atomic<int> g_np_req{1};

s32 PS4_SYSV_ABI np_ok() { return 0; }
s32 PS4_SYSV_ABI np_register_cb(void*, void*) { return 0; }
s32 PS4_SYSV_ABI np_get_state(int, int* state) {
    if (!state) return kNpErrInvalidArg;
    // Signed out without an account: the game plays offline rather than under a made-up name.
    *state = SignedIn() && !Refused() ? 2 : 1;
    return 0;
}
s32 PS4_SYSV_ABI np_online_id(int, void* id) {
    if (!id) return kNpErrInvalidArg;
    const std::string cur = OnlineId();
    std::memset(id, 0, 20);
    std::memcpy(id, cur.c_str(), std::min<std::size_t>(cur.size(), 16));
    return 0;
}
// The same handle as the online id: the session service matches peers by it.
s32 PS4_SYSV_ABI np_get_npid(int, void* id) {
    if (!id) return kNpErrInvalidArg;
    FillNpId(id, OnlineId().c_str());
    return 0;
}
s32 PS4_SYSV_ABI np_presence_status(int, int* st) {
    if (st) *st = 1;
    return 0;
}
// As shadPS4's (np_common.cpp): the name up to its terminator, then opt and reserved; anything after
// the name's terminator is not compared. A plain memcmp of the whole 36 bytes called the same player
// two different ones whenever two copies differed past the name, and the game then kept a second
// record and connection for that player (bbhost 0.2.31).
constexpr int kNpUtilErrorNotMatch = static_cast<int>(0x80550609);
s32 PS4_SYSV_ABI np_cmp_online(const void* a, const void* b) {
    if (!a || !b) return kNpErrInvalidArg;
    return std::strncmp(static_cast<const char*>(a), static_cast<const char*>(b), 16) != 0 ? kNpUtilErrorNotMatch : 0;
}
s32 PS4_SYSV_ABI np_cmp_npid(const void* a, const void* b) {
    if (!a || !b) return kNpErrInvalidArg;
    const auto* x = static_cast<const std::uint8_t*>(a);
    const auto* y = static_cast<const std::uint8_t*>(b);
    if (std::strncmp(reinterpret_cast<const char*>(x), reinterpret_cast<const char*>(y), 16) != 0) return kNpUtilErrorNotMatch;
    if (std::memcmp(x + 20, y + 20, 8) != 0) return kNpUtilErrorNotMatch; // opt
    if (std::memcmp(x + 28, y + 28, 8) != 0) return kNpUtilErrorNotMatch; // reserved
    return 0;
}
s32 PS4_SYSV_ABI np_create_request(const void*) { return g_np_req.fetch_add(1); }
s32 PS4_SYSV_ABI np_request_done(int) { return 0; }
s32 PS4_SYSV_ABI np_poll_async(int, int* result) {
    if (result) *result = 0;
    return 0; // finished
}
s32 PS4_SYSV_ABI np_check_availability(int, const void*, void*) { return 0; }
s32 PS4_SYSV_ABI np_parental(int, const void*, std::int8_t* age, std::uint8_t* info) {
    if (age) *age = 25;
    if (info) std::memset(info, 0, 3);
    return 0;
}
s32 PS4_SYSV_ABI np_check_plus(int, const void*, std::uint8_t* result) {
    if (result) result[0] = 1;
    return 0;
}
s32 PS4_SYSV_ABI np_plus_notify(int, u64) { return 0; }
// The game server binds the session to the account by X-BB-Token, not by this code.
s32 PS4_SYSV_ABI npauth_get_code(int, const void*, char* code, int* issuer) {
    if (code) {
        std::memset(code, 0, 128);
        std::strncpy(code, "DUMMY", 127);
    }
    if (issuer) *issuer = 10;
    return 0;
}

// ---- lookup ---------------------------------------------------------------------------------------------

std::atomic<int> g_lookup_id{1};
s32 PS4_SYSV_ABI lookup_title_ctx(const void*) { return g_lookup_id.fetch_add(1); }
s32 PS4_SYSV_ABI lookup_async(int, const void*) { return g_lookup_id.fetch_add(1); }
s32 PS4_SYSV_ABI lookup_npid(int, const void* online, void* npid, void*) {
    if (!online || !npid) return kNpErrInvalidArg;
    char name[17]{};
    std::memcpy(name, online, 16);
    FillNpId(npid, name);
    return 0;
}

// ---- score: no leaderboard server ------------------------------------------------------------------

constexpr int kScoreErrOffline = static_cast<int>(0x80550605u);
std::atomic<int> g_score_id{1};
s32 PS4_SYSV_ABI score_title_ctx(const void*, int) { return g_score_id.fetch_add(1); }
s32 PS4_SYSV_ABI score_create_request(int) { return g_score_id.fetch_add(1); }
s32 PS4_SYSV_ABI score_set_pc_id(int, int) { return 0; }
s32 PS4_SYSV_ABI score_offline() { return kScoreErrOffline; }
s32 PS4_SYSV_ABI score_comment_ok(int, const char*) { return 0; }
// sceNpScoreSanitizeComment(reqId, comment, sanitizedComment, option): the text back in the buffer.
// The game's own character name goes through it before it is sent to the other players, who show it
// when Player Name Display is "Name": an answer that leaves the buffer empty shows this player blank.
s32 PS4_SYSV_ABI score_sanitize_comment(int, const char* comment, char* sanitized, void*) {
    return NpScore::SanitizeCopy(comment, sanitized);
}

// ---- WebAPI: answered here, never sent anywhere -------------------------------------------------------

constexpr int kWebApiInvalid = static_cast<int>(0x80552902);
constexpr int kWebApiNoReq = static_cast<int>(0x80552906);
constexpr char kWebApiJsonType[] = "application/json; charset=utf-8";

struct WebApiReq {
    int status = 200;
    std::string body;
    std::size_t read_off = 0;
};
std::mutex g_web_mu;
std::atomic<int> g_web_id{1};
std::int64_t g_web_req = 1;
std::unordered_map<std::int64_t, WebApiReq> g_web_reqs;

std::string webapi_body_for_path(const char* path) {
    if (path && std::strstr(path, "friendList")) return "{\"totalResults\":0,\"friendList\":[]}";
    if (path && std::strstr(path, "blockList")) return "{\"totalResults\":0,\"blockList\":[]}";
    return "{\"totalResults\":0}";
}
s32 PS4_SYSV_ABI webapi_init(int, u64) { return g_web_id.fetch_add(1); }
s32 PS4_SYSV_ABI webapi_create_ctx(int, const void*) { return g_web_id.fetch_add(1); }
s32 PS4_SYSV_ABI webapi_create_filter(int, const void*, u64) { return g_web_id.fetch_add(1); }
s32 PS4_SYSV_ABI webapi_reg_push(int, int, void*, void*) { return g_web_id.fetch_add(1); }
s32 PS4_SYSV_ABI webapi_create_req(int, const char*, const char* path, int, const void*, std::int64_t* req_id) {
    if (!path || !req_id) return kWebApiInvalid;
    std::lock_guard lk(g_web_mu);
    const std::int64_t id = g_web_req++;
    g_web_reqs[id] = WebApiReq{200, webapi_body_for_path(path), 0};
    *req_id = id;
    return 0;
}
s32 PS4_SYSV_ABI webapi_delete_req(std::int64_t req) {
    std::lock_guard lk(g_web_mu);
    g_web_reqs.erase(req);
    return 0;
}
s32 PS4_SYSV_ABI webapi_send(std::int64_t req, const void*, u64) {
    std::lock_guard lk(g_web_mu);
    return g_web_reqs.count(req) ? 0 : kWebApiNoReq;
}
s32 PS4_SYSV_ABI webapi_status(std::int64_t req, int* code) {
    if (!code) return kWebApiInvalid;
    std::lock_guard lk(g_web_mu);
    const auto it = g_web_reqs.find(req);
    if (it == g_web_reqs.end()) return kWebApiNoReq;
    *code = it->second.status;
    return 0;
}
s32 PS4_SYSV_ABI webapi_read(std::int64_t req, void* data, u64 size) {
    std::lock_guard lk(g_web_mu);
    const auto it = g_web_reqs.find(req);
    if (it == g_web_reqs.end()) return kWebApiNoReq;
    WebApiReq& r = it->second;
    if (!data || size == 0 || r.read_off >= r.body.size()) return 0;
    const std::size_t n = std::min<std::size_t>(static_cast<std::size_t>(size), r.body.size() - r.read_off);
    std::memcpy(data, r.body.data() + r.read_off, n);
    r.read_off += n;
    return static_cast<int>(n);
}
bool is_content_type(const char* name) {
    return std::strcmp(name, "Content-Type") == 0 || std::strcmp(name, "content-type") == 0;
}
s32 PS4_SYSV_ABI webapi_hdr_len(std::int64_t req, const char* name, u64* len) {
    if (!name || !len) return kWebApiInvalid;
    std::lock_guard lk(g_web_mu);
    if (!g_web_reqs.count(req)) return kWebApiNoReq;
    if (!is_content_type(name)) return kWebApiInvalid;
    *len = sizeof(kWebApiJsonType);
    return 0;
}
s32 PS4_SYSV_ABI webapi_hdr_val(std::int64_t req, const char* name, char* buf, u64 n) {
    if (!name || !buf || n == 0) return kWebApiInvalid;
    std::lock_guard lk(g_web_mu);
    if (!g_web_reqs.count(req)) return kWebApiNoReq;
    if (!is_content_type(name)) return kWebApiInvalid;
    std::snprintf(buf, static_cast<std::size_t>(n), "%s", kWebApiJsonType);
    return 0;
}
s32 PS4_SYSV_ABI webapi_parse_npid(const char* json, void* npid) {
    if (!json || !npid) return kWebApiInvalid;
    const char* s = json;
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') ++s;
    char online[17]{};
    auto take_quoted = [&](const char* p) {
        const char* q = std::strchr(p, '"');
        if (!q) return false;
        ++q;
        const char* e = std::strchr(q, '"');
        if (!e) return false;
        const std::size_t n = static_cast<std::size_t>(e - q);
        if (n == 0 || n >= sizeof(online)) return false;
        std::memcpy(online, q, n);
        return true;
    };
    if (*s == '{') {
        const char* key = std::strstr(s, "onlineId");
        if (!key) key = std::strstr(s, "\"data\"");
        // The value after the key's colon.
        const char* colon = key ? std::strchr(key, ':') : nullptr;
        if (!colon || !take_quoted(colon)) return kWebApiInvalid;
    } else if (*s == '"') {
        if (!take_quoted(s)) return kWebApiInvalid;
    } else {
        std::snprintf(online, sizeof(online), "%s", s);
    }
    FillNpId(npid, online);
    return 0;
}

} // namespace

// A signaling datagram from another client (thd_net.cpp), on the dispatcher.
void SignalingControl(const std::uint8_t* data, std::size_t len, std::uint32_t addr, std::uint16_t port) {
    if (len < kPacketLen) return;
    const std::uint8_t kind = data[4];
    const Reason reason = static_cast<Reason>(data[5]);
    std::string from(reinterpret_cast<const char*>(data + kFromAt), kIdLen);
    from.resize(std::strlen(from.c_str()));
    if (from.empty()) return;
    std::uint64_t nonce = 0;
    std::memcpy(&nonce, data + kNonceAt, sizeof(nonce));
    const std::uint16_t from_port = static_cast<std::uint16_t>((port << 8) | (port >> 8));
    if (Trace()) Log("np: signaling %s from %s", kind_name(kind), from.c_str());
    std::vector<Event> events;
    {
        std::lock_guard lk(g_sig_mu);
        unsigned tid = find_transport_locked(from);
        switch (kind) {
        case kActivationRequest: {
            // The peer's game activated its connection to us.
            if (!tid) tid = ensure_transport_locked(from, addr, from_port);
            Transport& t = g_tr[tid];
            t.peer_activated = true;
            t.addr = addr;
            t.port = from_port;
            if (t.state == State::Inactive) t.state = State::WaitOffer;
            const unsigned ctx = g_sig_ctx.empty() ? 0 : g_sig_ctx.begin()->first;
            unsigned cid = ctx ? find_conn_locked(from, ctx) : 0;
            if (ctx && !cid) {
                cid = alloc_conn_id_locked();
                Conn c;
                c.ctx = ctx;
                c.online_id = from;
                c.transport = tid;
                c.member_id = t.member_id;
                c.peer_activated = true;
                g_conn[cid] = c;
            }
            if (cid) {
                g_conn[cid].peer_activated = true;
                g_conn[cid].transport = tid;
            }
            if (t.state == State::Established) establish_locked(tid, false, events);
            else start_stepping_locked(tid);
            send_locked(t, kActivationAck);
            break;
        }
        case kOffer: {
            if (!tid) break;  // no transport for the sender: ignored
            Transport& t = g_tr[tid];
            t.addr = addr;
            t.port = from_port;
            t.peer_activated = true;
            if (t.state != State::Established) t.state = State::SendingAccept;
            send_locked(t, kAccept);
            break;
        }
        case kAccept: {
            if (!tid) break;
            Transport& t = g_tr[tid];
            t.peer_activated = true;
            t.addr = addr;
            t.port = from_port;
            if (t.state == State::SendingOffer || t.state == State::WaitAccept || t.state == State::SendingAccept ||
                t.state == State::WaitOffer) {
                t.state = State::ConnCheck;
                send_locked(t, kCheck, kDeactivate, sig_now_us());
            }
            break;
        }
        case kCheck: {
            if (!tid) break;
            Transport& t = g_tr[tid];
            send_locked(t, kCheckAck, kDeactivate, nonce);
            if (t.state == State::SendingAccept || t.state == State::WaitOffer) t.state = State::ConnCheck;
            break;
        }
        case kCheckAck: {
            if (!tid) break;
            if (g_tr[tid].state != State::Established) establish_locked(tid, true, events);
            break;
        }
        case kActivationAck: {
            if (!tid) break;
            g_tr[tid].peer_activated = true;
            break;
        }
        case kEstablishedMsg: {
            if (!tid) break;
            Transport& t = g_tr[tid];
            t.peer_established = true;
            if (t.state == State::Established) establish_locked(tid, false, events);
            break;
        }
        case kClose: {
            if (!tid) break;
            const unsigned cid = find_conn_locked(from);
            if (reason == kDeactivate && cid && g_tr[tid].state == State::Established) {
                events.emplace_back(g_conn[cid].ctx, cid, kEvPeerDeactivated, 0);
            } else {
                if (cid) close_conn_dead_locked(cid, kSigErrTerminatedByPeer, events);
                else g_tr[tid].state = State::Inactive;
                queue_m2_dead(from, kM2ErrTerminatedByPeer);
            }
            break;
        }
        default:
            break;
        }
    }
    fire_all(events);
}

bool SignalingLive(const std::string& online_id) {
    std::lock_guard lk(g_sig_mu);
    return find_conn_locked(online_id) != 0;
}

// shadPS4's StartMatching2, for one member.
void SignalingM2Connect(const std::string& online_id) {
    std::lock_guard lk(g_sig_mu);
    const unsigned tid = ensure_transport_locked(online_id, 0, 0);
    Transport& t = g_tr[tid];
    refresh_addr_locked(t);
    if (t.state == State::WaitOffer) t.state = State::SendingOffer;
    if (Trace()) Log("np: matching2 signaling to %s: transport %u status %d", online_id.c_str(), tid, status_of(t));
    if (t.state == State::Established) {
        const std::string who = online_id;
        DispatchAfter(kM2EstablishDelayMs, Prio::Signaling, [who] { M2TransportUp(who); });
        return;
    }
    send_handshake_locked(t);
    start_stepping_locked(tid);
}

int SignalingTransportStatus(const std::string& online_id) {
    std::lock_guard lk(g_sig_mu);
    const unsigned tid = find_transport_locked(online_id);
    return tid ? status_of(g_tr[tid]) : 0;
}

// shadPS4's StopMatching2, for one member: the transport closes unless an NpSignaling connection of
// the game's is on it.
void SignalingM2Stop(const std::string& online_id) {
    std::lock_guard lk(g_sig_mu);
    if (find_conn_locked(online_id)) return;
    if (unsigned tid = find_transport_locked(online_id)) g_tr.erase(tid);
}

// A player is now a room member: its transport and connections know its peer-table entry (the
// address), and a connection waiting for it goes on.
void SignalingPeerKnown(std::uint16_t member_id) {
    Peer p;
    if (!PeersGet(member_id, &p)) return;
    std::vector<unsigned> waiting;
    {
        std::lock_guard lk(g_sig_mu);
        for (auto& [id, t] : g_tr) {
            if (t.online_id != p.online_id || t.state == State::Inactive) continue;
            t.member_id = member_id;
            if (p.addr && p.port) {
                t.addr = p.addr;
                t.port = p.port;
            }
        }
        for (auto& [id, c] : g_conn) {
            if (c.dead || c.online_id != p.online_id) continue;
            c.member_id = member_id;
            if (c.start_handshake) waiting.push_back(id);
        }
    }
    for (unsigned id : waiting) DispatchAfter(0, Prio::Signaling, [id] { activation(id); });
}

// The player left the room for good: its connections DEAD (terminated by peer).
void SignalingPeerDead(const std::string& online_id) {
    std::vector<Event> events;
    {
        std::lock_guard lk(g_sig_mu);
        if (unsigned tid = find_transport_locked(online_id)) close_transport_dead_locked(tid, kSigErrTerminatedByPeer, events);
        while (unsigned cid = find_conn_locked(online_id)) close_conn_dead_locked(cid, kSigErrTerminatedByPeer, events);
    }
    queue_all(std::move(events));
}

void SignalingReset() {
    std::lock_guard lk(g_sig_mu);
    g_conn.clear();
    g_tr.clear();
}

void RegisterNp(Core::Loader::SymbolsResolver* sym) {
#define SIG(nid, fn) LIB_FUNCTION(nid, "libSceNpSignaling", 1, "libSceNpSignaling", fn)
    SIG("3KOuC4RmZZU", sig_init);
    SIG("NPhw0UXaNrk", sig_term);
    SIG("5yYjEdd4t8Y", sig_create_ctx);
    SIG("hx+LIg-1koI", sig_delete_ctx);
    SIG("0UvTFeomAUM", sig_activate);
    SIG("6UEembipgrM", sig_deactivate);
    SIG("bD-JizUb3JM", sig_status);
#undef SIG
#define NP(nid, fn) LIB_FUNCTION(nid, "libSceNpManager", 1, "libSceNpManager", fn)
    NP("VfRSmPmj8Q8", np_register_cb);        // sceNpRegisterStateCallback
    NP("mjjTXh+NHWY", np_ok);                 // sceNpUnregisterStateCallback
    NP("uFJpaKNBAj4", np_register_cb);        // sceNpRegisterGamePresenceCallback
    NP("GImICnh+boA", np_register_cb);        // sceNpRegisterPlusEventCallback
    NP("xViqJdDgKl0", np_ok);                 // sceNpUnregisterPlusEventCallback
    NP("3Zl8BePTh9Y", np_ok);                 // sceNpCheckCallback
    NP("Ec63y59l9tw", np_ok);                 // sceNpSetNpTitleId
    NP("A2CQ3kgSopQ", np_ok);                 // sceNpSetContentRestriction
    NP("Gaxrp3EWY-M", np_plus_notify);        // sceNpNotifyPlusFeature
    NP("eQH7nWPcAgc", np_get_state);          // sceNpGetState
    NP("XDncXQIJUSk", np_online_id);          // sceNpGetOnlineId
    NP("p-o74CnoNzY", np_get_npid);           // sceNpGetNpId
    NP("IPb1hd1wAGc", np_presence_status);    // sceNpGetGamePresenceStatus
    NP("eiqMCt9UshI", np_create_request);     // sceNpCreateAsyncRequest
    NP("S7QTn72PrDw", np_request_done);       // sceNpDeleteRequest
    NP("OzKvTvg3ZYU", np_request_done);       // sceNpAbortRequest
    NP("uqcPJLWL08M", np_poll_async);         // sceNpPollAsync
    NP("2rsFmlGWleQ", np_check_availability); // sceNpCheckNpAvailability
    NP("ilwLM4zOmu4", np_parental);           // sceNpGetParentalControlInfo
    NP("r6MyYJkryz8", np_check_plus);         // sceNpCheckPlus
#undef NP
    LIB_FUNCTION("i8UmXTSq7N4", "libSceNpCommon", 1, "libSceNpCommon", np_cmp_npid);
    LIB_FUNCTION("dj+O5aD2a0Q", "libSceNpCommon", 1, "libSceNpCommon", np_cmp_online);
#define AUTH(nid, fn) LIB_FUNCTION(nid, "libSceNpAuth", 1, "libSceNpAuth", fn)
    AUTH("N+mr7GjTvr8", np_create_request);
    AUTH("H8wG9Bk-nPc", np_request_done);
    AUTH("gjSyfzSsDcE", np_poll_async);
    AUTH("KxGkOrQJTqY", npauth_get_code);
#undef AUTH
#define LOOKUP(nid, fn) LIB_FUNCTION(nid, "libSceNpManager", 1, "libSceNpManager", fn)
    LOOKUP("8533Q+LU7EQ", lookup_title_ctx);
    LOOKUP("mtqDK9zkoIE", np_request_done);
    LOOKUP("JA4+sS39GMs", lookup_async);
    LOOKUP("wLaxchvEEnk", np_request_done);
    LOOKUP("eYz4v5Uek9U", np_request_done);
    LOOKUP("V4EVrruHuy8", np_poll_async);
    LOOKUP("T6tnM1Uti4g", lookup_npid);
#undef LOOKUP
#define SCORE(nid, fn) LIB_FUNCTION(nid, "libSceNpScore", 1, "libSceNpScore", fn)
    SCORE("KnNA1TEgtBI", score_title_ctx);
    SCORE("G0pE+RNCwfk", np_request_done);
    SCORE("gW8qyjYrUbk", score_create_request);
    SCORE("dK8-SgYf6r4", np_request_done);
    SCORE("1i7kmKbX6hk", np_request_done);
    SCORE("bygbKdHmjn4", score_set_pc_id);
    SCORE("ETS-uM-vH9Q", score_offline); // GetRankingByNpIdPcId
    SCORE("KBHxDjyk-jA", score_offline); // GetRankingByRange
    SCORE("zKoVok6FFEI", score_offline); // GetGameData
    SCORE("zT0XBtgtOSI", score_offline); // RecordScore
    SCORE("LoVMVrijVOk", score_offline); // GetBoardInfo
    SCORE("bcoVwcBjQ9E", score_offline); // RecordGameData
    SCORE("2b3TI0mDYiI", score_comment_ok);
    SCORE("r4oAo9in0TA", score_sanitize_comment);
#undef SCORE
#define WEB(nid, fn) LIB_FUNCTION(nid, "libSceNpWebApi", 1, "libSceNpWebApi", fn)
    WEB("G3AnLNdRBjE", webapi_init);
    WEB("asz3TtIqGF8", np_request_done);      // Terminate
    WEB("x1Y7yiYSk7c", webapi_create_ctx);
    WEB("XUjdsSTTZ3U", np_request_done);      // DeleteContext
    WEB("y5Ta5JCzQHY", webapi_create_filter);
    WEB("zE+R6Rcx3W0", np_ok);                // DeletePushEventFilter
    WEB("PfSTDCgNMgc", webapi_reg_push);
    WEB("qK4o2656W4w", np_ok);                // UnregisterPushEventCallback
    WEB("rdgs5Z1MyFw", webapi_create_req);
    WEB("noQgleu+KLE", webapi_delete_req);
    WEB("JzhYTP2fG18", webapi_delete_req);    // AbortRequest
    WEB("kVbL4hL3K7w", webapi_send);
    WEB("k210oKgP80Y", webapi_status);
    WEB("CQtPRSF6Ds8", webapi_read);
    WEB("743ZzEBzlV8", webapi_hdr_len);
    WEB("VwJ5L0Higg0", webapi_hdr_val);
    WEB("or0e885BlXo", webapi_parse_npid);
#undef WEB
}

} // namespace BbThd
