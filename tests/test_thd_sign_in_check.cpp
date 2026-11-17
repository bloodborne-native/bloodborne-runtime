// SPDX-License-Identifier: GPL-3.0-or-later
// How the Hunter's Dream module reads the account service's answer to POST /auth/whoami
// (gpu/bbnet20/thd/thd_sign_in_check.h, from droogie/bbhost's sign_in_check.h, GPL-3.0-or-later):
// only a clear "no such token" is a verdict against the sign-in; a busy server, a proxy page or no
// answer never marks it refused.
#include <cassert>
#include <cstdio>

#include "gpu/bbnet20/thd/thd_sign_in_check.h"

using namespace BbThd;

int main() {
    // A token the server knows: valid, with the name as the server spells it and its expiry.
    SignInReply r = ReadSignInReply(200, R"({"ResKind":0,"OnlineId":"Hunter","ExpiresAt":1893456000})", "");
    assert(r.verdict == SignInVerdict::Valid && r.name == "Hunter" && r.expires_at == 1893456000 && r.status == 200);
    r = ReadSignInReply(200, R"({"ResKind":0,"OnlineId":"Hunter"})", "");
    assert(r.verdict == SignInVerdict::Valid && r.expires_at == 0);

    // One it does not know: refused, with the server's words (200 or 401 alike).
    r = ReadSignInReply(200, R"({"ResKind":1,"Message":"no such token"})", "");
    assert(r.verdict == SignInVerdict::Refused && r.why == "no such token");
    r = ReadSignInReply(401, R"({"ResKind":1})", "");
    assert(r.verdict == SignInVerdict::Refused && r.why == "refused");

    // Not a verdict: a busy server, a refusal-shaped body on a 5xx or 429, a server without the
    // account service, a proxy's page, no answer, a valid-looking answer without a name.
    assert(ReadSignInReply(503, R"({"ResKind":1,"Message":"no such token"})", "").verdict == SignInVerdict::Unchecked);
    assert(ReadSignInReply(429, R"({"ResKind":1})", "").verdict == SignInVerdict::Unchecked);
    r = ReadSignInReply(404, R"({"error":"not found"})", "");
    assert(r.verdict == SignInVerdict::Unchecked && r.why == "HTTP 404 without ResKind");
    assert(ReadSignInReply(200, "<html>sign in to the wifi</html>", "").verdict == SignInVerdict::Unchecked);
    assert(ReadSignInReply(200, "", "").verdict == SignInVerdict::Unchecked);
    r = ReadSignInReply(0, "", "Connection refused");
    assert(r.verdict == SignInVerdict::Unchecked && r.status == 0 && r.why == "Connection refused");
    r = ReadSignInReply(0, "", "");
    assert(r.verdict == SignInVerdict::Unchecked && r.why == "no answer");
    assert(ReadSignInReply(200, R"({"ResKind":0})", "").verdict == SignInVerdict::Unchecked);
    assert(ReadSignInReply(200, R"({"ResKind":0,"OnlineId":""})", "").verdict == SignInVerdict::Unchecked);
    // A transport error wins over a leftover status.
    r = ReadSignInReply(200, R"({"ResKind":0,"OnlineId":"Hunter"})", "timeout");
    assert(r.verdict == SignInVerdict::Unchecked && r.status == 0);

    std::puts("thd sign-in check: ok");
    return 0;
}
