#include "SettingsDialog.h"
#include "painter/PainterTheme.h"
#include "core/MediaDeviceManager.h"
#include "core/NotificationManager.h"
#include "core/AppSettings.h"
#include "core/AuthManager.h"

#include <QApplication>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QButtonGroup>
#include <QFrame>
#include <QFont>
#include <QRegularExpression>

// Helper: section header label matching QML SectionHeader style
static QLabel *makeSectionHeader(const QString &text)
{
    auto *label = new QLabel(text);
    QFont f = label->font();
    f.setPixelSize(10);
    f.setWeight(QFont::DemiBold);
    f.setLetterSpacing(QFont::AbsoluteSpacing, 1);
    label->setFont(f);

    // Use palette's mid role for subdued color
    QPalette pal = label->palette();
    QColor c = pal.color(QPalette::WindowText);
    c.setAlphaF(0.5f);
    pal.setColor(QPalette::WindowText, c);
    label->setPalette(pal);
    return label;
}

// Helper: horizontal divider line
static QFrame *makeDivider()
{
    auto *line = new QFrame;
    line->setFrameShape(QFrame::HLine);
    line->setFrameShadow(QFrame::Plain);
    line->setFixedHeight(1);

    PainterTheme theme(true, 1.0);
    QPalette pal = line->palette();
    pal.setColor(QPalette::WindowText, theme.divider);
    line->setPalette(pal);
    return line;
}

SettingsDialog::SettingsDialog(
    MediaDeviceManager *deviceManager,
    NotificationManager *notifications,
    AppSettings *appSettings,
    AuthManager *auth,
    QWidget *parent)
    : QDialog(parent)
    , m_deviceManager(deviceManager)
    , m_notifications(notifications)
    , m_appSettings(appSettings)
    , m_auth(auth)
    , m_settings("TalQ", "TalQ")
{
    setWindowTitle("Settings");
    setMinimumSize(460, 420);
    resize(480, 520);

    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    m_tabs = new QTabWidget(this);
    m_tabs->addTab(buildAudioVideoTab(), "Audio && Video");
    m_tabs->addTab(buildNotificationsTab(), "Notifications");
    m_tabs->addTab(buildGeneralTab(), "General");
    m_tabs->addTab(buildAccountTab(), "Account");
    mainLayout->addWidget(m_tabs);

    // Style the tab bar to match the dark palette
    PainterTheme theme(true, 1.0);
    QString tabStyle = QString(
        "QTabWidget::pane { border: none; }"
        "QTabBar::tab {"
        "  padding: 8px 16px;"
        "  border: none;"
        "  border-bottom: 2px solid transparent;"
        "  color: %1;"
        "  background: %2;"
        "}"
        "QTabBar::tab:selected {"
        "  border-bottom-color: %3;"
        "  color: %3;"
        "}"
        "QTabBar::tab:hover {"
        "  color: %4;"
        "}"
    ).arg(theme.textSecondary.name(),
          theme.bgSecondary.name(),
          theme.accent.name(),
          theme.textPrimary.name());
    m_tabs->setStyleSheet(tabStyle);

    // Connect device manager signals to refresh combos
    connect(m_deviceManager, &MediaDeviceManager::devicesChanged, this, &SettingsDialog::populateDeviceCombos);
}

void SettingsDialog::refresh()
{
    m_deviceManager->refresh();
    populateDeviceCombos();
    loadNotificationSettings();
    loadGeneralSettings();

    // Account tab
    if (m_auth) {
        m_displayNameLabel->setText(m_auth->displayName());
        QString url = m_auth->serverUrl();
        QString shortUrl = url;
        shortUrl.remove(QRegularExpression("^https?://"));
        m_serverUrlLabel->setText(shortUrl);
        m_ncVersionLabel->setText("Nextcloud: " + m_auth->nextcloudVersion());
        m_talkVersionLabel->setText("Talk: " + m_auth->talkVersion());
        m_talqVersionLabel->setText("TalQ " + QApplication::applicationVersion());
        // Update the full URL display inside the server frame
        auto *srvLabel = findChild<QLabel *>("serverUrlDisplay");
        if (srvLabel) srvLabel->setText(url);
    }
}

