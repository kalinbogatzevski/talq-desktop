#include "CallDialog.h"
#include "core/VideoFrameProvider.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QApplication>
#include <QScreen>
#include <QPainter>
#include <QNetworkReply>
#include <QDesktopServices>
#include <QUrl>

static QString circleButtonStyle(const QString &bg, const QString &fg, const QString &hoverBg)
{
    return QStringLiteral(
        "QPushButton {"
        "  border: none; border-radius: 20px;"
        "  font-size: 16px; min-width: 40px; min-height: 40px; max-width: 40px; max-height: 40px;"
        "  background: %1; color: %2;"
        "}"
        "QPushButton:hover { background: %3; }").arg(bg, fg, hoverBg);
}

void VideoWidget::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.fillRect(rect(), Qt::black);
    if (!m_image.isNull()) {
        QSize scaled = m_image.size().scaled(size(), Qt::KeepAspectRatio);
        QRect dst((width() - scaled.width()) / 2, (height() - scaled.height()) / 2,
                  scaled.width(), scaled.height());
        p.drawImage(dst, m_image);
    }
}

CallDialog::CallDialog(CallManager *callManager, ApiClient *api, QWidget *parent)
    : QDialog(parent, Qt::Dialog | Qt::WindowStaysOnTopHint)
    , m_callManager(callManager)
    , m_api(api)
{
    setWindowTitle("Call");
    setMinimumSize(300, 340);
    resize(300, 340);

    buildUi();

    connect(m_callManager, &CallManager::stateChanged, this, &CallDialog::onStateChanged);
    connect(m_callManager, &CallManager::durationChanged, this, &CallDialog::onDurationChanged);
    connect(m_callManager, &CallManager::muteChanged, this, &CallDialog::onMuteChanged);
    connect(m_callManager, &CallManager::cameraChanged, this, &CallDialog::onCameraChanged);
    connect(m_callManager, &CallManager::callInfoChanged, this, [this]() {
        m_peerLabel->setText(m_callManager->remotePeerName());
        QString peerId = m_callManager->remotePeerId();
        if (!peerId.isEmpty() && peerId != m_loadedAvatarUserId)
            fetchAvatar(peerId);
    });
    connect(m_callManager, &CallManager::statusDetailChanged, this, [this]() {
        m_statusDetailLabel->setText(m_callManager->statusDetail());
    });
    connect(m_callManager, &CallManager::remoteVideoProviderChanged, this, [this]() {
        m_videoConnected = false;  // new provider — reconnect
        connectVideoProviders();
    });
    connect(m_callManager, &CallManager::localVideoProviderChanged, this, [this]() {
        m_localConnected = false;
        if (!m_callManager->localVideoProvider())
            m_localPreview->hide();
        connectVideoProviders();
    });
    connect(m_callManager, &CallManager::audioLevelChanged, this, [this]() {
        double level = qBound(0.0, m_callManager->audioLevel(), 1.0);
        int maxW = m_activeRow->isVisible() ? m_activeRow->width() : 200;
        int w = qRound(maxW * level);
        m_micLevel->setFixedWidth(w);
    });
    connect(m_callManager, &CallManager::remoteMediaChanged, this, [this]() {
        if (m_callManager->remoteVideoMuted()) {
            // Disconnect remote video signal so stale frames don't re-show it
            if (m_lastRemoteProvider)
                disconnect(m_lastRemoteProvider, nullptr, this, nullptr);
            m_lastRemoteProvider = nullptr;
            m_videoConnected = false;
            m_remoteVideo->hide();
            if (!m_callManager->isCameraOn()) {
                setMinimumSize(300, 300);
                resize(300, 340);
            }
        } else {
            // Remote unmuted — reconnect video provider
            connectVideoProviders();
        }
        // Update peer label to show remote mic state
        QString name = m_callManager->remotePeerName();
        if (m_callManager->remoteAudioMuted())
            name += "  \xF0\x9F\x94\x87";  // 🔇
        m_peerLabel->setText(name);
    });
    connect(m_callManager, &CallManager::screenShareChanged, this, [this]() {
        if (m_callManager->isScreenSharing()) {
            m_shareBtn->setStyleSheet(circleButtonStyle("#2ec4b6", "white", "#3ed4c6"));
        } else {
            m_shareBtn->setStyleSheet(circleButtonStyle("#3a3a36", "#e4e0da", "#4a4a46"));
        }
    });
    connect(m_callManager, &CallManager::remoteScreenProviderChanged, this, [this]() {
        auto *provider = m_callManager->remoteScreenProvider();
        if (provider) {
            // Screen share started — maximize dialog for readability
            if (!m_remoteVideo->isVisible())
                m_remoteVideo->show();
            m_avatarLabel->hide();
            m_stateLabel->hide();
            m_statusDetailLabel->hide();
            m_durationLabel->hide();
            showMaximized();
            connect(provider, &VideoFrameProvider::imageReady, this, [this](const QImage &img) {
                if (img.width() > 32 && img.height() > 32)
                    m_remoteVideo->setImage(img);
            });
        } else {
            // Screen share stopped — restore dialog, reconnect camera
            if (m_lastRemoteProvider)
                disconnect(m_lastRemoteProvider, nullptr, this, nullptr);
            m_lastRemoteProvider = nullptr;
            m_videoConnected = false;
            m_avatarLabel->show();
            m_stateLabel->show();
            m_statusDetailLabel->show();
            m_durationLabel->show();
            showNormal();
            resize(400, 500);
            connectVideoProviders();
        }
    });
}

