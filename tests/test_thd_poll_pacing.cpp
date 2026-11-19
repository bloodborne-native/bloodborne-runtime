// SPDX-License-Identifier: GPL-3.0-or-later
// The Hunter's Dream module's event-poll pace (gpu/bbnet20/thd/thd_poll_pacing.h, from droogie/bbhost's
// poll_pacing.h, GPL-3.0-or-later): the contract of the server's long polls and of the poller's
// generations. Written against bbhost's documented behaviour, not copied from its tests.
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <thread>

#include "gpu/bbnet20/thd/thd_poll_pacing.h"

using namespace BbThd::PollPacing;

static Outcome Answer(long long wait_max, bool event, long long elapsed) {
    Outcome o;
    o.answered = true;
    o.wait_max_ms = wait_max;
    o.event = event;
    o.elapsed_ms = elapsed;
    return o;
}

int main() {
    // The first poll asks for the whole wait; its read timeout is the wait and 5 s, the connection's a
    // few seconds; a poll that names no wait keeps the old 4 s.
    State s;
    assert(s.wait_ms == 25000 && s.backoff_ms == 0);
    assert(TimeoutFor(25000) == 30000 && TimeoutFor(10000) == 15000 && TimeoutFor(0) == 4000);
    assert(ConnectFor(25000) == 5000 && ConnectFor(0) == 4000);

    // A held answer with nothing in it: the next poll at once, the wait capped by WaitMaxMs and by 25 s.
    Next n = NextPoll(s, Answer(25000, false, 25000), 0.5);
    assert(n.state.wait_ms == 25000 && n.sleep_ms == 0);
    n = NextPoll(s, Answer(10000, false, 10000), 0.5);
    assert(n.state.wait_ms == 10000 && n.sleep_ms == 0);
    n = NextPoll(s, Answer(60000, false, 25000), 0.5);
    assert(n.state.wait_ms == 25000);

    // An empty answer that was not held (under 250 ms) waits out the rest of the old pace: never a tight loop.
    n = NextPoll(s, Answer(25000, false, 10), 0.5);
    assert(n.state.wait_ms == 25000 && n.sleep_ms == 240);
    n = NextPoll(s, Answer(25000, false, 0), 0.5);
    assert(n.sleep_ms == 250);
    n = NextPoll(s, Answer(25000, false, 5000), 0.5);
    assert(n.sleep_ms == 0);

    // An event is followed at once.
    n = NextPoll(s, Answer(25000, true, 3), 0.5);
    assert(n.sleep_ms == 0);

    // A server that holds no polls (no WaitMaxMs): no wait named, the old pace of 250 ms.
    n = NextPoll(s, Answer(-1, false, 20), 0.5);
    assert(n.state.wait_ms == 0 && n.sleep_ms == 250);
    // ... and WaitMaxMs coming back turns the long polls on again.
    State plain = n.state;
    n = NextPoll(plain, Answer(25000, false, 25000), 0.5);
    assert(n.state.wait_ms == 25000);

    // Errors back off 1, 2, 4, ... 30 s with 20% of jitter either way, and keep the wait asked for.
    Outcome fail;
    State e = s;
    int expect = 1000;
    for (int i = 0; i < 8; ++i) {
        Next f = NextPoll(e, fail, 0.5);
        assert(f.state.backoff_ms == expect);
        assert(f.sleep_ms == expect);  // the middle of the jitter
        assert(f.state.wait_ms == 25000);
        e = f.state;
        expect = expect * 2 > 30000 ? 30000 : expect * 2;
    }
    assert(e.backoff_ms == 30000);
    assert(Jittered(1000, 0.0) == 800 && Jittered(1000, 0.999999) == 1200);
    assert(Jittered(1000, -3.0) == 800 && Jittered(1000, 7.0) == 1200);
    // An answer starts the back-off over.
    n = NextPoll(e, Answer(25000, false, 25000), 0.5);
    assert(n.state.backoff_ms == 0);
    Next again = NextPoll(n.state, fail, 0.5);
    assert(again.state.backoff_ms == 1000);
    // A 5xx or 429 is an error like any other.
    Outcome busy;
    busy.status = 503;
    assert(NextPoll(s, busy, 0.5).sleep_ms == 1000);
    busy.status = 429;
    assert(NextPoll(s, busy, 0.5).sleep_ms == 1000);

    // A refused sign-in asks once a minute and leaves the back-off where it was.
    Outcome refused;
    refused.status = 401;
    refused.refused = true;
    State b = s;
    b.backoff_ms = 4000;
    n = NextPoll(b, refused, 0.5);
    assert(n.sleep_ms == 60000 && n.state.backoff_ms == 4000 && n.state.wait_ms == 25000);
    refused.refused = false;  // a 401 alone is the same
    assert(NextPoll(b, refused, 0.5).sleep_ms == 60000);

    // The generations: a stop ends a poller's turn, a new start is a new number.
    Gate g;
    assert(!g.Running() && !g.Live(0) && !g.Live(1));
    const auto one = g.Start();
    assert(one != 0 && g.Running() && g.Live(one));
    assert(g.Begin(one));
    g.End();
    g.Stop();
    assert(!g.Running() && !g.Live(one) && !g.Begin(one));
    const auto two = g.Start();
    assert(two != one && g.Live(two) && !g.Live(one));

    // One request out at a time: a new poller's first poll waits for an old held one to end.
    assert(g.Begin(two));
    const auto three = g.Start();
    std::atomic<bool> got{false};
    std::thread waiter([&] { got = g.Begin(three); });
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    assert(!got.load());  // still waiting for the old poll
    g.End();              // the old poll ends
    waiter.join();
    assert(got.load());
    g.End();

    // A sleep ends at once when the generation does, and runs its time otherwise.
    const auto four = g.Start();
    const auto t0 = std::chrono::steady_clock::now();
    assert(g.Sleep(four, 30));
    assert(std::chrono::steady_clock::now() - t0 >= std::chrono::milliseconds(25));
    std::thread stopper([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        g.Stop();
    });
    const auto t1 = std::chrono::steady_clock::now();
    assert(!g.Sleep(four, 5000));
    assert(std::chrono::steady_clock::now() - t1 < std::chrono::milliseconds(2000));
    stopper.join();
    assert(!g.Sleep(four, 5000));  // over already: at once

    std::puts("thd poll pacing: ok");
    return 0;
}