// ============================================================
// Tab 1: Audio & Video
// ============================================================

QWidget *SettingsDialog::buildAudioVideoTab()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(20, 20, 20, 20);
    layout->setSpacing(8);

    // Microphone
    layout->addWidget(makeSectionHeader("MICROPHONE"));
    m_micCombo = new QComboBox;
    layout->addWidget(m_micCombo);
    connect(m_micCombo, QOverload<int>::of(&QComboBox::activated),
            this, [this](int idx) { m_deviceManager->setSelectedAudioInput(idx); });

    layout->addSpacing(4);

    // Speaker
    layout->addWidget(makeSectionHeader("SPEAKER"));
    m_speakerCombo = new QComboBox;
    layout->addWidget(m_speakerCombo);
    connect(m_speakerCombo, QOverload<int>::of(&QComboBox::activated),
            this, [this](int idx) { m_deviceManager->setSelectedAudioOutput(idx); });

    layout->addSpacing(4);

    // Camera
    layout->addWidget(makeSectionHeader("CAMERA"));
    m_cameraCombo = new QComboBox;
    layout->addWidget(m_cameraCombo);
    connect(m_cameraCombo, QOverload<int>::of(&QComboBox::activated),
            this, [this](int idx) { m_deviceManager->setSelectedVideoInput(idx); });

    layout->addSpacing(4);

    // Video quality
    layout->addWidget(makeSectionHeader("VIDEO QUALITY"));
    auto *qualRow = new QHBoxLayout;
    qualRow->setSpacing(8);
    m_res1080 = new QRadioButton("Full HD (1080p)");
    m_res720 = new QRadioButton("HD (720p)");
    auto *qualGroup = new QButtonGroup(this);
    qualGroup->addButton(m_res1080, 0);
    qualGroup->addButton(m_res720, 1);
    qualRow->addWidget(m_res1080);
    qualRow->addWidget(m_res720);
    qualRow->addStretch();
    layout->addLayout(qualRow);

    // Read saved resolution
    m_settings.beginGroup("Video");
    int savedRes = m_settings.value("resolution", 0).toInt();
    m_settings.endGroup();
    if (savedRes == 1) m_res720->setChecked(true);
    else m_res1080->setChecked(true);

    connect(qualGroup, &QButtonGroup::idClicked, this, [this](int id) {
        m_settings.beginGroup("Video");
        m_settings.setValue("resolution", id);
        m_settings.endGroup();
    });

    // Divider + Refresh button
    layout->addSpacing(8);
    layout->addWidget(makeDivider());
    layout->addSpacing(4);

    auto *btnRow = new QHBoxLayout;
    btnRow->addStretch();
    auto *refreshBtn = new QPushButton("Refresh Devices");
    connect(refreshBtn, &QPushButton::clicked, m_deviceManager, &MediaDeviceManager::refresh);
    btnRow->addWidget(refreshBtn);
    layout->addLayout(btnRow);

    layout->addStretch();
    return page;
}

// ============================================================
// Tab 2: Notifications
// ============================================================