void CallDialog::buildUi()
{
    setStyleSheet(
        "CallDialog { background: #1c1c1a; }"
        "QLabel { color: #e4e0da; }"
    );

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 16, 20, 16);
    layout->setSpacing(12);

    // Avatar
    m_avatarLabel = new QLabel(this);
    m_avatarLabel->setAlignment(Qt::AlignCenter);
    m_avatarLabel->setFixedSize(64, 64);
    m_avatarLabel->setStyleSheet("border-radius: 32px;");
    layout->addWidget(m_avatarLabel, 0, Qt::AlignCenter);

    // Peer name
    m_peerLabel = new QLabel(this);
    m_peerLabel->setAlignment(Qt::AlignCenter);
    m_peerLabel->setStyleSheet("font-size: 18px; font-weight: bold; color: #e4e0da;");
    layout->addWidget(m_peerLabel);

    // Call state
    m_stateLabel = new QLabel(this);
    m_stateLabel->setAlignment(Qt::AlignCenter);
    m_stateLabel->setStyleSheet("font-size: 13px; color: #8a8680;");
    layout->addWidget(m_stateLabel);

    // Status detail (signaling progress)
    m_statusDetailLabel = new QLabel(this);
    m_statusDetailLabel->setAlignment(Qt::AlignCenter);
    m_statusDetailLabel->setStyleSheet("font-size: 11px; color: #6a6660;");
    layout->addWidget(m_statusDetailLabel);

    // Duration
    m_durationLabel = new QLabel(this);
    m_durationLabel->setAlignment(Qt::AlignCenter);
    m_durationLabel->setStyleSheet("font-size: 22px; font-weight: bold; color: #2ec4b6;");
    layout->addWidget(m_durationLabel);

    // Spacer — fills space in audio-only mode, hidden when video is shown
    layout->addStretch(1);

    // Remote video display — hidden until real video frames arrive
    m_remoteVideo = new VideoWidget(this);
    m_remoteVideo->hide();
    layout->addWidget(m_remoteVideo, 10);

    // Local camera preview (small overlay, positioned in resizeEvent)
    m_localPreview = new VideoWidget(this);
    m_localPreview->setFixedSize(120, 90);
    m_localPreview->hide();

    // ── Active call buttons row ──
    m_activeRow = new QWidget(this);
    auto *activeLayout = new QHBoxLayout(m_activeRow);
    activeLayout->setContentsMargins(0, 0, 0, 0);
    activeLayout->setSpacing(12);

    m_muteBtn = new QPushButton(m_activeRow);
    m_muteBtn->setCursor(Qt::PointingHandCursor);
    m_muteBtn->setToolTip("Toggle mute");
    m_muteBtn->setStyleSheet(circleButtonStyle("#3a3a36", "#e4e0da", "#4a4a46"));
    m_muteBtn->setText("Mic");
    activeLayout->addStretch();
    activeLayout->addWidget(m_muteBtn);

    m_hangupBtn = new QPushButton(m_activeRow);
    m_hangupBtn->setCursor(Qt::PointingHandCursor);
    m_hangupBtn->setToolTip("Hang up");
    m_hangupBtn->setStyleSheet(circleButtonStyle("#d93025", "white", "#e84235"));
    m_hangupBtn->setText("\xE2\x9C\x96");  // X mark
    activeLayout->addWidget(m_hangupBtn);

    m_cameraBtn = new QPushButton(m_activeRow);
    m_cameraBtn->setCursor(Qt::PointingHandCursor);
    m_cameraBtn->setToolTip("Toggle camera");
    m_cameraBtn->setStyleSheet(circleButtonStyle("#3a3a36", "#e4e0da", "#4a4a46"));
    m_cameraBtn->setText("\xF0\x9F\x8E\xA5");  // camera emoji
    activeLayout->addWidget(m_cameraBtn);

    m_shareBtn = new QPushButton(m_activeRow);
    m_shareBtn->setCursor(Qt::PointingHandCursor);
    m_shareBtn->setToolTip("Share screen");
    m_shareBtn->setStyleSheet(circleButtonStyle("#3a3a36", "#e4e0da", "#4a4a46"));
    m_shareBtn->setText("\xF0\x9F\x96\xA5");  // desktop monitor emoji
    activeLayout->addWidget(m_shareBtn);

    auto *blurBtn = new QPushButton(m_activeRow);
    blurBtn->setCursor(Qt::PointingHandCursor);
    blurBtn->setToolTip("Open Windows Camera Settings\n(Background blur via Studio Effects)");
    blurBtn->setStyleSheet(circleButtonStyle("#3a3a36", "#e4e0da", "#4a4a46"));
    blurBtn->setText("\xF0\x9F\x8C\x80");  // cyclone emoji (blur)
    activeLayout->addWidget(blurBtn);
    connect(blurBtn, &QPushButton::clicked, this, []() {
        QDesktopServices::openUrl(QUrl("ms-settings:camera"));
    });
    activeLayout->addStretch();

    layout->addWidget(m_activeRow);

    // ── Mic level indicator ──
    m_micLevel = new QWidget(this);
    m_micLevel->setFixedHeight(3);
    m_micLevel->setStyleSheet("background: #2ec4b6;");
    m_micLevel->setFixedWidth(0);
    layout->addWidget(m_micLevel, 0, Qt::AlignLeft);

    // ── Incoming call buttons row ──
    m_incomingRow = new QWidget(this);
    auto *incomingLayout = new QHBoxLayout(m_incomingRow);
    incomingLayout->setContentsMargins(0, 0, 0, 0);
    incomingLayout->setSpacing(16);

    const QString wideBtn =
        "QPushButton {"
        "  border: none; border-radius: 16px;"
        "  font-size: 14px; font-weight: bold;"
        "  min-height: 40px; padding: 0 20px;"
        "}";

    m_acceptBtn = new QPushButton("Accept", m_incomingRow);
    m_acceptBtn->setCursor(Qt::PointingHandCursor);
    m_acceptBtn->setStyleSheet(wideBtn +
        "QPushButton { background: #2ec46a; color: white; }"
        "QPushButton:hover { background: #3ed67a; }");
    incomingLayout->addStretch();
    incomingLayout->addWidget(m_acceptBtn);

    m_declineBtn = new QPushButton("Decline", m_incomingRow);
    m_declineBtn->setCursor(Qt::PointingHandCursor);
    m_declineBtn->setStyleSheet(wideBtn +
        "QPushButton { background: #d93025; color: white; }"
        "QPushButton:hover { background: #e84235; }");
    incomingLayout->addWidget(m_declineBtn);
    incomingLayout->addStretch();

    layout->addWidget(m_incomingRow);

    // Both rows hidden initially
    m_activeRow->hide();
    m_incomingRow->hide();

    // ── Button connections ──
    connect(m_muteBtn, &QPushButton::clicked, m_callManager, &CallManager::toggleMute);
    connect(m_hangupBtn, &QPushButton::clicked, m_callManager, &CallManager::hangUp);
    connect(m_cameraBtn, &QPushButton::clicked, m_callManager, &CallManager::toggleCamera);
    connect(m_shareBtn, &QPushButton::clicked, m_callManager, &CallManager::toggleScreenShare);
    connect(m_acceptBtn, &QPushButton::clicked, this, [this]() {
        m_callManager->acceptCall(false);
    });
    connect(m_declineBtn, &QPushButton::clicked, m_callManager, &CallManager::declineCall);
}

