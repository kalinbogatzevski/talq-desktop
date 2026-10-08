#pragma once

#include <algorithm>

// PublisherSidPolicy -- which answers / candidates for OUR OWN camera publisher
// are stale and must not be applied to the pipeline that replaced theirs.
//
// A publisher rebuild (recoverPublisher -> buildAndStartPublisher) mints a FRESH
// sid and builds a new PublishPipeline. An answer or candidate the server sent
// for the OLD offer can still be in flight; it carries the old sid (the HPB
// echoes the publisher's sid on both, hub.go sendMcuMessageResponse). Applied to
// the new pipeline it is a remote description / candidate for a different
// negotiation.
//
// Dropping it is only SAFE on a server that tags messages with the sid of the
// offer it is answering. A stock HPB does not: GetOrCreatePublisher reuses the
// existing publisher for a new offer and keeps answering with the ORIGINAL sid
// (nextcloud-spreed-signaling clientsession.go), so after a rebuild the answer to
// the NEW offer carries a sid this client has just retired. Dropping that would
// leave the rebuilt publisher without a remote description. A message with a
// retired sid is therefore ignored only once the server has PROVEN it tags with
// our current sid for this build (one answer or own-session candidate carried the
// current sid). Until then everything is applied, exactly as before this guard
// existed; the proof is reset on every new mint.
//
// WHAT THIS DOES NOT COVER: a stale answer that arrives BEFORE the first message
// carrying the new sid is still applied (it cannot be told from the reuse case),
// and nothing here closes the old publisher on the server.
//
// Pure logic, templated so it needs neither Qt nor a string type:
// tests/publisher_sid_policy_test.cpp uses std::string, CallManager uses QString.
// `List` is a sequence of Str with push_back / erase / begin / end / size
// (QStringList, std::vector<std::string>).

namespace talq {

template <class Str, class List>
class PublisherSidGuard {
public:
    explicit PublisherSidGuard(int maxKept = 8) : m_maxKept(maxKept) {}

    // A camera publisher was (re)built with `sid`. The previous sid, if any and
    // different, is now replaced. Nothing is proven yet about how the server tags
    // messages for the new one.
    void onMinted(const Str &sid)
    {
        if (!(m_current == Str()) && !(m_current == sid)) retire(m_current);
        m_current = sid;
        m_echoed = false;
    }

    // An answer or own-session candidate arrived carrying `sid` ("" = none).
    // Returns true when it should be applied, false when it is stale. Seeing the
    // CURRENT sid is the proof that this server tags messages with our sids.
    bool accept(const Str &sid)
    {
        if (sid == Str()) return true;                    // no sid on the message: apply
        if (sid == m_current) { m_echoed = true; return true; }
        if (!m_echoed) return true;                       // server not proven to tag by our sid
        return std::find(m_retired.begin(), m_retired.end(), sid) == m_retired.end();
    }

    // Per-call state: forget everything (the pipeline is gone / a new call starts).
    void reset()
    {
        m_current = Str();
        m_echoed = false;
        m_retired.clear();
    }

    const Str &current() const { return m_current; }
    bool echoedCurrent() const { return m_echoed; }
    int  retiredCount() const { return int(m_retired.size()); }

private:
    // Bounded so a pathological flap cannot grow it; the oldest entries fall off (a
    // message for a sid that old is long gone anyway).
    void retire(const Str &sid)
    {
        if (std::find(m_retired.begin(), m_retired.end(), sid) != m_retired.end()) return;
        m_retired.push_back(sid);
        while (int(m_retired.size()) > m_maxKept) m_retired.erase(m_retired.begin());
    }

    int  m_maxKept;
    Str  m_current = Str();
    bool m_echoed = false;
    List m_retired;
};

} // namespace talq
