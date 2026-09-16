#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <utility>
#include <vector>

// SubscribeOfferPolicy -- pure decisions for the SUBSCRIBER side of an MCU call:
// parked offers, remote-candidate routing, the requestoffer gate, and sessions
// that have left the room. No Qt, no GStreamer: the string type is a template
// parameter (std::string in tests/subscribe_offer_policy_test.cpp, QString in
// CallManager), and it needs only ==, copy and size() (QString has no empty()).
//
// Field incident 2026-09-16 (a group call, full RCA "D3/D6"):
//  - The call-join POST hung server-side AFTER the join had committed. STUN/TURN
//    were fetched only in the POST's success callback, so every subscriber offer
//    was parked; parking returned before the requestoffer bookkeeping was cleared,
//    so the 8 s retry kept firing. Each requestoffer without a sid makes the MCU
//    tear the subscriber down and re-join on a NEW Janus handle, which sends a new
//    offer. 8 rounds x 2 peers = 16 parked offers, replayed in ~110 ms (16
//    webrtcbins, 14 destroyed at once) -- the burst that set off the fatal GLib
//    log race.
//  - Remote candidates were queued per PEER. The replay flushed all 16 (from 8
//    different handles) into the first, immediately destroyed subscriber, and the
//    two survivors sat in ICE "new" until the frame-stall watchdog rebuilt them.
//  - one client re-requested an offer from a peer's crashed session 17 times:
//    room/leave dropped the subscriber, but the subscribe-reconcile sweep still
//    held the participant and re-requested it one second later.
//
// Wire facts this relies on (nextcloud-spreed-signaling server/clientsession.go
// sendCandidate and server/hub.go sendMcuMessageResponse): both the subscriber
// OFFER and every trickled CANDIDATE carry "sid" = the MCU client's Sid(), which
// for Janus is the handle id and changes on every re-join. The proxy rejects a
// client candidate whose sid is not the subscriber's current one (proxy log
// 08:37:14Z "candidate message sid ... does not match subscriber sid").

namespace talq {

// Empty test that works for std::string AND QString (which has no empty()).
template <class Str>
inline bool noStr(const Str &s) { return s.size() == 0; }

// ── (a) Parked offers ────────────────────────────────────────────────────────
// Offers that arrive before ICE servers are usable. At most ONE per
// (peer, roomType): a newer offer for the same slot means the MCU already
// replaced the older handle, so replaying the older one only builds a pipeline
// that is torn down a few milliseconds later.
template <class Str>
struct ParkedOffer {
    Str peer;
    Str roomType;
    Str sid;
    Str sdp;
};

template <class Str>
class ParkedOfferQueue {
public:
    // Park `o`, replacing any older offer for the same (peer, roomType). Returns
    // the sid of the replaced offer ("" when nothing was replaced) so the caller
    // can retire that handle's buffered candidates.
    Str park(ParkedOffer<Str> o)
    {
        Str replaced;
        for (auto it = m_items.begin(); it != m_items.end(); ++it) {
            if (it->peer == o.peer && it->roomType == o.roomType) {
                replaced = it->sid;
                m_items.erase(it);
                break;
            }
        }
        m_items.push_back(std::move(o));   // arrival order of each slot's newest
        return replaced;
    }

    bool contains(const Str &peer, const Str &roomType) const
    {
        return std::any_of(m_items.begin(), m_items.end(), [&](const ParkedOffer<Str> &p) {
            return p.peer == peer && p.roomType == roomType;
        });
    }

    // Remove every parked offer of `peer`; returns them (their sids are dead).
    std::vector<ParkedOffer<Str>> dropPeer(const Str &peer)
    {
        std::vector<ParkedOffer<Str>> out;
        for (auto it = m_items.begin(); it != m_items.end();) {
            if (it->peer == peer) { out.push_back(std::move(*it)); it = m_items.erase(it); }
            else ++it;
        }
        return out;
    }

    // Take everything, in arrival order, leaving the queue empty.
    std::vector<ParkedOffer<Str>> drain()
    {
        std::vector<ParkedOffer<Str>> out;
        out.swap(m_items);
        return out;
    }

    std::size_t size() const { return m_items.size(); }
    bool empty() const { return m_items.empty(); }
    void clear() { m_items.clear(); }

private:
    std::vector<ParkedOffer<Str>> m_items;
};

// ── (d) Remote candidate routing, keyed by (peer, MCU sid) ──────────────────
// Deliver : a live subscriber exists and the candidate is for its sid (or the
//           message carries no sid -- P2P / older servers keep the per-peer rule).
// Buffer  : no subscriber for that sid yet. The MCU trickles a handle's
//           candidates just BEFORE its offer, and a parked offer's candidates
//           must survive until the replay builds it.
// Drop    : the sid was superseded (its subscriber was replaced by another
//           build, or its offer was replaced while parked or was data-only).
//           Feeding it anywhere is wrong.
template <class Str, class Cand>
class SubscriberCandidateRouter {
public:
    enum class Route { Deliver, Buffer, Drop };