void CallDialog::onStateChanged()
{
    auto state = m_callManager->state();

    switch (state) {
    case CallManager::Idle:
        m_remoteVideo->hide();
        m_localPreview->hide();
        m_videoConnected = false;
        m_localConnected = false;
        setMinimumSize(300, 340);
        resize(300, 340);
        hide();
        return;

    case CallManager::Incoming:
        showIncomingMode();
        m_stateLabel->setText("Incoming call...");
        m_durationLabel->clear();
        m_callManager->setUserActionReady();
        break;

    case CallManager::Outgoing:
        showActiveMode();
        m_stateLabel->setText("Calling...");
        m_durationLabel->clear();
        connectVideoProviders();
        break;

    case CallManager::Connecting:
        showActiveMode();
        m_stateLabel->setText("Connecting...");
        m_durationLabel->clear();
        connectVideoProviders();
        break;

    case CallManager::Active:
        showActiveMode();
        m_stateLabel->setText("In call");
        break;

    case CallManager::Ending:
        m_stateLabel->setText("Ending...");
        m_activeRow->hide();
        m_incomingRow->hide();
        break;
    }

    m_peerLabel->setText(m_callManager->remotePeerName());

    if (!isVisible()) {
        // Center on primary screen
        QScreen *screen = QApplication::primaryScreen();
        if (screen) {
            QRect geom = screen->availableGeometry();
            move(geom.center() - rect().center());
        }
        show();
        raise();
        activateWindow();
    }
}

