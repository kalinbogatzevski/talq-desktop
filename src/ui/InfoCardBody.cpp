#include "InfoCardBody.h"

#include "core/CtiEventLogic.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <QApplication>
#include <QClipboard>
#include <QCursor>
#include <QEvent>
#include <QFontMetrics>
#include <QToolTip>
#include "ui/TalqIconButton.h"
#include <QTextEdit>

InfoCardBody::InfoCardBody(QWidget *parent)
    : QWidget(parent)
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(4);

    m_title = new QLabel(this);
    m_title->setAttribute(Qt::WA_TranslucentBackground);
    m_title->setTextFormat(Qt::PlainText);
    m_title->setVisible(false);
    root->addWidget(m_title);

    m_subtitle = new QLabel(this);
    m_subtitle->setAttribute(Qt::WA_TranslucentBackground);
    m_subtitle->setTextFormat(Qt::PlainText);
    m_subtitle->setWordWrap(true);
    m_subtitle->setVisible(false);
    root->addWidget(m_subtitle);

    m_badgeRow = new QWidget(this);
    auto *badges = new QHBoxLayout(m_badgeRow);
    badges->setContentsMargins(0, 4, 0, 2);
    badges->setSpacing(6);
    badges->addStretch(1);
    m_badgeRow->setVisible(false);
    root->addWidget(m_badgeRow);

    auto *fieldsHost = new QWidget(this);
    m_fieldsLayout = new QVBoxLayout(fieldsHost);
    m_fieldsLayout->setContentsMargins(0, 4, 0, 0);
    m_fieldsLayout->setSpacing(2);
    root->addWidget(fieldsHost);

    m_more = new QLabel(this);
    m_more->setAttribute(Qt::WA_TranslucentBackground);
    m_more->setTextFormat(Qt::PlainText);
    m_more->setVisible(false);
    root->addWidget(m_more);

    m_actionRow = new QWidget(this);
    auto *actions = new QHBoxLayout(m_actionRow);
    actions->setContentsMargins(0, 10, 0, 0);
    actions->setSpacing(8);
    actions->addStretch(1);
    root->addWidget(m_actionRow);

    applyChrome();
}

QHBoxLayout *InfoCardBody::actionRow() const
{
    return qobject_cast<QHBoxLayout *>(m_actionRow->layout());
}

void InfoCardBody::setCard(const CardData &card)
{
    m_card = card;
    if (!card.title.isEmpty())
        setTitleText(card.title);
    if (!card.subtitle.isEmpty())
        setSubtitleText(card.subtitle);
    rebuildBody();
    rebuildActions();
}

void InfoCardBody::setValueWidthHint(int px)
{
    if (m_widthHint == px)
        return;
    m_widthHint = px;
    rebuildBody();
}

void InfoCardBody::setTitleText(const QString &text)
{
    m_title->setText(text);
    m_title->setVisible(!text.isEmpty());
}

void InfoCardBody::setSubtitleText(const QString &text)
{
    m_subtitle->setText(text);
    m_subtitle->setVisible(!text.isEmpty());
}

void InfoCardBody::setTheme(PainterTheme::Theme theme)
{
    m_theme = theme;
    applyChrome();
    rebuildBody();          // badge/field colours are baked into stylesheets
}

void InfoCardBody::applyChrome()
{
    const PainterTheme th(m_theme, 1.0);

    auto styleLabel = [](QLabel *l, const QColor &c, int pt, bool bold) {
        if (!l) return;
        QFont f = l->font();
        f.setBold(bold);
        f.setPointSize(pt);
        l->setFont(f);
        l->setStyleSheet(QStringLiteral("color: %1; background: transparent;")
                             .arg(c.name()));
    };

    styleLabel(m_title,    th.textPrimary,   th.fontSizeLarge, true);
    styleLabel(m_subtitle, th.textSecondary, th.fontSizeSmall, false);
}

