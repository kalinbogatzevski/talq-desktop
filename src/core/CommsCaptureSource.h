#pragma once
// CommsCaptureSource — own the capture IAudioClient so we can ask Windows for the
// COMMUNICATIONS audio category, which is the only way to engage the endpoint's
// built-in acoustic echo canceller.
//
// WHY THIS EXISTS (measured, not assumed — 2026-08-11)
// ----------------------------------------------------
// Probing KALIN-ZENBOOK's Realtek mic array with IAudioEffectsManager, same endpoint
// and same shared-mode mix format, changing ONLY the client category:
//
//     default category        ->  0 effects, no AEC
//     communications category ->  3 effects, AEC PRESENT and ON (canSetState=no)
//
// So the laptop has a working hardware echo canceller that exists only in the
// communications category. And no GStreamer capture element ever asks for it:
// `SetClientProperties` appears exactly ONCE in all of GStreamer (a vendored header
// in the old `sys/wasapi` plugin, used for AudioClient3 low-latency periods, not for
// a category), `wasapi2` never calls it, and `wasapisrc`'s `role` property is a red
// herring — gst_wasapi_device_role_to_erole() maps it to ERole for
// GetDefaultAudioEndpoint(), i.e. WHICH DEFAULT DEVICE, not the stream category.
//
// Net: TalQ runs its mic in the default category, so the machine's own echo canceller
// is switched off for us, while any client that sets the category gets it for free.
// That is a concrete mechanism for "same laptop, other clients have no echo problem".
//
// wasapi2src exposes no category property, so this cannot be fixed from the element
// API — owning the client is the only route. That same ownership is also the
// prerequisite for the other two approaches, so none of this work is wasted:
//   * IAcousticEchoCancellationControl::SetEchoCancellationRenderEndpoint on hardware
//     that supports it (E_NOINTERFACE on the Realtek array, but not universally);
//   * driving AEC3 directly with REAL device delays instead of webrtcechoprobe's
//     (running_time + pipeline_latency) guess.
//
// WHY THE ALIGNMENT PROBLEM DISAPPEARS ON THIS PATH
// -------------------------------------------------
// Every previous attempt fought the render->capture delay because webrtcdsp subtracts
// a reference we have to align ourselves. Here the OS cancels the echo BEFORE the
// samples reach us, using its own knowledge of both endpoints. There is no reference
// to align and no delay to estimate — which is precisely why this is worth trying
// ahead of any further work on the webrtcdsp chain.
//
// SCOPE: Windows only. Opt-in via TALQ_MIC=comms; default behaviour is unchanged.

#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <QByteArray>
#include <QDebug>
#include <QString>

#ifdef Q_OS_WIN
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <cmath>
#include <atomic>
#include <thread>

namespace talq {

// A capture client pinned to AudioCategory_Communications, pumping into an appsrc.
// Deliberately minimal: no format negotiation games, no resampling. It publishes the
// device's own mix format and lets the existing audioconvert/audioresample/capsfilter
// chain do what it already does for wasapi2src.
class CommsCaptureSource
{
public:
    ~CommsCaptureSource() { stop(); }

