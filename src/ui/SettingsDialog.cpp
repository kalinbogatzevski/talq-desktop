#include "SettingsDialog.h"
#include "painter/PainterTheme.h"
#include "core/MediaDeviceManager.h"
#include "core/NotificationManager.h"
#include "core/CallManager.h"
#include "core/TalqLog.h"
#include "core/AppSettings.h"
#include "core/AuthManager.h"

#ifndef TALQ_VERSION_NAME
#define TALQ_VERSION_NAME ""   // per-release codename (set in CMake)
#endif

#include <QApplication>
#include <QGuiApplication>
#include <QScreen>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QButtonGroup>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QFont>
#include <QListWidget>
#include <QRegularExpression>
#include <QScrollArea>
#include <QScrollBar>
#include <QSlider>
#include <QStandardPaths>
#include <QTimer>

// Group eyebrow: a calm uppercase caption that names a group of setting
// rows. Colour is theme-driven (AppStyle role="eyebrow"); only the
// letter-spacing (not a colour) is set in code, which the anti-drift
// rule permits.
static QLabel *makeSectionHeader(const QString &text)
{
    auto *label = new QLabel(text.toUpper());
    // #14 — Mission Control idiom: monospace, gentle tracking. Matches
    // the call screen's telemetry panel headers (CallStage::paintTelemetry)
    // so Settings reads as the same data surface as the call screen.
    QFont f("Cascadia Mono", -1);
    if (!QFontInfo(f).fixedPitch())
        f = QFont("Consolas");
    f.setPixelSize(11);
    f.setWeight(QFont::Medium);
    f.setLetterSpacing(QFont::AbsoluteSpacing, 1.4);
    label->setFont(f);
    label->setProperty("role", "eyebrow");   // AppStyle, theme-driven
    return label;
}

// One setting = one row: name (+ optional one-line description) on the
// left, the control aligned to a fixed right-hand column so every
// control in the dialog shares one vertical edge. The calm-surface /
// confident-control idiom from DESIGN.md; no cards, grouping by rhythm.
static constexpr int kControlCol = 200;   // right-hand control column width

static QWidget *makeSettingRow(const QString &name,
                               const QString &desc,
                               QWidget *control)
{
    // Row structure: a top HBox that contains the NAME and the CONTROL on
    // the SAME line, with the optional DESC hanging below the name only.
    // Pinning name+control to one line keeps a checkbox indicator and its
    // label on the same baseline regardless of how tall the control is
    // intrinsically (combo box ~ 24 px, checkbox indicator ~ 17 px).
    auto *row = new QWidget;
    auto *outer = new QVBoxLayout(row);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(2);

    auto *topLine = new QHBoxLayout;
    topLine->setContentsMargins(0, 0, 0, 0);
    topLine->setSpacing(16);

    auto *nameLbl = new QLabel(name);
    nameLbl->setProperty("role", "settingName");
    // AlignVCenter so a tall control on the same line still has the name
    // centered against it; a short label on a tall row stays centered.
    topLine->addWidget(nameLbl, 1, Qt::AlignVCenter);

    if (control) {
        // QCheckBox / QRadioButton are intrinsically narrow (just the
        // indicator + an empty label area, since the name is in the left
        // column). setMinimumWidth on them stretches the widget but the
        // indicator stays LEFT-aligned, so the indicator would float far
        // left of where combo-box edges line up. Wrap them in a row with
        // a leading stretch so the indicator sits flush against the same
        // right-hand edge as combos / line edits.
        const bool isToggle = qobject_cast<QCheckBox *>(control)
                           || qobject_cast<QRadioButton *>(control);
        if (isToggle) {
            auto *rightAlign = new QHBoxLayout;
            rightAlign->setContentsMargins(0, 0, 0, 0);
            rightAlign->addStretch();
            rightAlign->addWidget(control);
            auto *holder = new QWidget;
            holder->setLayout(rightAlign);
            holder->setMinimumWidth(kControlCol);
            topLine->addWidget(holder, 0, Qt::AlignVCenter);
        } else {
            control->setMinimumWidth(kControlCol);
            topLine->addWidget(control, 0, Qt::AlignVCenter);
        }
    }
    outer->addLayout(topLine);

    if (!desc.isEmpty()) {
        auto *descLbl = new QLabel(desc);
        descLbl->setProperty("role", "settingDesc");
        descLbl->setWordWrap(true);
        outer->addWidget(descLbl);
    }
    return row;
}

// Vertical rhythm: one gap between rows in a group, a larger gap before
// the next group's eyebrow. DESIGN.md spacing scale (8 / 20).
static constexpr int kRowGap   = 14;
static constexpr int kGroupGap = 26;

