// SPDX-License-Identifier: GPL-3.0-or-later
// sceNpScoreSanitizeComment's answer, apart so a test can hold it to its contract. Taken from
// droogie/bbhost src/hle/np_score.h (v0.2.31, PR #238), GPL-3.0-or-later.
//
// Playing online, the game puts its own character's name through it before the name goes to the
// other players, who show it under System > Network > Player Name Display "Name". PSN gave the text
// back with the words its filter matched starred out; the private server has no filter, so the text
// comes back as it went. Writing nothing (this module answered success with the buffer untouched)
// sent every name empty.
#pragma once

#include <cstddef>
#include <cstring>

namespace BbThd::NpScore {

// SCE_NP_SCORE_SANITIZE_COMMENT_MAXLEN: the output holds this many bytes and a NUL.
constexpr std::size_t kSanitizeMaxLen = 255;

// SCE_NP_COMMUNITY_ERROR_INVALID_ARGUMENT, the score library's answer to a missing comment or output
// buffer.
constexpr int kErrInvalidArgument = static_cast<int>(0x80550704u);

// Copies `comment` into `out` (kSanitizeMaxLen + 1 bytes) as a server with no word filter answers: the
// same text, cut at kSanitizeMaxLen bytes (back to the start of a UTF-8 sequence, so a cut never
// leaves half a character) and NUL-terminated. Returns 0, or kErrInvalidArgument for a null pointer
// (out untouched).
inline int SanitizeCopy(const char* comment, char* out) {
    if (!comment || !out) return kErrInvalidArgument;
    std::size_t n = 0;
    while (n < kSanitizeMaxLen && comment[n]) ++n;
    if (n == kSanitizeMaxLen && comment[n]) {
        // Cut inside the text: drop the continuation bytes (10xxxxxx) at the cut and the lead byte
        // they belong to.
        std::size_t k = n;
        while (k > 0 && (static_cast<unsigned char>(comment[k]) & 0xc0) == 0x80) --k;
        n = k;
    }
    std::memcpy(out, comment, n);
    out[n] = '\0';
    return 0;
}

} // namespace BbThd::NpScore