QWidget *SettingsDialog::buildNotificationsTab()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(20, 20, 20, 20);
    layout->setSpacing(8);

    // Enable notifications
    layout->addWidget(makeSectionHeader("DESKTOP NOTIFICATIONS"));
    m_notifEnabled = new QCheckBox("Enable notifications");
    layout->addWidget(m_notifEnabled);

    connect(m_notifEnabled, &QCheckBox::toggled, this, [this](bool checked) {
        m_notifications->setNotificationsEnabled(checked);
        m_settings.beginGroup("Notifications");
        m_settings.setValue("enabled", checked);
        m_settings.endGroup();
    });

    layout->addSpacing(4);

    // Notification style
    layout->addWidget(makeSectionHeader("NOTIFICATION STYLE"));
    auto *styleRow = new QHBoxLayout;
    styleRow->setSpacing(8);
    m_stylePopup = new QRadioButton("In-app popup");
    m_styleWindows = new QRadioButton("Windows toast");
    auto *styleGroup = new QButtonGroup(this);
    styleGroup->addButton(m_stylePopup, 0);
    styleGroup->addButton(m_styleWindows, 1);
    styleRow->addWidget(m_stylePopup);
    styleRow->addWidget(m_styleWindows);
    styleRow->addStretch();
    layout->addLayout(styleRow);

    connect(styleGroup, &QButtonGroup::idClicked, this, [this](int id) {
        QString style = (id == 0) ? "popup" : "windows";
        m_notifications->setNotifStyle(style);
        m_settings.beginGroup("Notifications");
        m_settings.setValue("style", style);
        m_settings.endGroup();
    });

    layout->addSpacing(4);

    // Sound
    layout->addWidget(makeSectionHeader("SOUND"));
    auto *soundRow = new QHBoxLayout;
    soundRow->setSpacing(8);
    m_soundInternal = new QRadioButton("TalQ chime");
    m_soundSystem = new QRadioButton("System sound");
    m_soundNone = new QRadioButton("None");
    auto *soundGroup = new QButtonGroup(this);
    soundGroup->addButton(m_soundInternal, 0);
    soundGroup->addButton(m_soundSystem, 1);
    soundGroup->addButton(m_soundNone, 2);
    soundRow->addWidget(m_soundInternal);
    soundRow->addWidget(m_soundSystem);
    soundRow->addWidget(m_soundNone);
    soundRow->addStretch();
    layout->addLayout(soundRow);

    connect(soundGroup, &QButtonGroup::idClicked, this, [this](int id) {
        QString mode;
        if (id == 0) mode = "internal";
        else if (id == 1) mode = "system";
        else mode = "none";
        m_notifications->setSoundMode(mode);
        m_settings.beginGroup("Notifications");
        m_settings.setValue("soundMode", mode);
        m_settings.endGroup();
    });

    layout->addSpacing(12);

    // Hint box
    PainterTheme theme(true, 1.0);
    auto *hintFrame = new QFrame;
    hintFrame->setStyleSheet(QString(
        "QFrame {"
        "  background: rgba(%1, %2, %3, 25);"
        "  border-left: 3px solid %4;"
        "  border-radius: 6px;"
        "  padding: 12px 14px;"
        "}"
    ).arg(theme.accent.red()).arg(theme.accent.green()).arg(theme.accent.blue())
     .arg(theme.accent.name()));

    auto *hintLayout = new QVBoxLayout(hintFrame);
    hintLayout->setContentsMargins(14, 12, 12, 12);
    auto *hintLabel = new QLabel("To mute individual conversations, right-click on them in the sidebar.");
    hintLabel->setWordWrap(true);
    QFont hintFont = hintLabel->font();
    hintFont.setPixelSize(11);
    hintLabel->setFont(hintFont);
    QPalette hintPal = hintLabel->palette();
    hintPal.setColor(QPalette::WindowText, theme.textSecondary);
    hintLabel->setPalette(hintPal);
    hintLayout->addWidget(hintLabel);
    layout->addWidget(hintFrame);

    layout->addStretch();
    return page;
}

// ============================================================
// Tab 3: General
// ============================================================

QWidget *SettingsDialog::buildGeneralTab()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(20, 20, 20, 20);
    layout->setSpacing(8);

    // Startup section
    layout->addWidget(makeSectionHeader("STARTUP"));

    m_autoStart = new QCheckBox("Start with Windows");
    layout->addWidget(m_autoStart);
    connect(m_autoStart, &QCheckBox::toggled, this, [this](bool checked) {
        m_appSettings->setAutoStart(checked);
        m_settings.beginGroup("General");
        m_settings.setValue("autoStart", checked);
        m_settings.endGroup();
    });

    m_startMinimized = new QCheckBox("Start minimized to tray");
    layout->addWidget(m_startMinimized);
    connect(m_startMinimized, &QCheckBox::toggled, this, [this](bool checked) {
        m_settings.beginGroup("General");
        m_settings.setValue("startMinimized", checked);
        m_settings.endGroup();
    });

    layout->addSpacing(8);
    layout->addWidget(makeSectionHeader("BEHAVIOR"));

    m_closeToTray = new QCheckBox("Close to tray");
    layout->addWidget(m_closeToTray);
    auto *closeHint = new QLabel("Minimize to tray instead of quitting");
    QFont hf = closeHint->font();
    hf.setPixelSize(11);
    closeHint->setFont(hf);
    PainterTheme theme(true, 1.0);
    QPalette hp = closeHint->palette();
    hp.setColor(QPalette::WindowText, theme.textSecondary);
    closeHint->setPalette(hp);
    layout->addWidget(closeHint);

    connect(m_closeToTray, &QCheckBox::toggled, this, [this](bool checked) {
        m_settings.beginGroup("General");
        m_settings.setValue("closeToTray", checked);
        m_settings.endGroup();
        emit closeToTrayChanged(checked);
    });

    layout->addStretch();
    return page;
}