// Helper: horizontal divider line
static QFrame *makeDivider()
{
    auto *line = new QFrame;
    line->setFrameShape(QFrame::HLine);
    line->setFrameShadow(QFrame::Plain);
    line->setFixedHeight(1);
    // Line colour from AppStyle QFrame[frameShape] (theme-driven).
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
    setMinimumSize(520, 460);
    // Clamp the initial height to fit a small laptop (1366×768 minus
    // task-bar and window chrome leaves ~700 px usable). The
    // per-tab scroll-area below handles overflow inside each tab so
    // long content (background grid, notification list) is reachable
    // even when the dialog is short.
    int targetH = 580;
    if (auto *scr = QGuiApplication::primaryScreen()) {
        const int avail = scr->availableGeometry().height();
        targetH = qMin(targetH, qMax(460, avail - 80));
    }
    resize(560, targetH);

    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    // Header: identity on the left, a Mission Control status strip on the
    // right that mirrors CallStage::paintTelemetry — same mono font, same
    // pulse dot, same data-density treatment. Both surfaces read as one
    // diagnostic system. Grounded on bg-primary, hairline divider beneath.
    auto *header = new QWidget(this);
    header->setObjectName("settingsHeader");
    auto *hlRoot = new QHBoxLayout(header);
    hlRoot->setContentsMargins(24, 18, 24, 14);
    hlRoot->setSpacing(16);

    auto *hl = new QVBoxLayout;
    hl->setContentsMargins(0, 0, 0, 0);
    hl->setSpacing(2);
    auto *hTitle = new QLabel(tr("Settings"), header);
    hTitle->setProperty("role", "title");
    auto *hSub = new QLabel(tr("Devices, notifications, updates and your account"),
                            header);
    hSub->setProperty("role", "secondary");
    { QFont f = hSub->font(); f.setPixelSize(11); hSub->setFont(f); }
    hl->addWidget(hTitle);
    hl->addWidget(hSub);
    hlRoot->addLayout(hl, 1);

    // #14 Mission Control mini-panel — version + build + channel chip
    // rendered in monospace, with a pulsing connection-health dot. Live
    // values, theme-driven colours, no hardcoded palette.
    auto *mcPanel = new QWidget(header);
    mcPanel->setObjectName("settingsMissionControl");
    auto *mcLayout = new QVBoxLayout(mcPanel);
    mcLayout->setContentsMargins(0, 2, 0, 0);
    mcLayout->setSpacing(3);

    QFont mono("Cascadia Mono", -1);
    if (!QFontInfo(mono).fixedPitch())
        mono = QFont("Consolas");

    auto monoLabel = [&](const QString &text, int px, const char *role) {
        auto *l = new QLabel(text, mcPanel);
        QFont f = mono;
        f.setPixelSize(px);
        f.setLetterSpacing(QFont::AbsoluteSpacing, 1.2);
        l->setFont(f);
        l->setProperty("role", role);
        l->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        return l;
    };

    mcLayout->addWidget(monoLabel(QStringLiteral("MISSION CONTROL"), 10, "eyebrow"));
    // Build identifier — version + codename + build timestamp baked in by CMake.
    const QString verLine = QStringLiteral("v" TALQ_VERSION " · ")
        + QString::fromLatin1(TALQ_VERSION_NAME)
#ifdef TALQ_BUILD_TS
        + QStringLiteral(" · ") + QString::fromLatin1(TALQ_BUILD_TS)
#endif
        ;
    mcLayout->addWidget(monoLabel(verLine, 11, "settingDesc"));

    // Channel chip — beta vs stable, matches the call-stage codec pill idiom.
#ifdef TALQ_PRERELEASE
    const QString chan = QStringLiteral("● BETA");
#else
    const QString chan = QStringLiteral("● STABLE");
#endif
    mcLayout->addWidget(monoLabel(chan, 11, "eyebrow"));

    hlRoot->addWidget(mcPanel, 0, Qt::AlignTop);
    mainLayout->addWidget(header);
    mainLayout->addWidget(makeDivider());

    // Each tab content can outgrow a small-screen laptop (the Audio &
    // Video tab in particular, with mic / speaker / camera / quality /
    // 8-tile background grid). Wrap each in a vertical-scroll
    // QScrollArea so the dialog stays at its compact default size and
    // the user can scroll within the active tab. Horizontal scrollbar
    // off; widgetResizable=true so the inner widget tracks the
    // viewport's width and only vertical content overflows.
    auto wrapInScroll = [this](QWidget *page) -> QWidget * {
        auto *scroll = new QScrollArea(this);
        scroll->setWidget(page);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        return scroll;
    };

    m_tabs = new QTabWidget(this);
    m_tabs->addTab(wrapInScroll(buildAudioVideoTab()),    "Audio && Video");
    m_tabs->addTab(wrapInScroll(buildNotificationsTab()), "Notifications");
    m_tabs->addTab(wrapInScroll(buildUpdatesTab()),       tr("Updates"));
    m_tabs->addTab(wrapInScroll(buildGeneralTab()),       "General");
    m_tabs->addTab(wrapInScroll(buildAccountTab()),       "Account");
    mainLayout->addWidget(m_tabs);

    // Tab bar inherits the app-wide AppStyle sheet (theme-driven, all 4
    // themes — no hardcoded dark palette here any more).

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
    layout->setContentsMargins(24, 22, 24, 22);
    layout->setSpacing(kRowGap);

    // ── Audio ──
    layout->addWidget(makeSectionHeader("Audio"));

    m_micCombo = new QComboBox;
    connect(m_micCombo, QOverload<int>::of(&QComboBox::activated),
            this, [this](int idx) { m_deviceManager->setSelectedAudioInput(idx); });
    layout->addWidget(makeSettingRow(tr("Microphone"), QString(), m_micCombo));

    m_noiseSuppression = new QCheckBox;
    m_noiseSuppression->setToolTip(
        tr("Filter background noise from your microphone during calls "
           "(applies to the next call)."));
    m_settings.beginGroup("Audio");
    m_noiseSuppression->setChecked(
        m_settings.value("noiseSuppression", true).toBool());
    m_settings.endGroup();
    connect(m_noiseSuppression, &QCheckBox::toggled, this, [this](bool checked) {
        m_settings.beginGroup("Audio");
        m_settings.setValue("noiseSuppression", checked);
        m_settings.endGroup();
    });
    layout->addWidget(makeSettingRow(
        tr("Noise suppression"),
        tr("Filter background noise during calls. Applies to the next call."),
        m_noiseSuppression));

    m_speakerCombo = new QComboBox;
    connect(m_speakerCombo, QOverload<int>::of(&QComboBox::activated),
            this, [this](int idx) { m_deviceManager->setSelectedAudioOutput(idx); });
    layout->addWidget(makeSettingRow(tr("Speaker"), QString(), m_speakerCombo));

    layout->addSpacing(kGroupGap - kRowGap);

    // ── Camera ──
    layout->addWidget(makeSectionHeader("Camera"));

    m_cameraCombo = new QComboBox;
    connect(m_cameraCombo, QOverload<int>::of(&QComboBox::activated),
            this, [this](int idx) {
                m_deviceManager->setSelectedVideoInput(idx);
                populateCameraQualityCombo();  // capabilities are per-camera
            });
    layout->addWidget(makeSettingRow(tr("Camera"), QString(), m_cameraCombo));

    // Real per-camera capability list (resolution × fps × format), not
    // fixed 1080p/720p presets. "Auto" = the absolute best mode the
    // device advertises (#126).
    m_cameraQualityCombo = new QComboBox;
    connect(m_cameraQualityCombo, QOverload<int>::of(&QComboBox::activated),
            this, [this](int) {
                m_deviceManager->setCameraQualityChoice(
                    m_cameraQualityCombo->currentData().toString());
            });
    layout->addWidget(makeSettingRow(
        tr("Camera quality"),
        tr("Auto picks the best mode your camera supports."),
        m_cameraQualityCombo));

    layout->addSpacing(kGroupGap - kRowGap);

    // ── Background (#20) ──
    // Settings keys mirror upstream Talk (spreed v23.0.4) so a hypothetical
    // cross-config import would round-trip: Talk/Backgrounds/virtualBackground{Enabled,Type,BlurStrength,Url}.
    layout->addWidget(makeSectionHeader("Background"));

    m_settings.beginGroup("Talk/Backgrounds");
    const bool    bgEnabled   = m_settings.value("virtualBackgroundEnabled", false).toBool();
    const QString bgType      = m_settings.value("virtualBackgroundType", "blur").toString();
    const int     bgStrength  = m_settings.value("virtualBackgroundBlurStrength", 10).toInt();
    const QString bgUrl       = m_settings.value("virtualBackgroundUrl", QString()).toString();
    m_settings.endGroup();

    // Debouncer shared by the slider and Choose… button. 200 ms collapse
    // window — fast enough that a release feels live, slow enough that
    // dragging the slider doesn't churn the publisher pipeline.
    m_bgSettingsDebounce = new QTimer(this);
    m_bgSettingsDebounce->setSingleShot(true);
    m_bgSettingsDebounce->setInterval(200);
    connect(m_bgSettingsDebounce, &QTimer::timeout, this,
            &SettingsDialog::backgroundSettingsChanged);

    m_bgModeCombo = new QComboBox;
    m_bgModeCombo->addItem(tr("Off"),   QStringLiteral("off"));
    m_bgModeCombo->addItem(tr("Blur"),  QStringLiteral("blur"));
    m_bgModeCombo->addItem(tr("Image"), QStringLiteral("image"));
    {
        const QString cur = bgEnabled ? bgType : QStringLiteral("off");
        int idx = m_bgModeCombo->findData(cur);
        m_bgModeCombo->setCurrentIndex(idx < 0 ? 0 : idx);
    }
    connect(m_bgModeCombo, QOverload<int>::of(&QComboBox::activated), this,
            [this](int) {
        const QString val = m_bgModeCombo->currentData().toString();
        m_settings.beginGroup("Talk/Backgrounds");
        m_settings.setValue("virtualBackgroundEnabled",
                            val != QStringLiteral("off"));
        // Always persist Type so Enabled/Type stay coherent. Off-mode
        // remembers the previously selected mode (defaults to "blur") so
        // an external Enabled toggle re-activates a sane state instead of
        // a stale (possibly-deleted-file) "image" leftover.
        if (val != QStringLiteral("off")) {
            m_settings.setValue("virtualBackgroundType", val);
        } else if (m_settings.value("virtualBackgroundType").toString().isEmpty()) {
            m_settings.setValue("virtualBackgroundType", QStringLiteral("blur"));
        }
        m_settings.endGroup();
        // Mode is a discrete choice — emit immediately, no debounce.
        emit backgroundSettingsChanged();
    });
    layout->addWidget(makeSettingRow(
        tr("Camera background"),
        tr("Off, Blur, or replace with an image. Applies live during calls."),
        m_bgModeCombo));

    m_bgBlurStrengthSlider = new QSlider(Qt::Horizontal);
    m_bgBlurStrengthSlider->setRange(1, 20);
    m_bgBlurStrengthSlider->setValue(qBound(1, bgStrength, 20));
    m_bgBlurStrengthSlider->setTickPosition(QSlider::NoTicks);
    connect(m_bgBlurStrengthSlider, &QSlider::valueChanged, this,
            [this](int v) {
        // Persist eagerly (cheap INI write) but coalesce the signal so
        // CallManager doesn't reconfigure the pipeline 20 times per drag.
        m_settings.beginGroup("Talk/Backgrounds");
        m_settings.setValue("virtualBackgroundBlurStrength", v);
        m_settings.endGroup();
        m_bgSettingsDebounce->start();
    });
    layout->addWidget(makeSettingRow(
        tr("Blur strength"),
        tr("Higher = stronger blur on the background plate."),
        m_bgBlurStrengthSlider));

    // Image picker — 8 bundled thumbnails (from qrc :/bg/backgrounds/) in
    // an IconMode grid + a separate "Choose…" button for user-supplied
    // images. Selection writes the qrc path (or the file path for user
    // images) to virtualBackgroundUrl. Bundled paths start with ":/bg/"
    // so the engine can distinguish them; QImage loads qrc paths natively.
    layout->addWidget(makeSectionHeader(tr("Background image")));

    m_bgImageGrid = new QListWidget;
    m_bgImageGrid->setViewMode(QListView::IconMode);
    m_bgImageGrid->setIconSize(QSize(120, 68));
    m_bgImageGrid->setGridSize(QSize(140, 92));
    m_bgImageGrid->setResizeMode(QListView::Adjust);
    m_bgImageGrid->setMovement(QListView::Static);
    m_bgImageGrid->setSelectionMode(QAbstractItemView::SingleSelection);
    m_bgImageGrid->setUniformItemSizes(true);
    m_bgImageGrid->setWrapping(true);
    m_bgImageGrid->setSpacing(8);
    m_bgImageGrid->setMinimumHeight(200);
    m_bgImageGrid->setFrameShape(QFrame::NoFrame);

    struct Bundled { const char *label; const char *qrc; };
    static const Bundled kBundled[] = {
        // Labels wrapped with QT_TR_NOOP so lupdate picks them up; the
        // tr() at use site below does the actual translation lookup.
        { QT_TR_NOOP("Office"),        ":/bg/backgrounds/1_office.jpg"        },
        { QT_TR_NOOP("Home"),          ":/bg/backgrounds/2_home.jpg"          },
        { QT_TR_NOOP("Abstract"),      ":/bg/backgrounds/3_abstract.jpg"      },
        { QT_TR_NOOP("Beach"),         ":/bg/backgrounds/4_beach.jpg"         },
        { QT_TR_NOOP("Park"),          ":/bg/backgrounds/5_park.jpg"          },
        { QT_TR_NOOP("Theater"),       ":/bg/backgrounds/6_theater.jpg"       },
        { QT_TR_NOOP("Library"),       ":/bg/backgrounds/7_library.jpg"       },
        { QT_TR_NOOP("Space Station"), ":/bg/backgrounds/8_space_station.jpg" },
    };

    // Decoded icons cached for the process lifetime — Settings is
    // reconstructed on every open in this codebase, so without the cache
    // the 8 ~1 MB JPGs would re-decode + re-scale (~150–300 ms) on
    // every Settings click. One-shot, ~2 MB of RGBA32 thumbnails resident.
    static const QList<QIcon> kIcons = []() {
        QList<QIcon> v;
        for (const Bundled &b : kBundled) {
            QImage img(QString::fromLatin1(b.qrc));
            if (img.isNull()) {
                qWarning() << "SettingsDialog: bundled background failed "
                              "to load from qrc:" << b.qrc
                           << "— tile suppressed";
                v.append(QIcon());   // empty, used as a "skip" sentinel below
                continue;
            }
            v.append(QIcon(QPixmap::fromImage(img.scaled(
                QSize(240, 136),
                Qt::KeepAspectRatioByExpanding,
                Qt::SmoothTransformation))));
        }
        return v;
    }();

    QListWidgetItem *initial = nullptr;

    // Lead with a (None) tile so users can clear an image-mode background
    // without flipping the mode combo to Off. UserRole = empty string;
    // the selection lambda writes empty + the engine returns rgba on
    // empty URL.
    {
        auto *none = new QListWidgetItem(tr("(None)"), m_bgImageGrid);
        none->setData(Qt::UserRole, QString());
        if (bgUrl.isEmpty()) initial = none;
    }

    for (int i = 0; i < int(sizeof(kBundled) / sizeof(kBundled[0])); ++i) {
        const Bundled &b = kBundled[i];
        if (kIcons[i].isNull()) continue;   // skip silently-failed tiles
        auto *item = new QListWidgetItem(kIcons[i], tr(b.label), m_bgImageGrid);
        item->setData(Qt::UserRole, QString::fromLatin1(b.qrc));
        if (bgUrl == QString::fromLatin1(b.qrc)) initial = item;
    }
    if (initial) m_bgImageGrid->setCurrentItem(initial);

    connect(m_bgImageGrid, &QListWidget::itemSelectionChanged, this, [this]() {
        auto *item = m_bgImageGrid->currentItem();
        if (!item) return;
        const QString path = item->data(Qt::UserRole).toString();
        m_settings.beginGroup("Talk/Backgrounds");
        m_settings.setValue("virtualBackgroundUrl", path);
        m_settings.endGroup();
        emit backgroundSettingsChanged();
    });

    layout->addWidget(m_bgImageGrid);

    // Browse… for user-supplied images. Sits below the grid; picking a
    // file clears the grid selection (we don't try to match user files
    // against the bundled set).
    auto *browseRow = new QHBoxLayout;
    browseRow->setContentsMargins(0, 0, 0, 0);
    m_bgImagePathLabel = new QLabel(bgUrl.isEmpty() || bgUrl.startsWith(QStringLiteral(":/"))
        ? tr("(none — pick a bundled background above, or browse for your own)")
        : QFileInfo(bgUrl).fileName());
    m_bgImagePathLabel->setProperty("role", "settingDesc");
    auto *browseBtn = new QPushButton(tr("Choose your own…"));
    browseBtn->setProperty("variant", "ghost");
    connect(browseBtn, &QPushButton::clicked, this, [this]() {
        const QString path = QFileDialog::getOpenFileName(this,
            tr("Choose background image"),
            QStandardPaths::writableLocation(QStandardPaths::PicturesLocation),
            tr("Images (*.png *.jpg *.jpeg *.webp *.bmp *.gif);;All files (*)"));
        if (path.isEmpty()) return;
        m_settings.beginGroup("Talk/Backgrounds");
        m_settings.setValue("virtualBackgroundUrl", path);
        m_settings.endGroup();
        m_bgImagePathLabel->setText(QFileInfo(path).fileName());
        m_bgImageGrid->clearSelection();   // user file beats any bundled pick
        emit backgroundSettingsChanged();
    });
    browseRow->addWidget(m_bgImagePathLabel, 1);
    browseRow->addWidget(browseBtn, 0);
    layout->addLayout(browseRow);

    layout->addStretch();

    auto *refreshBtn = new QPushButton(tr("Refresh devices"));
    refreshBtn->setProperty("variant", "ghost");
    connect(refreshBtn, &QPushButton::clicked, m_deviceManager,
            &MediaDeviceManager::refresh);
    auto *btnRow = new QHBoxLayout;
    btnRow->setContentsMargins(0, 0, 0, 0);
    btnRow->addStretch();
    btnRow->addWidget(refreshBtn);
    layout->addLayout(btnRow);

    return page;
}

