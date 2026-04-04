#pragma once

#include <QDialog>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QImage>
#include <QPixmap>
#include "core/CallManager.h"
#include "core/ApiClient.h"

class VideoWidget : public QWidget
{
    Q_OBJECT
public:
    explicit VideoWidget(QWidget *parent = nullptr) : QWidget(parent) {
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        setMinimumSize(160, 120);
    }
    void setImage(const QImage &img) {
        // Pre-scale to widget size to avoid expensive scaling in paintEvent
        if (!img.isNull() && (img.width() > width() * 2 || img.height() > height() * 2)) {
            m_image = img.scaled(size(), Qt::KeepAspectRatio, Qt::FastTransformation);
        } else {
            m_image = img;
        }
        update();
    }
protected:
    void paintEvent(QPaintEvent *) override;
private:
    QImage m_image;
};

class CallDialog : public QDialog
{
    Q_OBJECT

public:
    explicit CallDialog(CallManager *callManager, ApiClient *api, QWidget *parent = nullptr);

private slots:
    void onStateChanged();
    void onDurationChanged();
    void onMuteChanged();
    void onCameraChanged();
    void onRemoteFrame(const QImage &image);

private:
    void buildUi();
    void showIncomingMode();
    void showActiveMode();
    void connectVideoProviders();
    QString formatDuration(int seconds) const;

    CallManager *m_callManager;
    ApiClient *m_api;
    void fetchAvatar(const QString &userId);

    // UI elements
    QLabel *m_avatarLabel = nullptr;
    QLabel *m_peerLabel = nullptr;
    QPixmap m_avatarPixmap;
    QString m_loadedAvatarUserId;
    QLabel *m_stateLabel = nullptr;
    QLabel *m_statusDetailLabel = nullptr;
    QLabel *m_durationLabel = nullptr;
    VideoWidget *m_remoteVideo = nullptr;
    VideoWidget *m_localPreview = nullptr;

    // Active call buttons
    QPushButton *m_muteBtn = nullptr;
    QPushButton *m_hangupBtn = nullptr;
    QPushButton *m_cameraBtn = nullptr;
    QPushButton *m_shareBtn = nullptr;

    // Mic level indicator + info pills
    QWidget *m_micLevel = nullptr;
    QWidget *m_micRow = nullptr;
    QLabel *m_codecPill = nullptr;
    QLabel *m_decoderPill = nullptr;

    // Incoming call buttons
    QPushButton *m_acceptBtn = nullptr;
    QPushButton *m_declineBtn = nullptr;

    QWidget *m_activeRow = nullptr;
    QWidget *m_incomingRow = nullptr;
    bool m_videoConnected = false;
    bool m_localConnected = false;
    QPointer<QObject> m_lastRemoteProvider;
    QPointer<QObject> m_lastLocalProvider;
};