    // deviceId: WASAPI endpoint id, or empty for the default communications capture
    // endpoint. Returns false and changes nothing on any failure — every caller must
    // be able to fall back to wasapi2src.
    bool start(GstElement *appsrc, const QString &deviceId)
    {
        if (m_running.load()) return true;
        if (!appsrc) return false;

        if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {
            // Already initialised on this thread with another model is fine; a hard
            // failure is not. Both surface as a failed Activate below anyway.
        }

        IMMDeviceEnumerator *en = nullptr;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                    __uuidof(IMMDeviceEnumerator),
                                    reinterpret_cast<void **>(&en))) || !en) {
            qWarning() << "CommsCaptureSource: no device enumerator";
            return false;
        }

        IMMDevice *dev = nullptr;
        HRESULT hr;
        if (deviceId.isEmpty()) {
            // eCommunications picks the endpoint Windows considers the comms mic —
            // the same one the OS effect chain is configured for.
            hr = en->GetDefaultAudioEndpoint(eCapture, eCommunications, &dev);
        } else {
            hr = en->GetDevice(reinterpret_cast<LPCWSTR>(deviceId.utf16()), &dev);
        }
        en->Release();
        if (FAILED(hr) || !dev) {
            qWarning() << "CommsCaptureSource: capture endpoint unavailable hr=" << Qt::hex << hr;
            return false;
        }

        hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_INPROC_SERVER, nullptr,
                           reinterpret_cast<void **>(&m_client));
        if (FAILED(hr) || !m_client) { dev->Release(); return fail("Activate(IAudioClient)", hr); }
        dev->Release();

        // THE POINT OF THIS CLASS. Must precede Initialize; the category selects which
        // APO effect chain Windows applies, and the AEC effect lives only in the
        // communications chain. If IAudioClient2 is missing we are on an OS too old
        // to have the effect anyway, so bail rather than capture without AEC and
        // silently look like the wasapi2src path we are trying to improve on.
        {
            IAudioClient2 *c2 = nullptr;
            if (SUCCEEDED(m_client->QueryInterface(__uuidof(IAudioClient2),
                                                   reinterpret_cast<void **>(&c2))) && c2) {
                AudioClientProperties props{};
                props.cbSize     = sizeof(props);
                props.bIsOffload = FALSE;
                props.eCategory  = AudioCategory_Communications;
                HRESULT hp = c2->SetClientProperties(&props);
                c2->Release();
                if (FAILED(hp)) return fail("SetClientProperties(Communications)", hp);
            } else {
                return fail("no IAudioClient2 (cannot request the communications category)", E_NOINTERFACE);
            }
        }

        hr = m_client->GetMixFormat(&m_format);
        if (FAILED(hr) || !m_format) return fail("GetMixFormat", hr);

        // Event-driven shared mode. 20 ms buffer: enough to absorb scheduling jitter
        // without adding latency the AEC would have to model.
        hr = m_client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                  AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                  200'000 /*20ms in 100ns units*/, 0, m_format, nullptr);
        if (FAILED(hr)) return fail("Initialize", hr);

        m_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!m_event) return fail("CreateEvent", E_FAIL);
        if (FAILED(hr = m_client->SetEventHandle(m_event))) return fail("SetEventHandle", hr);

        if (FAILED(hr = m_client->GetService(__uuidof(IAudioCaptureClient),
                                             reinterpret_cast<void **>(&m_capture))))
            return fail("GetService(IAudioCaptureClient)", hr);

        applyCaps(appsrc);

        if (FAILED(hr = m_client->Start())) return fail("Start", hr);

        m_appsrc = GST_ELEMENT(gst_object_ref(appsrc));
        m_running.store(true);
        m_thread = std::thread([this] { pump(); });

        qInfo().nospace() << "CommsCaptureSource: capturing in AudioCategory_Communications ("
                          << m_format->nSamplesPerSec << " Hz, " << m_format->nChannels
                          << " ch, " << m_format->wBitsPerSample << " bit) — the OS effect "
                             "chain, including any endpoint AEC, is now engaged";
        return true;
    }

    void stop()
    {
        if (!m_running.exchange(false)) { release(); return; }
        if (m_event) SetEvent(m_event);              // wake the pump so it can exit
        if (m_thread.joinable()) m_thread.join();
        if (m_client) m_client->Stop();
        release();
    }

    bool running() const { return m_running.load(); }

