#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>

// LogRedaction -- masks credentials in a log line before it is written to
// talq_debug.log. Pure (no Qt, no GLib) so it can run inside the Qt message
// handler, the GLib log writer and the GStreamer log function alike.
//
// Why it exists (2026-09-16): GStreamer's webrtcnice logs "Could not set TURN
// server turn://<user>:<credential>@host:3478" at ERROR, which the default
// GST_DEBUG=2 captures. TalQ masked its own qDebug copy of that URI but not
// GStreamer's, so live TURN REST credentials reached a log that was then
// shared. Masking at each call site cannot cover third-party output, so every
// line is masked once, here, on its way to the file.
//
// What is masked:
//   scheme://USERINFO@host    -> scheme://***@host    (any scheme)
//   turn:USERINFO@host        -> turn:***@host        (turn/turns/stun/stuns)
//   scheme%3A%2F%2FUSERINFO%40host -> scheme%3A%2F%2F***%40host (either case)
//   Authorization: Basic|Bearer VALUE -> ... Basic|Bearer ***
//   Basic|Bearer VALUE (no header name) when VALUE looks like a credential
//
// What is deliberately NOT masked: an '@' after the authority (a WebDAV path
// holding a user id that is an e-mail), a bare e-mail address, and words that
// merely follow "Basic"/"Bearer" in prose ("Microsoft Basic Display Adapter").
// Masking those would cost the log the diagnostics it exists for.
//
// turn/turns/stun/stuns URIs have no path, so a '/' inside their userinfo (an
// unencoded base64 credential) does not end it. For every other scheme '/'
// ends the authority, as RFC 3986 says.
//
// Known residuals -- do not rely on this for them:
//   - A raw turn/stun userinfo containing "//" stops the scan there and is NOT
//     masked (turn://u:ab//cd==@host). Every TalQ TURN URI builder runs the
//     credential through QUrl::toPercentEncoding ('/' -> %2F), so none is
//     produced today; a new builder that skips it would reopen this.
//   - Credentials printed as separate fields, not a URI, are NOT masked, e.g.
//     libnice's "... with user/pass : U -- P". libnice logs that through
//     nice_debug (G_LOG_LEVEL_DEBUG), which GLibLogWriter.h drops unless
//     G_MESSAGES_DEBUG names libnice; TalQ never sets it.
//   - Cookie / Set-Cookie values (oc_sessionPassphrase=...), "password=" and
//     "token=" parameters and X-API-Key headers are NOT masked. TalQ's own
//     code logs none of them (grep, 2026-09-16).
//   - False positive: a standalone "Basic|Bearer <word>" is masked when the
//     word is 16+ token characters with a digit, a symbol or mixed case, so
//     "Basic H264EncoderProfile2" logs as "Basic ***". Kept that way: a real
//     base64 credential almost always has one of those, and a digit-or-symbol
//     rule would still hide that identifier.

