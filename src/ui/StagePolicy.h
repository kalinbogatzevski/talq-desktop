#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

// StagePolicy — the PURE stage decisions for the in-call surface. No Qt, no
// GStreamer (plain std::string session ids, injected monotonic clock), so
// tests/stage_policy_test.cpp can drive them through simulated minutes of a
// call in microseconds. Two units live here:
//
//   - StageSpeakerLatch  — the active-speaker LATCH for the Zoom-style
//     speaker stage (directly below);
//   - StageSourcePolicy  — WHO OWNS THE MAIN STAGE: the pick() precedence and
//     the manual-pin lifecycle (end of file), extracted from CallStage after
//     the 0.60.1 "stuck in share mode" field bug so the decision can be
//     pinned by tests.
//
// The problem the LATCH solves: CallParticipant::speaking() is driven by a GStreamer `level`
// element at ~10 Hz and is far too jittery to drive a layout directly — raw
// flags flip on every syllable gap, and a stage wired straight to them would
// strobe. This latch turns those jittery per-peer flags into a STABLE,
// ordered set of who belongs on the main stage:
//
//   - promotion needs sustained speech (kPromoteMs), not a blip;
//   - a promoted speaker rides out inter-sentence gaps (kHoldMs) and is
//     guaranteed a minimum residency (kMinDwellMs);
//   - the set NEVER collapses to empty on silence (sticky-empty — Zoom does
//     this; without it the layout flips every time the room pauses);
//   - insertion order is preserved, never re-sorted by loudness;
//   - all promote/demote decisions are rate-limited (kMutateCooldownMs).
//
// The caller (CallStage) feeds update() once per VAD tick with every present
// peer's raw speaking flag + level and a monotonic clock reading; a `true`
// return means the promoted set changed and a relayout (via StageMotion.h)
// is due.

namespace talq::stage {

// ---- Tunables --------------------------------------------------------------

// Continuous speaking required before a peer is promoted onto the stage.
// 600 ms is past coughs, "mm-hm" backchannels and single VAD blips (the raw
// flag toggles at the ~10 Hz level cadence) but still answers a genuine
// utterance within the first word or two — the stage must feel immediate.
constexpr std::int64_t kPromoteMs = 600;

// How long a promoted speaker keeps the stage after speaking() goes false.
// Natural inter-SENTENCE pauses run 0.5–2 s; 3 s rides over them so one
// conversational turn is ONE stage residency, not a demote per sentence.
constexpr std::int64_t kHoldMs = 3000;

// Once promoted, a peer cannot be demoted for at least this long, whatever
// the VAD says. 2 s guarantees the viewer can register WHO just arrived on
// stage — a sub-second cameo is pure churn, not information.
constexpr std::int64_t kMinDwellMs = 2000;

// Minimum spacing between ANY two mutations (promote OR demote). One layout
// change per second is the fastest the eye tracks comfortably; anything
// quicker reads as thrash even when each individual change is justified.
constexpr std::int64_t kMutateCooldownMs = 1000;

// Stage capacity. Two side-by-side speakers is a conversation; 3+ concurrent
// talkers is crosstalk, not conversation — the extras stay on the rail until
// someone yields.
constexpr int kMaxStageSpeakers = 2;

// Smoothed-level EMA, fast attack / slow decay — mirrors the VU meter in
// CallStage.cpp (`s += (target - s) * (target > s ? 0.6 : 0.16)`) so the
// latch ranks concurrent talkers by the same loudness the meters display.
constexpr double kEmaAttack = 0.6;
constexpr double kEmaDecay  = 0.16;

// ---- The latch -------------------------------------------------------------

class StageSpeakerLatch {
public:
    // Raw per-peer VAD state for one tick. `level` is CallParticipant's
    // audioLevel() (0.0–1.0); `speaking` its raw jittery speaking() flag.
    struct Speaker {
        std::string sessionId;
        double      level    = 0.0;
        bool        speaking = false;
    };