void InfoCardBody::clearLayout(QVBoxLayout *layout)
{
    if (!layout)
        return;
    while (QLayoutItem *item = layout->takeAt(0)) {
        if (QWidget *w = item->widget())
            w->deleteLater();
        delete item;
    }
}

void InfoCardBody::rebuildBody()
{
    const PainterTheme th(m_theme, 1.0);

    // ── badges ──────────────────────────────────────────────────────────────
    if (auto *row = qobject_cast<QHBoxLayout *>(m_badgeRow->layout())) {
        while (row->count() > 1) {                    // keep the trailing stretch
            QLayoutItem *item = row->takeAt(0);
            if (QWidget *w = item->widget())
                w->deleteLater();
            delete item;
        }
        for (const Badge &b : m_card.badges) {
            if (b.text.isEmpty())
                continue;
            auto *chip = new QLabel(QStringLiteral(" %1 ").arg(b.text), m_badgeRow);
            chip->setTextFormat(Qt::PlainText);
            QFont f = chip->font();
            f.setBold(true);
            f.setPointSize(th.fontSizeTiny);
            chip->setFont(f);
            // A FILLED chip is a raised voice, and only warning/danger have
            // earned one. `normal` used textPrimary as the FILL, which made a
            // routine status ("Paired") the brightest object on the card --
            // louder than the person's own name. Normal and muted are
            // outlined instead: same closed vocabulary, same AA-scored ink,
            // a quarter of the shout.
            const QColor accent = inkForStyle(b.style);
            const talq::CardStyle cs = talq::cardStyleFromWire(b.style.toStdString());
            const bool loud = (cs == talq::CardStyle::Warning || cs == talq::CardStyle::Danger);
            chip->setStyleSheet(loud
                ? QStringLiteral("color: %1; background: %2; border-radius: 4px;")
                      .arg(th.inkOn(accent).name(), accent.name())
                : QStringLiteral("color: %1; background: transparent;"
                                 "border: 1px solid %2; border-radius: 4px;")
                      .arg(th.textSecondary.name(), th.divider.name()));
            row->insertWidget(row->count() - 1, chip);
        }
        m_badgeRow->setVisible(!m_card.badges.isEmpty());
    }

    // ── fields ──────────────────────────────────────────────────────────────
    clearLayout(m_fieldsLayout);

    const int total = m_card.fields.size();
    const int shown = talq::visibleFieldCount(total, m_card.maxFields);

    // The labels form a real COLUMN, sized once to the widest of them, so the
    // values all start on the same x and the eye can run straight down them.
    // The old row was label + stretch + right-aligned value, which pushed
    // every value to a different x AND, being a non-wrapping QLabel whose
    // minimum width is its full text, could not be shrunk by the layout: a
    // long address simply ran off the fixed-width card and was cut. Nobody
    // could read it and nobody could copy it either.
    QFont lf = font();
    lf.setPointSize(th.fontSizeSmall);
    const QFontMetrics lfm(lf);
    int labelW = 0;
    for (int i = 0; i < shown; ++i)
        labelW = qMax(labelW, lfm.horizontalAdvance(m_card.fields.at(i).label));
    labelW = qMin(labelW, 130);          // a verbose label must not eat the value's room

    // What is left for the value once the label column, the two gaps and the
    // copy button have taken theirs. Falls back to this widget's own width
    // when no owner supplied a hint.
    const int hintW = m_widthHint > 0 ? m_widthHint : width();
    const int valueWidth = hintW - labelW - 10 - 22 - 10;

    for (int i = 0; i < shown; ++i) {
        const Field &f = m_card.fields.at(i);
        auto *rowWidget = new QWidget(this);
        auto *row = new QHBoxLayout(rowWidget);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(10);

        auto *label = new QLabel(f.label, rowWidget);
        label->setTextFormat(Qt::PlainText);
        label->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        label->setFixedWidth(labelW);
        label->setStyleSheet(QStringLiteral("color: %1; background: transparent;")
                                 .arg(th.textSecondary.name()));
        label->setFont(lf);

        // A read-only QTextEdit, not a QLabel.
        //
        // QLabel cannot do this job. Its word wrap breaks at word boundaries
        // only, and an email address is ONE unbreakable token -- so a long
        // address was not wrapped, it was silently cut ("...@123net.co", with
        // the .za gone). The alternatives all corrupt the clipboard: eliding
        // hides characters, and inserting breaks or zero-width spaces to force
        // a wrap puts those characters into anything the user then selects and
        // pastes. A text document wraps ANYWHERE and still yields the exact
        // original characters to a selection, which is the only combination
        // that satisfies both "show me all of it" and "let me copy it".
        auto *value = new QTextEdit(rowWidget);
        value->setReadOnly(true);
        value->setFrameStyle(QFrame::NoFrame);
        value->setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        value->setLineWrapMode(QTextEdit::WidgetWidth);
        value->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        value->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        value->setTextInteractionFlags(Qt::TextSelectableByMouse
                                       | Qt::TextSelectableByKeyboard);
        value->document()->setDocumentMargin(0);
        value->viewport()->setAutoFillBackground(false);
        value->setStyleSheet(QStringLiteral("QTextEdit { color: %1; background: transparent;"
                                            " border: none; }")
                                 .arg(inkForStyle(f.style).name()));
        QFont vf = value->font();
        vf.setPointSize(th.fontSizeSmall);
        vf.setBold(f.style == QLatin1String("warning") || f.style == QLatin1String("danger"));
        // Font BEFORE text: the first layout pass has to use the metrics we
        // actually render with, or the height it reports is for a different
        // font than the one on screen.
        value->setFont(vf);
        value->document()->setDefaultFont(vf);
        value->setPlainText(f.value);
        // A QTextEdit has no useful sizeHint for this, so it would claim a
        // scroll-area's worth of height. Track the laid-out document instead:
        // the signal fires again whenever the wrap changes the line count, so
        // the row is exactly as tall as its text and no taller.
        // Height is MEASURED, not observed. Two earlier attempts drove it from
        // the live document -- the documentSizeChanged signal, then a
        // singleShot re-fit -- and both lost the race with the layout: the
        // first measurement happens while the widget is still 0 px wide, so
        // the document reports one line, and a QTextEdit that ends up shorter
        // than its content SCROLLS. That is what put the digits half out of
        // frame and cut the second line off the address.
        //
        // So measure against the width this row will actually get, which is
        // arithmetic the owner's fixed card width makes knowable up front, and
        // set the height once. A widget that is never too short never scrolls,
        // and the whole class of timing bug goes away.
        const int avail = qMax(40, valueWidth);
        QFontMetrics vfm(vf);
        const int textH = vfm.boundingRect(QRect(0, 0, avail, 0),
                                           Qt::TextWordWrap | Qt::TextWrapAnywhere,
                                           f.value).height();
        // +8, not +2. A QTextEdit is a scroll area: its viewport is inset from
        // the widget by the frame and the scroll-area chrome, so a widget sized
        // to exactly the text leaves a viewport SHORTER than the text -- and a
        // viewport shorter than its content scrolls. That is what cut the tops
        // off the digits and hid the second line of the address, with the
        // measured height correct all along (verified: fixedH 22 against docH
        // 20, and it still clipped). The slack absorbs the chrome.
        value->setFixedHeight(qMax(vfm.height(), textH) + 8);
        value->document()->setTextWidth(avail);
        // Park the cursor at the start: a cursor left at the end of the
        // document makes the view scroll to it the moment the widget is shown.
        value->moveCursor(QTextCursor::Start);
        // A pointing hand, not an I-beam: the click does not place a caret, it
        // takes the whole value (see eventFilter).
        value->viewport()->setCursor(Qt::PointingHandCursor);
        value->viewport()->installEventFilter(this);

        // One click puts the value on the clipboard. It copies f.value, the
        // string the server sent, NOT whatever the label is currently
        // rendering -- so wrapping, eliding or any future display transform
        // can never put a mangled address in somebody's paste buffer.
        auto *copyBtn = new TalqIconButton(QStringLiteral("copy"), rowWidget);
        copyBtn->setFixedSize(22, 22);
        copyBtn->setToolTip(tr("Copy %1").arg(f.label.toLower()));
        const QString exact = f.value;
        connect(copyBtn, &QAbstractButton::clicked, this, [exact, copyBtn]() {
            QApplication::clipboard()->setText(exact);
            QToolTip::showText(QCursor::pos(), tr("Copied"), copyBtn);
        });

        row->addWidget(label, 0, Qt::AlignTop);
        row->addWidget(value, 1);
        row->addWidget(copyBtn, 0, Qt::AlignTop);
        m_fieldsLayout->addWidget(rowWidget);
    }

    // Say so rather than silently truncating: someone who cannot see the count
    // has no way to know the card is not the whole story.
    const int hidden = talq::hiddenFieldCount(total, m_card.maxFields);
    m_more->setVisible(hidden > 0);
    if (hidden > 0) {
        m_more->setText(tr("+%n more…", nullptr, hidden));
        m_more->setStyleSheet(QStringLiteral("color: %1; background: transparent;")
                                  .arg(th.textMuted.name()));
        QFont mf = m_more->font();
        mf.setPointSize(th.fontSizeTiny);
        m_more->setFont(mf);
    }
}

