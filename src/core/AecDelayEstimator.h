#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <vector>

// AecDelayEstimator -- how far BEHIND its echo does the AEC loopback reference run?
// No toolkit, no media framework, no clock: the caller feeds timestamped 48 kHz S16 blocks
// (running time in ns, as the pipeline stamps them) and asks for an estimate at a window
// end, so the whole thing is unit-testable without a call -- see
// tests/aec_delay_estimator_test.cpp.
//
// Why it exists (bench 2026-10-30, two laptops): the echo canceller can only subtract an
// echo whose reference it received EARLIER, and the reference's timestamps depend on the
// machine's state. On one laptop the reference ran about 285 ms late (D_total -245 ms, no
// cancellation, residual -23 dB); a forced 280 ms shift of its timestamps restored full
// cancellation (-68 dB), 330 ms and more did not. The controller needs the real figure,
// not a guess: D_raw = the running time of the echo in the microphone minus the running
// time of the reference sample that caused it, on the RAW (unshifted) reference labels.
//
// How it measures: both streams are low-passed and decimated to 8 kHz onto the absolute
// sample grid of their own timestamps, kept in two 16 s rings, and a 7.5 s window of each
// (the SAME interval) is correlated with GCC-PHAT over 200-3600 Hz and +-600 ms. PHAT
// weights every frequency bin alike, so the peak is a sharp spike at the delay whatever
// the voice sounds like. A Tukey taper is applied first: an untapered window cuts both
// signals at the same instant, and that shared edge is itself a (false) correlation at
// lag 0 that PHAT amplifies.
//
// Definitions the tests pin (z, psr):
//   z   = |peak| / rms(cc) over all lags OUTSIDE +-psrGuardMs of the peak. Pure noise
//         gives about 4 (the largest of ~9600 draws); a real echo gives tens to hundreds.
//   psr = |peak| / max|cc| outside +-psrGuardMs of the peak. About 1 when two peaks are
//         equally good (a periodic signal: the delay is ambiguous), large for one echo.
// A window is GOOD only when all of z, psr, reference activity, mic level and coverage
// pass; a gated window is "no measurement", never "the delay is X". The thresholds come
// from synthetic speech (see the test's printed table); real speech statistics are
// still to be collected.
//
// Threading: PtsRing8k::push() is called on a streaming thread (one per ring, each ring has
// its own mutex, nothing is allocated) and NEVER waits for it: the worker runs at a lower
// priority and Windows has no priority inheritance, so a worker preempted inside the lock
// would otherwise stall a capture thread. A push that finds the lock taken is dropped and
// counted, and the next one starts a new segment. estimateAt() copies a window out of both
// rings in short slices under those mutexes, then computes with no lock held.
// EstimatorScratch and OffsetTracker are worker-private.
//
// Drift above about 1 ms/s defeats the estimate (the lag moves inside one window).

namespace talq {

inline constexpr int     kAecEstRate         = 8000;          // working rate (Hz)
inline constexpr int     kAecEstDecim        = 6;             // 48000 / 8000
inline constexpr int     kAecEstHalfTaps     = 48;            // 97-tap FIR, group delay 1 ms
inline constexpr int     kAecEstBlock        = 80;            // validity block: 10 ms of 8 kHz
inline constexpr int     kAecEstRingBlocks   = 1600;          // 16 s
inline constexpr int     kAecEstWindowBlocks = 750;           // 7.5 s
inline constexpr int     kAecEstCopySlice    = 50;            // blocks copied per lock hold (0.5 s)
inline constexpr int     kAecEstWindowSamples = kAecEstWindowBlocks * kAecEstBlock;   // 60000
inline constexpr int     kAecEstFft          = 65536;         // >= window + max lag: no wrap
inline constexpr int     kAecEstMaxLag       = 4800;          // 600 ms at 8 kHz
inline constexpr int     kAecEstFrame        = 160;           // reference activity frame: 20 ms
inline constexpr int64_t kAecEstBlockNs      = 10000000;      // 80 samples
inline constexpr int64_t kAecEstSampleNs     = 125000;        // 1 / 8 kHz
inline constexpr int64_t kAecEstGapTolNs     = 1000000;       // label jitter that is NOT a gap (1 ms)
inline constexpr int64_t kAecEstWindowNs     = kAecEstWindowBlocks * kAecEstBlockNs;
inline constexpr int64_t kAecEstEndMarginNs  = 100000000;     // newest 100 ms is not used

// ---- 97-tap low-pass for the 48 -> 8 kHz decimation ---------------------------------
// Kaiser-windowed sinc, -6 dB at 3.7 kHz, about 50 dB down from 4.4 kHz. Both streams go
// through the same filter, so its (linear-phase) delay cancels in D; it is compensated
// anyway so a ring's time axis is the signal's time axis.
inline const float *aecDecimatorTaps()   // [0..48]: the centre tap and the 48 on one side
{
    static const std::vector<float> taps = [] {
        const int m = kAecEstHalfTaps;
        const double pi = 3.14159265358979323846;
        const double fc = 3700.0 / 48000.0;     // cycles per input sample
        const double beta = 5.0;
        auto i0 = [](double x) {
            double sum = 1.0, term = 1.0;
            for (int k = 1; k < 60; ++k) {
                term *= (x / (2.0 * k)) * (x / (2.0 * k));
                sum += term;
                if (term < 1e-14 * sum) break;
            }
            return sum;
        };
        std::vector<double> h(2 * m + 1);
        double total = 0.0;
        for (int n = 0; n <= 2 * m; ++n) {
            const double x = n - m;
            const double s = (x == 0.0) ? 2.0 * fc : std::sin(2.0 * pi * fc * x) / (pi * x);
            const double r = x / m;
            h[n] = s * i0(beta * std::sqrt(std::max(0.0, 1.0 - r * r))) / i0(beta);
            total += h[n];
        }
        std::vector<float> half(m + 1);
        for (int k = 0; k <= m; ++k) half[k] = static_cast<float>(h[m + k] / total);   // unity DC gain
        return half;
    }();
    return taps.data();
}

// ---- 48 -> 8 kHz decimator, anchored to the ABSOLUTE 48 kHz sample index ------------
// A segment is a run of contiguous input samples; its first sample has absolute index a0
// (the PTS rounded to the 48 kHz grid). Outputs are the filtered values at the absolute
// indices that are multiples of 6, so output j sits at exactly j / 8000 s of running
// time and two rings share one grid without rounding bias. An output needs 48 samples on
// each side; none is produced before a0 + 48 (the priming transient), so the first 1 ms
// of a segment is simply absent and its block stays incomplete rather than wrong.
class AecDecimator8k {
public:
    static constexpr int kMaxChunk = 1024;

