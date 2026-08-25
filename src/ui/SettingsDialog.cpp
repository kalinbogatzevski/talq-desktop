#include "SettingsDialog.h"

#include "core/CtiService.h"

#include <QLineEdit>
#include "painter/PainterTheme.h"
#include "core/MediaDeviceManager.h"
#include "core/Diagnostics.h"
#include "core/NotificationManager.h"
#include "core/CallManager.h"
#include "core/TalqLog.h"
#include "core/AppSettings.h"
#include "core/AuthManager.h"
#include "core/BackgroundEngine.h"
#include "core/EncodeTier.h"
#include "core/MicTester.h"
#include "BgPreviewSource.h"
#include <QPainter>

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
#include <QFile>
#include <QDir>
#include <QUrl>
#include <QDateTime>
#include <QMessageBox>
#include <QSignalBlocker>
#include <QPushButton>
#include <QDesktopServices>
#include <QFrame>
#include <QFont>
#include <QListWidget>
#include <QMenu>
#include <QRegularExpression>
#include <QScrollArea>
#include <QScrollBar>
#include <QSlider>
#include <QStandardPaths>
#include <QTimer>
#include <QElapsedTimer>
#include <memory>
#include <QPaintEvent>

#include "CodenameBlurb.h"   // codenameBlurb() — shared with MainWindow's codename pill

// A small horizontal mic-test level meter: a calm track with a fill that
// tracks the live capture level (green, warming to amber then red near the
// top) plus a slowly-decaying peak tick. No Q_OBJECT — pure paint, driven by
// setLevel() from MicTester. Green/amber/red here are functional level
// semantics (like an audio VU), not theme chrome.
class MicLevelBar : public QWidget
{
public:
    explicit MicLevelBar(QWidget *parent = nullptr) : QWidget(parent)
    {
        setFixedHeight(10);
    }
    void setLevel(double v)
    {
        m_level = qBound(0.0, v, 1.0);
        m_peak  = (m_level > m_peak) ? m_level : qMax(0.0, m_peak - 0.04);
        update();
    }
protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        const qreal rad = r.height() / 2.0;
        p.setPen(Qt::NoPen);
        QColor track = palette().color(QPalette::Mid); track.setAlphaF(0.35);
        p.setBrush(track);
        p.drawRoundedRect(r, rad, rad);
        if (m_level > 0.001) {
            QRectF f = r; f.setWidth(r.width() * m_level);
            const QColor c = (m_level < 0.7) ? QColor(0x35, 0xC7, 0x59)
                           : (m_level < 0.9) ? QColor(0xE8, 0xB3, 0x39)
                                             : QColor(0xE0, 0x5A, 0x4F);
            p.setBrush(c);
            p.drawRoundedRect(f, rad, rad);
        }
        if (m_peak > 0.02) {
            const qreal x = r.left() + r.width() * m_peak;
            QColor pk = palette().color(QPalette::Text); pk.setAlphaF(0.6);
            p.setPen(QPen(pk, 1.5));
            p.drawLine(QPointF(x, r.top() + 1), QPointF(x, r.bottom() - 1));
        }
    }
private:
    double m_level = 0.0;
    double m_peak  = 0.0;
};

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
// the next group's eyebrow. Was a local 14/26 pair with a comment claiming
// "DESIGN.md spacing scale (8 / 20)" -- neither number is in DESIGN.md's
// actual scale (tiny/sm/md/lg/xl = 4/8/12/16/24) nor matched the comment's
// own claimed values. Now the real tokens: spacingNormal for the tight
// in-group gap, spacingXLarge for the group break.
static constexpr int kRowGap   = PainterTheme::spacingNormal;
static constexpr int kGroupGap = PainterTheme::spacingXLarge;

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

// The active theme as persisted by the picker (same key MainWindow reads),
// so the Background tab's hand-styled chrome is tinted with the user's
// chosen theme instead of a fixed teal/gray.
static PainterTheme activeTheme()
{
    QSettings s("TalQ", "TalQ");
    s.beginGroup("Theme");
    const QString tid = s.value("theme",
        PainterTheme::themeId(PainterTheme::Theme::Vivid)).toString();
    s.endGroup();
    return PainterTheme(PainterTheme::themeFromId(tid, PainterTheme::Theme::Vivid));
}

// Re-style the Background tab's two hand-rolled chrome widgets from the
// active PainterTheme. The live-preview placeholder uses a recessed ground
// (one step down the warm ladder) + secondary text; the image grid marks
// the selected thumbnail with an accent tint + accent border + primary
// text and lifts hovered items by one ladder step. Re-applied on
// ApplicationPaletteChange so a live theme switch retints both. The mic
// test's level-meter colours are functional (VU semantics) and live in
// MicLevelBar — untouched here.
static void styleBackgroundChrome(QLabel *previewLabel, QListWidget *imageGrid)
{
    const PainterTheme th = activeTheme();
    auto n = [](const QColor &c) { return c.name(QColor::HexRgb); };
    // Pre-blend the accent over the surface ground so the selected-tile fill
    // is a calm solid tint (no alpha in the stylesheet), matching the
    // TopicTabBar selected-chip idiom.
    auto blend = [](const QColor &fg, const QColor &bg, double a) {
        return QColor(int(fg.red()   * a + bg.red()   * (1 - a)),
                      int(fg.green() * a + bg.green() * (1 - a)),
                      int(fg.blue()  * a + bg.blue()  * (1 - a)));
    };

    if (previewLabel) {
        previewLabel->setStyleSheet(QStringLiteral(
            "QLabel { background:%1; border-radius:6px; color:%2; }")
            .arg(n(th.bgSidebar), n(th.textSecondary)));
    }
    if (imageGrid) {
        imageGrid->setStyleSheet(QStringLiteral(
            "QListWidget { background:transparent; }"
            "QListWidget::item { "
            "  border:2px solid transparent; "
            "  border-radius:6px; "
            "  padding:4px; }"
            "QListWidget::item:hover { "
            "  background:%1; }"
            "QListWidget::item:selected { "
            "  border:2px solid %2; "
            "  background:%3; "
            "  color:%4; }")
            .arg(n(th.bgHover),
                 n(th.accent),
                 n(blend(th.accent, th.bgSurface, 0.18)),
                 n(th.textPrimary)));
    }
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
    {
        auto *verLbl = monoLabel(verLine, 11, "settingDesc");
        // 0.53.0 — hover the codename to read what it means (also shown in full as
        // the About-tab credit). Same source so the two never drift.
        const QString cb = codenameBlurb(QString::fromLatin1(TALQ_VERSION_NAME));
        if (!cb.isEmpty()) verLbl->setToolTip(cb);
        mcLayout->addWidget(verLbl);
    }

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
    m_tabs->addTab(wrapInScroll(buildAudioVideoTab()),    tr("Audio && Video"));
    m_tabs->addTab(wrapInScroll(buildBackgroundTab()),    tr("Background"));
    m_tabs->addTab(wrapInScroll(buildNotificationsTab()), tr("Notifications"));
    m_tabs->addTab(wrapInScroll(buildPhoneTab()),         tr("Phone"));
    m_tabs->addTab(wrapInScroll(buildUpdatesTab()),       tr("Updates"));
    m_tabs->addTab(wrapInScroll(buildGeneralTab()),       tr("General"));
    m_tabs->addTab(wrapInScroll(buildAccountTab()),       tr("Account"));
    mainLayout->addWidget(m_tabs);

    // Tab bar inherits the app-wide AppStyle sheet (theme-driven, all 4
    // themes — no hardcoded dark palette here any more).

    // Connect device manager signals to refresh combos
    connect(m_deviceManager, &MediaDeviceManager::devicesChanged, this, &SettingsDialog::populateDeviceCombos);

    // Disable mouse-wheel scrolling on EVERY QComboBox in the dialog.
    // Reasoning is the same as for the BG mode combo: wheel-while-
    // hovering can silently change a selection if the save handler is
    // wired to activated() (click-only), which several of our combos
    // are. The eventFilter at this dialog level returns true for any
    // QEvent::Wheel that arrives at a QComboBox - one filter, all
    // combos covered (including ones added in the future).
    for (auto *combo : findChildren<QComboBox *>())
        combo->installEventFilter(this);
}