void CallDialog::onDurationChanged()
{
    if (m_callManager->state() == CallManager::Active) {
        m_durationLabel->setText(formatDuration(m_callManager->callDuration()));
    }
}

void CallDialog::onMuteChanged()
{
    if (m_callManager->isMuted()) {
        m_muteBtn->setText("Muted");
        m_muteBtn->setStyleSheet(circleButtonStyle("#d93025", "white", "#e84235"));
    } else {
        m_muteBtn->setText("Mic");
        m_muteBtn->setStyleSheet(circleButtonStyle("#3a3a36", "#e4e0da", "#4a4a46"));
    }
}

void CallDialog::onCameraChanged()
{
    if (m_callManager->isCameraOn()) {
        m_cameraBtn->setStyleSheet(circleButtonStyle("#2ec4b6", "white", "#3ed4c6"));
    } else {
        m_cameraBtn->setStyleSheet(circleButtonStyle("#3a3a36", "#e4e0da", "#4a4a46"));
        // Disconnect local preview signal so stale frames don't re-show it
        if (m_lastLocalProvider)
            disconnect(m_lastLocalProvider, nullptr, this, nullptr);
        m_lastLocalProvider = nullptr;
        m_localConnected = false;
        m_localPreview->hide();
        // Shrink dialog if remote video also not showing
        if (!m_remoteVideo->isVisible()) {
            setMinimumSize(300, 300);
            resize(300, 340);
        }
    }
}

