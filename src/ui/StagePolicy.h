#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

// StagePolicy — the active-speaker LATCH for the Zoom-style speaker stage.
// Pure logic, no Qt, no GStreamer (plain std::string session ids, injected
// monotonic clock), so tests/stage_policy_test.cpp can drive it through
// simulated minutes of conversation in microseconds.
//
// The problem: CallParticipant::speaking() is driven by a GStreamer `level`
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

} // namespace talq::stage