void SettingsDialog::selectAudioVideoTab()
{
    if (m_tabs) m_tabs->setCurrentIndex(0);
}

void SettingsDialog::setUpdateNowStatus(const QString &text)
{
    // 0.52.14 — MainWindow reports the manual "Update now" outcome here. Show it,
    // then auto-revert to the actionable label (covers "You're up to date" /
    // "Couldn't check"; on a real install the app restarts before this fires).
    if (!m_updateNowBtn) return;
    m_updateNowBtn->setText(text);
    QTimer::singleShot(4000, this, [this]() {
        if (!m_updateNowBtn) return;
        m_updateNowBtn->setText(tr("Update now"));
        m_updateNowBtn->setEnabled(true);
    });
}

void SettingsDialog::refresh()
{
    // Re-scan devices ONLY if we have never enumerated. The GstDeviceMonitor
    // scan OPENS the camera to read its caps, which cold-loads the MediaFoundation
    // Frame-Server + camera driver DLLs (they unload after each scan) and holds
    // the Windows loader lock for ~5 s — that was THE Settings-open freeze, on
    // EVERY open (the scan ran every time). The live hotplug monitor already
    // keeps the device lists current between opens, so a per-open re-scan is
    // pure cost. First population happens at startup (behind the splash) and on
    // real device hotplug — never on the UI-blocking Settings-open path.
    if (!m_deviceManager->hasEnumerated())
        m_deviceManager->refreshAsync();
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
            this, [this](int idx) {
        m_deviceManager->setSelectedAudioInput(idx);
        // Re-point the live test at the newly chosen device.
        if (m_micTester && isVisible())
            m_micTester->start(m_deviceManager->selectedInputDeviceId());
    });
    layout->addWidget(makeSettingRow(tr("Microphone"), QString(), m_micCombo));

    // Live mic test: a level meter that moves when you speak, so you can
    // confirm the selected device actually captures (the lowest-friction
    // "is my mic working?" check). Fed by a MicTester capture pipeline on the
    // selected device; started/stopped with the dialog (showEvent/hideEvent).
    m_micLevel = new MicLevelBar;
    layout->addWidget(makeSettingRow(
        tr("Mic test"),
        tr("Speak — the bar moves if the selected microphone is working."),
        m_micLevel));
    m_micTester = new MicTester(this);
    connect(m_micTester, &MicTester::level, this, [this](double v) {
        if (m_micLevel) m_micLevel->setLevel(v);
    });

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

    m_autoGainControl = new QCheckBox;
    m_autoGainControl->setToolTip(
        tr("Automatically level your microphone volume so you stay at a "
           "consistent loudness (applies to the next call)."));
    m_settings.beginGroup("Audio");
    m_autoGainControl->setChecked(
        m_settings.value("audioAutoGainControl", true).toBool());
    m_settings.endGroup();
    connect(m_autoGainControl, &QCheckBox::toggled, this, [this](bool checked) {
        m_settings.beginGroup("Audio");
        m_settings.setValue("audioAutoGainControl", checked);
        m_settings.endGroup();
    });
    layout->addWidget(makeSettingRow(
        tr("Automatic gain control"),
        tr("Keep your mic at a steady level so quiet speech is boosted. "
           "Applies to the next call."),
        m_autoGainControl));

    m_echoCancellation = new QCheckBox;
    m_echoCancellation->setToolTip(
        tr("Stop the people you call from hearing their own voice echoed back "
           "when you use speakers instead of a headset (applies to the next call)."));
    m_settings.beginGroup("Audio");
    m_echoCancellation->setChecked(
        m_settings.value("echoCancellation", true).toBool());
    m_settings.endGroup();
    connect(m_echoCancellation, &QCheckBox::toggled, this, [this](bool checked) {
        // A FOOTGUN WITH INVERTED INCENTIVES — confirm before disabling.
        //
        // Whoever unticks this box suffers NOTHING: echo is heard by the people they
        // call, who cannot fix it from their side and cannot even see that it is off.
        // One silent click here can make every future call sound broken to everyone
        // else, and the person responsible never hears it.
        //
        // Measured 2026-08-12, which removes the usual reason to want it off: with AEC
        // on, our send path suppresses echo 8-14 dB BELOW the room's noise floor AND
        // preserves the near-end talker during double-talk (+1.23 dB — the voice comes
        // through slightly LOUDER, not ducked). Turning it off buys no audio quality;
        // it only exports a defect to whoever you call.
        if (!checked) {
            const auto answer = QMessageBox::warning(
                this, tr("Turn off echo cancellation?"),
                tr("<b>You will not hear the problem this causes — the people you call "
                   "will.</b><br><br>"
                   "With echo cancellation off, everyone you call hears their own voice "
                   "echoed back from your microphone. They cannot fix it from their side, "
                   "and they have no way to see that you have switched it off.<br><br>"
                   "It costs you nothing to leave on: it removes your speakers from your "
                   "microphone without making your own voice any quieter.<br><br>"
                   "Only turn this off if your headset or audio hardware already cancels "
                   "echo itself."),
                QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
            if (answer != QMessageBox::Yes) {
                // Restore without re-entering this handler.
                QSignalBlocker block(m_echoCancellation);
                m_echoCancellation->setChecked(true);
                return;
            }
        }
        m_settings.beginGroup("Audio");
        m_settings.setValue("echoCancellation", checked);
        m_settings.endGroup();
    });
    layout->addWidget(makeSettingRow(
        tr("Echo cancellation"),
        tr("Remove your speaker output from your microphone so callers don't "
           "hear themselves. Best with speakers. Applies to the next call."),
        m_echoCancellation));

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

    // 0.41.4-beta — Presentation mode + Maximum send resolution. The
    // 0.41.0 combo exposed 720p/1080p/1440p/2160p unconditionally; a
    // field report ("both streams froze mid-call at 22.69 Mbps") made
    // it clear that 2K/4K saturate residential uplinks on BOTH ends
    // and are not safe defaults for casual calls. We now gate the
    // high tiers behind an explicit "Presentation mode" checkbox.
    // No intrinsic label text: makeSettingRow() supplies the name in the
    // left column and right-aligns the bare indicator. A text label here
    // would render a SECOND (wide) caption inside the narrow control column
    // that overflows left and collides with the row name — the "checkboxes
    // overlapping" report.
    auto *presentationCheck = new QCheckBox(this);
    auto *resCombo = new QComboBox(this);

    auto repopulateRes = [resCombo](bool presentationMode, int currentSaved) {
        resCombo->blockSignals(true);
        resCombo->clear();
        resCombo->addItem(tr("720p HD — recommended (fits most connections)"),  720);
        resCombo->addItem(tr("1080p Full HD — Ultra (needs a strong upload)"),  1080);
        if (presentationMode) {
            resCombo->addItem(tr("1440p 2K (presentation only)"),    1440);
            resCombo->addItem(tr("2160p 4K (presentation only)"),    2160);
        }
        // If the saved value is no longer in the list (e.g. user just
        // turned presentation OFF while max was 2160), fall back to 1080.
        int idx = resCombo->findData(currentSaved);
        if (idx < 0 && !presentationMode && currentSaved > 1080) {
            QSettings vs("TalQ", "TalQ");
            vs.beginGroup("Video");
            vs.setValue("maxSendHeight", 1080);
            vs.endGroup();
            idx = resCombo->findData(1080);
        }
        resCombo->setCurrentIndex(idx >= 0 ? idx : 0);
        resCombo->blockSignals(false);
    };

    bool presentationMode;
    int  savedRes;
    {
        QSettings vs("TalQ", "TalQ");
        vs.beginGroup("Video");
        presentationMode = vs.value("presentationMode", false).toBool();
        savedRes         = vs.value("maxSendHeight",    720).toInt();
        vs.endGroup();
    }
    presentationCheck->setChecked(presentationMode);
    repopulateRes(presentationMode, savedRes);

    connect(presentationCheck, &QCheckBox::toggled, this,
            [presentationCheck, resCombo, repopulateRes]() {
                const bool on = presentationCheck->isChecked();
                QSettings vs("TalQ", "TalQ");
                vs.beginGroup("Video");
                vs.setValue("presentationMode", on);
                const int cur = vs.value("maxSendHeight", 720).toInt();
                vs.endGroup();
                repopulateRes(on, cur);
            });
    connect(resCombo, QOverload<int>::of(&QComboBox::activated),
            this, [resCombo]() {
                QSettings vs("TalQ", "TalQ");
                vs.beginGroup("Video");
                vs.setValue("maxSendHeight",
                             resCombo->currentData().toInt());
                vs.endGroup();
            });
    layout->addWidget(makeSettingRow(
        tr("Presentation mode"),
        tr("Unlocks 2K and 4K send resolutions for screen-share and "
           "high-bandwidth demos. Casual video calls should keep this off."),
        presentationCheck));
    layout->addWidget(makeSettingRow(
        tr("Maximum send resolution"),
        tr("Takes effect on the next call. Higher resolutions need both a "
           "capable camera and a healthy link — TalQ adapts down to fit."),
        resCombo));

    // 0.61.0 — weak-tier HD opt-in. Only affects machines classified below
    // Capable (they send ONE stream: 360p normally, 480p when this is on).
    // Greyed on a Capable machine (its resolution is governed by the combo above).
    auto *weakHdCheck = new QCheckBox(this);
    {
        QSettings vs("TalQ", "TalQ");
        vs.beginGroup("Video");
        weakHdCheck->setChecked(vs.value("weakHdEnable", false).toBool());
        const int lastCls = vs.value("lastGpuClass", int(talq::GpuClass::Capable)).toInt();
        vs.endGroup();
        const bool isCapable = (lastCls == int(talq::GpuClass::Capable));
        weakHdCheck->setEnabled(!isCapable);
        weakHdCheck->setToolTip(isCapable
            ? tr("This device sends full-quality video; the HD option applies only to "
                 "low-power devices that fall back to a single stream.")
            : tr("Send 480p instead of 360p as this device's single video stream. "
                 "Uses more CPU."));
    }
    connect(weakHdCheck, &QCheckBox::toggled, this, [](bool on) {
        QSettings vs("TalQ", "TalQ");
        vs.beginGroup("Video");
        vs.setValue("weakHdEnable", on);
        vs.endGroup();
    });
    layout->addWidget(makeSettingRow(
        tr("Send HD (480p) on this device"),
        tr("Weak devices normally send 360p as their single video stream; this "
           "switches them to 480p. Has no effect on capable machines."),
        weakHdCheck));

    // GPU performance override. The device-capability caps (camera 480p + capped
    // screen share on weak iGPUs, to stop the screen-share overload freeze)
    // auto-classify the GPU from its model name + hardware encoder. This lets a
    // user override a wrong guess: "Always full quality" lifts the caps (native
    // screen share + uncapped camera) on a capable GPU TalQ didn't recognise;
    // "Always protected" forces the caps. Stored at Video/gpuClassOverride
    // (0 Auto / 1 AlwaysFull / 2 AlwaysProtected), read in CallManager at startup.
    auto *gpuPerfCombo = new QComboBox(this);
    gpuPerfCombo->addItem(tr("Auto (recommended)"),           0);
    gpuPerfCombo->addItem(tr("Always full quality"),          1);
    gpuPerfCombo->addItem(tr("Always protected (cap video)"), 2);
    {
        QSettings vs("TalQ", "TalQ");
        vs.beginGroup("Video");
        const int cur = vs.value("gpuClassOverride", 0).toInt();
        vs.endGroup();
        const int gi = gpuPerfCombo->findData(cur);
        gpuPerfCombo->setCurrentIndex(gi >= 0 ? gi : 0);
    }
    connect(gpuPerfCombo, QOverload<int>::of(&QComboBox::activated),
            this, [gpuPerfCombo]() {
                QSettings vs("TalQ", "TalQ");
                vs.beginGroup("Video");
                vs.setValue("gpuClassOverride", gpuPerfCombo->currentData().toInt());
                vs.endGroup();
            });
    layout->addWidget(makeSettingRow(
        tr("GPU performance"),
        tr("TalQ caps video quality on weak integrated GPUs to stop screen "
           "sharing from overloading them. Auto detects your GPU; choose "
           "“Always full quality” to lift the caps (takes effect next call)."),
        gpuPerfCombo));

    // Verbose call diagnostics — gates the per-second [MEDIA]/[LEAK] heartbeats
    // CallManager logs during a call (call-health trajectory + cross-thread
    // frame-flow gauges) for tracking down freezes / memory growth in the
    // field. OFF by default (they qInfo every second and force a disk sync).
    // Write-only here; CallManager reads Debug/mediaDiagnostics at the start of
    // each call (and the TALQ_MEDIA_DIAG env var overrides it on dev builds).
    auto *diagCheck = new QCheckBox();
    diagCheck->setToolTip(
        tr("Logs detailed per-second call diagnostics to the debug log to help "
           "track down call freezes or memory issues. Leave off for normal "
           "use; turn it on only when reproducing a problem."));
    {
        QSettings ds("TalQ", "TalQ");
        diagCheck->setChecked(ds.value("Debug/mediaDiagnostics", false).toBool());
    }
    connect(diagCheck, &QCheckBox::toggled, this, [](bool checked) {
        QSettings ds("TalQ", "TalQ");
        ds.setValue("Debug/mediaDiagnostics", checked);
    });
    layout->addWidget(makeSettingRow(
        tr("Verbose call diagnostics"),
        tr("Logs detailed call diagnostics for troubleshooting freezes or "
           "memory issues. Off by default; takes effect on the next call."),
        diagCheck));

    // #72 — screen-share monitor border toggle. The thin frame around a
    // full-screen share is on by default as a "you are sharing" signal;
    // users who find it distracting (the Zoom-green-frame complaint) switch
    // it off here. Window/app shares never draw a frame regardless.
    m_ssBorderCheck = new QCheckBox;
    m_ssBorderCheck->setToolTip(
        tr("Draw a thin border around the screen you are sharing. Only applies "
           "to full-screen shares; sharing a single window never shows a "
           "border."));
    {
        QSettings vs("TalQ", "TalQ");
        m_ssBorderCheck->setChecked(
            vs.value("Video/screenShareBorder", true).toBool());
    }
    connect(m_ssBorderCheck, &QCheckBox::toggled, this, [](bool checked) {
        QSettings("TalQ", "TalQ").setValue("Video/screenShareBorder", checked);
    });
    layout->addWidget(makeSettingRow(
        tr("Show border around shared screen"),
        tr("A thin frame marks the screen you're sharing. Applies to the next "
           "share. Sharing a single window never shows a border."),
        m_ssBorderCheck));

    // 0.41.3-beta — playback-gain control removed. Telegram, Zoom and
    // Meet do not surface a "playback volume" setting; their receive-
    // side AudioProcessing AGC handles it automatically. TalQ now
    // does the same with audiodynamic + rglimiter on the receive
    // chain (PeerPipeline / SubscribePipeline). The 1.8× constant in
    // QSettings("TalQ","TalQ")/Audio/playbackGain is the only knob,
    // and it stays at default unless a power user edits it directly.

    layout->addStretch();

    auto *refreshBtn = new QPushButton(tr("Refresh devices"));
    refreshBtn->setProperty("variant", "default");
    connect(refreshBtn, &QPushButton::clicked, m_deviceManager,
            &MediaDeviceManager::refreshAsync);
    auto *btnRow = new QHBoxLayout;
    btnRow->setContentsMargins(0, 0, 0, 0);
    btnRow->addStretch();
    btnRow->addWidget(refreshBtn);
    layout->addLayout(btnRow);

    return page;
}