    void begin(int64_t a0)
    {
        m_next = a0;
        const int64_t first = a0 + kAecEstHalfTaps;
        m_center = (first + kAecEstDecim - 1) / kAecEstDecim * kAecEstDecim;
        std::memset(m_hist, 0, sizeof(m_hist));
    }
    int64_t nextAbs() const { return m_next; }
    // Absolute 8 kHz index of the first output the segment will produce.
    int64_t firstOutput() const { return m_center / kAecEstDecim; }

    // `n` <= kMaxChunk samples, contiguous with the previous call. sink(index8k, value).
    template <class Sink>
    void feed(const int16_t *in, int n, Sink &&sink)
    {
        constexpr int H = 2 * kAecEstHalfTaps;                 // samples of history kept
        float work[H + kMaxChunk];
        std::memcpy(work, m_hist, sizeof(m_hist));
        for (int i = 0; i < n; ++i) work[H + i] = in[i] * (1.0f / 32768.0f);
        const float *tap = aecDecimatorTaps();
        const int64_t base = m_next - H;                       // absolute index of work[0]
        const int64_t last = m_next + n - 1;                   // newest input
        while (m_center + kAecEstHalfTaps <= last) {
            const float *x = work + (m_center - base);
            float acc = tap[0] * x[0];
            for (int k = 1; k <= kAecEstHalfTaps; ++k) acc += tap[k] * (x[-k] + x[k]);
            sink(m_center / kAecEstDecim, acc);
            m_center += kAecEstDecim;
        }
        std::memcpy(m_hist, work + n, sizeof(m_hist));         // the newest H samples
        m_next += n;
    }

private:
    int64_t m_next = 0;                                        // absolute index of the next input
    int64_t m_center = 0;                                      // next output centre (multiple of 6)
    float   m_hist[2 * kAecEstHalfTaps] = {};
};

// ---- a 16 s ring of 8 kHz samples keyed by running time ------------------------------
// Slot = absolute 8 kHz index modulo the capacity. Validity is kept per 80-sample block
// as (absolute block number, which of its 80 samples were written): a block is usable
// only if every sample is present, so a wrap, a gap or a half-written block can never be
// read as data. Everything happens under one mutex; a push is ~80 outputs of a 49-MAC
// filter and the worker's copy is ~240 KB taken in 50-block slices, both far below a
// buffer period. The streaming side only ever try-locks (see push).
class PtsRing8k {
public:
    // Everything a push needs is allocated here (and the filter taps built), so the streaming
    // thread never allocates.
    PtsRing8k() : m_buf(static_cast<size_t>(kAecEstRingBlocks) * kAecEstBlock, 0.0f), m_blk(kAecEstRingBlocks)
    {
        (void)aecDecimatorTaps();
    }

