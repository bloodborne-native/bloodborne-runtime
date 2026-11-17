// SPDX-License-Identifier: GPL-3.0-or-later
// What the server says of this PC's sign-in: its answer to POST /auth/whoami {Token}, read from the
// HTTP status, the body and the transport's error. Taken from droogie/bbhost src/net/sign_in_check.h
// (v0.2.35), GPL-3.0-or-later, without the connection-test and relay-switch fields the launcher has
// no screen for.
//
// The account service answers 200 in both cases: {ResKind 0, OnlineId, ExpiresAt, ...} for a token it
// knows, {ResKind 1, Message "no such token"} for one it does not (expired, or revoked when this PC
// was signed out: a sign-out elsewhere, a recovery, "sign out everywhere"). Anything else is no
// verdict: no answer, a 5xx or 429, a server without the account service (its framework's 404 without
// ResKind), a body that is not JSON.
#pragma once

#include <string>

#include <nlohmann/json.hpp>

namespace BbThd {

enum class SignInVerdict {
    Valid,      // the server knows the token
    Refused,    // the server does not: a new sign-in is needed
    Unchecked,  // no verdict; `why` says what came back instead
};

struct SignInReply {
    SignInVerdict verdict = SignInVerdict::Unchecked;
    std::string name;       // Valid: the account's name, as the server spells it
    double expires_at = 0;  // Valid: when the token expires (Unix seconds); 0 when not said
    std::string why;        // Refused: the server's Message; Unchecked: what came instead
    long status = 0;        // the HTTP status, 0 when no answer came
};

inline SignInReply ReadSignInReply(long status, const std::string& body, const std::string& transport_error) {
    SignInReply r;
    r.status = transport_error.empty() ? status : 0;
    if (!transport_error.empty() || status <= 0) {
        r.why = transport_error.empty() ? std::string("no answer") : transport_error;
        return r;
    }
    const nlohmann::json v = nlohmann::json::parse(body, nullptr, false);
    const auto kind = v.is_object() ? v.find("ResKind") : v.end();
    if (!v.is_object() || kind == v.end() || !kind->is_number()) {
        // A server without the account service: its framework's own "not found", or a proxy's page.
        r.why = "HTTP " + std::to_string(status) + " without ResKind";
        return r;
    }
    if (kind->get<double>() != 0) {
        // The account service's refusal. A busy server (429, 5xx) is not a verdict on the token.
        if (status != 200 && status != 401) {
            r.why = "HTTP " + std::to_string(status);
            return r;
        }
        const auto msg = v.find("Message");
        r.verdict = SignInVerdict::Refused;
        r.why = msg != v.end() && msg->is_string() && !msg->get<std::string>().empty() ? msg->get<std::string>() : "refused";
        return r;
    }
    const auto id = v.find("OnlineId");
    if (status < 200 || status >= 300 || id == v.end() || !id->is_string() || id->get<std::string>().empty()) {
        r.why = "HTTP " + std::to_string(status) + " without OnlineId";
        return r;
    }
    r.verdict = SignInVerdict::Valid;
    r.name = id->get<std::string>();
    const auto exp = v.find("ExpiresAt");
    if (exp != v.end() && exp->is_number()) r.expires_at = exp->get<double>();
    return r;
}

} // namespace BbThd