// ============================================================
// Tab: Background (selfie blur / image replacement, #20)
// ============================================================
// Split out of the Audio & Video tab (0.41.x): the background controls —
// live preview, blur slider, the 8-tile image grid and the file picker —
// are the single biggest block in Settings and made the combined tab a
// long, dense scroll. On their own page they have room to breathe and the
// Audio & Video tab is back to a short, scannable device list.

QWidget *SettingsDialog::buildBackgroundTab()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(24, 22, 24, 22);
    layout->setSpacing(kRowGap);

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
    // Disable mouse-wheel scroll on this combo. Default Qt behaviour
    // changes the current item on wheel hover, but our save handler is
    // wired to activated() (click only), so a wheel scroll desyncs the
    // visible item from the saved setting - the user sees "Off" while
    // the engine is still running Blur/Image. Easiest fix: swallow
    // wheel events on the combo with a tiny eventFilter.
    m_bgModeCombo->setFocusPolicy(Qt::StrongFocus);
    m_bgModeCombo->installEventFilter(this);
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
        // Live preview reflects the new mode immediately too.
        syncBgPreview();
    });
    layout->addWidget(makeSettingRow(
        tr("Camera background"),
        tr("Off, Blur, or replace with an image. Applies live during calls."),
        m_bgModeCombo));

    // Live preview - shows the user's selected background applied to
    // their own camera in real time, so they can dial in Blur strength
    // or pick an image without having to start a call. Default to
    // hidden; revealed by syncBgPreview() once the user picks a
    // non-Off mode. The widget is a plain QLabel with a fixed 16:9
    // aspect so the preview pipeline runs at a known size (640x360 →
    // scaled to 320x180 for display).
    m_bgPreviewLabel = new QLabel;
    m_bgPreviewLabel->setFixedSize(320, 180);
    m_bgPreviewLabel->setAlignment(Qt::AlignCenter);
    // Chrome (recessed ground + secondary placeholder text) is theme-driven;
    // styled together with the image grid below via styleBackgroundChrome().
    m_bgPreviewLabel->setText(tr("Preview will appear when Blur or Image is selected"));
    m_bgPreviewLabel->setWordWrap(true);
    m_bgPreviewLabel->hide();
    {
        auto *previewRow = new QHBoxLayout;
        previewRow->addStretch(1);
        previewRow->addWidget(m_bgPreviewLabel);
        previewRow->addStretch(1);
        layout->addLayout(previewRow);
    }

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
        // Slider drag should feel live - the preview engine handles the
        // new strength directly without waiting for the publisher-side
        // debounce.
        if (m_bgPreviewEngine
            && m_bgPreviewEngine->mode() == BackgroundEngine::Mode::Blur) {
            m_bgPreviewEngine->setBlurStrength(v);
        }
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
    //
    // The whole image-picker block lives inside m_bgImageSection so it
    // can be hidden when the mode is Off or Blur (where the picker is
    // irrelevant) and only revealed on mode = Image. Reduces visual
    // clutter on the most common path.
    m_bgImageSection = new QWidget;
    auto *imageSectionLayout = new QVBoxLayout(m_bgImageSection);
    imageSectionLayout->setContentsMargins(0, 0, 0, 0);
    imageSectionLayout->setSpacing(8);
    imageSectionLayout->addWidget(makeSectionHeader(tr("Background image")));

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
    // Make the currently-selected thumbnail visible at a glance: an accent
    // tint + accent-coloured border for the selected item; hovered items
    // get a one-ladder-step lift so it's clear they're clickable. All
    // colours resolve from the active PainterTheme (see
    // styleBackgroundChrome), so the picker retints with the user's theme
    // instead of a fixed teal.
    styleBackgroundChrome(m_bgPreviewLabel, m_bgImageGrid);
    // Re-tint both chrome widgets when the app palette changes (live theme
    // switch). Installed app-wide so this dialog sees ApplicationPaletteChange
    // even while it isn't the active window; handled in eventFilter().
    qApp->installEventFilter(this);

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
        // Preview's image-mode engine reads the new path immediately so
        // selecting a different bundled background updates the live view.
        if (m_bgPreviewEngine
            && m_bgPreviewEngine->mode() == BackgroundEngine::Mode::Image) {
            m_bgPreviewEngine->setImagePath(path);
        }
    });

    imageSectionLayout->addWidget(m_bgImageGrid);

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
    browseBtn->setProperty("variant", "default");
    // Restore previously chosen custom images so they survive a TalQ
    // restart. Stored as a deduped string list under
    // Talk/Backgrounds/customImages; bundled qrc thumbs are NEVER in
    // that list. Missing files (user moved or deleted them) are
    // silently skipped on load.
    auto appendCustomThumb = [this](const QString &path) -> QListWidgetItem * {
        if (path.isEmpty() || path.startsWith(QStringLiteral(":/")))
            return nullptr;
        if (!QFileInfo::exists(path)) return nullptr;
        for (int i = 0; i < m_bgImageGrid->count(); ++i) {
            auto *it = m_bgImageGrid->item(i);
            if (it->data(Qt::UserRole).toString() == path)
                return it;
        }
        QImage img(path);
        if (img.isNull()) return nullptr;
        QPixmap pm = QPixmap::fromImage(img.scaled(120, 68,
            Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation));
        auto *item = new QListWidgetItem(QIcon(pm),
            QFileInfo(path).completeBaseName(), m_bgImageGrid);
        item->setData(Qt::UserRole, path);
        // Tag custom items so the context-menu remove only fires on
        // user-supplied paths; bundled qrc thumbs are immutable.
        item->setData(Qt::UserRole + 1, true);
        return item;
    };
    {
        m_settings.beginGroup("Talk/Backgrounds");
        const QStringList custom = m_settings.value("customImages")
                                       .toStringList();
        m_settings.endGroup();
        for (const QString &p : custom) {
            if (auto *it = appendCustomThumb(p);
                it && p == bgUrl) m_bgImageGrid->setCurrentItem(it);
        }
    }

    connect(browseBtn, &QPushButton::clicked, this,
            [this, appendCustomThumb]() {
        const QString path = QFileDialog::getOpenFileName(this,
            tr("Choose background image"),
            QStandardPaths::writableLocation(QStandardPaths::PicturesLocation),
            tr("Images (*.png *.jpg *.jpeg *.webp *.bmp *.gif);;All files (*)"));
        if (path.isEmpty()) return;
        m_settings.beginGroup("Talk/Backgrounds");
        m_settings.setValue("virtualBackgroundUrl", path);
        QStringList custom = m_settings.value("customImages").toStringList();
        if (!custom.contains(path)) {
            custom.append(path);
            m_settings.setValue("customImages", custom);
        }
        m_settings.endGroup();
        m_bgImagePathLabel->setText(QFileInfo(path).fileName());
        if (auto *it = appendCustomThumb(path)) {
            m_bgImageGrid->setCurrentItem(it);
        }
        emit backgroundSettingsChanged();
    });

    // Right-click on a custom thumb opens a small remove menu. Bundled
    // thumbs are immutable - the menu only opens for user-supplied
    // paths (marked by Qt::UserRole + 1 in appendCustomThumb).
    m_bgImageGrid->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_bgImageGrid, &QListWidget::customContextMenuRequested, this,
            [this](const QPoint &pos) {
        auto *item = m_bgImageGrid->itemAt(pos);
        if (!item) return;
        if (!item->data(Qt::UserRole + 1).toBool()) return;
        const QString path = item->data(Qt::UserRole).toString();
        QMenu menu(this);
        QAction *removeAct = menu.addAction(tr("Remove from grid"));
        QAction *chosen = menu.exec(m_bgImageGrid->mapToGlobal(pos));
        if (chosen != removeAct) return;
        m_settings.beginGroup("Talk/Backgrounds");
        QStringList custom = m_settings.value("customImages").toStringList();
        custom.removeAll(path);
        m_settings.setValue("customImages", custom);
        // If the removed thumb was the active background, fall back
        // to the first bundled image so the engine still has a valid
        // path to render.
        if (m_settings.value("virtualBackgroundUrl").toString() == path) {
            const QString fallback = QStringLiteral(":/bg/backgrounds/1_office.jpg");
            m_settings.setValue("virtualBackgroundUrl", fallback);
            for (int i = 0; i < m_bgImageGrid->count(); ++i) {
                if (m_bgImageGrid->item(i)->data(Qt::UserRole).toString() == fallback) {
                    m_bgImageGrid->setCurrentRow(i);
                    break;
                }
            }
        }
        m_settings.endGroup();
        delete m_bgImageGrid->takeItem(m_bgImageGrid->row(item));
        emit backgroundSettingsChanged();
    });
    browseRow->addWidget(m_bgImagePathLabel, 1);
    browseRow->addWidget(browseBtn, 0);
    imageSectionLayout->addLayout(browseRow);

    // Add the whole image-picker block to the outer tab layout. It's
    // shown/hidden by syncBgPreview based on the current BG mode.
    layout->addWidget(m_bgImageSection);
    m_bgImageSection->setVisible(bgEnabled
                                  && bgType == QLatin1String("image"));

    layout->addStretch();
    return page;
}