    // Streaming thread: one 48 kHz S16 mono buffer stamped `ptsNs` (running time). A
    // negative stamp (unknown) continues the current segment; with none it is dropped.
    // A forward jump or a backward jump of more than 1 ms starts a new segment.
    //
    // Never waits: if the worker holds the lock the buffer is dropped (noteDropped), because
    // a capture thread must not queue behind a lower-priority one. The missing 10 ms just
    // shows up as a gap, which the next buffer's label turns into a new segment.
    void push(const int16_t *s, int n, int64_t ptsNs)
    {
        if (n <= 0) return;
        std::unique_lock<std::mutex> lk(m_mx, std::try_to_lock);
        if (!lk.owns_lock()) {
            noteDropped();
            return;
        }
        const bool lostSync = m_lostSync.exchange(false, std::memory_order_relaxed);
        if (ptsNs < 0) {
            // No label to re-anchor on: after a dropped buffer this one cannot be placed.
            if (lostSync) m_active = false;
            if (!m_active) return;
        } else {
            // Label jitter inside the tolerance is ignored (the samples stay contiguous).
            const int64_t a = nsToAbs(ptsNs), next = m_dec.nextAbs();
            const int64_t tol = kAecEstGapTolNs * 48000 / 1000000000;
            if (!m_active || lostSync || a > next + tol || a < next - tol) startSegment(a);
        }
        for (int off = 0; off < n; off += AecDecimator8k::kMaxChunk) {
            const int c = std::min(n - off, AecDecimator8k::kMaxChunk);
            m_dec.feed(s + off, c, [this](int64_t j, float v) { write(j, v); });
        }
    }

    // A buffer that could not be pushed (lock busy): counted, and the next push starts a new
    // segment whatever its label says. Public so a caller that skips a buffer on purpose can
    // say so; push() uses it. Lock-free.
    void noteDropped()
    {
        m_dropped.fetch_add(1, std::memory_order_relaxed);
        m_lostSync.store(true, std::memory_order_relaxed);
    }
    int64_t dropped() const { return m_dropped.load(std::memory_order_relaxed); }

    // Forget everything (a new epoch: the reference was re-opened, the timeline changed).
    void reset()
    {
        std::lock_guard<std::mutex> lk(m_mx);
        for (Blk &b : m_blk) b = Blk{};
        m_active = false;
        m_endIdx = 0;
        ++m_resets;
    }

    // Running time (ns) just past the newest sample held; -1 when empty.
    int64_t endNs() const
    {
        std::lock_guard<std::mutex> lk(m_mx);
        return m_endIdx > 0 ? m_endIdx * kAecEstSampleNs : -1;
    }
    int64_t segments() const { std::lock_guard<std::mutex> lk(m_mx); return m_segments; }
    int64_t jumps() const { std::lock_guard<std::mutex> lk(m_mx); return m_jumps; }
    int64_t resets() const { std::lock_guard<std::mutex> lk(m_mx); return m_resets; }

    // Worker: copy blocks [firstBlock, firstBlock + nBlocks) (block b spans running time
    // b * 10 ms ..). Invalid blocks are zero-filled and ok[i] = 0. Returns the valid count.
    // The lock is taken per slice of kAecEstCopySlice blocks, not once for the lot, so the
    // longest a preempted worker can hold it is a few microseconds' worth of copying. A block
    // is validated and copied under the same hold, so a slice boundary cannot tear one.
    int copyBlocks(int64_t firstBlock, int nBlocks, float *out, uint8_t *ok) const
    {
        int valid = 0;
        for (int s0 = 0; s0 < nBlocks; s0 += kAecEstCopySlice) {
            const int s1 = std::min(nBlocks, s0 + kAecEstCopySlice);
            std::lock_guard<std::mutex> lk(m_mx);
            for (int i = s0; i < s1; ++i) {
                const int64_t b = firstBlock + i;
                const bool good = b >= 0 && blockFull(b);
                float *dst = out + static_cast<size_t>(i) * kAecEstBlock;
                if (good) {
                    const size_t slot = static_cast<size_t>(b % kAecEstRingBlocks);
                    std::memcpy(dst, m_buf.data() + slot * kAecEstBlock, sizeof(float) * kAecEstBlock);
                    ++valid;
                } else {
                    std::memset(dst, 0, sizeof(float) * kAecEstBlock);
                }
                ok[i] = good ? 1 : 0;
            }
        }
        return valid;
    }

private:
    struct Blk { int64_t id = -1; uint64_t lo = 0, hi = 0; };

    // 48 kHz grid index of a timestamp, rounded. Split so a day-long running time cannot overflow.
    static int64_t nsToAbs(int64_t ns)
    {
        const int64_t sec = ns / 1000000000, rem = ns % 1000000000;
        return sec * 48000 + (rem * 48000 + 500000000) / 1000000000;
    }

    bool blockFull(int64_t b) const
    {
        const Blk &k = m_blk[static_cast<size_t>(b % kAecEstRingBlocks)];
        return k.id == b && k.lo == ~0ULL && (k.hi & 0xFFFFULL) == 0xFFFFULL;
    }

