#include "SharedFarEndBus.h"

#include <QDebug>

namespace {
// The single, pinned far-end format. webrtcechoprobe and the publisher's
// webrtcdsp MUST agree on sample rate; we pin 48k S16LE MONO everywhere on the
// AEC path (mono also side-steps the gst-plugins-bad #3220 stereo-collapse bug
// on webrtc-audio-processing 1.x).
constexpr const char *kFarCaps =
    "audio/x-raw,rate=48000,format=S16LE,channels=1,layout=interleaved";
}

SharedFarEndBus::SharedFarEndBus(QObject *parent) : QObject(parent) {}

SharedFarEndBus::~SharedFarEndBus()
{
    stop();
}

bool SharedFarEndBus::start()
{
    if (m_pipeline)
        return m_ready;

    // Gate on the two load-bearing elements first. If either is missing the
    // whole design is impossible — return false so the caller keeps AEC off.
    GstElement *probe = gst_element_factory_make("webrtcechoprobe", kProbeName);
    GstElement *mixer = gst_element_factory_make("audiomixer", "talq-far-mix");
    GstElement *caps  = gst_element_factory_make("capsfilter", "talq-far-caps");
    GstElement *queue = gst_element_factory_make("queue", "talq-far-q");
    GstElement *sink  = gst_element_factory_make("fakesink", "talq-far-sink");
    GstElement *ka    = gst_element_factory_make("audiotestsrc", "talq-far-keepalive");
    GstElement *kaCaps= gst_element_factory_make("capsfilter", "talq-far-ka-caps");

    if (!probe || !mixer || !caps || !queue || !sink || !ka || !kaCaps) {
        qWarning() << "SharedFarEndBus: required element unavailable "
                      "(webrtcechoprobe/audiomixer/...); AEC disabled";
        // None added to a bin yet — drop the floating refs we hold.
        for (GstElement *e : {probe, mixer, caps, queue, sink, ka, kaCaps})
            if (e) gst_object_unref(e);
        return false;
    }

    m_pipeline = gst_pipeline_new("talq-far-bus");

    // Pin a steady real clock; the probe's far-end play-time math must not run
    // on a free-running pipeline (docs §3.3 / aec-design.md §3.4).
    GstClock *clk = gst_system_clock_obtain();
    gst_pipeline_use_clock(GST_PIPELINE(m_pipeline), clk);
    gst_object_unref(clk);

    GstCaps *c = gst_caps_from_string(kFarCaps);
    g_object_set(caps, "caps", c, nullptr);
    g_object_set(kaCaps, "caps", c, nullptr);
    gst_caps_unref(c);

    // Silent keepalive so the mixer always produces output (probe fed at N=0).
    gst_util_set_object_arg(G_OBJECT(ka), "wave", "silence");
    g_object_set(ka, "is-live", TRUE, nullptr);
    // fakesink at real-time so the probe sees the far-end on the wall clock,
    // mirroring the real wasapi2sink timeline; no preroll (live pipeline).
    g_object_set(sink, "sync", TRUE, "async", FALSE,
                 "enable-last-sample", FALSE, "silent", TRUE, nullptr);

    gst_bin_add_many(GST_BIN(m_pipeline),
                     ka, kaCaps, mixer, caps, probe, queue, sink, nullptr);

    bool ok = gst_element_link(ka, kaCaps);
    // keepalive → a mixer request pad
    if (ok) {
        GstPad *src = gst_element_get_static_pad(kaCaps, "src");
        GstPad *mp  = gst_element_request_pad_simple(mixer, "sink_%u");
        ok = src && mp && (gst_pad_link(src, mp) == GST_PAD_LINK_OK);
        if (src) gst_object_unref(src);
        if (mp)  gst_object_unref(mp);   // mixer keeps the pad; freed at teardown
    }
    // mixer → caps → probe → queue → fakesink
    if (ok)
        ok = gst_element_link_many(mixer, caps, probe, queue, sink, nullptr);

    if (!ok) {
        qWarning() << "SharedFarEndBus: link failed; AEC disabled";
        gst_element_set_state(m_pipeline, GST_STATE_NULL);
        gst_object_unref(m_pipeline);
        m_pipeline = nullptr;
        return false;
    }

    m_mixer = mixer;
    m_probe = probe;
    m_sink  = sink;

    if (gst_element_set_state(m_pipeline, GST_STATE_PLAYING)
            == GST_STATE_CHANGE_FAILURE) {
        qWarning() << "SharedFarEndBus: pipeline failed to start; AEC disabled";
        gst_element_set_state(m_pipeline, GST_STATE_NULL);
        gst_object_unref(m_pipeline);
        m_pipeline = nullptr;
        m_mixer = m_probe = m_sink = nullptr;
        return false;
    }

    m_ready = true;
    qInfo() << "SharedFarEndBus: probe" << kProbeName
            << "registered + bus PLAYING (AEC far-end reference up)";
    return true;
}