// ============================================================
// Tab 2: Notifications
// ============================================================

QWidget *SettingsDialog::buildNotificationsTab()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(24, 22, 24, 22);
    layout->setSpacing(kRowGap);

    layout->addWidget(makeSectionHeader("Notifications"));

    m_notifEnabled = new QCheckBox;
    connect(m_notifEnabled, &QCheckBox::toggled, this, [this](bool checked) {
        m_notifications->setNotificationsEnabled(checked);
        m_settings.beginGroup("Notifications");
        m_settings.setValue("enabled", checked);
        m_settings.endGroup();
    });
    layout->addWidget(makeSettingRow(
        tr("Desktop notifications"),
        tr("Show a notification when a new message arrives."),
        m_notifEnabled));

    // Style — radios stacked in the row's control column.
    m_stylePopup = new QRadioButton(tr("In-app popup"));
    m_styleWindows = new QRadioButton(tr("Windows toast"));
    auto *styleGroup = new QButtonGroup(this);
    styleGroup->addButton(m_stylePopup, 0);
    styleGroup->addButton(m_styleWindows, 1);
    auto *styleCtl = new QWidget;
    {
        auto *v = new QVBoxLayout(styleCtl);
        v->setContentsMargins(0, 0, 0, 0);
        v->setSpacing(6);
        v->addWidget(m_stylePopup);
        v->addWidget(m_styleWindows);
    }
    connect(styleGroup, &QButtonGroup::idClicked, this, [this](int id) {
        QString style = (id == 0) ? "popup" : "windows";
        m_notifications->setNotifStyle(style);
        m_settings.beginGroup("Notifications");
        m_settings.setValue("style", style);
        m_settings.endGroup();
    });
    layout->addWidget(makeSettingRow(tr("Style"), QString(), styleCtl));

    layout->addSpacing(kGroupGap - kRowGap);
    layout->addWidget(makeSectionHeader("Sounds"));

    // Message sound — one combo for None / System default / each bundled
    // tone. Roster comes from NotificationManager::bundledTones() so the
    // Settings list and the tray submenu can never drift apart. Picking a
    // real tone auditions it once.
    m_soundCombo = new QComboBox;
    m_soundCombo->addItem(tr("None"),           QStringLiteral("none"));
    m_soundCombo->addItem(tr("System default"), QStringLiteral("system"));
    for (const auto &t : NotificationManager::bundledTones())
        m_soundCombo->addItem(t.second, t.first);
    connect(m_soundCombo, QOverload<int>::of(&QComboBox::activated),
            this, [this](int idx) {
        const QString id = m_soundCombo->itemData(idx).toString();
        m_notifications->setSoundId(id);
        m_settings.beginGroup("Notifications");
        m_settings.setValue("soundId", id);
        m_settings.endGroup();
        if (id != "none" && id != "system")
            m_notifications->playCurrentSound();  // audition
    });
    layout->addWidget(makeSettingRow(
        tr("Message sound"),
        tr("Plays when a new message arrives."),
        m_soundCombo));

    // Incoming-call ringtone — loops while a call is ringing. Roster from
    // CallManager::ringtones(); picking a tone auditions it once.
    m_ringtoneCombo = new QComboBox;
    for (const auto &r : CallManager::ringtones())
        m_ringtoneCombo->addItem(r.second, r.first);
    connect(m_ringtoneCombo, QOverload<int>::of(&QComboBox::activated),
            this, [this](int idx) {
        const QString id = m_ringtoneCombo->itemData(idx).toString();
        m_settings.beginGroup("Calls");
        m_settings.setValue("incomingRingtone", id);
        m_settings.endGroup();
        if (id != "none")
            CallManager::auditionRingtone(id);  // brief one-shot preview
    });
    layout->addWidget(makeSettingRow(
        tr("Call ringtone"),
        tr("Plays when someone calls you."),
        m_ringtoneCombo));

    layout->addSpacing(kGroupGap - kRowGap);

    // Calm callout (AppStyle role="hint" — full tint, no side-stripe).
    auto *hintFrame = new QFrame;
    hintFrame->setProperty("role", "hint");
    auto *hintLayout = new QVBoxLayout(hintFrame);
    hintLayout->setContentsMargins(14, 12, 12, 12);
    auto *hintLabel = new QLabel(
        tr("To mute individual conversations, right-click them in the sidebar."));
    hintLabel->setWordWrap(true);
    hintLabel->setProperty("role", "secondary");
    { QFont f = hintLabel->font(); f.setPixelSize(11); hintLabel->setFont(f); }
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
    layout->setContentsMargins(24, 22, 24, 22);
    layout->setSpacing(kRowGap);

    layout->addWidget(makeSectionHeader("Appearance"));

    m_themeCombo = new QComboBox();
    const PainterTheme::Theme kThemes[] = {
        PainterTheme::Theme::Ember, PainterTheme::Theme::Warm,
        PainterTheme::Theme::Vivid, PainterTheme::Theme::Paper
    };
    for (auto th : kThemes)
        m_themeCombo->addItem(PainterTheme::themeLabel(th), PainterTheme::themeId(th));
    m_settings.beginGroup("Theme");
    QString curThemeId = m_settings.value("theme",
        PainterTheme::themeId(PainterTheme::Theme::Vivid)).toString();
    m_settings.endGroup();
    {
        int idx = m_themeCombo->findData(curThemeId);
        m_themeCombo->setCurrentIndex(idx < 0 ? 2 : idx);  // 2 == Vivid (default)
    }
    connect(m_themeCombo, &QComboBox::currentIndexChanged, this, [this](int) {
        PainterTheme::Theme th = PainterTheme::themeFromId(
            m_themeCombo->currentData().toString(), PainterTheme::Theme::Vivid);
        emit themeIdChanged(static_cast<int>(th));
    });
    layout->addWidget(makeSettingRow(
        tr("Theme"),
        tr("Or cycle with Ctrl+D or the sidebar swatch."),
        m_themeCombo));

    layout->addSpacing(kGroupGap - kRowGap);
    layout->addWidget(makeSectionHeader("Startup"));

    m_autoStart = new QCheckBox;
    connect(m_autoStart, &QCheckBox::toggled, this, [this](bool checked) {
        m_appSettings->setAutoStart(checked);
        m_settings.beginGroup("General");
        m_settings.setValue("autoStart", checked);
        m_settings.endGroup();
    });
    layout->addWidget(makeSettingRow(
        tr("Start with Windows"), QString(), m_autoStart));

    m_startMinimized = new QCheckBox;
    connect(m_startMinimized, &QCheckBox::toggled, this, [this](bool checked) {
        m_settings.beginGroup("General");
        m_settings.setValue("startMinimized", checked);
        m_settings.endGroup();
    });
    layout->addWidget(makeSettingRow(
        tr("Start minimized to tray"), QString(), m_startMinimized));

    layout->addSpacing(kGroupGap - kRowGap);
    layout->addWidget(makeSectionHeader("Behavior"));

    m_closeToTray = new QCheckBox;
    connect(m_closeToTray, &QCheckBox::toggled, this, [this](bool checked) {
        m_settings.beginGroup("General");
        m_settings.setValue("closeToTray", checked);
        m_settings.endGroup();
        emit closeToTrayChanged(checked);
    });
    layout->addWidget(makeSettingRow(
        tr("Close to tray"),
        tr("Minimize to the tray instead of quitting."),
        m_closeToTray));

    layout->addSpacing(kGroupGap - kRowGap);
    layout->addWidget(makeSectionHeader("Diagnostics"));

    // Detailed debug logging. Always-on by default — a comms app dying
    // mid-call without a log is the recurring "no evidence" failure. Users
    // who want a smaller log can opt out here; effect on next launch (the
    // flag is read once in main.cpp before Qt is up).
    m_detailedLogging = new QCheckBox;
    m_settings.beginGroup("Diagnostics");
    m_detailedLogging->setChecked(m_settings.value("detailedLogging", true).toBool());
    m_settings.endGroup();
    connect(m_detailedLogging, &QCheckBox::toggled, this, [this](bool on) {
        m_settings.beginGroup("Diagnostics");
        m_settings.setValue("detailedLogging", on);
        m_settings.endGroup();
        // Live-apply for app qDebug. GST_DEBUG level is captured by GStreamer
        // at gst_init time → only a restart flips the GST trace floor.
        TalqLog::g_verbose = on;
    });
    layout->addWidget(makeSettingRow(
        tr("Detailed debug logging"),
        tr("Capture app diagnostics in talq_debug.log "
           "(recommended — helps debug call issues). Disable for a smaller log."),
        m_detailedLogging));

    layout->addStretch();
    return page;
}