// ============================================================
// Tab 4: Account
// ============================================================

QWidget *SettingsDialog::buildAccountTab()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(20, 20, 20, 20);
    layout->setSpacing(8);

    // Profile
    m_displayNameLabel = new QLabel;
    QFont nameFont = m_displayNameLabel->font();
    nameFont.setPixelSize(14);
    nameFont.setWeight(QFont::Medium);
    m_displayNameLabel->setFont(nameFont);
    layout->addWidget(m_displayNameLabel);

    m_serverUrlLabel = new QLabel;
    QFont urlFont = m_serverUrlLabel->font();
    urlFont.setPixelSize(12);
    m_serverUrlLabel->setFont(urlFont);
    PainterTheme theme(true, 1.0);
    QPalette secPal = m_serverUrlLabel->palette();
    secPal.setColor(QPalette::WindowText, theme.textSecondary);
    m_serverUrlLabel->setPalette(secPal);
    layout->addWidget(m_serverUrlLabel);

    layout->addSpacing(4);
    layout->addWidget(makeDivider());
    layout->addSpacing(4);

    // Server info
    layout->addWidget(makeSectionHeader("SERVER"));

    auto *serverFrame = new QFrame;
    serverFrame->setStyleSheet(QString(
        "QFrame { background: %1; border-radius: 6px; padding: 8px; }"
    ).arg(theme.bgSurface.name()));

    auto *serverLayout = new QVBoxLayout(serverFrame);
    serverLayout->setContentsMargins(8, 8, 8, 8);
    auto *serverUrlDisplay = new QLabel;
    serverUrlDisplay->setFont(urlFont);
    QPalette srvPal = serverUrlDisplay->palette();
    srvPal.setColor(QPalette::WindowText, theme.textSecondary);
    serverUrlDisplay->setPalette(srvPal);
    serverLayout->addWidget(serverUrlDisplay);
    layout->addWidget(serverFrame);

    // Updated from refresh() via findChild("serverUrlDisplay")
    serverUrlDisplay->setObjectName("serverUrlDisplay");

    layout->addSpacing(4);
    layout->addWidget(makeSectionHeader("NEXTCLOUD"));

    auto *infoRow = new QHBoxLayout;
    infoRow->setSpacing(20);
    m_ncVersionLabel = new QLabel;
    QFont infoFont = m_ncVersionLabel->font();
    infoFont.setPixelSize(11);
    m_ncVersionLabel->setFont(infoFont);
    m_ncVersionLabel->setPalette(secPal);
    m_talkVersionLabel = new QLabel;
    m_talkVersionLabel->setFont(infoFont);
    m_talkVersionLabel->setPalette(secPal);
    infoRow->addWidget(m_ncVersionLabel);
    infoRow->addWidget(m_talkVersionLabel);
    infoRow->addStretch();
    layout->addLayout(infoRow);

    layout->addStretch();

    // Bottom: divider + version + logout
    layout->addWidget(makeDivider());
    auto *bottomRow = new QHBoxLayout;
    m_talqVersionLabel = new QLabel;
    m_talqVersionLabel->setFont(infoFont);
    m_talqVersionLabel->setPalette(secPal);
    bottomRow->addWidget(m_talqVersionLabel);
    bottomRow->addStretch();

    auto *logoutBtn = new QPushButton("Log out");
    logoutBtn->setStyleSheet(
        "QPushButton { background: #e07060; color: white; border: none;"
        "  border-radius: 6px; padding: 6px 16px; }"
        "QPushButton:hover { background: #c85a4a; }"
    );
    connect(logoutBtn, &QPushButton::clicked, this, [this]() {
        m_auth->logout();
        accept(); // close the dialog
    });
    bottomRow->addWidget(logoutBtn);
    layout->addLayout(bottomRow);

    return page;
}

