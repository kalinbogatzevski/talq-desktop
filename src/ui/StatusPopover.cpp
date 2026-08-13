#include "ui/StatusPopover.h"
#include "ui/EmojiPickerWidget.h"
#include "painter/PainterTheme.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QToolButton>
#include <QLineEdit>
#include <QComboBox>
#include <QLabel>
#include <QFrame>
#include <QPainter>
#include <QDateTime>
#include <QScreen>
#include <QGuiApplication>
#include <QRegularExpression>
#include <QEvent>
#include <QPalette>

// ─── StatusDot ───

StatusDot::StatusDot(QWidget *parent) : QWidget(parent)
{
    setFixedSize(14, 14);
}

void StatusDot::setColor(const QColor &c) { m_color = c; update(); }
void StatusDot::setRingColor(const QColor &c) { m_ring = c; update(); }

void StatusDot::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const qreal d = 10.0;
    const qreal x = (width() - d) / 2.0;
    const qreal y = (height() - d) / 2.0;
    p.setPen(Qt::NoPen);
    p.setBrush(m_ring);
    p.drawEllipse(QRectF(x - 2, y - 2, d + 4, d + 4));
    p.setBrush(m_color);
    p.drawEllipse(QRectF(x, y, d, d));
}

// ─── StatusPopover ───

namespace {
const UserStatusManager::Status kTypes[] = {
    UserStatusManager::Status::Online,
    UserStatusManager::Status::Away,
    UserStatusManager::Status::Dnd,
    UserStatusManager::Status::Invisible,
};

QPixmap circlePixmap(const QColor &c)
{
    QPixmap pm(14, 14);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(c);
    p.drawEllipse(QRectF(2, 2, 10, 10));
    return pm;
}
}

StatusPopover::StatusPopover(UserStatusManager *mgr, QWidget *parent)
    : QDialog(parent)
    , m_mgr(mgr)
{
    setWindowFlags(Qt::FramelessWindowHint | Qt::Dialog);
    setAttribute(Qt::WA_TranslucentBackground, false);
    setObjectName("statusPopover");
    // Card frame only — palette-driven. Labels/buttons/inputs inherit the
    // app-wide AppStyle sheet (theme-driven, single source of truth).
    applyChrome();
    buildUi();

    connect(m_mgr, &UserStatusManager::statusChanged,
            this, &StatusPopover::refreshFromManager);
    connect(m_mgr, &UserStatusManager::predefinedLoaded,
            this, &StatusPopover::rebuildPresets);
    connect(m_mgr, &UserStatusManager::error, this, [this](const QString &m) {
        m_err->setText(m);
        m_err->show();
        // Re-grow the fixed-width card so a long wrapped error doesn't clip,
        // matching popupNear()'s adjustSize().
        adjustSize();
    });
}

void StatusPopover::applyChrome()
{
    // Self-contained, palette-driven sheet that reproduces the original
    // (pre-design-system) formatting EXACTLY: left-aligned padded status
    // rows, a centred accent primary, themed inputs/divider. The popover
    // must NOT depend on the global AppStyle button rules — those are
    // intentionally zero-padding so app-wide icon buttons aren't clipped,
    // which squished this popover. #stErr is left to AppStyle's themed
    // role="danger" (palette has no danger token).
    const QPalette p = palette();
    auto n = [&](QPalette::ColorRole r){ return p.color(r).name(); };
    setStyleSheet(QString(
        "QDialog#statusPopover { background:%1; border:1px solid %2;"
        "  border-radius:%9px; }"
        "QLabel { color:%3; background:transparent; }"
        "QLabel#stTitle { color:%4; font-size:11px; letter-spacing:1px; }"
        "QPushButton { color:%3; background:transparent; border:none;"
        "  text-align:left; padding:7px 10px; border-radius:7px; }"
        "QPushButton:hover { background:%6; }"
        "QPushButton#stPrimary { background:%7; color:%8; padding:7px 14px;"
        "  text-align:center; font-weight:600; }"
        "QPushButton#stPrimary:hover { background:%7; }"
        "QToolButton { color:%3; background:%5; border:1px solid %2;"
        "  border-radius:7px; padding:4px 8px; }"
        "QToolButton:hover { background:%6; }"
        "QLineEdit { background:%5; border:1px solid %2; border-radius:7px;"
        "  padding:6px 8px; color:%3; }"
        "QComboBox { background:%5; border:1px solid %2; border-radius:7px;"
        "  padding:4px 8px; color:%3; }"
        "QComboBox QAbstractItemView { background:%5; color:%3;"
        "  selection-background-color:%6; outline:none; }"
        "QFrame#stDiv { background:%2; max-height:1px; border:none; }")
        .arg(n(QPalette::Window), n(QPalette::Mid), n(QPalette::WindowText),
             n(QPalette::PlaceholderText), n(QPalette::Base),
             n(QPalette::AlternateBase), n(QPalette::Highlight),
             n(QPalette::HighlightedText))
        .arg(PainterTheme::radiusCard));
}