    void startSegment(int64_t a)
    {
        if (m_active) ++m_jumps;
        ++m_segments;
        m_dec.begin(a);
        m_active = true;
        // A backward jump rewrites time that is already in the ring: drop what the old
        // timeline put there, or a half-overwritten block would pass as valid.
        const int64_t j0 = m_dec.firstOutput();
        if (j0 < m_endIdx) {
            const int64_t b0 = j0 / kAecEstBlock, b1 = (m_endIdx - 1) / kAecEstBlock;
            if (b1 - b0 < kAecEstRingBlocks) {
                for (int64_t b = b0; b <= b1; ++b) {
                    Blk &k = m_blk[static_cast<size_t>(b % kAecEstRingBlocks)];
                    if (k.id == b) k = Blk{};
                }
            } else {
                // A jump back across more than the ring (an hour into a call, to near 0): visit
                // each slot once instead of walking every block number, under the streaming
                // thread's lock. Not just the newest 1600 blocks: after a long forward jump an
                // old block can still sit in its slot, and a new segment writing into it would
                // merge with the stale samples.
                for (Blk &k : m_blk) if (k.id >= b0 && k.id <= b1) k = Blk{};
            }
            m_endIdx = j0;
        }
    }

    void write(int64_t j, float v)
    {
        const int64_t b = j / kAecEstBlock;
        const int o = static_cast<int>(j % kAecEstBlock);
        const size_t slot = static_cast<size_t>(b % kAecEstRingBlocks);
        Blk &k = m_blk[slot];
        if (k.id != b) { k.id = b; k.lo = 0; k.hi = 0; }
        if (o < 64) k.lo |= 1ULL << o; else k.hi |= 1ULL << (o - 64);
        m_buf[slot * kAecEstBlock + o] = v;
        if (j + 1 > m_endIdx) m_endIdx = j + 1;
    }

    mutable std::mutex m_mx;
    AecDecimator8k     m_dec;
    std::vector<float> m_buf;
    std::vector<Blk>   m_blk;
    bool    m_active = false;
    int64_t m_endIdx = 0;
    int64_t m_segments = 0, m_jumps = 0, m_resets = 0;
    std::atomic<int64_t> m_dropped{0};       // pushes skipped because the lock was busy
    std::atomic<bool>    m_lostSync{false};  // a push was dropped: the next one must re-anchor
};

// ---- iterative radix-2 FFT (single precision, twiddles built in double) --------------
// One plan serves every power-of-two size up to maxN (a smaller transform strides the
// same tables), so the real-signal inverse below can use the half-size transform.
class AecFft {
public:
    explicit AecFft(int maxN) : m_n(maxN), m_cos(maxN / 2), m_sin(maxN / 2), m_rev(maxN)
    {
        const double pi = 3.14159265358979323846;
        for (int k = 0; k < maxN / 2; ++k) {
            m_cos[k] = static_cast<float>(std::cos(2.0 * pi * k / maxN));
            m_sin[k] = static_cast<float>(std::sin(2.0 * pi * k / maxN));
        }
        while ((1 << m_log2) < maxN) ++m_log2;
        for (int i = 0; i < maxN; ++i) {
            uint32_t r = 0;
            for (int b = 0; b < m_log2; ++b) if (i & (1 << b)) r |= 1u << (m_log2 - 1 - b);
            m_rev[i] = r;
        }
    }
    int maxSize() const { return m_n; }
    float cosTw(int k) const { return m_cos[k]; }      // cos(2 pi k / maxN), k < maxN / 2
    float sinTw(int k) const { return m_sin[k]; }

