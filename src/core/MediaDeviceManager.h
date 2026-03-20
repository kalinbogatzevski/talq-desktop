#pragma once

#include <QObject>
#include <QStringList>
#include <QVector>
#include <gst/gst.h>

struct MediaDevice {
    QString id;
    QString name;
    QString type;  // "audio-input", "audio-output", "video-input"
};

class MediaDeviceManager : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QStringList audioInputNames READ audioInputNames NOTIFY devicesChanged)
    Q_PROPERTY(QStringList audioOutputNames READ audioOutputNames NOTIFY devicesChanged)
    Q_PROPERTY(QStringList videoInputNames READ videoInputNames NOTIFY devicesChanged)

public:
    explicit MediaDeviceManager(QObject *parent = nullptr);
    ~MediaDeviceManager() override;

    Q_INVOKABLE void refresh();

    QStringList audioInputNames() const;
    QStringList audioOutputNames() const;
    QStringList videoInputNames() const;

    const QVector<MediaDevice> &audioInputs() const { return m_audioInputs; }
    const QVector<MediaDevice> &audioOutputs() const { return m_audioOutputs; }
    const QVector<MediaDevice> &videoInputs() const { return m_videoInputs; }

signals:
    void devicesChanged();

private:
    QVector<MediaDevice> m_audioInputs;
    QVector<MediaDevice> m_audioOutputs;
    QVector<MediaDevice> m_videoInputs;
};