namespace LogRedaction {

namespace detail {

inline constexpr std::string_view kMask = "***";

inline bool isAlpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
inline bool isDigit(char c) { return c >= '0' && c <= '9'; }
inline bool isSchemeChar(char c) { return isAlpha(c) || isDigit(c) || c == '+' || c == '-' || c == '.'; }
inline char lower(char c) { return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c; }

// Case-insensitive match of a lower-case literal at `pos`.
inline bool matchAt(std::string_view s, std::size_t pos, std::string_view lit)
{
    if (pos > s.size() || s.size() - pos < lit.size()) return false;
    for (std::size_t i = 0; i < lit.size(); ++i)
        if (lower(s[pos + i]) != lit[i]) return false;
    return true;
}

inline std::size_t findI(std::string_view s, std::string_view lit, std::size_t from)
{
    for (std::size_t i = from; i + lit.size() <= s.size(); ++i)
        if (matchAt(s, i, lit)) return i;
    return std::string_view::npos;
}

// Start of the URI scheme ending at `end` (exclusive), or npos. A scheme
// starts with a letter.
inline std::size_t schemeStart(std::string_view s, std::size_t end)
{
    std::size_t i = end;
    while (i > 0 && isSchemeChar(s[i - 1])) --i;
    while (i < end && !isAlpha(s[i])) ++i;
    return i < end ? i : std::string_view::npos;
}

inline bool isAuthorityOnlyScheme(std::string_view scheme)
{
    return scheme.size() >= 4 && scheme.size() <= 5
        && (matchAt(scheme, 0, "turn") || matchAt(scheme, 0, "stun"))
        && (scheme.size() == 4 || lower(scheme[4]) == 's');
}

inline bool isHardStop(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '"' || c == '\''
        || c == '<' || c == '>' || c == '`' || c == ',' || c == ';' || c == '&';
}

// No real authority is this long; the cap keeps a pathological line (thousands
// of "turn:" with no separator) from being rescanned end to end per candidate.
inline constexpr std::size_t kMaxAuthority = 2048;

// Scans the authority that starts at `start` and returns the position of its
// last '@' (or "%40" when `encoded`), or npos when it has no userinfo.
inline std::size_t userinfoEnd(std::string_view s, std::size_t start, bool authorityOnly, bool encoded)
{
    std::size_t lastAt = std::string_view::npos;
    const std::size_t limit = s.size() - start > kMaxAuthority ? start + kMaxAuthority : s.size();
    for (std::size_t j = start; j < limit; ++j) {
        const char c = s[j];
        if (isHardStop(c) || c == '?' || c == '#') break;
        if (c == '/' && (!authorityOnly || (j + 1 < s.size() && s[j + 1] == '/'))) break;
        if (matchAt(s, j, "://")) break;
        if (encoded) {
            if (matchAt(s, j, "%3a%2f%2f") || matchAt(s, j, "%3f") || matchAt(s, j, "%23")) break;
            if (!authorityOnly && matchAt(s, j, "%2f")) break;
            if (matchAt(s, j, "%40")) { lastAt = j; continue; }
        }
        if (c == '@') lastAt = j;
    }
    return lastAt;
}

// Copies what precedes `from`, then the mask in place of [from, to).
inline void maskRange(std::string &out, std::string_view s, std::size_t &copied,
                      std::size_t from, std::size_t to)
{
    out.append(s.substr(copied, from - copied));
    out.append(kMask);
    copied = to;
}

// "turn:" / "turns:" / "stun:" / "stuns:" as a word, NOT followed by "//"
// (that form is handled as a plain URI).
inline std::size_t findBareTurnScheme(std::string_view s, std::size_t from)
{
    for (std::size_t i = from; i + 5 <= s.size(); ++i) {
        if (!matchAt(s, i, "turn") && !matchAt(s, i, "stun")) continue;
        if (i > 0 && isSchemeChar(s[i - 1])) continue;
        std::size_t colon = i + 4;
        if (colon < s.size() && lower(s[colon]) == 's') ++colon;
        if (colon < s.size() && s[colon] == ':' && !matchAt(s, colon, "://")) return i;
    }
    return std::string_view::npos;
}

inline std::string maskUriUserinfo(std::string_view s)
{
    constexpr std::size_t npos = std::string_view::npos;
    std::string out;
    out.reserve(s.size());
    std::size_t copied = 0;
    // Next candidate of each URI shape, re-searched only once `pos` passes it,
    // so a long line with many URIs stays linear.
    std::size_t plain = s.find("://");
    std::size_t enc = findI(s, "%3a%2f%2f", 0);
    std::size_t bare = findBareTurnScheme(s, 0);
    // Next '@', "%40" and hard stop, each re-searched only when behind: a
    // candidate whose authority hits a hard stop before any '@' has no
    // userinfo and is skipped without a scan.
    constexpr const char *kHardStops = " \t\r\n\"'<>`,;&";
    std::size_t rawAt = s.find('@');
    std::size_t encAt = findI(s, "%40", 0);
    std::size_t stop = s.find_first_of(kHardStops);
    std::size_t pos = 0;
    for (;;) {
        if (plain != npos && plain < pos) plain = s.find("://", pos);
        if (enc != npos && enc < pos) enc = findI(s, "%3a%2f%2f", pos);
        if (bare != npos && bare < pos) bare = findBareTurnScheme(s, pos);
        const std::size_t next = std::min({plain, enc, bare});
        if (next == npos) break;

        std::size_t start = 0;
        if (next == bare) {
            std::size_t colon = bare + 4;
            if (lower(s[colon]) == 's') ++colon;
            start = colon + 1;
        } else {
            start = next + (next == enc ? 9 : 3);
        }
        if (rawAt != npos && rawAt < start) rawAt = s.find('@', start);
        if (encAt != npos && encAt < start) encAt = findI(s, "%40", start);
        if (stop != npos && stop < start) stop = s.find_first_of(kHardStops, start);
        const std::size_t at = std::min(rawAt, encAt);
        if (at == npos || at - start > kMaxAuthority || (stop != npos && stop < at)) {
            pos = next + 1;
            continue;
        }

        std::size_t end = npos;
        if (next == bare) {
            end = userinfoEnd(s, start, /*authorityOnly=*/true, /*encoded=*/false);
        } else {
            const std::size_t ss = schemeStart(s, next);
            if (ss != npos)
                end = userinfoEnd(s, start, isAuthorityOnlyScheme(s.substr(ss, next - ss)), next == enc);
        }
        if (end != npos && end > start) {
            if (s.substr(start, end - start) != kMask)
                maskRange(out, s, copied, start, end);
            pos = end + 1;
        } else {
            pos = next + 1;
        }
    }
    out.append(s.substr(copied));
    return out;
}

inline bool isToken68Char(char c)
{
    return isAlpha(c) || isDigit(c) || c == '-' || c == '.' || c == '_' || c == '~' || c == '+' || c == '/';
}

inline std::string maskAuthSchemes(std::string_view s)
{
    std::string out;
    out.reserve(s.size());
    std::size_t copied = 0;
    std::size_t pos = 0;
    std::size_t b = findI(s, "basic", 0);
    std::size_t r = findI(s, "bearer", 0);
    for (;;) {
        if (b != std::string_view::npos && b < pos) b = findI(s, "basic", pos);
        if (r != std::string_view::npos && r < pos) r = findI(s, "bearer", pos);
        const std::size_t w = b < r ? b : r;
        if (w == std::string_view::npos) break;
        const std::size_t wordEnd = w + (w == b ? 5 : 6);
        pos = wordEnd;
        if (w > 0 && (isAlpha(s[w - 1]) || isDigit(s[w - 1]))) continue;
        std::size_t t = wordEnd;
        while (t < s.size() && (s[t] == ' ' || s[t] == '\t')) ++t;
        if (t == wordEnd) continue;                 // "Basically", "Basic:"
        std::size_t e = t;
        bool digit = false, symbol = false, upper = false, low = false;
        while (e < s.size() && isToken68Char(s[e])) {
            const char c = s[e++];
            digit |= isDigit(c);
            upper |= (c >= 'A' && c <= 'Z');
            low |= (c >= 'a' && c <= 'z');
            symbol |= !isAlpha(c) && !isDigit(c);
        }
        while (e < s.size() && s[e] == '=') { ++e; symbol = true; }
        if (e == t) continue;
        const std::size_t ctxFrom = w > 32 ? w - 32 : 0;
        const bool headerNamed = findI(s.substr(ctxFrom, w - ctxFrom), "authorization", 0) != std::string_view::npos;
        const bool looksLikeCredential = (e - t) >= 16 && (digit || symbol || (upper && low));
        if (headerNamed || looksLikeCredential) {
            maskRange(out, s, copied, t, e);
            pos = e;
        }
    }
    out.append(s.substr(copied));
    return out;
}

} // namespace detail

// Returns `line` with credentials masked. Lines without a candidate marker are
// returned unchanged after a cheap scan.
inline std::string redact(std::string_view line)
{
    std::string out;
    const bool uriCandidate = line.find('@') != std::string_view::npos
                           || detail::findI(line, "%40", 0) != std::string_view::npos;
    if (uriCandidate)
        out = detail::maskUriUserinfo(line);
    else
        out.assign(line);
    if (detail::findI(out, "basic", 0) != std::string_view::npos
        || detail::findI(out, "bearer", 0) != std::string_view::npos)
        out = detail::maskAuthSchemes(out);
    return out;
}

} // namespace LogRedaction