    // Feed the current raw VAD state for EVERY present peer + a monotonic
    // clock reading (milliseconds). The clock is injected — the latch never
    // reads a clock itself, which is what keeps it deterministic under test.
    // Returns true if the promoted set CHANGED (caller should relayout).
    bool update(const std::vector<Speaker> &now, std::int64_t nowMs)
    {
        bool changed = false;

        // (1) Vanished peers: drop from the stage AND all tracking state
        // immediately, BYPASSING the cooldown — a peer who LEFT the call must
        // not hold the stage. This is the one path that may empty the set:
        // the sticky-empty rule below covers SILENCE, not departure.
        // Departures neither consume nor arm the cooldown; they are facts,
        // not layout decisions, and must not delay the next real mutation.
        auto present = [&now](const std::string &id) {
            for (const Speaker &s : now)
                if (s.sessionId == id) return true;
            return false;
        };
        m_tracks.erase(std::remove_if(m_tracks.begin(), m_tracks.end(),
                           [&](const Track &t) { return !present(t.sessionId); }),
                       m_tracks.end());
        for (std::size_t i = 0; i < m_promoted.size();) {
            if (!present(m_promoted[i])) {
                m_promoted.erase(m_promoted.begin() + long(i));
                m_promotedAtMs.erase(m_promotedAtMs.begin() + long(i));
                changed = true;
            } else {
                ++i;
            }
        }

        // (2) Per-session tracking: smoothed level (fast attack / slow decay,
        // same curve as the on-screen VU meter) + the continuous-speaking
        // window. A single false tick resets the window — "continuous" means
        // continuous. (Linear scans throughout: n = call participants, a few
        // dozen at most, at ~10 Hz — negligible.)
        for (const Speaker &s : now) {
            Track &t = trackFor(s.sessionId);
            const double lvl = std::clamp(s.level, 0.0, 1.0);
            t.ema += (lvl - t.ema) * (lvl > t.ema ? kEmaAttack : kEmaDecay);
            if (s.speaking) {
                if (t.speakStartMs < 0) t.speakStartMs = nowMs;
                t.lastSpokeMs = nowMs;
            } else {
                t.speakStartMs = -1;
            }
        }

        // (3) Challenger lead windows (for the at-cap displacement rule):
        // a non-promoted, currently-speaking peer "leads" while its smoothed
        // level exceeds the QUIETEST incumbent's. The window resets the
        // moment it stops leading or stops speaking — a displacement needs a
        // full kPromoteMs of sustained lead, not a momentary spike.
        if (!m_promoted.empty()) {
            double quietestIncumbent = 1.0e30;
            for (const std::string &id : m_promoted)
                if (const Track *t = findTrack(id))
                    quietestIncumbent = std::min(quietestIncumbent, t->ema);
            for (Track &t : m_tracks) {
                const bool leading = !isPromoted(t.sessionId)
                                     && t.speakStartMs >= 0
                                     && t.ema > quietestIncumbent;
                if (!leading)                t.leadSinceMs = -1;
                else if (t.leadSinceMs < 0)  t.leadSinceMs = nowMs;
            }
        }

        // (4) Mutations — at most ONE per kMutateCooldownMs, promote or
        // demote alike. Each branch below returns immediately on a mutation,
        // so a single tick can never both evict and admit the same peer.
        if (m_haveMutation && nowMs - m_lastMutationMs < kMutateCooldownMs)
            return changed;

        // (4a) Promotion into a free slot: sustained speech for kPromoteMs.
        // Among several qualifiers, the highest smoothed level wins the slot
        // (ranking uses the EMA, never the raw instantaneous level).
        // Promotion APPENDS — insertion order is the display order and is
        // never re-sorted: if A and B swapped places whenever C arrived, the
        // viewer would lose track of both.
        if (int(m_promoted.size()) < kMaxStageSpeakers) {
            if (const Track *best = bestCandidate(nowMs)) {
                m_promoted.push_back(best->sessionId);
                m_promotedAtMs.push_back(nowMs);
                armCooldown(nowMs);
                return true;
            }
        } else {
            // (4b) At-cap displacement: a challenger that has BOTH spoken
            // continuously for kPromoteMs AND led the quietest incumbent's
            // smoothed level for a full kPromoteMs displaces the LEAST-
            // RECENTLY-PROMOTED incumbent — but only once that incumbent's
            // kMinDwellMs has expired. Evict + append is ONE mutation (a
            // swap): one cooldown arming, and the evicted peer cannot be
            // readmitted in the same tick.
            const Track *ch = bestCandidate(nowMs, /*requireLead=*/true);
            if (ch && nowMs - m_promotedAtMs.front() >= kMinDwellMs) {
                m_promoted.erase(m_promoted.begin());
                m_promotedAtMs.erase(m_promotedAtMs.begin());
                m_promoted.push_back(ch->sessionId);
                m_promotedAtMs.push_back(nowMs);
                armCooldown(nowMs);
                return true;
            }
        }

        // (4c) Demotion: silent for kHoldMs AND past kMinDwellMs since
        // promotion — but NEVER down to an empty set. STICKY EMPTY SET is
        // the single most important anti-flicker rule: when the whole room
        // pauses, the last promoted set is RETAINED (Zoom does this; without
        // it the layout flips on every lull). Demotion removes and closes
        // the gap; least-recently-promoted eligible goes first.
        if (m_promoted.size() >= 2) {
            for (std::size_t i = 0; i < m_promoted.size(); ++i) {
                const Track *t = findTrack(m_promoted[i]);
                if (!t) continue;
                if (nowMs - m_promotedAtMs[i] < kMinDwellMs) continue;
                if (t->lastSpokeMs >= 0 && nowMs - t->lastSpokeMs < kHoldMs) continue;
                m_promoted.erase(m_promoted.begin() + long(i));
                m_promotedAtMs.erase(m_promotedAtMs.begin() + long(i));
                armCooldown(nowMs);
                return true;
            }
        }

        return changed;
    }

