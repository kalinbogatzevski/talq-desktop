#pragma once
// Pure, Qt-free construction of a forwarded message body.
//
// Talk has NO forward endpoint and no forward concept at all — verified against
// spreed 24.0.4 on 2026-08-26: `lib/Controller/ChatController.php` contains not
// one occurrence of "forward", and `sendMessage()` takes only
//   message, actorDisplayName, referenceId, replyTo, replyToToken,
//   silent, threadTitle, threadId
// so there is no field in which to record that something was forwarded, nor
// from whom. Forwarding is therefore *posting a new message*, and any
// attribution has to be part of the body or it does not exist.
//
// `replyToToken` looks like it might carry the original as a parent, but it is
// the "reply privately" feature: resolveReplyTo() (ChatController.php:158-198)
// rejects it unless the TARGET room is the one-to-one between exactly the
// sender and the parent's author, and refuses replies to one's own messages.
// It cannot express a forward.
//
// WHY THIS EXISTS AS A SEPARATE UNIT: forwarding used to send
// QTextDocument::toPlainText() of the ALREADY-RENDERED html, i.e. the body went
// markup -> html -> plain text and arrived stripped of every bit of formatting.
// The rule below is one line of intent -- "send the server's own bytes back" --
// and it is the kind of thing that silently regresses the moment someone
// reaches for the convenient already-formatted string sitting next to it.
//
// UPSTREAM REFERENCE, read out of the shipped web client
// (js/talk-main.js, store action `forwardMessage`):
//   - posts the RAW `message` string verbatim, markdown and all;
//   - rewrites {mention-*} placeholders back into the source syntax so the
//     server re-parses them: `@"<id>"`, or `**<name>**` for mention-call;
//     (a raw "{mention-user1}" would otherwise arrive as literal text)
//   - routes messages carrying a rich `object` parameter through the
//     share-object endpoint instead of sending text;
//   - clears thread/parent details and forces silent=false;
//   - adds NO attribution of any kind.
// We match it exactly, and add the attribution line upstream does not have.

#include <string>
#include <vector>

namespace talq {

// One {mention-*} entry of the server's `messageParameters`. Only the fields a
// forward needs; everything else on the parameter is irrelevant here.
struct MentionParam {
    std::string key;    // the placeholder key, e.g. "mention-user1"
    std::string id;     // param["id"]   — what the server re-parses
    std::string name;   // param["name"] — display name, used for mention-call
};

// True for the call-wide mention, which has no id the server can resolve and so
// is carried across as bold text rather than as a mention.
inline bool isCallMention(const std::string &key)
{
    return key.find("mention-call") != std::string::npos;
}

inline std::string replaceAll(std::string subject, const std::string &from,
                              const std::string &to)
{
    if (from.empty()) return subject;
    for (std::string::size_type at = subject.find(from);
         at != std::string::npos;
         at = subject.find(from, at + to.size())) {
        subject.replace(at, from.size(), to);
    }
    return subject;
}

// Put {mention-*} placeholders back into the form the server parses on the way
// in. Non-mention parameters (file, talk-poll, and any rich object) are left
// untouched on purpose: those are not text and the caller re-shares them
// through their own endpoint rather than describing them in prose.
inline std::string restoreMentions(std::string raw,
                                   const std::vector<MentionParam> &mentions)
{
    for (const MentionParam &m : mentions) {
        const std::string placeholder = "{" + m.key + "}";
        const std::string replacement =
            isCallMention(m.key) ? "**" + m.name + "**"
                                 : "@\"" + m.id + "\"";
        raw = replaceAll(raw, placeholder, replacement);
    }
    return raw;
}

// True when the raw text still points at a rich object this layer cannot turn
// back into prose -- a poll, a deck card, a shared file, a geo-location.
//
// Those are not text. Upstream re-shares them through the share-object endpoint
// instead of describing them, and sending the raw placeholder would deliver a
// literal "{object}" to the recipient. The caller uses this to keep such
// messages on whatever path already handles them rather than forwarding a
// string of braces -- which is a worse bug than the one being fixed, so it is
// checked rather than assumed.
inline bool carriesRichObject(const std::string &raw,
                              const std::vector<std::string> &nonMentionKeys)
{
    for (const std::string &key : nonMentionKeys) {
        if (raw.find("{" + key + "}") != std::string::npos)
            return true;
    }
    return false;
}

// Strip the characters that would turn the attribution line into markup.
//
// The line is emitted as *Forwarded from NAME*, so an asterisk or underscore in
// a display name does not merely look wrong -- it closes the emphasis early and
// the leftover marker runs on into the forwarded body, corrupting content we
// promised to reproduce byte for byte. Backslash-escaping is not a fix: it
// relies on the reader supporting escapes, and a client that does not shows the
// backslash instead. Dropping the character cannot corrupt anything, and a
// display name containing markdown punctuation is not meaningfully altered by
// losing it.
inline std::string sanitizeAuthor(const std::string &name)
{
    std::string out;
    out.reserve(name.size());
    for (char c : name) {
        if (c == '*' || c == '_' || c == '`' || c == '~' || c == '\\'
            || c == '\r' || c == '\n')
            continue;
        out.push_back(c);
    }
    // A name that was nothing but markup leaves nothing to attribute to.
    std::string::size_type first = out.find_first_not_of(" \t");
    if (first == std::string::npos) return std::string();
    std::string::size_type last = out.find_last_not_of(" \t");
    return out.substr(first, last - first + 1);
}

// The exact body to POST when forwarding `rawMessage`.
//
// The forwarded content is reproduced VERBATIM below the attribution line --
// no re-wrapping, no escaping, no normalisation. The only transformation is the
// mention rewrite, which is not a change of content: it restores the source
// text the server itself rendered the placeholders from.
inline std::string forwardBody(const std::string &rawMessage,
                               const std::vector<MentionParam> &mentions,
                               const std::string &authorDisplayName)
{
    const std::string body = restoreMentions(rawMessage, mentions);
    if (body.empty()) return std::string();     // nothing to forward

    const std::string author = sanitizeAuthor(authorDisplayName);
    if (author.empty()) return body;            // no one to attribute it to

    // Blank line between: without it a body starting with a list or a heading
    // is absorbed into the attribution paragraph and renders wrong.
    return "*Forwarded from " + author + "*\n\n" + body;
}

} // namespace talq
