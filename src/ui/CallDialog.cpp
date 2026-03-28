#include "CallDialog.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QApplication>
#include <QScreen>

CallDialog::CallDialog(CallManager *callManager, QWidget *parent)
    : QDialog(parent, Qt::Dialog | Qt::WindowStaysOnTopHint)
    , m_callManager(callManager)
{
    setWindowTitle("Call");
    setFixedSize(300, 200);

    buildUi();

    connect(m_callManager, &CallManager::stateChanged, this, &CallDialog::onStateChanged);
    connect(m_callManager, &CallManager::durationChanged, this, &CallDialog::onDurationChanged);
    connect(m_callManager, &CallManager::muteChanged, this, &CallDialog::onMuteChanged);
    connect(m_callManager, &CallManager::cameraChanged, this, &CallDialog::onCameraChanged);
    connect(m_callManager, &CallManager::callInfoChanged, this, [this]() {
        m_peerLabel->setText(m_callManager->remotePeerName());
    });
    connect(m_callManager, &CallManager::statusDetailChanged, this, [this]() {
        m_statusDetailLabel->setText(m_callManager->statusDetail());
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

    layout->addStretch();

    // ── Active call buttons row ──
    m_activeRow = new QWidget(this);
    auto *activeLayout = new QHBoxLayout(m_activeRow);
    activeLayout->setContentsMargins(0, 0, 0, 0);
    activeLayout->setSpacing(12);

    const QString btnBase =
        "QPushButton {"
        "  border: none; border-radius: 20px;"
        "  font-size: 16px; min-width: 40px; min-height: 40px; max-width: 40px; max-height: 40px;"
        "}";

    m_muteBtn = new QPushButton(m_activeRow);
    m_muteBtn->setCursor(Qt::PointingHandCursor);
    m_muteBtn->setToolTip("Toggle mute");
    m_muteBtn->setStyleSheet(btnBase +
        "QPushButton { background: #3a3a36; color: #e4e0da; }"
        "QPushButton:hover { background: #4a4a46; }");
    m_muteBtn->setText("\xF0\x9F\x94\x8A");  // speaker emoji
    activeLayout->addStretch();
    activeLayout->addWidget(m_muteBtn);

    m_hangupBtn = new QPushButton(m_activeRow);
    m_hangupBtn->setCursor(Qt::PointingHandCursor);
    m_hangupBtn->setToolTip("Hang up");
    m_hangupBtn->setStyleSheet(btnBase +
        "QPushButton { background: #d93025; color: white; }"
        "QPushButton:hover { background: #e84235; }");
    m_hangupBtn->setText("\xE2\x9C\x96");  // X mark
    activeLayout->addWidget(m_hangupBtn);

    m_cameraBtn = new QPushButton(m_activeRow);
    m_cameraBtn->setCursor(Qt::PointingHandCursor);
    m_cameraBtn->setToolTip("Toggle camera");
    m_cameraBtn->setStyleSheet(btnBase +
        "QPushButton { background: #3a3a36; color: #e4e0da; }"
        "QPushButton:hover { background: #4a4a46; }");
    m_cameraBtn->setText("\xF0\x9F\x8E\xA5");  // camera emoji
    activeLayout->addWidget(m_cameraBtn);
    activeLayout->addStretch();

    layout->addWidget(m_activeRow);

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
        hide();
        return;

    case CallManager::Incoming:
        showIncomingMode();
        m_stateLabel->setText("Incoming call...");
        m_durationLabel->clear();
        break;

    case CallManager::Outgoing:
        showActiveMode();
        m_stateLabel->setText("Calling...");
        m_durationLabel->clear();
        break;

    case CallManager::Connecting:
        showActiveMode();
        m_stateLabel->setText("Connecting...");
        m_durationLabel->clear();
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
        // Position near top-right of parent
        if (parentWidget()) {
            QPoint topRight = parentWidget()->mapToGlobal(
                QPoint(parentWidget()->width() - width() - 20, 20));
            move(topRight);
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
        m_muteBtn->setText("\xF0\x9F\x94\x87");  // muted speaker
        m_muteBtn->setStyleSheet(
            "QPushButton {"
            "  border: none; border-radius: 20px;"
            "  font-size: 16px; min-width: 40px; min-height: 40px; max-width: 40px; max-height: 40px;"
            "  background: #d93025; color: white;"
            "}"
            "QPushButton:hover { background: #e84235; }");
    } else {
        m_muteBtn->setText("\xF0\x9F\x94\x8A");  // speaker
        m_muteBtn->setStyleSheet(
            "QPushButton {"
            "  border: none; border-radius: 20px;"
            "  font-size: 16px; min-width: 40px; min-height: 40px; max-width: 40px; max-height: 40px;"
            "  background: #3a3a36; color: #e4e0da;"
            "}"
            "QPushButton:hover { background: #4a4a46; }");
    }
}

void CallDialog::onCameraChanged()
{
    if (m_callManager->isCameraOn()) {
        m_cameraBtn->setStyleSheet(
            "QPushButton {"
            "  border: none; border-radius: 20px;"
            "  font-size: 16px; min-width: 40px; min-height: 40px; max-width: 40px; max-height: 40px;"
            "  background: #2ec4b6; color: white;"
            "}"
            "QPushButton:hover { background: #3ed4c6; }");
    } else {
        m_cameraBtn->setStyleSheet(
            "QPushButton {"
            "  border: none; border-radius: 20px;"
            "  font-size: 16px; min-width: 40px; min-height: 40px; max-width: 40px; max-height: 40px;"
            "  background: #3a3a36; color: #e4e0da;"
            "}"
            "QPushButton:hover { background: #4a4a46; }");
    }
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
