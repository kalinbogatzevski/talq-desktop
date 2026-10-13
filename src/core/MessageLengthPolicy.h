#pragma once
// Whether a composer message is too long for the server, decided BEFORE it is sent.
//
// Talk 24 refuses a chat message longer than `MAX_CHAT_LENGTH` (32000) with HTTP 413, and
// advertises the number as `capabilities.spreed.config.chat.max-length`. Without a client check
// the composer cleared the box the moment Send was pressed, the send then failed as a
// permanent refusal (SendRetryPolicy), and "Discard" on the failed bubble threw away the only
// copy of what could have been a long, carefully written message.
//
// The unit is CODE POINTS, not bytes and not UTF-16 units: the server counts with
// mb_strlen on UTF-8 (ScheduledMessage.php:91; ChatController's message check reads the same
// constant), so an emoji outside the BMP is ONE character there but TWO in QString::length().
// Counting bytes would refuse a perfectly legal message written in Cyrillic.
//
// A limit that is not known (an old server that does not advertise it, or no capabilities
// fetched yet) is 0 or less and NEVER blocks: the guard exists to protect the user's text, not
// to second-guess a server it cannot read. The server stays the authority; a refusal there still
// surfaces as a failed row.
//
// Pure standard C++ (no Qt) so tests/message_length_policy_test.cpp covers it in milliseconds.

#include <cstddef>
#include <string>

namespace talq::msglen {

// Number of Unicode code points in a UTF-8 string: every byte that is not a continuation byte
// (10xxxxxx) starts one. Well-formed input is what Qt's toUtf8() produces; a stray continuation
// byte is simply not counted, which can only under-count, never block a legal message.
inline std::size_t codePoints(const std::string &utf8)
{
    std::size_t n = 0;
    for (const unsigned char b : utf8)
        if ((b & 0xC0) != 0x80) ++n;
    return n;
}

struct Check {
    bool        tooLong = false;
    std::size_t count   = 0;   // code points in the text
    std::size_t limit   = 0;   // the limit applied (0 = unknown)
    std::size_t excess  = 0;   // how many code points to remove; 0 when not too long
};

// `limit` <= 0 means "unknown": never too long.
inline Check check(const std::string &utf8, long long limit)
{
    Check c;
    c.count = codePoints(utf8);
    if (limit <= 0) return c;
    c.limit = static_cast<std::size_t>(limit);
    if (c.count > c.limit) {
        c.tooLong = true;
        c.excess  = c.count - c.limit;
    }
    return c;
}

// Cheap pre-test for a UI that re-checks on every keystroke: a text of `utf16Units` UTF-16 code
// units has AT MOST that many code points, so when even the unit count fits, the exact count
// cannot exceed the limit and the whole string need not be converted and scanned.
inline bool mayExceed(std::size_t utf16Units, long long limit)
{
    return limit > 0 && utf16Units > static_cast<std::size_t>(limit);
}

// ---- a caption sent with a file --------------------------------------------------------------
// A caption is NOT counted like a message. Talk stores a file share as a system message whose body
// is json_encode(['message' => 'file_shared', 'parameters' => ['share' => id, 'metaData' =>
// [caption, mimeType, ...]]]) with PHP's default flags, and it is THAT string that is held to the
// 32000 (lib/Chat/ChatManager.php: addSystemMessage -> setMessage($message, MAX_CHAT_LENGTH)).
// json_encode escapes every non-ASCII character as \uXXXX (6 characters; 12 for one outside the
// BMP, a surrogate pair), and '"', '\\', '/' and the control characters as two. So a Cyrillic
// caption of ~5300 letters is already too long, although it is nowhere near 32000 code points.

// Characters json_encode() spends on `utf8` inside a JSON string.
inline std::size_t phpJsonStringLength(const std::string &utf8)
{
    std::size_t n = 0;
    for (const unsigned char b : utf8) {
        if (b < 0x80) {
            if (b == '"' || b == '\\' || b == '/' || b == '\b' || b == '\f' || b == '\n' || b == '\r' || b == '\t')
                n += 2;
            else if (b < 0x20)
                n += 6;          // \u00XX
            else
                n += 1;          // includes 0x7F, which PHP does not escape
        } else if ((b & 0xE0) == 0xC0 || (b & 0xF0) == 0xE0) {
            n += 6;              // lead byte of a 2- or 3-byte sequence: one BMP character, \uXXXX
        } else if ((b & 0xF8) == 0xF0) {
            n += 12;             // lead byte of a 4-byte sequence: a surrogate pair, \uXXXX\uXXXX
        }                        // continuation bytes were paid for by their lead byte
    }
    return n;
}

// What the rest of the file message's JSON costs, kept back from the limit: the key names, the
// share id, the mime type (up to ~75 characters for an Office document), and, for a voice note or
// a silent send, the extra metaData keys. Measured at ~170; rounded up so a caption that passes
// never lands on the edge.
constexpr std::size_t kFileShareWrapperReserve = 250;

// Like check(), for the caption of a staged file. `count` is the caption's encoded length and
// `limit` the budget left once the wrapper is paid for. A limit too small to leave any budget is
// treated as unknown, which never blocks.
inline Check checkCaption(const std::string &utf8, long long limit)
{
    Check c;
    c.count = phpJsonStringLength(utf8);
    if (limit <= static_cast<long long>(kFileShareWrapperReserve)) return c;
    c.limit = static_cast<std::size_t>(limit) - kFileShareWrapperReserve;
    if (c.count > c.limit) {
        c.tooLong = true;
        c.excess  = c.count - c.limit;
    }
    return c;
}

// Per-keystroke pre-test for captions: every UTF-16 unit costs at most 6 encoded characters (a
// character outside the BMP is two units for 12), so when even that worst case fits, the exact
// measure cannot exceed the budget.
inline bool mayExceedCaption(std::size_t utf16Units, long long limit)
{
    if (limit <= static_cast<long long>(kFileShareWrapperReserve)) return false;
    return utf16Units * 6 > static_cast<std::size_t>(limit) - kFileShareWrapperReserve;
}

} // namespace talq::msglen