    // In place on `n` interleaved complex values (re, im). inverse = conjugate kernel, unscaled.
    void transform(float *d, int n, bool inverse) const
    {
        int lg = 0;
        while ((1 << lg) < n) ++lg;
        const int shift = m_log2 - lg;
        for (int i = 0; i < n; ++i) {
            const int j = static_cast<int>(m_rev[i] >> shift);
            if (j > i) { std::swap(d[2 * i], d[2 * j]); std::swap(d[2 * i + 1], d[2 * j + 1]); }
        }
        const float sgn = inverse ? 1.0f : -1.0f;
        for (int len = 2; len <= n; len <<= 1) {
            const int half = len >> 1, stride = m_n / len;
            if (half >= 4) {
                for (int i = 0; i < n; i += len) {
                    float *a = d + 2 * i, *b = a + 2 * half;
                    for (int j = 0; j < half; ++j) {
                        const float wr = m_cos[j * stride], wi = sgn * m_sin[j * stride];
                        const float tr = b[2 * j] * wr - b[2 * j + 1] * wi;
                        const float ti = b[2 * j] * wi + b[2 * j + 1] * wr;
                        b[2 * j] = a[2 * j] - tr;  b[2 * j + 1] = a[2 * j + 1] - ti;
                        a[2 * j] += tr;            a[2 * j + 1] += ti;
                    }
                }
            } else {
                for (int j = 0; j < half; ++j) {
                    const float wr = m_cos[j * stride], wi = sgn * m_sin[j * stride];
                    for (int i = 0; i < n; i += len) {
                        float *a = d + 2 * (i + j), *b = a + 2 * half;
                        const float tr = b[0] * wr - b[1] * wi;
                        const float ti = b[0] * wi + b[1] * wr;
                        b[0] = a[0] - tr;  b[1] = a[1] - ti;
                        a[0] += tr;        a[1] += ti;
                    }
                }
            }
        }
    }

private:
    int m_n;
    int m_log2 = 0;
    std::vector<float> m_cos, m_sin;
    std::vector<uint32_t> m_rev;
};

inline const AecFft &aecFftPlan()
{
    static const AecFft plan(kAecEstFft);
    return plan;
}

// ---- estimate ---------------------------------------------------------------------------
struct EstimatorParams {
    double zMin         = 15.0;     // peak over the cc rms outside the guard
    double psrMin       = 2.5;      // peak over the best peak outside the guard
    double refActiveMin = 0.15;     // fraction of 20 ms reference frames above refFrameDbfs
    double micMinDbfs   = -90.0;    // window rms of the microphone
    double coverageMin  = 0.90;     // fraction of 10 ms blocks present in BOTH streams
    double refFrameDbfs = -55.0;    // a reference frame counts as active above this rms
    double bandLoHz     = 200.0;
    double bandHiHz     = 3600.0;
    double psrGuardMs   = 2.0;      // main lobe and its side lobes are not a "second peak"
    double taperAlpha   = 0.25;     // Tukey; 0 = rectangular (test mutation only)
    double phatFloor    = 0.01;     // PHAT divisor floor, relative to the mean band |cross spectrum|
};

struct OffsetEstimate {
    bool        valid = false;      // every gate passed
    const char *reason = "none";    // static string: ok | coverage | mic-silent | no-ref-activity | z-low | psr-low
    double      lagMs = 0.0;        // D_raw: mic echo time minus reference time (+ = echo later)
    double      z = 0.0;
    double      psr = 0.0;
    double      peak = 0.0;         // |cc| at the peak; about 0.85 x coherence
    int         sign = 0;           // polarity of the peak
    double      refActive = 0.0;
    double      micDbfs = -120.0;
    double      coverage = 0.0;
    int64_t     endNs = 0;          // window end, running time (10 ms aligned)
};

// Gate order is fixed so the reason is deterministic: an all-zero mic is "mic-silent",
// never "z-low". Returns nullptr when the window is good.
inline const char *aecClassify(double coverage, double micDbfs, double refActive, double z, double psr,
                               const EstimatorParams &p)
{
    if (coverage < p.coverageMin) return "coverage";
    if (micDbfs < p.micMinDbfs) return "mic-silent";
    if (refActive < p.refActiveMin) return "no-ref-activity";
    if (z < p.zMin) return "z-low";
    if (psr < p.psrMin) return "psr-low";
    return nullptr;
}

struct EstimatorScratch {
    std::vector<float>   fft;       // 2 * kAecEstFft
    std::vector<float>   spec;      // one-sided PHAT spectrum, 2 * (kAecEstFft / 2 + 1)
    std::vector<float>   refWin, micWin;
    std::vector<float>   cc;        // 2 * kAecEstMaxLag + 1
    std::vector<float>   ramp;      // Tukey ramp
    std::vector<uint8_t> okRef, okMic;
    int rampLen = -1;
    void ensure()
    {
        if (!fft.empty()) return;
        fft.assign(2 * static_cast<size_t>(kAecEstFft), 0.0f);
        spec.assign(2 * (static_cast<size_t>(kAecEstFft) / 2 + 1), 0.0f);
        refWin.assign(kAecEstWindowSamples, 0.0f);
        micWin.assign(kAecEstWindowSamples, 0.0f);
        cc.assign(2 * kAecEstMaxLag + 1, 0.0f);
        okRef.assign(kAecEstWindowBlocks, 0);
        okMic.assign(kAecEstWindowBlocks, 0);
    }
};

inline double aecDbfs(double sumSq, double count)
{
    if (count < 1.0 || sumSq <= 0.0) return -120.0;
    const double rms = std::sqrt(sumSq / count);
    return rms > 1e-6 ? std::max(-120.0, 20.0 * std::log10(rms)) : -120.0;
}

// Fraction of 20 ms frames whose rms is above `thrDbfs`.
inline double aecActiveFraction(const float *x, int n, double thrDbfs)
{
    const int frames = n / kAecEstFrame;
    if (frames <= 0) return 0.0;
    const double thr = std::pow(10.0, thrDbfs / 20.0);
    const double thr2 = thr * thr * kAecEstFrame;
    int active = 0;
    for (int f = 0; f < frames; ++f) {
        double s = 0.0;
        const float *p = x + static_cast<size_t>(f) * kAecEstFrame;
        for (int i = 0; i < kAecEstFrame; ++i) s += static_cast<double>(p[i]) * p[i];
        if (s > thr2) ++active;
    }
    return static_cast<double>(active) / frames;
}

// GCC-PHAT of two equal-length 8 kHz windows (`ref`, `mic`, already zero where either
// stream was missing). `coverage` is reported by the caller. n <= 60736.
inline OffsetEstimate estimateOffset(const float *ref, const float *mic, int n, double coverage,
                                     EstimatorScratch &sc, const EstimatorParams &p = EstimatorParams{})
{
    OffsetEstimate e;
    n = std::min(n, kAecEstFft - kAecEstMaxLag);
    e.coverage = coverage;
    double micSq = 0.0;
    for (int i = 0; i < n; ++i) micSq += static_cast<double>(mic[i]) * mic[i];
    e.micDbfs = aecDbfs(micSq, std::max(1.0, coverage * n));
    e.refActive = aecActiveFraction(ref, n, p.refFrameDbfs);
    const double inf = std::numeric_limits<double>::infinity();
    if (const char *why = aecClassify(coverage, e.micDbfs, e.refActive, inf, inf, p)) {   // cheap gates first: no FFT
        e.reason = why;
        return e;
    }

    sc.ensure();
    const int N = kAecEstFft, H = N / 2;
    const AecFft &plan = aecFftPlan();

    // Tukey taper on both windows, then pack ref + j*mic into one complex transform.
    const int rl = static_cast<int>(p.taperAlpha * n / 2.0);
    if (rl != sc.rampLen) {
        sc.ramp.assign(static_cast<size_t>(rl), 0.0f);
        for (int i = 0; i < rl; ++i)
            sc.ramp[i] = static_cast<float>(0.5 * (1.0 - std::cos(3.14159265358979323846 * (i + 0.5) / rl)));
        sc.rampLen = rl;
    }
    float *P = sc.fft.data();
    for (int i = 0; i < n; ++i) {
        float w = 1.0f;
        if (i < rl) w = sc.ramp[i]; else if (i >= n - rl) w = sc.ramp[n - 1 - i];
        P[2 * i] = ref[i] * w;
        P[2 * i + 1] = mic[i] * w;
    }
    std::fill(P + 2 * static_cast<size_t>(n), P + 2 * static_cast<size_t>(N), 0.0f);
    plan.transform(P, N, false);

    // Cross spectrum C = M conj(R) on the band, from the packed transform:
    // 2R = P[k] + conj(P[N-k]), 2jM = P[k] - conj(P[N-k]).
    const int kLo = static_cast<int>(std::ceil(p.bandLoHz * N / kAecEstRate));
    const int kHi = std::min(H - 1, static_cast<int>(std::floor(p.bandHiHz * N / kAecEstRate)));
    float *X = sc.spec.data();
    std::fill(X, X + 2 * static_cast<size_t>(H + 1), 0.0f);
    double magSum = 0.0;
    for (int k = kLo; k <= kHi; ++k) {
        const float ar = P[2 * k], ai = P[2 * k + 1], br = P[2 * (N - k)], bi = P[2 * (N - k) + 1];
        const float rr = ar + br, ri = ai - bi;
        const float mr = ai + bi, mi = br - ar;
        const float xr = mr * rr + mi * ri, xi = mi * rr - mr * ri;
        X[2 * k] = xr;
        X[2 * k + 1] = xi;
        magSum += std::sqrt(static_cast<double>(xr) * xr + static_cast<double>(xi) * xi);
    }
    const float floorMag = static_cast<float>(p.phatFloor * magSum / std::max(1, kHi - kLo + 1));
    for (int k = kLo; k <= kHi; ++k) {
        const float mag = std::sqrt(X[2 * k] * X[2 * k] + X[2 * k + 1] * X[2 * k + 1]) + floorMag;
        const float inv = mag > 0.0f ? 1.0f / mag : 0.0f;
        X[2 * k] *= inv;
        X[2 * k + 1] *= inv;
    }

    // Real inverse through an N/2-point complex transform: with E[k] = (X[k] + conj(X[H-k]))/2
    // and O[k] = e^{+j 2 pi k / N} (X[k] - conj(X[H-k]))/2, z = IDFT_H(E + jO) holds the
    // even samples in its real part and the odd ones in its imaginary part.
    float *Z = sc.fft.data();
    for (int k = 0; k < H; ++k) {
        const float xr = X[2 * k], xi = X[2 * k + 1], yr = X[2 * (H - k)], yi = -X[2 * (H - k) + 1];
        const float er = 0.5f * (xr + yr), ei = 0.5f * (xi + yi);
        const float dr = 0.5f * (xr - yr), di = 0.5f * (xi - yi);
        const float c = plan.cosTw(k), s = plan.sinTw(k);
        const float or_ = c * dr - s * di, oi = c * di + s * dr;
        Z[2 * k] = er - oi;
        Z[2 * k + 1] = ei + or_;
    }
    plan.transform(Z, H, true);
    const float scale = 1.0f / H;
    auto at = [&](int lag) {               // cc at a signed lag, in samples
        const int idx = lag >= 0 ? lag : N + lag;
        return Z[2 * (idx >> 1) + (idx & 1)] * scale;
    };
    const int L = kAecEstMaxLag;
    float *cc = sc.cc.data();
    int kPk = 0;
    float best = -1.0f;
    for (int i = 0; i <= 2 * L; ++i) {
        cc[i] = at(i - L);
        const float a = std::fabs(cc[i]);
        if (a > best) { best = a; kPk = i; }
    }

    const int guard = static_cast<int>(std::lround(p.psrGuardMs * kAecEstRate / 1000.0));
    double sq = 0.0;
    int cnt = 0;
    float second = 0.0f;
    for (int i = 0; i <= 2 * L; ++i) {
        if (std::abs(i - kPk) <= guard) continue;
        const float a = std::fabs(cc[i]);
        sq += static_cast<double>(a) * a;
        ++cnt;
        if (a > second) second = a;
    }
    const double rms = cnt > 0 ? std::sqrt(sq / cnt) : 0.0;
    e.peak = best;
    e.sign = cc[kPk] >= 0.0f ? 1 : -1;
    e.z = rms > 0.0 ? best / rms : 0.0;
    e.psr = second > 0.0f ? best / second : inf;

    // Sub-sample lag by a parabola through the peak and its neighbours.
    double delta = 0.0;
    if (kPk > 0 && kPk < 2 * L) {
        const double y0 = e.sign * cc[kPk - 1], y1 = e.sign * cc[kPk], y2 = e.sign * cc[kPk + 1];
        const double den = y0 - 2.0 * y1 + y2;
        if (den != 0.0) delta = std::max(-0.5, std::min(0.5, 0.5 * (y0 - y2) / den));
    }
    e.lagMs = (kPk - L + delta) * 1000.0 / kAecEstRate;

    const char *why = aecClassify(coverage, e.micDbfs, e.refActive, e.z, e.psr, p);
    e.valid = why == nullptr;
    e.reason = why ? why : "ok";
    return e;
}

// Window end for "now": the older of the two ring ends less 100 ms (the newest audio may
// still be arriving on the other leg). -1 when either ring is empty.
inline int64_t aecLatestWindowEndNs(const PtsRing8k &ref, const PtsRing8k &mic)
{
    const int64_t a = ref.endNs(), b = mic.endNs();
    if (a < 0 || b < 0) return -1;
    return std::min(a, b) - kAecEstEndMarginNs;
}

// Copy the 7.5 s window ending at `endNs` (rounded down to a 10 ms block) from both rings
// over the SAME interval, zero both wherever either is missing, and estimate.
inline OffsetEstimate estimateAt(const PtsRing8k &ref, const PtsRing8k &mic, int64_t endNs,
                                 EstimatorScratch &sc, const EstimatorParams &p = EstimatorParams{})
{
    sc.ensure();
    const int64_t endBlock = endNs / kAecEstBlockNs;
    const int64_t first = endBlock - kAecEstWindowBlocks;
    OffsetEstimate e;
    e.endNs = endBlock * kAecEstBlockNs;
    if (endNs < 0 || first < 0) { e.reason = "coverage"; return e; }
    ref.copyBlocks(first, kAecEstWindowBlocks, sc.refWin.data(), sc.okRef.data());
    mic.copyBlocks(first, kAecEstWindowBlocks, sc.micWin.data(), sc.okMic.data());
    int both = 0;
    for (int b = 0; b < kAecEstWindowBlocks; ++b) {
        if (sc.okRef[b] && sc.okMic[b]) { ++both; continue; }
        std::memset(sc.refWin.data() + static_cast<size_t>(b) * kAecEstBlock, 0, sizeof(float) * kAecEstBlock);
        std::memset(sc.micWin.data() + static_cast<size_t>(b) * kAecEstBlock, 0, sizeof(float) * kAecEstBlock);
    }
    const int64_t end = e.endNs;
    e = estimateOffset(sc.refWin.data(), sc.micWin.data(), kAecEstWindowSamples,
                       static_cast<double>(both) / kAecEstWindowBlocks, sc, p);
    e.endNs = end;
    return e;
}

// ---- tracker -----------------------------------------------------------------------------
// Turns a stream of good windows into a lock. One good window is a candidate; a second
// that agrees AND covers a different 7.5 s (so the two are independent evidence) is a
// lock. A gated window is "no measurement": it neither moves nor drops a lock.
struct TrackerParams {
    double  agreeMs   = 2.0;               // two good windows agree within this
    double  holdMs    = 5.0;               // a good window this close to the lock refines it
    int64_t minSepNs  = kAecEstWindowNs;   // their window ends are at least this far apart
    double  emaAlpha  = 0.3;
    int     nullWindows = 3;               // null windows with an active reference => no coupling
    int64_t candidateMaxAgeNs = 60000000000LL;   // an unconfirmed candidate this old is dropped
};

enum class TrackState { Search, Locked, NoCoupling };
enum class TrackEvent { None, Gated, Candidate, Inconsistent, Locked, Refined, Relocked };

inline const char *trackStateName(TrackState s)
{
    switch (s) {
    case TrackState::Search: return "SEARCH";
    case TrackState::Locked: return "LOCKED";
    case TrackState::NoCoupling: return "NOCOUPLING";
    }
    return "?";
}

class OffsetTracker {
public:
    OffsetTracker() = default;
    explicit OffsetTracker(const TrackerParams &p) : m_p(p) {}