    // The promoted set, in INSERTION ORDER (oldest promotion first). Never
    // re-sorted — stable positions are what lets the viewer keep track.
    const std::vector<std::string> &promoted() const { return m_promoted; }

    void reset()
    {
        m_tracks.clear();
        m_promoted.clear();
        m_promotedAtMs.clear();
        m_haveMutation = false;
        m_lastMutationMs = 0;
    }

private:
    struct Track {
        std::string  sessionId;
        double       ema          = 0.0;  // smoothed level (attack/decay above)
        std::int64_t speakStartMs = -1;   // continuous-speech start; -1 = silent now
        std::int64_t lastSpokeMs  = -1;   // last tick observed speaking
        std::int64_t leadSinceMs  = -1;   // leading the quietest incumbent since
    };

    Track *findTrack(const std::string &id)
    {
        for (Track &t : m_tracks)
            if (t.sessionId == id) return &t;
        return nullptr;
    }
    const Track *findTrack(const std::string &id) const
    {
        for (const Track &t : m_tracks)
            if (t.sessionId == id) return &t;
        return nullptr;
    }
    Track &trackFor(const std::string &id)
    {
        if (Track *t = findTrack(id)) return *t;
        m_tracks.push_back(Track{});
        m_tracks.back().sessionId = id;
        return m_tracks.back();
    }
    bool isPromoted(const std::string &id) const
    {
        for (const std::string &p : m_promoted)
            if (p == id) return true;
        return false;
    }

    // Best (loudest-smoothed) non-promoted peer that has spoken continuously
    // for kPromoteMs; with requireLead it must also have led the quietest
    // incumbent for kPromoteMs (the at-cap displacement bar).
    const Track *bestCandidate(std::int64_t nowMs, bool requireLead = false) const
    {
        const Track *best = nullptr;
        for (const Track &t : m_tracks) {
            if (isPromoted(t.sessionId)) continue;
            if (t.speakStartMs < 0 || nowMs - t.speakStartMs < kPromoteMs) continue;
            if (requireLead
                && (t.leadSinceMs < 0 || nowMs - t.leadSinceMs < kPromoteMs)) continue;
            if (!best || t.ema > best->ema) best = &t;
        }
        return best;
    }

    void armCooldown(std::int64_t nowMs)
    {
        m_haveMutation = true;
        m_lastMutationMs = nowMs;
    }