void StatusPopover::changeEvent(QEvent *e)
{
    QDialog::changeEvent(e);
    // ApplicationPaletteChange only — PaletteChange recurses via setStyleSheet.
    if (e->type() == QEvent::ApplicationPaletteChange
        || e->type() == QEvent::ThemeChange)
        applyChrome();
}

void StatusPopover::buildUi()
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(14, 14, 14, 12);
    root->setSpacing(6);

    auto *title = new QLabel(tr("SET STATUS"), this);
    title->setObjectName("stTitle");
    root->addWidget(title);

    for (auto t : kTypes) {
        auto *b = new QPushButton(UserStatusManager::label(t), this);
        b->setIcon(circlePixmap(UserStatusManager::colorFor(t)));
        b->setCursor(Qt::PointingHandCursor);
        connect(b, &QPushButton::clicked, this, [this, t]() {
            m_err->hide();
            m_mgr->setStatusType(t);
            accept();
        });
        m_typeRows.append(b);
        root->addWidget(b);
    }

    auto *div1 = new QFrame(this);
    div1->setObjectName("stDiv");
    div1->setFrameShape(QFrame::HLine);
    root->addWidget(div1);

    // Custom message row
    auto *msgRow = new QHBoxLayout;
    msgRow->setSpacing(6);
    m_emojiBtn = new QToolButton(this);
    m_emojiBtn->setText(QStringLiteral("🙂"));
    m_emojiBtn->setCursor(Qt::PointingHandCursor);
    m_emojiBtn->setToolTip(tr("Pick an emoji"));
    connect(m_emojiBtn, &QToolButton::clicked, this, &StatusPopover::openEmojiPicker);
    m_msg = new QLineEdit(this);
    m_msg->setPlaceholderText(tr("What's your status?"));
    m_msg->setMaxLength(80);
    msgRow->addWidget(m_emojiBtn);
    msgRow->addWidget(m_msg, 1);
    root->addLayout(msgRow);

    // Clear-after
    auto *clrRow = new QHBoxLayout;
    clrRow->setSpacing(6);
    clrRow->addWidget(new QLabel(tr("Clear after"), this));
    m_clear = new QComboBox(this);
    m_clear->addItems({ tr("Don't clear"), tr("30 minutes"), tr("1 hour"),
                        tr("4 hours"), tr("Today"), tr("This week") });
    clrRow->addWidget(m_clear, 1);
    root->addLayout(clrRow);

    // Action buttons
    auto *actRow = new QHBoxLayout;
    auto *clearBtn = new QPushButton(tr("Clear status"), this);
    clearBtn->setCursor(Qt::PointingHandCursor);
    connect(clearBtn, &QPushButton::clicked, this, [this]() {
        m_err->hide();
        m_mgr->clearStatusMessage();
        m_msg->clear();
        m_icon.clear();
        m_emojiBtn->setText(QStringLiteral("🙂"));
        refreshSetEnabled();
        accept();
    });
    m_setBtn = new QPushButton(tr("Set status"), this);
    m_setBtn->setObjectName("stPrimary");
    m_setBtn->setCursor(Qt::PointingHandCursor);
    connect(m_setBtn, &QPushButton::clicked, this, [this]() {
        m_err->hide();
        m_mgr->setCustom(m_icon, m_msg->text().trimmed(), selectedClearAt());
        accept();
    });
    // Disable while there's nothing to set (empty message AND no emoji) so a
    // click can't push an empty custom status.
    connect(m_msg, &QLineEdit::textChanged, this, &StatusPopover::refreshSetEnabled);
    refreshSetEnabled();
    actRow->addWidget(clearBtn);
    actRow->addStretch(1);
    actRow->addWidget(m_setBtn);
    root->addLayout(actRow);

    auto *div2 = new QFrame(this);
    div2->setObjectName("stDiv");
    div2->setFrameShape(QFrame::HLine);
    root->addWidget(div2);

    // Predefined presets
    m_presetHost = new QWidget(this);
    m_presetLay = new QVBoxLayout(m_presetHost);
    m_presetLay->setContentsMargins(0, 0, 0, 0);
    m_presetLay->setSpacing(2);
    root->addWidget(m_presetHost);
    rebuildPresets();

    m_err = new QLabel(this);
    m_err->setObjectName("stErr");
    m_err->setProperty("role", "danger");
    m_err->setWordWrap(true);
    m_err->hide();
    root->addWidget(m_err);

    setFixedWidth(290);
}