    // One estimate per worker tick, with its window end (int64 ns).
    TrackEvent update(const OffsetEstimate &e, int64_t windowEndNs)
    {
        if (!e.valid) {
            m_reason = e.reason;
            // A null window: the reference was active, the mic was live and complete, and
            // still no echo showed. Other gates say nothing about coupling.
            if (std::strcmp(e.reason, "z-low") == 0 || std::strcmp(e.reason, "psr-low") == 0) ++m_nullStreak;
            return TrackEvent::Gated;
        }
        m_nullStreak = 0;
        m_reason = "ok";
        ++m_good;
        m_lastGoodEnd = windowEndNs;
        m_lastGoodLag = e.lagMs;
        if (m_cand && windowEndNs - m_candEnd > m_p.candidateMaxAgeNs) m_cand = false;

        if (m_locked && closeTo(e.lagMs, m_lag, m_p.holdMs)) {
            m_lag += m_p.emaAlpha * (e.lagMs - m_lag);
            m_cand = false;                                  // the lock is confirmed again
            return TrackEvent::Refined;
        }
        if (!m_cand) return setCandidate(e.lagMs, windowEndNs, TrackEvent::Candidate);
        if (!closeTo(e.lagMs, m_candLag, m_p.agreeMs)) {
            m_reason = "inconsistent";
            return setCandidate(e.lagMs, windowEndNs, TrackEvent::Inconsistent);
        }
        if (windowEndNs - m_candEnd < m_p.minSepNs) return TrackEvent::Candidate;   // same audio: not independent
        const bool relock = m_locked;
        m_lag = 0.5 * (m_candLag + e.lagMs);
        m_locked = true;
        m_cand = false;
        ++m_locks;
        return relock ? TrackEvent::Relocked : TrackEvent::Locked;
    }