void SharedFarEndBus::stop()
{
    // Detach any peers still attached (defensive — CallManager normally tears
    // subscribers down first). Snapshot keys under the lock, then detach.
    QStringList ids;
    {
        QMutexLocker lk(&m_mutex);
        ids = m_peers.keys();
    }
    for (const QString &id : ids)
        detach(id);

    if (m_pipeline) {
        gst_element_set_state(m_pipeline, GST_STATE_NULL);
        gst_object_unref(m_pipeline);
        m_pipeline = nullptr;
    }
    m_mixer = m_probe = m_sink = nullptr;
    m_ready = false;
}

void SharedFarEndBus::attach(const QString &peerId)
{
    if (!m_ready || !m_pipeline || !m_mixer)
        return;

    QMutexLocker lk(&m_mutex);
    if (m_peers.contains(peerId))
        return;

    GstElement *appsrc = gst_element_factory_make("appsrc", nullptr);
    GstElement *queue  = gst_element_factory_make("queue", nullptr);
    if (!appsrc || !queue) {
        if (appsrc) gst_object_unref(appsrc);
        if (queue)  gst_object_unref(queue);
        qWarning() << "SharedFarEndBus: attach failed (appsrc/queue) for"
                   << peerId.left(20);
        return;
    }

    GstCaps *c = gst_caps_from_string(kFarCaps);
    g_object_set(appsrc,
                 "is-live", TRUE,
                 "format", GST_FORMAT_TIME,
                 "do-timestamp", TRUE,
                 "caps", c,
                 "max-bytes", (guint64)(1u << 20),
                 "block", FALSE,
                 nullptr);
    gst_caps_unref(c);
    // Drop the oldest buffer rather than ever blocking the subscriber's
    // streaming thread; a little latency is fine, a stall is not.
    g_object_set(queue,
                 "leaky", 2,                                   // downstream
                 "max-size-time", (guint64)(200 * GST_MSECOND),
                 "max-size-buffers", (guint)0,
                 "max-size-bytes", (guint)0,
                 nullptr);

    gst_bin_add_many(GST_BIN(m_pipeline), appsrc, queue, nullptr);

    GstPad *mp = gst_element_request_pad_simple(m_mixer, "sink_%u");
    GstPad *qsrc = gst_element_get_static_pad(queue, "src");
    bool ok = mp && qsrc
              && gst_element_link(appsrc, queue)
              && (gst_pad_link(qsrc, mp) == GST_PAD_LINK_OK);
    if (qsrc) gst_object_unref(qsrc);

    if (!ok) {
        qWarning() << "SharedFarEndBus: attach link failed for" << peerId.left(20);
        if (mp) {
            gst_element_release_request_pad(m_mixer, mp);
            gst_object_unref(mp);
        }
        gst_element_set_state(appsrc, GST_STATE_NULL);
        gst_element_set_state(queue, GST_STATE_NULL);
        gst_bin_remove_many(GST_BIN(m_pipeline), appsrc, queue, nullptr);
        return;
    }

    gst_element_sync_state_with_parent(appsrc);
    gst_element_sync_state_with_parent(queue);

    Peer p;
    p.appsrc   = appsrc;
    p.queue    = queue;
    p.mixerPad = mp;        // keep the ref; released in detach
    m_peers.insert(peerId, p);
    qInfo() << "SharedFarEndBus: attached far-end input for" << peerId.left(20);
}

void SharedFarEndBus::detach(const QString &peerId)
{
    Peer p;
    {
        QMutexLocker lk(&m_mutex);
        auto it = m_peers.find(peerId);
        if (it == m_peers.end())
            return;
        p = it.value();
        m_peers.erase(it);   // no further pushSample can find it now
    }

    if (p.appsrc) {
        gst_app_src_end_of_stream(GST_APP_SRC(p.appsrc));
        gst_element_set_state(p.appsrc, GST_STATE_NULL);
    }
    if (p.queue)
        gst_element_set_state(p.queue, GST_STATE_NULL);
    if (p.mixerPad && m_mixer) {
        gst_element_release_request_pad(m_mixer, p.mixerPad);
        gst_object_unref(p.mixerPad);
    }
    if (m_pipeline) {
        if (p.appsrc) gst_bin_remove(GST_BIN(m_pipeline), p.appsrc);
        if (p.queue)  gst_bin_remove(GST_BIN(m_pipeline), p.queue);
    }
    qInfo() << "SharedFarEndBus: detached far-end input for" << peerId.left(20);
}

void SharedFarEndBus::pushSample(const QString &peerId, GstSample *sample)
{
    if (!sample)
        return;
    // Held across the push so detach() can't free the appsrc mid-push.
    QMutexLocker lk(&m_mutex);
    auto it = m_peers.find(peerId);
    if (it == m_peers.end() || !it->appsrc)
        return;
    gst_app_src_push_sample(GST_APP_SRC(it->appsrc), sample);
}
