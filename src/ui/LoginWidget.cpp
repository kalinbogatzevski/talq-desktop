#include "LoginWidget.h"
#include "core/AuthManager.h"
#include <QLineEdit>
#include <QPushButton>
#include <QLabel>
#include <QVBoxLayout>
#include <QPainter>
#include <QApplication>
#include <QPixmap>

LoginWidget::LoginWidget(AuthManager *auth, QWidget *parent)
    : QWidget(parent)
    , m_auth(auth)
{
#ifdef TALQ_BRAND_123NET
#include "brand_identity.inc"   // private; defines TALQ_BRAND_SERVER_STR
    m_isBranded = true;
    m_brandServer = TALQ_BRAND_SERVER_STR;
    m_brandName = "123NET TalQ";
#else
    m_isBranded = false;
    m_brandName = "TalQ";
#endif

    auto *outerLayout = new QVBoxLayout(this);
    outerLayout->setAlignment(Qt::AlignCenter);

    auto *centerWidget = new QWidget(this);
    centerWidget->setMaximumWidth(320);
    auto *layout = new QVBoxLayout(centerWidget);
    layout->setAlignment(Qt::AlignCenter);
    layout->setSpacing(8);

    // Logo
    auto *logoLabel = new QLabel(centerWidget);
    QPixmap logo(m_isBranded ? ":/123net-logo.png" : ":/logo.png");
    logoLabel->setPixmap(logo.scaled(72, 72, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    logoLabel->setAlignment(Qt::AlignCenter);
    layout->addWidget(logoLabel);

    // Brand sub-logo (show TalQ logo below brand logo)
    if (m_isBranded) {
        auto *subLogoLabel = new QLabel(centerWidget);
        QPixmap subLogo(":/logo.png");
        subLogoLabel->setPixmap(subLogo.scaled(40, 40, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        subLogoLabel->setAlignment(Qt::AlignCenter);
        layout->addWidget(subLogoLabel);
    }

    // Brand name
    auto *nameLabel = new QLabel(m_brandName, centerWidget);
    nameLabel->setAlignment(Qt::AlignCenter);
    QFont nameFont;
    nameFont.setPixelSize(28);
    nameFont.setWeight(QFont::DemiBold);
    nameLabel->setFont(nameFont);
    layout->addWidget(nameLabel);
    layout->addSpacing(20);

    // Server URL input (hidden for branded)
    if (!m_isBranded) {
        auto *serverLabel = new QLabel("Server address", centerWidget);
        QFont sf; sf.setPixelSize(11); sf.setWeight(QFont::DemiBold);
        serverLabel->setFont(sf);
        layout->addWidget(serverLabel);
    }

    m_serverInput = new QLineEdit(centerWidget);
    m_serverInput->setText(m_isBranded ? m_brandServer : "");
    m_serverInput->setPlaceholderText("https://cloud.example.com");
    m_serverInput->setVisible(!m_isBranded);
    QFont inputFont; inputFont.setPixelSize(14);
    m_serverInput->setFont(inputFont);
    m_serverInput->setMinimumHeight(36);
    layout->addWidget(m_serverInput);

    // Status
    m_statusLabel = new QLabel(centerWidget);
    m_statusLabel->setProperty("role", "secondary");
    m_statusLabel->setAlignment(Qt::AlignCenter);
    m_statusLabel->setWordWrap(true);
    m_statusLabel->hide();
    layout->addWidget(m_statusLabel);

    // Error
    m_errorLabel = new QLabel(centerWidget);
    m_errorLabel->setProperty("role", "danger");
    m_errorLabel->setAlignment(Qt::AlignCenter);
    m_errorLabel->setWordWrap(true);
    m_errorLabel->hide();
    layout->addWidget(m_errorLabel);

    layout->addSpacing(8);

    // Connect / Cancel button
    m_connectBtn = new QPushButton("Connect", centerWidget);
    m_connectBtn->setProperty("variant", "primary");
    m_connectBtn->setMinimumHeight(40);
    QFont btnFont; btnFont.setPixelSize(14); btnFont.setWeight(QFont::DemiBold);
    m_connectBtn->setFont(btnFont);
    layout->addWidget(m_connectBtn);

    layout->addSpacing(24);

    // Version
    m_versionLabel = new QLabel("v" + QApplication::applicationVersion(), centerWidget);
    m_versionLabel->setProperty("role", "muted");
    m_versionLabel->setAlignment(Qt::AlignCenter);
    QFont vf; vf.setPixelSize(10);
    m_versionLabel->setFont(vf);
    layout->addWidget(m_versionLabel);

    outerLayout->addWidget(centerWidget);

    // Connections
    connect(m_connectBtn, &QPushButton::clicked, this, [this]() {
        if (m_auth->isWaitingForBrowser()) {
            m_auth->cancelLogin();
        } else {
            QString url = m_isBranded ? m_brandServer : m_serverInput->text().trimmed();
            if (!url.isEmpty())
                m_auth->startLogin(url);
        }
    });

    connect(m_serverInput, &QLineEdit::returnPressed, m_connectBtn, &QPushButton::click);

    connect(m_auth, &AuthManager::waitingChanged, this, &LoginWidget::updateState);
    connect(m_auth, &AuthManager::errorChanged, this, &LoginWidget::updateState);
    connect(m_auth, &AuthManager::statusChanged, this, &LoginWidget::updateState);

    updateState();
}

void LoginWidget::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    // Theme-driven (QApplication palette is set from PainterTheme.bgPrimary).
    p.fillRect(rect(), palette().color(QPalette::Window));
}

void LoginWidget::updateState()
{
    bool waiting = m_auth->isWaitingForBrowser();
    m_connectBtn->setText(waiting ? "Cancel" : "Connect");
    m_serverInput->setEnabled(!waiting && !m_isBranded);

    if (!m_auth->statusMessage().isEmpty() && !waiting) {
        m_statusLabel->setText(m_auth->statusMessage());
        m_statusLabel->show();
    } else if (waiting) {
        m_statusLabel->setText("Waiting for browser login...\nComplete sign-in in your browser, then return here.");
        m_statusLabel->show();
    } else {
        m_statusLabel->hide();
    }

    if (!m_auth->errorMessage().isEmpty()) {
        m_errorLabel->setText(m_auth->errorMessage());
        m_errorLabel->show();
    } else {
        m_errorLabel->hide();
    }
}