    void reset()
    {
        const TrackerParams p = m_p;
        *this = OffsetTracker(p);
    }

    TrackState state() const
    {
        if (m_locked) return TrackState::Locked;
        return m_nullStreak >= m_p.nullWindows ? TrackState::NoCoupling : TrackState::Search;
    }
    bool locked() const { return m_locked; }
    double lagMs() const { return m_lag; }             // meaningful only while locked
    int locks() const { return m_locks; }              // locks plus re-locks so far
    int good() const { return m_good; }
    int nullStreak() const { return m_nullStreak; }
    const char *reason() const { return m_reason; }    // last window: ok | a gate | inconsistent
    int64_t lastGoodEndNs() const { return m_lastGoodEnd; }
    double lastGoodLagMs() const { return m_lastGoodLag; }
    bool hasCandidate() const { return m_cand; }
    double candidateLagMs() const { return m_candLag; }
    const TrackerParams &params() const { return m_p; }

private:
    // Not "near": windows.h defines that (and "far") as empty macros, and this header is
    // included by translation units that include windows.h first.
    static bool closeTo(double a, double b, double tol) { return std::fabs(a - b) <= tol + 1e-6; }
    TrackEvent setCandidate(double lag, int64_t end, TrackEvent ev)
    {
        m_cand = true;
        m_candLag = lag;
        m_candEnd = end;
        return ev;
    }

    TrackerParams m_p{};
    bool    m_locked = false;
    double  m_lag = 0.0;
    bool    m_cand = false;
    double  m_candLag = 0.0;
    int64_t m_candEnd = 0;
    int     m_locks = 0, m_good = 0, m_nullStreak = 0;
    int64_t m_lastGoodEnd = -1;
    double  m_lastGoodLag = 0.0;
    const char *m_reason = "none";
};

}  // namespace talq
