// SPDX-License-Identifier: GPL-3.0-or-later
// Which signaling datagrams the P2P port acts on (thd_np.cpp's SignalingControl). bbhost's receiver
// (v0.2.31) acts on any datagram with the magic: it never reads the `to` id, and a datagram naming a
// partner moves that partner's address to its sender, ends its connection, or makes a new transport.
// This is stricter, after review of the pull request:
//   - `to` must be our own online id;
//   - for a player we already talk to (a transport) or know (the room's peer table), the datagram
//     must come from that player's address (the IP; a NAT may change the port), so a forged one
//     cannot move or close the link;
//   - an unknown player may only open a link with an ActivationRequest, and only when the server
//     resolves its id to the datagram's address (the host activates its connection to a guest it
//     summons before the guest has joined the room);
//   - at most kMaxTransports live transports (a room holds at most 4 other players).
#pragma once

#include <cstdint>

namespace BbThd::SignalAdmit {

constexpr int kMaxTransports = 8;

enum class Verdict {
    Accept,   // act on it
    Drop,     // ignore it
    Resolve,  // act on it only if the server resolves the sender's id to the datagram's address
};

struct Input {
    bool to_ours = false;              // the datagram's `to` is our online id
    bool activation_request = false;   // its kind
    bool have_transport = false;       // a live transport to the sender exists
    std::uint32_t transport_addr = 0;  // its address (network order), 0 if none
    bool peer_known = false;           // the sender is in the room's peer table
    std::uint32_t peer_addr = 0;       // network order, 0 if none
    std::uint32_t peer_local_addr = 0; // network order, 0 if none
    std::uint32_t src_addr = 0;        // where the datagram came from (network order)
    int live_transports = 0;           // live transports, all players
};

inline bool KnownAddress(const Input& in) {
    if (!in.src_addr) return false;
    return in.src_addr == in.transport_addr || in.src_addr == in.peer_addr || in.src_addr == in.peer_local_addr;
}

inline Verdict Admit(const Input& in) {
    if (!in.to_ours) return Verdict::Drop;
    if (in.have_transport || in.peer_known) {
        // A known player: from one of its addresses only, and no new transport past the cap.
        if (!in.have_transport && in.live_transports >= kMaxTransports) return Verdict::Drop;
        if (!in.have_transport && !in.activation_request) return Verdict::Drop;
        return KnownAddress(in) ? Verdict::Accept : Verdict::Drop;
    }
    if (!in.activation_request || in.live_transports >= kMaxTransports) return Verdict::Drop;
    return Verdict::Resolve;
}

// The server's answer for an unknown sender: its address is the datagram's.
inline bool ResolvedMatches(std::uint32_t resolved_addr, std::uint32_t src_addr) {
    return resolved_addr != 0 && resolved_addr == src_addr;
}

}  // namespace BbThd::SignalAdmit