    static constexpr std::size_t kMaxBufferedPerSid      = 16;  // ~1-2 offers' worth
    static constexpr std::size_t kMaxBufferedSidsPerPeer = 8;
    static constexpr std::size_t kMaxRetiredPerPeer      = 32;

    Route onCandidate(const Str &peer, const Str &sid, const Cand &c, bool subscriberExists)
    {
        PeerState &st = stateFor(peer);
        const bool legacy = noStr(sid);
        if (subscriberExists && (legacy || (st.hasCurrent && sid == st.current)))
            return Route::Deliver;
        if (!legacy && isRetired(st, sid))
            return Route::Drop;
        Buffer &b = bufferFor(st, sid);
        b.cands.push_back(c);
        while (b.cands.size() > kMaxBufferedPerSid) b.cands.pop_front();
        return Route::Buffer;
    }

    // A subscriber for (peer, sid) was just built. `sid` becomes current and the
    // previous current sid is retired. Other buffered sids are KEPT: Janus handle
    // ids are random, not ordered (spreed-signaling sfu/janus/subscriber.go), so
    // a merely buffered handle may be the newer one whose offer is still on the
    // way. Handles known to be superseded are retired explicitly by the caller
    // (parked offer replaced, data-only offer); the rest stay bounded by the
    // per-peer caps, forgetPeer and clear. Returns the candidates to feed the
    // new subscriber: sid-less legacy ones first, then that sid's, in arrival order.
    std::vector<Cand> onSubscriberBuilt(const Str &peer, const Str &sid)
    {
        PeerState &st = stateFor(peer);
        if (st.hasCurrent && !(st.current == sid) && !noStr(st.current))
            retireInto(st, st.current);
        unretire(st, sid);
        std::vector<Cand> out;
        for (const Buffer &b : st.buffers)
            if (noStr(b.sid))
                out.insert(out.end(), b.cands.begin(), b.cands.end());
        if (!noStr(sid))
            for (const Buffer &b : st.buffers)
                if (b.sid == sid)
                    out.insert(out.end(), b.cands.begin(), b.cands.end());
        st.buffers.erase(std::remove_if(st.buffers.begin(), st.buffers.end(),
                                        [&](const Buffer &b) { return noStr(b.sid) || b.sid == sid; }),
                         st.buffers.end());
        // A handle retired as the previous current may still hold a buffer.
        st.buffers.erase(std::remove_if(st.buffers.begin(), st.buffers.end(),
                                        [&](const Buffer &b) { return isRetired(st, b.sid); }),
                         st.buffers.end());
        st.current = sid;
        st.hasCurrent = true;
        return out;
    }

    // The offer for (peer, sid) was superseded before any subscriber was built
    // for it (replaced while parked, or a data-only offer that is discarded).
    void retire(const Str &peer, const Str &sid)
    {
        if (noStr(sid)) return;
        PeerState &st = stateFor(peer);
        retireInto(st, sid);
        st.buffers.erase(std::remove_if(st.buffers.begin(), st.buffers.end(),
                                        [&](const Buffer &b) { return b.sid == sid; }),
                         st.buffers.end());
    }

    // The peer's signaling session is gone (room leave, left the call, or OUR
    // session was reset so every handle under it is dead).
    void forgetPeer(const Str &peer)
    {
        m_peers.erase(std::remove_if(m_peers.begin(), m_peers.end(),
                                     [&](const PeerState &p) { return p.peer == peer; }),
                      m_peers.end());
    }

    void clear() { m_peers.clear(); }

    std::size_t bufferedCount(const Str &peer, const Str &sid) const
    {
        for (const PeerState &p : m_peers)
            if (p.peer == peer)
                for (const Buffer &b : p.buffers)
                    if (b.sid == sid) return b.cands.size();
        return 0;
    }
    std::size_t bufferedSidCount(const Str &peer) const
    {
        for (const PeerState &p : m_peers)
            if (p.peer == peer) return p.buffers.size();
        return 0;
    }

private:
    struct Buffer { Str sid; std::deque<Cand> cands; };
    struct PeerState {
        Str peer;
        Str current;
        bool hasCurrent = false;
        std::vector<Buffer> buffers;   // insertion order = age
        std::deque<Str> retired;
    };