void InfoCardBody::rebuildActions()
{
    auto *row = actionRow();
    if (!row)
        return;

    // Drop only the buttons THIS widget generated. An owner's own controls
    // (Dismiss, Call back, Message) live in the same row and must survive a
    // server card arriving.
    for (int i = row->count() - 1; i >= 0; --i) {
        QWidget *w = row->itemAt(i)->widget();
        if (w && w->property("infoCardGenerated").toBool()) {
            QLayoutItem *item = row->takeAt(i);
            w->deleteLater();
            delete item;
        }
    }

    for (const Action &a : m_card.actions) {
        if (a.label.isEmpty() || !a.url.isValid())
            continue;
        auto *btn = new QPushButton(a.label, m_actionRow);
        btn->setProperty("infoCardGenerated", true);
        // The app's bare QPushButton is transparent and borderless by design --
        // the filled/outlined looks are OPT-IN via the `variant` property (see
        // AppStyle). Without one, these read as plain text rather than
        // controls, which is exactly how they first shipped.
        btn->setProperty("variant", "primary");   // the call to action, filled
        btn->setCursor(Qt::PointingHandCursor);
        btn->setFocusPolicy(Qt::NoFocus);
        const QUrl url = a.url;
        connect(btn, &QPushButton::clicked, this, [this, url]() {
            // The owner validates the scheme; this widget never opens anything.
            emit openRequested(url);
        });
        row->addWidget(btn);
    }
}

QColor InfoCardBody::inkForStyle(const QString &style) const
{
    const PainterTheme th(m_theme, 1.0);
    switch (talq::cardStyleFromWire(style.toStdString())) {
    case talq::CardStyle::Muted:   return th.textSecondary;
    case talq::CardStyle::Warning: return th.amber;
    case talq::CardStyle::Danger:  return th.danger;
    case talq::CardStyle::Normal:  break;
    }
    return th.textPrimary;
}

bool InfoCardBody::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::MouseButtonPress) {
        // The filter is installed on the VIEWPORT, so the value widget is its
        // parent. Selecting everything and swallowing the press is the whole
        // behaviour: without swallowing it, Qt would immediately collapse the
        // selection to a caret under the pointer and begin a drag.
        if (auto *edit = qobject_cast<QTextEdit *>(watched->parent())) {
            edit->selectAll();
            edit->setFocus(Qt::MouseFocusReason);
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}