private:
    bool fail(const char *what, HRESULT hr)
    {
        qWarning().nospace() << "CommsCaptureSource: " << what << " failed hr=0x"
                             << QString::number((quint32)hr, 16)
                             << " — falling back to the default capture path";
        release();
        return false;
    }

    void release()
    {
        if (m_capture) { m_capture->Release(); m_capture = nullptr; }
        if (m_client)  { m_client->Release();  m_client  = nullptr; }
        if (m_format)  { CoTaskMemFree(m_format); m_format = nullptr; }
        if (m_event)   { CloseHandle(m_event);  m_event  = nullptr; }
        if (m_appsrc)  { gst_object_unref(m_appsrc); m_appsrc = nullptr; }
    }

    // Publish the device's own format; downstream audioconvert/audioresample already
    // normalises to the 48k/S16/mono the encoder and webrtcdsp want.
    void applyCaps(GstElement *appsrc)
    {
        // Deliberately NOT IsEqualGUID(..., KSDATAFORMAT_SUBTYPE_IEEE_FLOAT).
        //
        // On MSVC that name resolves to a compile-time __uuidof; MinGW-w64 leaves it a
        // plain external data symbol living in libksguid.a / libksuser.a, which is NOT
        // libuuid.a (already on our link line). Referencing it therefore compiles fine
        // and fails at LINK with an undefined reference — which is exactly what it did.
        //
        // Adding -lksguid for one GUID would put a new link dependency in the build for
        // every target that touches this header. Unnecessary: every KSDATAFORMAT_SUBTYPE_*
        // GUID has the fixed shape {0000xxxx-0000-0010-8000-00AA00389B71} where Data1 IS
        // the wave format tag. Comparing Data1 to WAVE_FORMAT_IEEE_FLOAT is the same test
        // with no symbol to resolve.
        const bool isFloat =
            m_format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
            (m_format->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
             m_format->cbSize >= 22 &&
             reinterpret_cast<WAVEFORMATEXTENSIBLE *>(m_format)->SubFormat.Data1
                 == WAVE_FORMAT_IEEE_FLOAT);
        const char *fmt = isFloat ? "F32LE"
                        : (m_format->wBitsPerSample == 16 ? "S16LE" : "S32LE");

        GstCaps *caps = gst_caps_new_simple(
            "audio/x-raw",
            "format",   G_TYPE_STRING, fmt,
            "rate",     G_TYPE_INT,    (gint)m_format->nSamplesPerSec,
            "channels", G_TYPE_INT,    (gint)m_format->nChannels,
            "layout",   G_TYPE_STRING, "interleaved",
            nullptr);

        // CHANNEL MASK IS MANDATORY ABOVE 2 CHANNELS.
        //
        // The Realtek mic array's mix format is 4 channels. Without a mask GStreamer
        // cannot derive channel positions, so audioconvert refuses to negotiate and
        // the whole publish branch dies with "not-negotiated (-4)" the moment it is
        // linked — which is exactly what the first run did. Reproduced offline:
        //   channels=4, no mask            -> FAIL, not-negotiated
        //   channels=4, channel-mask=0x33  -> PASS, negotiates through to S16LE mono
        //   channels=4, channel-mask=0x0   -> FAIL, audioconvert will not downmix
        //                                    UNPOSITIONED channels to mono
        //
        // Prefer the device's OWN dwChannelMask: WAVEFORMATEXTENSIBLE's mask and
        // GStreamer's channel-mask share the SPEAKER_* bit layout for the standard
        // positions, so it is the same value and describes this microphone honestly.
        //
        // Some endpoints (this one included, per gst-device-monitor) report a ZERO
        // mask, meaning "unpositioned" — which downmix rejects. Falling back to a
        // conventional positioned layout for the channel count is what makes those
        // usable, and is harmless: downstream only ever wants mono, so the positions
        // just have to be valid, not physically exact.
        if (m_format->nChannels > 2) {
            guint64 mask = 0;
            if (m_format->wFormatTag == WAVE_FORMAT_EXTENSIBLE && m_format->cbSize >= 22)
                mask = reinterpret_cast<WAVEFORMATEXTENSIBLE *>(m_format)->dwChannelMask;
            if (mask == 0) {
                switch (m_format->nChannels) {
                    case 4:  mask = 0x33;  break;   // FL FR RL RR  (quad)
                    case 6:  mask = 0x3F;  break;   // 5.1
                    case 8:  mask = 0x63F; break;   // 7.1
                    default:                        // N front channels, arbitrary but positioned
                        mask = (m_format->nChannels >= 64)
                             ? ~G_GUINT64_CONSTANT(0)
                             : ((G_GUINT64_CONSTANT(1) << m_format->nChannels) - 1);
                        break;
                }
                qInfo() << "CommsCaptureSource: device reports an unpositioned channel mask;"
                        << "using a conventional layout for" << m_format->nChannels
                        << "channels so audioconvert can downmix";
            }
            gst_caps_set_simple(caps, "channel-mask", GST_TYPE_BITMASK, mask, nullptr);
        }
        g_object_set(appsrc, "caps", caps, "format", GST_FORMAT_TIME,
                     "is-live", TRUE, "do-timestamp", TRUE, nullptr);
        gst_caps_unref(caps);
        m_bytesPerFrame = m_format->nBlockAlign;
        m_rawIsFloat    = isFloat;   // for the raw-source level instrument
    }

    void pump()
    {
        while (m_running.load()) {
            if (WaitForSingleObject(m_event, 200) != WAIT_OBJECT_0) continue;
            if (!m_running.load()) break;

            UINT32 packet = 0;
            while (SUCCEEDED(m_capture->GetNextPacketSize(&packet)) && packet > 0) {
                BYTE *data = nullptr; UINT32 frames = 0; DWORD flags = 0;
                if (FAILED(m_capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr)))
                    break;

                const gsize bytes = (gsize)frames * m_bytesPerFrame;
                if (bytes > 0) {
                    // RAW-SOURCE INSTRUMENT — settles "is the OS cancelling, or is the
                    // capture dead?". Downstream those look identical: an A/B on
                    // 2026-08-11 saw the comms arm go ~100% digitally silent while the
                    // far end held at -24 dB, which is equally consistent with a superb
                    // canceller and with a stopped stream. Measuring HERE, before a
                    // single GStreamer element, tells them apart: silence at this point
                    // means the OS chain produced silence, and anything non-silent here
                    // with silence downstream means we broke it ourselves.
                    if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT))
                        accumulateRawLevel(data, frames);
                    else
                        m_rawSilentFlagged++;
                    maybeLogRawLevel();
                }
                if (bytes > 0) {
                    GstBuffer *buf = gst_buffer_new_allocate(nullptr, bytes, nullptr);
                    // AUDCLNT_BUFFERFLAGS_SILENT means "treat as silence"; the memory
                    // itself may hold anything, so it must be zeroed rather than copied.
                    if (flags & AUDCLNT_BUFFERFLAGS_SILENT)
                        gst_buffer_memset(buf, 0, 0, bytes);
                    else
                        gst_buffer_fill(buf, 0, data, bytes);
                    GstFlowReturn fr = GST_FLOW_OK;
                    g_signal_emit_by_name(m_appsrc, "push-buffer", buf, &fr);
                    gst_buffer_unref(buf);
                    if (fr != GST_FLOW_OK && fr != GST_FLOW_FLUSHING) {
                        qWarning() << "CommsCaptureSource: push-buffer returned" << fr;
                        m_running.store(false);
                    }
                }
                m_capture->ReleaseBuffer(frames);
                if (!m_running.load()) break;
            }
        }
    }

    // Peak-of-|sample| over the raw device buffers, reported ~every 2 s. Peak rather
    // than RMS on purpose: it cannot be dragged down by silent frames the way an
    // average can, which is the exact artifact that made the downstream numbers
    // untrustworthy in the first place.
    void accumulateRawLevel(const BYTE *data, UINT32 frames)
    {
        const UINT32 ch = m_format->nChannels ? m_format->nChannels : 1;
        const UINT32 n  = frames * ch;
        double peak = 0.0;
        if (m_rawIsFloat) {
            const float *f = reinterpret_cast<const float *>(data);
            for (UINT32 i = 0; i < n; ++i) { const double a = std::fabs((double)f[i]); if (a > peak) peak = a; }
        } else if (m_format->wBitsPerSample == 16) {
            const gint16 *p = reinterpret_cast<const gint16 *>(data);
            for (UINT32 i = 0; i < n; ++i) { const double a = std::fabs((double)p[i]) / 32768.0; if (a > peak) peak = a; }
        } else if (m_format->wBitsPerSample == 32) {
            const gint32 *p = reinterpret_cast<const gint32 *>(data);
            for (UINT32 i = 0; i < n; ++i) { const double a = std::fabs((double)p[i]) / 2147483648.0; if (a > peak) peak = a; }
        }
        if (peak > m_rawPeak) m_rawPeak = peak;
        ++m_rawBlocks;
        if (peak <= 0.0) ++m_rawSilentBlocks;
    }

    void maybeLogRawLevel()
    {
        const gint64 now = g_get_monotonic_time();
        if (m_rawLastLogUs == 0) { m_rawLastLogUs = now; return; }
        if (now - m_rawLastLogUs < 2'000'000) return;
        const double db = (m_rawPeak > 0.0) ? 20.0 * std::log10(m_rawPeak) : -999.0;
        qInfo().nospace() << "CommsCaptureSource: RAW device level peak=" << db
                          << " dBFS over " << m_rawBlocks << " block(s); "
                          << m_rawSilentBlocks << " all-zero, "
                          << m_rawSilentFlagged << " WASAPI-SILENT-flagged"
                          << "  [raw silence here => the OS chain produced it, not us]";
        m_rawPeak = 0.0; m_rawBlocks = 0; m_rawSilentBlocks = 0; m_rawSilentFlagged = 0;
        m_rawLastLogUs = now;
    }

    bool    m_rawIsFloat       = false;
    double  m_rawPeak          = 0.0;
    quint64 m_rawBlocks        = 0;
    quint64 m_rawSilentBlocks  = 0;
    quint64 m_rawSilentFlagged = 0;
    gint64  m_rawLastLogUs     = 0;

    IAudioClient        *m_client  = nullptr;
    IAudioCaptureClient *m_capture = nullptr;
    WAVEFORMATEX        *m_format  = nullptr;
    HANDLE               m_event   = nullptr;
    GstElement          *m_appsrc  = nullptr;
    UINT32               m_bytesPerFrame = 4;
    std::atomic<bool>    m_running{false};
    std::thread          m_thread;
};

} // namespace talq

#endif // Q_OS_WIN