bool SettingsDialog::eventFilter(QObject *obj, QEvent *event)
{
    // Live theme switch: re-tint the Background tab's hand-styled chrome
    // (preview placeholder + image grid) from the new PainterTheme. The
    // filter is installed on qApp, so gate on obj==this to fire exactly
    // once per palette change. ApplicationPaletteChange (NOT PaletteChange)
    // — the latter recurses via setStyleSheet.
    if (obj == this && event->type() == QEvent::ApplicationPaletteChange)
        styleBackgroundChrome(m_bgPreviewLabel, m_bgImageGrid);
    // Block wheel-scroll on ANY combo box in the dialog. Wheel-while-
    // hovering used to change a combo's selection without firing
    // activated(), so handlers that save on activated only (most of
    // ours) would silently desync the visible from the saved value.
    if (event->type() == QEvent::Wheel && qobject_cast<QComboBox *>(obj))
        return true;
    return QDialog::eventFilter(obj, event);
}

void SettingsDialog::hideEvent(QHideEvent *event)
{
    QDialog::hideEvent(event);
    // Release the camera as soon as the dialog goes away so an outgoing
    // call from the main window can claim it without contention.
    if (m_bgPreviewSource) m_bgPreviewSource->stop();
    // Stop the mic test so it doesn't hold the capture device open.
    if (m_micTester) m_micTester->stop();
}