// ============================================================
// Populate helpers
// ============================================================

void SettingsDialog::populateDeviceCombos()
{
    // Block signals to avoid triggering activated() during repopulation
    m_micCombo->blockSignals(true);
    m_speakerCombo->blockSignals(true);
    m_cameraCombo->blockSignals(true);

    m_micCombo->clear();
    m_micCombo->addItems(m_deviceManager->audioInputNames());
    int micIdx = m_deviceManager->selectedAudioInput();
    if (micIdx >= 0 && micIdx < m_micCombo->count())
        m_micCombo->setCurrentIndex(micIdx);
    m_micCombo->setEnabled(m_micCombo->count() > 0);

    m_speakerCombo->clear();
    m_speakerCombo->addItems(m_deviceManager->audioOutputNames());
    int spkIdx = m_deviceManager->selectedAudioOutput();
    if (spkIdx >= 0 && spkIdx < m_speakerCombo->count())
        m_speakerCombo->setCurrentIndex(spkIdx);
    m_speakerCombo->setEnabled(m_speakerCombo->count() > 0);

    m_cameraCombo->clear();
    m_cameraCombo->addItems(m_deviceManager->videoInputNames());
    int camIdx = m_deviceManager->selectedVideoInput();
    if (camIdx >= 0 && camIdx < m_cameraCombo->count())
        m_cameraCombo->setCurrentIndex(camIdx);
    m_cameraCombo->setEnabled(m_cameraCombo->count() > 0);

    m_micCombo->blockSignals(false);
    m_speakerCombo->blockSignals(false);
    m_cameraCombo->blockSignals(false);
}

void SettingsDialog::loadNotificationSettings()
{
    m_settings.beginGroup("Notifications");
    bool enabled = m_settings.value("enabled", true).toBool();
    QString style = m_settings.value("style", "popup").toString();
    QString soundMode = m_settings.value("soundMode", "internal").toString();
    m_settings.endGroup();

    m_notifEnabled->blockSignals(true);
    m_notifEnabled->setChecked(enabled);
    m_notifEnabled->blockSignals(false);

    m_stylePopup->blockSignals(true);
    m_styleWindows->blockSignals(true);
    if (style == "windows") m_styleWindows->setChecked(true);
    else m_stylePopup->setChecked(true);
    m_stylePopup->blockSignals(false);
    m_styleWindows->blockSignals(false);

    m_soundInternal->blockSignals(true);
    m_soundSystem->blockSignals(true);
    m_soundNone->blockSignals(true);
    if (soundMode == "system") m_soundSystem->setChecked(true);
    else if (soundMode == "none") m_soundNone->setChecked(true);
    else m_soundInternal->setChecked(true);
    m_soundInternal->blockSignals(false);
    m_soundSystem->blockSignals(false);
    m_soundNone->blockSignals(false);
}

void SettingsDialog::loadGeneralSettings()
{
    m_settings.beginGroup("General");
    bool autoStart = m_settings.value("autoStart", false).toBool();
    bool startMin = m_settings.value("startMinimized", false).toBool();
    bool closeTray = m_settings.value("closeToTray", true).toBool();
    m_settings.endGroup();

    m_autoStart->blockSignals(true);
    m_autoStart->setChecked(autoStart);
    m_autoStart->blockSignals(false);

    m_startMinimized->blockSignals(true);
    m_startMinimized->setChecked(startMin);
    m_startMinimized->blockSignals(false);

    m_closeToTray->blockSignals(true);
    m_closeToTray->setChecked(closeTray);
    m_closeToTray->blockSignals(false);
}
