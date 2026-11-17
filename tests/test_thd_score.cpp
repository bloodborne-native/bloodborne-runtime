// SPDX-License-Identifier: GPL-3.0-or-later
// The Hunter's Dream module's sceNpScoreSanitizeComment answer (gpu/bbnet20/thd/thd_score.h, from
// droogie/bbhost's np_score.h): the game's character name goes through it before it is sent to the
// other players, so it must come back as it went. Taken from bbhost's tests/np_score_test.cpp in
// spirit (GPL-3.0-or-later).
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>

#include "gpu/bbnet20/thd/thd_score.h"

using BbThd::NpScore::kErrInvalidArgument;
using BbThd::NpScore::kSanitizeMaxLen;
using BbThd::NpScore::SanitizeCopy;

int main() {
    char out[kSanitizeMaxLen + 1];

    // A name comes back as it went, NUL-terminated.
    std::memset(out, 0x7f, sizeof(out));
    assert(SanitizeCopy("Hunter", out) == 0);
    assert(std::strcmp(out, "Hunter") == 0);

    // An empty name is an empty answer, still terminated.
    std::memset(out, 0x7f, sizeof(out));
    assert(SanitizeCopy("", out) == 0 && out[0] == '\0');

    // UTF-8 survives untouched.
    std::memset(out, 0x7f, sizeof(out));
    assert(SanitizeCopy("獵人", out) == 0);
    assert(std::strcmp(out, "獵人") == 0);

    // A comment of exactly the maximum fits whole.
    const std::string full(kSanitizeMaxLen, 'a');
    assert(SanitizeCopy(full.c_str(), out) == 0 && std::strcmp(out, full.c_str()) == 0);

    // A longer one is cut at the maximum, and a cut inside a UTF-8 sequence goes back to its start:
    // 254 ASCII bytes then a three-byte character that would end at byte 257.
    const std::string longer = std::string(kSanitizeMaxLen - 1, 'a') + "獵" + "tail";
    assert(SanitizeCopy(longer.c_str(), out) == 0);
    assert(std::strlen(out) == kSanitizeMaxLen - 1);
    assert(out[kSanitizeMaxLen - 1] == '\0');

    // A missing comment or buffer is the invalid-argument answer, and the output is left alone.
    std::memset(out, 0x7f, sizeof(out));
    assert(SanitizeCopy(nullptr, out) == kErrInvalidArgument);
    assert(SanitizeCopy("x", nullptr) == kErrInvalidArgument);
    assert(static_cast<unsigned char>(out[0]) == 0x7f);

    std::puts("thd score: ok");
    return 0;
}