// ============================================================
// Tab 4: Updates
// ============================================================

QWidget *SettingsDialog::buildUpdatesTab()
{
    auto *w = new QWidget(this);
    auto *lay = new QVBoxLayout(w);
    lay->setContentsMargins(24, 22, 24, 22);
    lay->setSpacing(kRowGap);

    lay->addWidget(makeSectionHeader("Updates"));

    m_updatesAutoCheck = new QCheckBox(w);
    m_updatesAutoCheck->setChecked(
        QSettings().value(QStringLiteral("updates/autoCheck"), true).toBool());
    connect(m_updatesAutoCheck, &QCheckBox::toggled, this, [](bool checked) {
        QSettings().setValue(QStringLiteral("updates/autoCheck"), checked);
    });
    lay->addWidget(makeSettingRow(
        tr("Automatic updates"),
        tr("Check at startup and every 5 minutes. A banner appears when a "
           "new version is ready."),
        m_updatesAutoCheck));

    // #116 — opt-in pre-release channel. Persist the flag and trigger a
    // re-check (mirrors autoCheck's decoupling). UpdateChecker reads
    // updates/betaChannel on its next check and falls back to stable when
    // no beta build / manifest is available.
    m_updatesBeta = new QCheckBox(w);
    m_updatesBeta->setChecked(
        QSettings().value(QStringLiteral("updates/betaChannel"), false).toBool());
    connect(m_updatesBeta, &QCheckBox::toggled, this, [this](bool checked) {
        QSettings().setValue(QStringLiteral("updates/betaChannel"), checked);
        emit checkForUpdatesRequested();
    });
    lay->addWidget(makeSettingRow(
        tr("Pre-release updates"),
        tr("Get beta builds before general release. Falls back to stable "
           "automatically if no beta is available."),
        m_updatesBeta));

    lay->addSpacing(kGroupGap - kRowGap);

    auto *checkBtn = new QPushButton(tr("Check for updates now"), w);
    checkBtn->setProperty("variant", "ghost");
    auto *checkStatus = new QLabel(w);
    checkStatus->setProperty("role", "secondary");
    { QFont f = checkStatus->font(); f.setPixelSize(11); checkStatus->setFont(f); }
    auto *btnRow = new QHBoxLayout;
    btnRow->setContentsMargins(0, 0, 0, 0);
    btnRow->setSpacing(12);
    btnRow->addWidget(checkBtn);
    btnRow->addWidget(checkStatus, 1);
    lay->addLayout(btnRow);

    connect(checkBtn, &QPushButton::clicked, this, [this, checkBtn, checkStatus]() {
        checkBtn->setEnabled(false);
        checkStatus->setText(tr("Checking…"));
        emit checkForUpdatesRequested();
        // No checkFinished signal on UpdateChecker — re-enable + show a
        // neutral status after a delay. If an update IS found, the banner
        // mechanism shows it at the top of the chat regardless.
        QTimer::singleShot(3500, this, [checkBtn, checkStatus]() {
            checkBtn->setEnabled(true);
            checkStatus->setText(tr("Checked just now. If no banner appeared, "
                                    "you're on the latest version."));
        });
    });

    lay->addStretch();
    return w;
}