    std::vector<Track>        m_tracks;
    std::vector<std::string>  m_promoted;      // insertion order (display order)
    std::vector<std::int64_t> m_promotedAtMs;  // parallel: promotion timestamps
    bool                      m_haveMutation   = false;
    std::int64_t              m_lastMutationMs = 0;
};

// ---- The stage-source decision ---------------------------------------------
//
// WHO OWNS THE MAIN STAGE, extracted from CallStage::stageSource() /
// validatePin() after the 0.60.1 field bug: two peers were both screen-
// sharing; a release-click that fell through a missing chrome guard onto rail
// tile 0 (the "You" tile) had silently pinned SELF's camera. The pin was
// invisible while a share held the stage and detonated the instant the last
// share stopped — own camera parked on the full stage, indistinguishable from
// "stuck in share mode". CallStage maps its live Qt state (CallParticipant*,
// QPointer pins) to the plain structs below, calls the policy, and maps the
// answer back; every pointer-lifetime and painting concern stays on the Qt
// side. Only the DECISION lives here, where tests can pin it.

// Our own share has no remote sessionId, so this key stands in for self
// wherever a session key is needed (candidates, pins, the known-sharer set).
inline constexpr char kSelfSessionKey[] = "@self";

// Everything the stage decision needs to know about one participant.
// Candidates arrive in PARTICIPANT ORDER — self is always index 0 (CallManager
// prepends it) — and order breaks ties: the first sharing remote wins the
// stage, the first remote is the no-speaker fallback.
struct StageCandidate {
    std::string sessionId;     // remote sessionId; kSelfSessionKey for self
    bool isSelf     = false;
    bool screenLive = false;   // decodable screen stream RIGHT NOW (flag AND provider)
    bool speaking   = false;   // raw VAD flag; only consulted for remotes
};

// A manual pin (click-to-promote). A pin names a STREAM, not just a
// participant: the old participant-only pin could only ever mean "that peer's
// CAMERA", which is why a stray self-pin put our own camera on the full stage
// the moment the last share ended (the 0.60.1 field bug) and why a screen
// share could not be pinned at all. Carrying the kind is what makes
// click-to-promote work for shares as well as cameras.
struct StagePin {
    std::string sessionId;     // empty = no pin
    bool isScreen = false;
    bool set() const { return !sessionId.empty(); }
    void clear() { sessionId.clear(); isScreen = false; }
    bool operator==(const StagePin &o) const
    { return sessionId == o.sessionId && isScreen == o.isScreen; }
    bool operator!=(const StagePin &o) const { return !(*this == o); }
};

// What the stage should show. An empty sessionId means there is NOTHING to
// stage (no participants at all) — never "show self by accident".
struct StagePick {
    std::string sessionId;
    bool isScreen = false;
};

class StageSourcePolicy {
public:
    // Which source owns the stage right now. Precedence, highest first:
    //   1. manual pin (camera or screen, self or remote)   — click-to-promote
    //   2. a remote peer's screen share
    //   3. our own screen share
    //   4. the active speaker, then the first remote
    // The pin used to be consulted LAST (after both share branches), which made
    // it invisible during a share and let it detonate when the share stopped.
    static StagePick pick(const std::vector<StageCandidate> &now, const StagePin &pin)
    {
        // PRECEDENCE 1 — the manual pin. A pin names a STREAM, so it can be a
        // camera OR a screen, ours or a peer's. This deliberately OUTRANKS an
        // active share, reversing the old order: clicking a peer's camera
        // while they are presenting must actually put that camera on the
        // stage, and their share drops to the rail (still a tile, so it can be
        // clicked back). It is safe to let a pin beat a share ONLY because
        // validatePin() runs first on every relayout: it drops a pin whose
        // source died, and surrenders the pin to any NEWLY started share — so
        // a pin can never hide content somebody just began sharing.
        if (pin.set()) return {pin.sessionId, pin.isScreen};

        // PRECEDENCE 2 — a remote peer's share. Skip self — self is always
        // index 0, so an unguarded scan would match OUR OWN active share
        // before ever reaching a remote peer's, permanently winning the stage
        // even after a peer starts sharing too (backlog bug: a peer's incoming
        // share never displayed while we were already sharing our own screen).
        for (const StageCandidate &c : now)
            if (!c.isSelf && c.screenLive) return {c.sessionId, true};

        // PRECEDENCE 3 — 0.53.0 Zoom-style self-view: when WE are sharing and
        // no PEER is, our OWN share takes the stage (rendered from the local
        // capture preview tap), so the presenter sees what they're
        // broadcasting full-size — the same view the remote gets — instead of
        // only a small thumbnail. A peer's share still wins the stage above:
        // their content is what the call is watching. Applies to BOTH window
        // AND monitor shares (consistent local-share UI, per Kalin — replaces
        // the blunt 0.53.1 window-only gate); the monitor-share hall-of-
        // mirrors is handled at paint time (CallStage::paintTile placeholder),
        // not by demoting the source here.
        for (const StageCandidate &c : now)
            if (c.isSelf && c.screenLive) return {c.sessionId, true};

        // PRECEDENCE 4 — nobody is sharing and nothing is pinned: the active
        // speaker, else the first remote.
        const StageCandidate *speaker = nullptr, *firstRemote = nullptr;
        for (const StageCandidate &c : now) {
            if (c.isSelf) continue;
            if (!firstRemote) firstRemote = &c;
            if (c.speaking && !speaker) speaker = &c;
        }
        if (speaker)     return {speaker->sessionId, false};
        if (firstRemote) return {firstRemote->sessionId, false};

        // Self (index 0) is only ever reached when we are ALONE in the call:
        // the firstRemote return above fires whenever any remote exists.
        // Showing our own camera to an empty room is correct, so this is NOT
        // the "big self-camera" path from the 0.60.1 field report — that one
        // came from a stray self-pin (see requestPin's guard). Do not "harden"
        // this to empty; it would blank the solo/waiting-for-others stage.
        return now.empty() ? StagePick{} : StagePick{now.front().sessionId, false};
    }