void SettingsDialog::showEvent(QShowEvent *event)
{
    QDialog::showEvent(event);
    // The mic-test capture pipeline and (with a virtual background on) the camera
    // preview each open a hardware device — wasapi2src/mfvideosrc init blocks the
    // calling thread for ~100-200 ms. showEvent() runs BEFORE the dialog's first
    // paint, and a singleShot(0) posted here still runs before Qt's deferred
    // paint — either way the paint is starved and the window shows blank-white
    // until the device opens. Instead flag it and let paintEvent() open the
    // devices once the content is actually on screen. The 200 ms fallback timer
    // covers the (unlikely) case where the parent paintEvent is fully occluded by
    // an opaque child and never fires — content is up by then regardless.
    m_deferredDeviceStart = true;
    QTimer::singleShot(200, this, [this]() { startDeferredDevices(); });
}

void SettingsDialog::paintEvent(QPaintEvent *event)
{
    QDialog::paintEvent(event);
    startDeferredDevices();          // fires once; no-op after the first paint
}

void SettingsDialog::startDeferredDevices()
{
    if (!m_deferredDeviceStart)
        return;
    m_deferredDeviceStart = false;
    // The dialog content is on screen now. Open the devices on the NEXT event-loop
    // turn (singleShot(0) so we never block inside a paint handler), so the
    // ~100-200 ms wasapi2src/mfvideosrc init happens with the window already fully
    // drawn instead of behind a blank-white surface.
    QTimer::singleShot(0, this, [this]() {
        if (!isVisible()) return;                  // closed again before we ran
        QElapsedTimer t; t.start();
        // Mirror the persisted state into the preview (re-shows Blur/Image if the
        // user left it active). Only opens the camera when a background is on.
        // NOTE: frame processing for the preview runs on the GStreamer
        // streaming thread (BgPreviewSource::onNewSample), NOT here — so this
        // must stay cheap. Split timing below so a residual UI-thread stall can
        // be attributed to the preview-open vs the mic-test-open.
        syncBgPreview();
        const qint64 bgMs = t.elapsed();
        // Start the live mic test on the currently-selected input device.
        if (m_micTester)
            m_micTester->start(m_deviceManager->selectedInputDeviceId());
        const qint64 totalMs = t.elapsed();
        // Always log (one line per Settings-open, negligible) so the
        // Off-path revert can be positively confirmed as fast in the field,
        // not just inferred from the absence of a slow-path warning.
        qInfo() << "SettingsDialog: deferred device open took" << totalMs
                << "ms (syncBgPreview" << bgMs << "ms, micTest"
                << (totalMs - bgMs) << "ms)";
    });
}

void SettingsDialog::setSharedBackgroundEngine(BackgroundEngine *engine)
{
    // 0.60.2 (FIX 2) — see the header comment. Injected by MainWindow right
    // after construction, i.e. before the first syncBgPreview() can run
    // (showEvent → deferred device start), so the lazy fallback in
    // syncBgPreview never fires in production. Ignore null so a broken
    // caller can't downgrade an already-injected engine.
    if (engine)
        m_bgPreviewEngine = engine;
}

