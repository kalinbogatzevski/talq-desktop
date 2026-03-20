#include "core/CallPipeline.h"
#include <QDebug>

CallPipeline::CallPipeline(QObject *parent)
    : QObject(parent)
{
}

CallPipeline::~CallPipeline()
{
    stop();
}

bool CallPipeline::startAudioLoopback()
{
    if (m_running) {
        qWarning() << "CallPipeline: already running";
        return false;
    }

    m_pipeline = gst_parse_launch(
        "wasapi2src ! audioconvert ! audioresample ! wasapi2sink",
        nullptr
    );

    if (!m_pipeline) {
        emit error("Failed to create audio loopback pipeline");
        return false;
    }

    GstStateChangeReturn ret = gst_element_set_state(m_pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        emit error("Failed to start audio loopback pipeline");
        cleanup();
        return false;
    }

    m_running = true;
    qDebug() << "CallPipeline: audio loopback started";
    return true;
}

void CallPipeline::stop()
{
    if (!m_running)
        return;

    cleanup();
    m_running = false;
    qDebug() << "CallPipeline: stopped";
}

void CallPipeline::cleanup()
{
    if (m_pipeline) {
        gst_element_set_state(m_pipeline, GST_STATE_NULL);
        gst_object_unref(m_pipeline);
        m_pipeline = nullptr;
    }
}