void StatusPopover::rebuildPresets()
{
    while (QLayoutItem *it = m_presetLay->takeAt(0)) {
        if (it->widget()) it->widget()->deleteLater();
        delete it;
    }
    for (const auto &p : m_mgr->predefinedStatuses()) {
        auto *b = new QPushButton(
            (p.icon.isEmpty() ? QString() : p.icon + QStringLiteral("  ")) + p.message,
            m_presetHost);
        b->setCursor(Qt::PointingHandCursor);
        const QString id = p.id;
        connect(b, &QPushButton::clicked, this, [this, id]() {
            m_err->hide();
            m_mgr->setPredefined(id, selectedClearAt());
            accept();
        });
        m_presetLay->addWidget(b);
    }
}

void StatusPopover::refreshFromManager()
{
    if (!m_msg->hasFocus()) {
        m_msg->setText(m_mgr->message());
        m_icon = m_mgr->icon();
        m_emojiBtn->setText(m_icon.isEmpty() ? QStringLiteral("🙂") : m_icon);
        refreshSetEnabled();
    }
}

void StatusPopover::refreshSetEnabled()
{
    if (m_setBtn)
        m_setBtn->setEnabled(!m_msg->text().trimmed().isEmpty()
                             || !m_icon.isEmpty());
}

qint64 StatusPopover::selectedClearAt() const
{
    const QDateTime now = QDateTime::currentDateTime();
    switch (m_clear->currentIndex()) {
    case 1: return now.addSecs(1800).toSecsSinceEpoch();
    case 2: return now.addSecs(3600).toSecsSinceEpoch();
    case 3: return now.addSecs(4 * 3600).toSecsSinceEpoch();
    case 4: {  // end of today
        QDateTime e(now.date(), QTime(23, 59, 59));
        return e.toSecsSinceEpoch();
    }
    case 5: {  // end of this week (Sunday)
        int daysToSun = 7 - now.date().dayOfWeek();   // Qt: Mon=1..Sun=7
        QDateTime e(now.date().addDays(daysToSun), QTime(23, 59, 59));
        return e.toSecsSinceEpoch();
    }
    default: return 0;  // Don't clear
    }
}

QString StatusPopover::codepointsToEmoji(const QString &cp)
{
    QString out;
    const QStringList parts = cp.split(QRegularExpression("[-_ ]"),
                                       Qt::SkipEmptyParts);
    for (const QString &t : parts) {
        bool ok = false;
        const uint v = t.toUInt(&ok, 16);
        if (ok) {
            const char32_t c = static_cast<char32_t>(v);
            out += QString::fromUcs4(&c, 1);
        }
    }
    return out;
}

void StatusPopover::openEmojiPicker()
{
    auto *picker = new EmojiPickerWidget(this);
    picker->setWindowFlags(Qt::Popup);
    picker->setAttribute(Qt::WA_DeleteOnClose);
    connect(picker, &EmojiPickerWidget::emojiSelected, this,
            [this, picker](const QString &codepoints) {
        m_icon = codepointsToEmoji(codepoints);
        m_emojiBtn->setText(m_icon.isEmpty() ? QStringLiteral("🙂") : m_icon);
        refreshSetEnabled();
        picker->close();
    });
    connect(picker, &EmojiPickerWidget::cancelled, picker, &QWidget::close);
    // The picker is a Qt::Popup, so showing it deactivates this dialog.
    // Suppress click-away dismissal until the picker is gone.
    m_emojiOpen = true;
    connect(picker, &QObject::destroyed, this, [this]() { m_emojiOpen = false; });
    QPoint g = m_emojiBtn->mapToGlobal(QPoint(0, m_emojiBtn->height() + 4));
    picker->move(g);
    picker->show();
}

bool StatusPopover::event(QEvent *e)
{
    if (e->type() == QEvent::WindowDeactivate && isVisible() && !m_emojiOpen)
        close();   // click-away dismissal, like a real dropdown
    return QDialog::event(e);
}

void StatusPopover::popupNear(const QRect &anchorGlobal)
{
    m_err->hide();
    refreshFromManager();
    adjustSize();

    QScreen *scr = QGuiApplication::screenAt(anchorGlobal.center());
    if (!scr) scr = QGuiApplication::primaryScreen();
    const QRect avail = scr ? scr->availableGeometry() : QRect(0, 0, 1920, 1080);

    int x = anchorGlobal.left();
    int y = anchorGlobal.bottom() + 6;          // prefer below the profile bar
    if (y + height() > avail.bottom())
        y = anchorGlobal.top() - height() - 6;  // else above
    x = qBound(avail.left() + 4, x, avail.right() - width() - 4);
    y = qBound(avail.top() + 4, y, avail.bottom() - height() - 4);

    move(x, y);
    show();
    raise();
    activateWindow();
}