void SettingsDialog::setCallActive(bool active)
{
    if (m_callActive == active)
        return;
    m_callActive = active;
    if (!isVisible())
        return;                              // hidden: hideEvent() already stopped the source
    if (active) {
        // A call just STARTED while open — release the preview's camera hold NOW so
        // the publisher can claim the (exclusive Windows-MF) device immediately.
        syncBgPreview();
        return;
    }
    // A call just ENDED. Do NOT restart the preview synchronously: hangup's teardown
    // releases the publisher's camera ASYNCHRONOUSLY (mfvideosrc→NULL on a GStreamer
    // worker / a detached unref thread, which can park for a beat), and this slot
    // runs INSIDE the same CallManager::stateChanged→teardown stack. Restarting the
    // preview inline would race the still-held device — the preview's mfvideosrc
    // loses and (BgPreviewSource has no bus watch) sticks on "Starting camera
    // preview…" forever. Defer past the async release; re-check state on fire (the
    // user may have closed the dialog or a new call may have started in the gap).
    QTimer::singleShot(1000, this, [this]() {
        if (!m_callActive && isVisible())
            syncBgPreview();
    });
}

void SettingsDialog::syncBgPreview()
{
    if (!m_bgPreviewLabel) return;

    m_settings.beginGroup("Talk/Backgrounds");
    const bool    enabled  = m_settings.value("virtualBackgroundEnabled", false).toBool();
    const QString type     = m_settings.value("virtualBackgroundType",   "blur").toString();
    const int     strength = m_settings.value("virtualBackgroundBlurStrength", 10).toInt();
    const QString imgUrl   = m_settings.value("virtualBackgroundUrl",    QString()).toString();
    m_settings.endGroup();

    // Show the image picker (header + grid + Choose… row) only when
    // mode is Image. Off and Blur don't need it - hiding cuts visual
    // clutter on the most common path.
    if (m_bgImageSection)
        m_bgImageSection->setVisible(enabled && type == QLatin1String("image"));

    // Off mode (or feature disabled) — tear down the preview and show the
    // placeholder text. CRITICAL for a snappy dialog: do NOT construct the
    // BackgroundEngine or the BgPreviewSource here, and do NOT open any
    // camera pipeline. Off is the common case, and spinning up the engine +
    // camera preview on the UI thread stalls the Settings-open for seconds
    // (measured 5.7 s on an Intel iGPU box). This early-return is 0.60.0's
    // behaviour; a proper fully-async raw-camera preview for Off is a
    // separate follow-up (see TODO), deliberately out of scope here.
    if (!enabled || type == QLatin1String("off")) {
        if (m_bgPreviewSource)
            m_bgPreviewSource->stop();
        m_bgPreviewLabel->setPixmap(QPixmap());
        m_bgPreviewLabel->setText(
            tr("Preview will appear when Blur or Image is selected"));
        m_bgPreviewLabel->hide();
        return;
    }

    // On-mode (Blur / Image). 0.60.2 (2026-07-13 field RCA): the engine is
    // CallManager's, injected via setSharedBackgroundEngine() — ONE engine,
    // ONE ONNX session process-wide. The dialog used to lazy-construct its
    // OWN BackgroundEngine here, so a single mode-combo click built TWO ORT
    // sessions ~1 ms apart (the immediate backgroundSettingsChanged emit at
    // the combo handler kicked CallManager's engine, then this very slot
    // constructed + kicked the preview's). Sharing is safe: processFrame()
    // serialises all callers on the engine's worker thread, the preview
    // never runs during a call (m_callActive gate below), and both sides
    // apply the same persisted Talk/Backgrounds values so the shared mode/
    // strength/path state cannot diverge. The fallback construction below
    // is defensive only (a dialog used standalone, e.g. in a harness) —
    // production always injects before the first syncBgPreview().
    if (!m_bgPreviewEngine) {
        qWarning() << "SettingsDialog: no shared BackgroundEngine injected —"
                      " constructing a private preview engine (fallback)";
        m_bgPreviewEngine = new BackgroundEngine(this);
    }
    if (type == QLatin1String("image")) {
        m_bgPreviewEngine->setImagePath(imgUrl);
        m_bgPreviewEngine->setMode(BackgroundEngine::Mode::Image);
    } else {
        m_bgPreviewEngine->setBlurStrength(strength);
        m_bgPreviewEngine->setMode(BackgroundEngine::Mode::Blur);
    }

    // A live call already holds the (exclusive, on Windows MediaFoundation)
    // camera via the publisher pipeline. Opening a SECOND camera consumer here
    // would STEAL the device from the call — the in-call camera then errors out
    // and can't re-acquire until the call is rebuilt (Ilko, 0.52.16: opened
    // Settings mid-call → camera dead for the rest of the call). So while a call
    // is active we do NOT open the camera for the preview. The chosen background
    // still applies live to the call via backgroundSettingsChanged(); only this
    // settings-side preview pauses.
    if (m_callActive) {
        if (m_bgPreviewSource)
            m_bgPreviewSource->stop();
        m_bgPreviewLabel->setText(
            tr("Live preview pauses during a call — your camera is in use.\n"
               "Your background still applies to the call."));
        m_bgPreviewLabel->show();
        return;
    }

    if (!m_bgPreviewSource) {
        m_bgPreviewSource = new BgPreviewSource(m_bgPreviewEngine, this);
        connect(m_bgPreviewSource, &BgPreviewSource::imageReady, this,
                [this](const QImage &img) {
            if (!m_bgPreviewLabel) return;
            m_bgPreviewLabel->setPixmap(QPixmap::fromImage(img).scaled(
                m_bgPreviewLabel->size(),
                Qt::KeepAspectRatio, Qt::SmoothTransformation));
        });
        connect(m_bgPreviewSource, &BgPreviewSource::unavailable, this,
                [this](const QString &reason) {
            if (!m_bgPreviewLabel) return;
            m_bgPreviewLabel->setPixmap(QPixmap());
            m_bgPreviewLabel->setText(
                tr("Live preview unavailable: %1").arg(reason));
            m_bgPreviewLabel->show();
        });
    }

    m_bgPreviewLabel->show();
    if (!m_bgPreviewSource->isRunning()) {
        m_bgPreviewLabel->setText(tr("Starting camera preview…"));
        m_bgPreviewSource->start();
    }
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

    // Outgoing-call (calling…) tone — separate from the incoming ringtone, since
    // some of the bundled tones suit the calling side better. "Ringback tone"
    // (default) is the classic brrr-brrr; the rest mirror the ringtone roster.
    m_outgoingRingtoneCombo = new QComboBox;
    m_outgoingRingtoneCombo->addItem(tr("Ringback tone"), QStringLiteral("ringback"));
    for (const auto &r : CallManager::ringtones())
        m_outgoingRingtoneCombo->addItem(r.second, r.first);
    connect(m_outgoingRingtoneCombo, QOverload<int>::of(&QComboBox::activated),
            this, [this](int idx) {
        const QString id = m_outgoingRingtoneCombo->itemData(idx).toString();
        m_settings.beginGroup("Calls");
        m_settings.setValue("outgoingRingtone", id);
        m_settings.endGroup();
        if (id != "none")
            CallManager::auditionRingtone(id);  // brief one-shot preview
    });
    layout->addWidget(makeSettingRow(
        tr("Outgoing call sound"),
        tr("Plays while you're calling someone."),
        m_outgoingRingtoneCombo));

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
        tr("Minimize when closing"),
        tr("Clicking X minimizes TalQ to the taskbar — keeping your unread "
           "count visible there — instead of quitting."),
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

    // Log access — make it easy to send the log when reporting a call/video
    // issue, without hunting through AppData. "Save a copy…" writes the live
    // talq_debug.log somewhere the user picks (defaults to the Desktop);
    // "Open log folder" reveals it in the file manager.
    {
        auto *saveBtn = new QPushButton(tr("Save a copy…"));
        saveBtn->setProperty("variant", "default");
        connect(saveBtn, &QPushButton::clicked, this, [this]() {
            const QString src = TalqLog::g_logPath;
            if (src.isEmpty() || !QFile::exists(src)) {
                QMessageBox::information(this, tr("Save log"),
                    tr("No log file is available yet."));
                return;
            }
            // Default name carries a timestamp so multiple saved logs don't
            // overwrite each other (e.g. talq_debug_20260601_111530.log).
            const QString stamp =
                QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
            const QString suggested =
                QStandardPaths::writableLocation(QStandardPaths::DesktopLocation)
                + "/talq_debug_" + stamp + ".log";
            const QString dst = QFileDialog::getSaveFileName(this,
                tr("Save log copy"), suggested, tr("Log files (*.log)"));
            if (dst.isEmpty()) return;   // cancelled
            QFile::remove(dst);          // overwrite if the user picked an existing name
            if (QFile::copy(src, dst)) {
                QMessageBox::information(this, tr("Save log"),
                    tr("Saved a copy of the log to:\n%1").arg(dst));
            } else {
                QMessageBox::warning(this, tr("Save log"),
                    tr("Couldn't save the log copy. The log may be in use; "
                       "try again."));
            }
        });

        auto *openBtn = new QPushButton(tr("Open log folder"));
        openBtn->setProperty("variant", "default");
        connect(openBtn, &QPushButton::clicked, this, [this]() {
            const QString src = TalqLog::g_logPath;
            const QString dir = src.isEmpty()
                ? QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                : QFileInfo(src).absolutePath();
            QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
        });

        auto *logRow = new QWidget;
        auto *logRowLayout = new QHBoxLayout(logRow);
        logRowLayout->setContentsMargins(0, 0, 0, 0);
        logRowLayout->setSpacing(8);
        logRowLayout->addStretch();
        logRowLayout->addWidget(saveBtn);
        logRowLayout->addWidget(openBtn);
        layout->addWidget(makeSettingRow(
            tr("Diagnostic log"),
            tr("Save a copy to send when reporting a call or video problem, "
               "or open the folder it lives in."),
            logRow));
    }

    // One-click full diagnostics: app/OS/GPU/displays/GStreamer/WGC/devices/
    // settings + the recent log, written to a single .txt the user can send.
    // This is what answers "what GPU / Windows build / plugins do they have?"
    // in one shot instead of a remote-debugging back-and-forth.
    {
        auto *diagBtn = new QPushButton(tr("Collect diagnostics…"));
        diagBtn->setProperty("variant", "default");
        connect(diagBtn, &QPushButton::clicked, this, [this]() {
            QString text =
                QStringLiteral("TalQ diagnostics\n================\nGenerated: ")
                + QDateTime::currentDateTime().toString(Qt::ISODate)
                + QStringLiteral("\n\n[App]\nVersion:  ")
                + QString::fromLatin1(TALQ_VERSION) + " \""
                + QString::fromLatin1(TALQ_VERSION_NAME) + "\"\n";
#ifdef TALQ_BUILD_TS
            text += QStringLiteral("Build:    ") + QString::fromLatin1(TALQ_BUILD_TS) + "\n";
#endif
#ifdef TALQ_PRERELEASE
            text += QStringLiteral("Channel:  PRE-RELEASE build\n");
#else
            text += QStringLiteral("Channel:  stable build\n");
#endif
#ifdef TALQ_BRAND_123NET
            text += QStringLiteral("Brand:    123NET\n");
#else
            text += QStringLiteral("Brand:    generic\n");
#endif
            text += "\n";
            text += talq::collectSystemDiagnostics();
            text += QStringLiteral("\n[Recent log: talq_debug.log]\n");
            text += talq::tailDebugLog(2 * 1024 * 1024);

            const QString stamp =
                QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
            const QString path =
                QStandardPaths::writableLocation(QStandardPaths::DesktopLocation)
                + "/TalQ-diagnostics-" + stamp + ".txt";
            QFile f(path);
            if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
                f.write(text.toUtf8());
                f.close();
                QDesktopServices::openUrl(QUrl::fromLocalFile(path));
                QMessageBox::information(this, tr("Diagnostics"),
                    tr("Saved diagnostics to:\n%1\n\nIt has been opened — send "
                       "this file to the developers when reporting a problem.").arg(path));
            } else {
                QMessageBox::warning(this, tr("Diagnostics"),
                    tr("Couldn't write the diagnostics file."));
            }
        });
        auto *diagRow = new QWidget;
        auto *diagRowLayout = new QHBoxLayout(diagRow);
        diagRowLayout->setContentsMargins(0, 0, 0, 0);
        diagRowLayout->addStretch();
        diagRowLayout->addWidget(diagBtn);
        layout->addWidget(makeSettingRow(
            tr("Debug info"),
            tr("Collect your system info (OS, graphics, devices, plugins) and "
               "recent log into one text file to share with the developers."),
            diagRow));
    }

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

    // 0.40.2 —opt-in auto-install with an idle gate. Defaults: ON, 5 min
    // (matches the auto-away threshold). Hard gates checked at install
    // time: never auto-install during a call, with unsent composer text,
    // or mid-upload — those checks live in MainWindow.
    m_updatesAutoInstall = new QCheckBox(w);
    m_updatesAutoInstall->setChecked(
        QSettings().value(QStringLiteral("updates/autoInstall"), true).toBool());
    connect(m_updatesAutoInstall, &QCheckBox::toggled, this, [](bool checked) {
        QSettings().setValue(QStringLiteral("updates/autoInstall"), checked);
    });
    lay->addWidget(makeSettingRow(
        tr("Install updates when I'm idle"),
        tr("After a new version is downloaded, TalQ restarts itself when "
           "you've been idle for a while. Cancelled if you touch the "
           "keyboard or mouse, start a call, or have unsent text."),
        m_updatesAutoInstall));

    {
        auto *waitRow = new QWidget(w);
        auto *waitLay = new QHBoxLayout(waitRow);
        waitLay->setContentsMargins(0, 0, 0, 0);
        waitLay->setSpacing(8);
        auto *waitLbl = new QLabel(tr("Wait time before auto-install:"), waitRow);
        waitLbl->setProperty("role", "secondary");
        m_updatesIdleWait = new QComboBox(waitRow);
        m_updatesIdleWait->addItem(tr("1 minute"),  1);
        m_updatesIdleWait->addItem(tr("5 minutes"), 5);
        m_updatesIdleWait->addItem(tr("15 minutes"), 15);
        const int storedMin = QSettings()
            .value(QStringLiteral("updates/autoInstallIdleMinutes"), 5).toInt();
        int sel = m_updatesIdleWait->findData(storedMin);
        if (sel < 0) sel = m_updatesIdleWait->findData(5);
        m_updatesIdleWait->setCurrentIndex(qMax(0, sel));
        connect(m_updatesIdleWait, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this](int) {
            QSettings().setValue(QStringLiteral("updates/autoInstallIdleMinutes"),
                                 m_updatesIdleWait->currentData().toInt());
        });
        waitLay->addWidget(waitLbl);
        waitLay->addWidget(m_updatesIdleWait);
        waitLay->addStretch();
        lay->addWidget(waitRow);
    }

    // 0.52.14 — manual "Update now": check + install on demand instead of waiting
    // for the periodic poll. Bypasses the idle wait, but the no-restart-during-a-
    // call gate still applies (enforced in MainWindow). The button doubles as its
    // own status line; MainWindow drives the result via setUpdateNowStatus().
    {
        auto *row = new QWidget(w);
        auto *rl = new QHBoxLayout(row);
        rl->setContentsMargins(0, 0, 0, 0);
        rl->setSpacing(8);
        m_updateNowBtn = new QPushButton(tr("Update now"), row);
        m_updateNowBtn->setCursor(Qt::PointingHandCursor);
        // Standalone secondary action -- exactly AppStyle.cpp's own
        // variant="default" instruction ("Cancel / Done / Refresh / Edit"),
        // but was shipping with no variant, so it fell through to the base
        // QPushButton rule and read as plain text at rest.
        m_updateNowBtn->setProperty("variant", "default");
        connect(m_updateNowBtn, &QPushButton::clicked, this, [this]() {
            m_updateNowBtn->setEnabled(false);
            m_updateNowBtn->setText(tr("Checking for updates…"));
            emit updateNowRequested();
        });
        rl->addWidget(m_updateNowBtn);
        rl->addStretch();
        lay->addWidget(row);
    }

    lay->addSpacing(kGroupGap - kRowGap);

    auto *checkBtn = new QPushButton(tr("Check for updates now"), w);
    checkBtn->setProperty("variant", "default");
    auto *checkStatus = new QLabel(w);
    checkStatus->setProperty("role", "secondary");
    checkStatus->setWordWrap(true);   // long "Checked just now…" text was clipped
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

    auto *logoutBtn = new QPushButton(tr("Log out"));
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
        // Blurb chosen PER codename so it always matches TALQ_VERSION_NAME.
        // A previous version special-cased only "Deep Thought" and lumped
        // every other codename into the April-Uprising blurb, which then
        // mis-described the Hitchhiker's-Guide names (Magrathea,
        // Slartibartfast) and the Bulgarian-holiday names (Botev = Heroes'
        // Day, Margaritka = Children's Day). Unknown/future codenames now
        // fall back to a neutral label rather than a wrong story.
        const QString creditText = codenameBlurb(verName);
        auto *credit = new QLabel(creditText);
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

    m_settings.beginGroup("Calls");
    const QString outRingId = m_settings.value("outgoingRingtone", "landline").toString();
    m_settings.endGroup();
    m_outgoingRingtoneCombo->blockSignals(true);
    int oidx = m_outgoingRingtoneCombo->findData(outRingId);
    if (oidx < 0) oidx = m_outgoingRingtoneCombo->findData(QStringLiteral("landline"));
    m_outgoingRingtoneCombo->setCurrentIndex(oidx);
    m_outgoingRingtoneCombo->blockSignals(false);
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

