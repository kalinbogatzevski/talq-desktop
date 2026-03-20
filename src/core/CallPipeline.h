#pragma once

#include <QObject>
#include <gst/gst.h>

class CallPipeline : public QObject
{
    Q_OBJECT

public:
    explicit CallPipeline(QObject *parent = nullptr);
    ~CallPipeline() override;

    bool startAudioLoopback();
    void stop();

    bool isRunning() const { return m_running; }

signals:
    void error(const QString &message);

private:
    GstElement *m_pipeline = nullptr;
    bool m_running = false;

    void cleanup();
};
