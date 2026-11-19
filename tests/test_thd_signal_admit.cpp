// SPDX-License-Identifier: GPL-3.0-or-later
// Which signaling datagrams the Hunter's Dream module acts on (gpu/bbnet20/thd/thd_signal_admit.h):
// forged ones must not move a partner's link, close it, or fill the transport table.
#include <cassert>

#include "gpu/bbnet20/thd/thd_signal_admit.h"

using namespace BbThd::SignalAdmit;

int main() {
    const std::uint32_t partner = 0x0100000a, other = 0x0200000a, lan = 0x0100a8c0;

    Input in;
    in.to_ours = true;
    in.src_addr = partner;

    // A datagram meant for someone else is dropped, whoever sent it.
    Input wrong = in;
    wrong.to_ours = false;
    wrong.have_transport = true;
    wrong.transport_addr = partner;
    assert(Admit(wrong) == Verdict::Drop);

    // A partner with a transport: from its address is accepted, from another address is not.
    Input known = in;
    known.have_transport = true;
    known.transport_addr = partner;
    assert(Admit(known) == Verdict::Accept);
    known.src_addr = other;
    assert(Admit(known) == Verdict::Drop);  // a forged Offer / Accept / Close

    // The room's peer table counts: its address or its LAN address.
    Input member = in;
    member.peer_known = true;
    member.activation_request = true;
    member.peer_addr = partner;
    assert(Admit(member) == Verdict::Accept);
    member.src_addr = lan;
    member.peer_local_addr = lan;
    assert(Admit(member) == Verdict::Accept);
    member.src_addr = other;
    assert(Admit(member) == Verdict::Drop);

    // A member with no transport yet may open one only with an ActivationRequest.
    Input handshake = in;
    handshake.peer_known = true;
    handshake.peer_addr = partner;
    handshake.activation_request = false;
    assert(Admit(handshake) == Verdict::Drop);

    // A stranger: only an ActivationRequest, and then only after the server vouches for it.
    Input stranger = in;
    stranger.activation_request = true;
    assert(Admit(stranger) == Verdict::Resolve);
    stranger.activation_request = false;
    assert(Admit(stranger) == Verdict::Drop);  // Close / Offer from nobody
    assert(ResolvedMatches(partner, partner));
    assert(!ResolvedMatches(partner, other));
    assert(!ResolvedMatches(0, 0));

    // The table has a cap: no new transport past it, an existing one still works.
    Input flood = stranger;
    flood.activation_request = true;
    flood.live_transports = kMaxTransports;
    assert(Admit(flood) == Verdict::Drop);
    member.src_addr = partner;
    member.live_transports = kMaxTransports;
    assert(Admit(member) == Verdict::Drop);
    known.src_addr = partner;
    known.live_transports = kMaxTransports;
    assert(Admit(known) == Verdict::Accept);

    // A transport with no address yet cannot vouch for a datagram.
    Input blank = in;
    blank.have_transport = true;
    assert(Admit(blank) == Verdict::Drop);
    return 0;
}