// ═══════════════════════════════════════════════════════
// Phone — inbound-call screen-pop
// ═══════════════════════════════════════════════════════

void SettingsDialog::setCtiService(CtiService *cti)
{
    m_cti = cti;
    if (!m_cti)
        return;

    connect(m_cti, &CtiService::pairingMessage, this,
            [this](const QString &message, bool isError) {
        if (!m_ctiStatus) return;
        m_ctiStatus->setText(message);
        // Errors are the only thing worth colouring here; danger-as-text is
        // already scored by the theme conformance suite.
        const PainterTheme th = activeTheme();
        m_ctiStatus->setStyleSheet(
            QStringLiteral("color: %1;").arg(isError ? th.danger.name()
                                                     : th.textSecondary.name()));
        if (m_ctiPairBtn) m_ctiPairBtn->setEnabled(true);
    });

    connect(m_cti, &CtiService::pairingSucceeded, this,
            [this](const QString &displayName, const QString &extension) {
        if (m_ctiEnabled) m_ctiEnabled->setChecked(true);
        if (!m_ctiStatus) return;
        m_ctiStatus->setText(extension.isEmpty()
            ? tr("Paired as %1.").arg(displayName)
            : tr("Paired as %1 on extension %2.").arg(displayName, extension));
    });
}