    // Drop a pin that no longer names a live source, and surrender the pin to
    // any NEWLY started share (diffed against the known-sharer set held here).
    // Must run before every pick() — load-bearing, not hardening: now that a
    // pin outranks an active share, a stale pin is MORE visible than it used
    // to be. Returns the surviving pin: either the input unchanged or cleared;
    // validatePin never redirects a pin to a different stream.
    StagePin validatePin(StagePin pin, const std::vector<StageCandidate> &now)
    {
        // Who is sharing RIGHT NOW.
        std::vector<std::string> sharers;
        for (const StageCandidate &c : now)
            if (c.screenLive) sharers.push_back(c.sessionId);

        // A NEWLY started share always claims the stage, surrendering any
        // manual pin. This is the one deliberate exception to "the pin wins":
        // a stale pin quietly hiding content a peer just started presenting is
        // far worse than losing a pin. An ALREADY-KNOWN sharer never re-steals
        // the pin — otherwise no pin could be held at all while a share runs.
        for (const std::string &s : sharers) {
            if (std::find(m_knownSharers.begin(), m_knownSharers.end(), s)
                == m_knownSharers.end()) { pin.clear(); break; }
        }
        m_knownSharers = std::move(sharers);

        if (!pin.set()) return pin;

        // The pinned participant left the call: a dead pin is cleared so the
        // stage falls back down the precedence instead of showing nothing.
        const StageCandidate *c = nullptr;
        for (const StageCandidate &k : now)
            if (k.sessionId == pin.sessionId) { c = &k; break; }
        if (!c) { pin.clear(); return pin; }

        // A pinned SHARE that has stopped must release the stage, or we
        // reproduce the 0.60.1 bug in a new place: a dead pin holding a stage
        // that has nothing to show.
        if (pin.isScreen && !c->screenLive) pin.clear();
        return pin;
    }

    // Never pin our own CAMERA: a self camera-pin puts our own face
    // full-screen, which is the 0.60.1 "stuck showing myself" symptom itself
    // and is never what clicking your own thumbnail means. Our own SHARE is
    // pinnable — that is a legitimate "show me what I'm presenting, big".
    // This is the direct guard against the field bug: with it, the
    // self-camera-pin state is UNREACHABLE through any click path.
    static bool pinnable(bool isSelf, bool isScreen) { return isScreen || !isSelf; }

    // Rail-tile click semantics: pin the clicked stream; clicking the stream
    // that is already pinned unpins (back to automatic). A click the guard
    // rejects returns `current` UNCHANGED, so the caller can compare and skip
    // the relayout when nothing actually happened.
    static StagePin requestPin(const StagePin &current, const std::string &sessionId,
                               bool isSelf, bool isScreen)
    {
        if (!pinnable(isSelf, isScreen)) return current;
        const StagePin want{sessionId, isScreen};
        return (current == want) ? StagePin{} : want;
    }

    void reset() { m_knownSharers.clear(); }

private:
    // Sharers seen on the previous validatePin() (session keys; kSelfSessionKey
    // stands in for our own share). The surrender rule above diffs the current
    // sharers against this, so only a share that STARTED since the last layout
    // steals the pin — one that merely keeps running does not.
    std::vector<std::string> m_knownSharers;
};

} // namespace talq::stage
