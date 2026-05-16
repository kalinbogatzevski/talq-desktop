#pragma once

#include <QObject>
#include <QSettings>
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
    void saveDevices();
    void restoreDevices();

    Q_PROPERTY(int selectedAudioInput READ selectedAudioInput WRITE setSelectedAudioInput NOTIFY selectedChanged)
    Q_PROPERTY(int selectedAudioOutput READ selectedAudioOutput WRITE setSelectedAudioOutput NOTIFY selectedChanged)
    Q_PROPERTY(int selectedVideoInput READ selectedVideoInput WRITE setSelectedVideoInput NOTIFY selectedChanged)

    int selectedAudioInput() const { return m_selectedInput; }
    int selectedAudioOutput() const { return m_selectedOutput; }
    int selectedVideoInput() const { return m_selectedVideo; }
    void setSelectedAudioInput(int idx);
    void setSelectedAudioOutput(int idx);
    void setSelectedVideoInput(int idx);

    // Get selected device name for pipeline configuration
    QString selectedInputName() const;
    QString selectedOutputName() const;

    // Get selected device ID (strid/path) for pipeline configuration
    QString selectedInputDeviceId() const;
    QString selectedOutputDeviceId() const;

    QStringList audioInputNames() const;
    QStringList audioOutputNames() const;
    QStringList videoInputNames() const;

    const QVector<MediaDevice> &audioInputs() const { return m_audioInputs; }
    const QVector<MediaDevice> &audioOutputs() const { return m_audioOutputs; }
    const QVector<MediaDevice> &videoInputs() const { return m_videoInputs; }

signals:
    void devicesChanged();
    void selectedChanged();

private:
    int m_selectedInput = -1;   // -1 = system default
    int m_selectedOutput = -1;
    int m_selectedVideo = -1;
    QVector<MediaDevice> m_audioInputs;
    QVector<MediaDevice> m_audioOutputs;
    QVector<MediaDevice> m_videoInputs;
    QSettings m_settings;
    bool m_restoring = false;  // suppress saveDevices() during restoreDevices()
};