QWidget *SettingsDialog::buildPhoneTab()
{
    auto *w = new QWidget(this);
    auto *lay = new QVBoxLayout(w);
    lay->setContentsMargins(24, 22, 24, 22);
    lay->setSpacing(kRowGap);

    lay->addWidget(makeSectionHeader("Incoming calls"));

    m_ctiEnabled = new QCheckBox(w);
    m_ctiEnabled->setChecked(CtiService::enabledSetting());
    connect(m_ctiEnabled, &QCheckBox::toggled, this, [this](bool checked) {
        CtiService::setEnabledSetting(checked);
        if (m_cti) {
            if (checked) m_cti->start();
            else         m_cti->stop();
        }
    });
    lay->addWidget(makeSettingRow(
        tr("Show who is calling"),
        tr("When your desk phone rings, a card appears with the caller's "
           "details and a link to open them."),
        m_ctiEnabled));

    m_ctiServerUrl = new QLineEdit(w);
    m_ctiServerUrl->setPlaceholderText(QStringLiteral("wss://pbx.example.com:8790"));
    // The EFFECTIVE value, so a branded install shows its own address already
    // filled in rather than an empty box the user is expected to guess at.
    m_ctiServerUrl->setText(CtiService::serverUrl().toString());
    m_ctiServerUrl->setMinimumWidth(260);
    connect(m_ctiServerUrl, &QLineEdit::editingFinished, this, [this]() {
        CtiService::setServerUrl(QUrl(m_ctiServerUrl->text().trimmed()));
        if (m_cti) m_cti->start();
    });
    lay->addWidget(makeSettingRow(
        tr("Call service address"),
        tr("Supplied by whoever runs your phone system."),
        m_ctiServerUrl));

    m_ctiErpUrl = new QLineEdit(w);
    m_ctiErpUrl->setPlaceholderText(QStringLiteral("https://erp.example.com"));
    m_ctiErpUrl->setText(CtiService::erpBaseUrl().toString());
    m_ctiErpUrl->setMinimumWidth(260);
    connect(m_ctiErpUrl, &QLineEdit::editingFinished, this, [this]() {
        CtiService::setErpBaseUrl(QUrl(m_ctiErpUrl->text().trimmed()));
    });
    lay->addWidget(makeSettingRow(
        tr("Customer system address"),
        tr("Where caller details are looked up."),
        m_ctiErpUrl));

    m_ctiPairBtn = new QPushButton(tr("Pair this device"), w);
    connect(m_ctiPairBtn, &QPushButton::clicked, this, [this]() {
        if (!m_cti) return;
        // Save whatever is typed first: pairing needs the customer-system
        // address, and losing a call because a field was never committed is a
        // silly way to fail.
        if (m_ctiErpUrl)
            CtiService::setErpBaseUrl(QUrl(m_ctiErpUrl->text().trimmed()));
        if (m_ctiServerUrl)
            CtiService::setServerUrl(QUrl(m_ctiServerUrl->text().trimmed()));
        m_ctiPairBtn->setEnabled(false);
        if (m_ctiStatus) m_ctiStatus->setText(tr("Starting…"));
        m_cti->beginPairing();
    });
    lay->addWidget(makeSettingRow(
        tr("Authorise this computer"),
        tr("Opens your browser, where you are already signed in, and asks you "
           "to approve. Your password is never typed here."),
        m_ctiPairBtn));

    m_ctiStatus = new QLabel(w);
    m_ctiStatus->setWordWrap(true);
    // Derived from LIVE state, not just whether a token exists on disk: a
    // revoked device still has its token, and reporting "paired" while the
    // client is latched off is worse than saying nothing.
    if (CtiService::token().isEmpty())
        m_ctiStatus->setText(tr("This device is not paired yet."));
    else if (m_cti && m_cti->isConnected())
        m_ctiStatus->setText(tr("Paired and connected."));
    else if (CtiService::serverUrl().isEmpty())
        m_ctiStatus->setText(tr("Paired, but no call service address is set."));
    else
        m_ctiStatus->setText(tr("Paired, but not connected right now."));
    lay->addWidget(m_ctiStatus);

    lay->addStretch(1);
    return w;
}