void CallDialog::fetchAvatar(const QString &userId)
{
    m_loadedAvatarUserId = userId;
    auto *reply = m_api->getAbsoluteUrl("/index.php/avatar/" + userId + "/128");
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) return;
        QPixmap pm;
        if (!pm.loadFromData(reply->readAll())) return;
        // Circular crop
        int sz = qMin(pm.width(), pm.height());
        QPixmap circle(sz, sz);
        circle.fill(Qt::transparent);
        QPainter p(&circle);
        p.setRenderHint(QPainter::Antialiasing);
        p.setBrush(QBrush(pm.scaled(sz, sz, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation)));
        p.setPen(Qt::NoPen);
        p.drawEllipse(0, 0, sz, sz);
        m_avatarPixmap = circle.scaled(64, 64, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        m_avatarLabel->setPixmap(m_avatarPixmap);
    });
}

void CallDialog::connectVideoProviders()
{
    auto *remote = m_callManager->remoteVideoProvider();
    if (remote && !m_videoConnected) {
        if (m_lastRemoteProvider)
            disconnect(m_lastRemoteProvider, nullptr, this, nullptr);
        connect(remote, &VideoFrameProvider::imageReady, this, &CallDialog::onRemoteFrame);
        m_lastRemoteProvider = remote;
        m_videoConnected = true;
        qDebug() << "CallDialog: connected to remote video provider";
    }
    auto *local = m_callManager->localVideoProvider();
    if (local && !m_localConnected) {
        if (m_lastLocalProvider)
            disconnect(m_lastLocalProvider, nullptr, this, nullptr);
        connect(local, &VideoFrameProvider::imageReady, this, [this](const QImage &img) {
            if (!m_localPreview->isVisible()) {
                m_localPreview->show();
                m_localPreview->raise();
                if (!m_remoteVideo->isVisible()) {
                    m_remoteVideo->show();
                    setMinimumSize(400, 400);
                    resize(400, 500);
                }
            }
            m_localPreview->setImage(img);
            // Position bottom-right of remote video
            m_localPreview->move(m_remoteVideo->x() + m_remoteVideo->width() - 128,
                                m_remoteVideo->y() + m_remoteVideo->height() - 98);
        });
        m_lastLocalProvider = local;
        m_localConnected = true;
        qDebug() << "CallDialog: connected to local video provider";
    }
}

void CallDialog::onRemoteFrame(const QImage &image)
{
    // Skip tiny frames (16x16 dummy black from MCU placeholder)
    if (image.width() <= 32 && image.height() <= 32)
        return;
    // During screen share: show camera in small local preview overlay
    if (m_callManager->remoteScreenProvider()) {
        if (!m_localPreview->isVisible()) {
            m_localPreview->show();
            m_localPreview->raise();
        }
        m_localPreview->setImage(image);
        m_localPreview->move(m_remoteVideo->x() + m_remoteVideo->width() - 128,
                             m_remoteVideo->y() + m_remoteVideo->height() - 98);
        return;
    }
    // Don't show video if remote has muted their camera
    if (m_callManager->remoteVideoMuted())
        return;
    if (!m_remoteVideo->isVisible()) {
        m_remoteVideo->show();
        setMinimumSize(400, 450);
        resize(400, 500);
    }
    m_remoteVideo->setImage(image);
}

void CallDialog::showIncomingMode()
{
    m_incomingRow->show();
    m_activeRow->hide();
}

void CallDialog::showActiveMode()
{
    m_activeRow->show();
    m_incomingRow->hide();
}

QString CallDialog::formatDuration(int seconds) const
{
    int m = seconds / 60;
    int s = seconds % 60;
    return QStringLiteral("%1:%2").arg(m).arg(s, 2, 10, QChar('0'));
}