// ============================================================
// Tab 5: Account
// ============================================================

QWidget *SettingsDialog::buildAccountTab()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(24, 22, 24, 22);
    layout->setSpacing(10);

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
    m_serverUrlLabel->setProperty("role", "secondary");
    layout->addWidget(m_serverUrlLabel);

    layout->addSpacing(4);
    layout->addWidget(makeDivider());
    layout->addSpacing(4);

    // Server info
    layout->addWidget(makeSectionHeader("SERVER"));

    auto *serverFrame = new QFrame;
    serverFrame->setProperty("role", "card");

    auto *serverLayout = new QVBoxLayout(serverFrame);
    serverLayout->setContentsMargins(8, 8, 8, 8);
    auto *serverUrlDisplay = new QLabel;
    serverUrlDisplay->setFont(urlFont);
    serverUrlDisplay->setProperty("role", "secondary");
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
    m_ncVersionLabel->setProperty("role", "secondary");
    m_talkVersionLabel = new QLabel;
    m_talkVersionLabel->setFont(infoFont);
    m_talkVersionLabel->setProperty("role", "secondary");
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
    m_talqVersionLabel->setProperty("role", "secondary");
    bottomRow->addWidget(m_talqVersionLabel);
    bottomRow->addStretch();

    auto *logoutBtn = new QPushButton("Log out");
    logoutBtn->setProperty("variant", "danger");
    connect(logoutBtn, &QPushButton::clicked, this, [this]() {
        m_auth->logout();
        accept(); // close the dialog
    });
    bottomRow->addWidget(logoutBtn);
    layout->addLayout(bottomRow);

    // Release codename credit. 0.39.x betas carry "Aprilsko Vastanie"
    // (Aprilsko vastanie / Априлско въстание — the April Uprising of
    // 1876), marking the 150th anniversary in 2026 of the revolt that
    // led to Bulgaria's liberation. Shown only when a codename is set
    // for the release (TALQ_VERSION_NAME baked in by CMake).
    const QString verName = QStringLiteral(TALQ_VERSION_NAME);
    if (!verName.isEmpty()) {
        layout->addSpacing(6);
        auto *credit = new QLabel(
            tr("Codename \"%1\" — Bulgaria's April Uprising of 1876, "
               "150th anniversary (2026).").arg(verName));
        credit->setFont(infoFont);
        credit->setProperty("role", "secondary");
        credit->setWordWrap(true);
        layout->addWidget(credit);
    }

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

    populateCameraQualityCombo();
}

