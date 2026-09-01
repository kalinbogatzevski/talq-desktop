#include "core/AudioPlayer.h"

#include <QDebug>
#include <QFileInfo>
#include <QUrl>

namespace {
// 100 ms is the coarsest tick where a progress bar still reads as moving
// rather than stepping. It also bounds how stale the bus is, since errors and
// end-of-stream are drained here rather than from a GLib main loop.
constexpr int kTickMs = 100;
}

AudioPlayer::AudioPlayer(QObject *parent) : QObject(parent)
{
    m_timer.setInterval(kTickMs);
    connect(&m_timer, &QTimer::timeout, this, &AudioPlayer::tick);
}

AudioPlayer::~AudioPlayer()
{
    teardown();
}

void AudioPlayer::play(int fileId, const QString &localPath)
{
    // Same clip: this is a resume, not a restart. Clicking a paused bubble
    // must not throw away where the listener had got to.
    if (m_pipeline && fileId == m_fileId && m_path == localPath) {
        gst_element_set_state(m_pipeline, GST_STATE_PLAYING);
        m_playing = true;
        m_timer.start();
        emit changed();
        return;
    }

    teardown();

    if (!QFileInfo::exists(localPath)) {
        emit failed(fileId, tr("The audio file is no longer on disk."));
        return;
    }

    m_pipeline = gst_element_factory_make("playbin", "talq-audio");
    if (!m_pipeline) {
        // playbin lives in gst-plugins-base; if it is missing the deployment
        // is broken in a way no retry fixes.
        emit failed(fileId, tr("Audio playback is unavailable in this build."));
        return;
    }

    const QByteArray uri = QUrl::fromLocalFile(localPath).toEncoded();
    g_object_set(m_pipeline, "uri", uri.constData(), nullptr);
    // playbin would otherwise open a video window for a file that turns out to
    // carry a video track. This is an audio surface; drop anything else.
    g_object_set(m_pipeline, "video-sink",
                 gst_element_factory_make("fakesink", nullptr), nullptr);

    m_fileId = fileId;
    m_path = localPath;
    m_positionMs = 0;
    m_durationMs = 0;

    if (gst_element_set_state(m_pipeline, GST_STATE_PLAYING)
        == GST_STATE_CHANGE_FAILURE) {
        teardown();
        emit failed(fileId, tr("Could not play this audio file."));
        return;
    }

    m_playing = true;
    m_timer.start();
    emit changed();
}

void AudioPlayer::togglePause()
{
    if (!m_pipeline)
        return;
    m_playing = !m_playing;
    gst_element_set_state(m_pipeline, m_playing ? GST_STATE_PLAYING : GST_STATE_PAUSED);
    if (m_playing) m_timer.start();
    else           m_timer.stop();
    emit changed();
}

void AudioPlayer::stop()
{
    if (!m_pipeline)
        return;
    teardown();
    emit changed();
}

void AudioPlayer::seekFraction(double fraction)
{
    if (!m_pipeline || m_durationMs <= 0)
        return;
    fraction = qBound(0.0, fraction, 1.0);
    const gint64 target = gint64(fraction * double(m_durationMs)) * GST_MSECOND;
    gst_element_seek_simple(m_pipeline, GST_FORMAT_TIME,
                            GstSeekFlags(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT),
                            target);
    m_positionMs = target / GST_MSECOND;
    emit changed();
}

void AudioPlayer::teardown()
{
    m_timer.stop();
    if (m_pipeline) {
        gst_element_set_state(m_pipeline, GST_STATE_NULL);
        gst_object_unref(m_pipeline);
        m_pipeline = nullptr;
    }
    m_playing = false;
    m_fileId = 0;
    m_path.clear();
    m_positionMs = 0;
    m_durationMs = 0;
}

void AudioPlayer::tick()
{
    if (!m_pipeline)
        return;

    // Drain the bus here rather than from a GLib main loop: this app runs a Qt
    // event loop, so gst_bus_add_watch's GSource would never be dispatched.
    // MediaDeviceManager notes the same constraint for its device monitor.
    if (GstBus *bus = gst_element_get_bus(m_pipeline)) {
        while (GstMessage *msg = gst_bus_pop_filtered(
                   bus, GstMessageType(GST_MESSAGE_ERROR | GST_MESSAGE_EOS))) {
            const bool isErr = GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR;
            QString reason;
            if (isErr) {
                GError *err = nullptr;
                gchar *dbg = nullptr;
                gst_message_parse_error(msg, &err, &dbg);
                reason = err ? QString::fromUtf8(err->message) : tr("Playback error");
                qWarning() << "AudioPlayer error:" << reason << (dbg ? dbg : "");
                if (err) g_error_free(err);
                g_free(dbg);
            }
            gst_message_unref(msg);
            const int id = m_fileId;
            teardown();
            gst_object_unref(bus);
            if (isErr)
                emit failed(id, reason);
            emit changed();
            return;
        }
        gst_object_unref(bus);
    }

    gint64 pos = 0, dur = 0;
    if (gst_element_query_position(m_pipeline, GST_FORMAT_TIME, &pos))
        m_positionMs = pos / GST_MSECOND;
    if (gst_element_query_duration(m_pipeline, GST_FORMAT_TIME, &dur))
        m_durationMs = dur / GST_MSECOND;

    emit changed();
}