    PeerState &stateFor(const Str &peer)
    {
        for (PeerState &p : m_peers)
            if (p.peer == peer) return p;
        m_peers.push_back(PeerState{});
        m_peers.back().peer = peer;
        return m_peers.back();
    }
    Buffer &bufferFor(PeerState &st, const Str &sid)
    {
        for (Buffer &b : st.buffers)
            if (b.sid == sid) return b;
        if (st.buffers.size() >= kMaxBufferedSidsPerPeer) {
            // Evict the buffer that started first. Handle ids carry no order, so
            // this is only a bound: that many live handles at once for one peer
            // means the MCU re-created it repeatedly, and the first-trickled one
            // is the likeliest to be dead.
            if (!noStr(st.buffers.front().sid)) retireInto(st, st.buffers.front().sid);
            st.buffers.erase(st.buffers.begin());
        }
        st.buffers.push_back(Buffer{});
        st.buffers.back().sid = sid;
        return st.buffers.back();
    }
    static bool isRetired(const PeerState &st, const Str &sid)
    {
        return std::find(st.retired.begin(), st.retired.end(), sid) != st.retired.end();
    }
    static void retireInto(PeerState &st, const Str &sid)
    {
        if (noStr(sid) || isRetired(st, sid)) return;
        st.retired.push_back(sid);
        while (st.retired.size() > kMaxRetiredPerPeer) st.retired.pop_front();
    }
    static void unretire(PeerState &st, const Str &sid)
    {
        st.retired.erase(std::remove(st.retired.begin(), st.retired.end(), sid), st.retired.end());
    }

    std::vector<PeerState> m_peers;
};

// ── (c) requestoffer gate: is OUR OWN session in the call yet? ──────────────
// The HPB rejects a requestoffer with not_allowed while the SENDER is not in the
// call (server/hub.go isInSameCall), so a request sent before our own join has
// committed is guaranteed to fail and used to count toward the not_allowed ->
// rebuild escalation. Confirmation is whichever comes first: REST 200 on
// POST call/{token}, or the HPB participants update listing our own session
// in the call.
class OwnJoinGate {
public:
    // After a REST-only confirmation the HPB may apply the in-call state a beat
    // later (the backend notification can land on another cluster member), so a
    // rejection inside this window is not evidence against the peer.
    static constexpr int64_t kHpbPropagationGraceMs = 8000;

    // Each returns true on the confirming EDGE (first evidence of either kind).
    bool noteRestConfirmed(int64_t nowMs)
    {
        const bool edge = !m_confirmed;
        m_confirmed = true;
        if (m_reconfirmPending) {
            // The session-reset re-register landed: grace runs from now.
            m_reconfirmPending = false;
            m_rest = true;
            m_confirmedAtMs = nowMs;
            return edge;
        }
        if (!m_rest) { m_rest = true; if (edge) m_confirmedAtMs = nowMs; }
        return edge;
    }
    bool noteHpbSelfInCall(int64_t nowMs)
    {
        const bool edge = !m_confirmed;
        m_confirmed = true;
        m_reconfirmPending = false;
        if (!m_hpb) { m_hpb = true; if (edge) m_confirmedAtMs = nowMs; }
        return edge;
    }

    // Our signaling session was replaced mid-call. The join stays confirmed (the
    // call is up and re-subscribes must go out), but the HPB evidence belonged
    // to the dead session and the new one is not in the call until the
    // re-register POST lands or the HPB lists it -- rejections meanwhile do not
    // count. Before any confirmation there is nothing to re-confirm.
    void noteSessionReset()
    {
        if (!m_confirmed) return;
        m_hpb = false;
        m_reconfirmPending = true;
    }

    bool confirmed() const { return m_confirmed; }
    bool hpbSawSelfInCall() const { return m_hpb; }

    // Should a not_allowed requestoffer rejection count toward escalation?
    bool countsRejection(int64_t nowMs) const
    {
        if (!m_confirmed || m_reconfirmPending) return false;
        if (m_hpb) return true;
        return nowMs - m_confirmedAtMs >= kHpbPropagationGraceMs;
    }

    void reset()
    {
        m_confirmed = false; m_rest = false; m_hpb = false;
        m_reconfirmPending = false; m_confirmedAtMs = 0;
    }

private:
    bool m_confirmed = false;        // latched: the join was proven (either kind)
    bool m_rest = false;             // REST evidence (POST 200 / reconcile / re-register)
    bool m_hpb = false;              // the HPB listed our CURRENT session in the call
    bool m_reconfirmPending = false; // session reset: waiting for the new session
    int64_t m_confirmedAtMs = 0;
};

// ── (F10) sessions that left the room ───────────────────────────────────────
// HPB session ids are per signaling session: once one leaves the room it cannot
// be in the call, so every requestoffer to it is rejected (after a 25 s gRPC
// wait when its POP is unreachable). It becomes requestable again only if the
// same sid re-enters the room (room join event or a participants update).
template <class Str>
class DepartedSessions {
public:
    static constexpr std::size_t kMaxTracked = 64;

    void noteLeft(const Str &sid)
    {
        if (noStr(sid) || !mayRequest(sid)) return;
        m_sids.push_back(sid);
        while (m_sids.size() > kMaxTracked) m_sids.pop_front();
    }
    void noteJoined(const Str &sid)
    {
        m_sids.erase(std::remove(m_sids.begin(), m_sids.end(), sid), m_sids.end());
    }
    bool mayRequest(const Str &sid) const
    {
        return std::find(m_sids.begin(), m_sids.end(), sid) == m_sids.end();
    }
    std::size_t size() const { return m_sids.size(); }
    void clear() { m_sids.clear(); }

private:
    std::deque<Str> m_sids;
};

} // namespace talq