void SettingsDialog::populateCameraQualityCombo()
{
    if (!m_cameraQualityCombo) return;
    m_cameraQualityCombo->blockSignals(true);
    m_cameraQualityCombo->clear();

    const int idx = m_deviceManager->selectedVideoInput();
    const QVector<CameraMode> modes = m_deviceManager->cameraModes(idx);
    const CameraMode best = m_deviceManager->autoCameraMode(idx);

    Q_UNUSED(best);
    // Auto = let the camera negotiate (always starts). Explicit modes
    // below are opt-in overrides.
    m_cameraQualityCombo->addItem(tr("Automatic (recommended)"),
                                  QStringLiteral("auto"));
    for (const CameraMode &m : modes)
        m_cameraQualityCombo->addItem(m.label(), m.key());

    const QString choice = m_deviceManager->cameraQualityChoice();
    int sel = m_cameraQualityCombo->findData(choice);
    m_cameraQualityCombo->setCurrentIndex(sel >= 0 ? sel : 0);  // 0 = Auto
    m_cameraQualityCombo->setEnabled(m_cameraQualityCombo->count() > 1);

    m_cameraQualityCombo->blockSignals(false);
}

void SettingsDialog::loadNotificationSettings()
{
    m_settings.beginGroup("Notifications");
    bool enabled = m_settings.value("enabled", true).toBool();
    QString style = m_settings.value("style", "popup").toString();
    // Migrate the pre-0.33 soundMode key if soundId isn't set yet.
    QString soundId = m_settings.value("soundId").toString();
    if (soundId.isEmpty()) {
        const QString old = m_settings.value("soundMode", "internal").toString();
        soundId = (old == "system") ? "system" : (old == "none") ? "none" : "chime";
    }
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

    m_soundCombo->blockSignals(true);
    int idx = m_soundCombo->findData(soundId);
    if (idx < 0) idx = m_soundCombo->findData(QStringLiteral("chime"));
    m_soundCombo->setCurrentIndex(idx);
    m_soundCombo->blockSignals(false);

    m_settings.beginGroup("Calls");
    const QString ringId = m_settings.value("incomingRingtone", "classic").toString();
    m_settings.endGroup();
    m_ringtoneCombo->blockSignals(true);
    int ridx = m_ringtoneCombo->findData(ringId);
    if (ridx < 0) ridx = m_ringtoneCombo->findData(QStringLiteral("classic"));
    m_ringtoneCombo->setCurrentIndex(ridx);
    m_ringtoneCombo->blockSignals(false);
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

    if (m_themeCombo) {
        m_settings.beginGroup("Theme");
        QString tid = m_settings.value("theme",
            PainterTheme::themeId(PainterTheme::Theme::Vivid)).toString();
        m_settings.endGroup();
        int idx = m_themeCombo->findData(tid);
        m_themeCombo->blockSignals(true);
        m_themeCombo->setCurrentIndex(idx < 0 ? 2 : idx);
        m_themeCombo->blockSignals(false);
    }
}
