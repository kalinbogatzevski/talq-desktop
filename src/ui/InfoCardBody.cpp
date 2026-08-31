#include "InfoCardBody.h"

#include "core/CtiEventLogic.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

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
            const QColor fill = inkForStyle(b.style);
            chip->setStyleSheet(
                QStringLiteral("color: %1; background: %2; border-radius: 4px;")
                    .arg(th.inkOn(fill).name(), fill.name()));
            row->insertWidget(row->count() - 1, chip);
        }
        m_badgeRow->setVisible(!m_card.badges.isEmpty());
    }

    // ── fields ──────────────────────────────────────────────────────────────
    clearLayout(m_fieldsLayout);

    const int total = m_card.fields.size();
    const int shown = talq::visibleFieldCount(total, m_card.maxFields);
    for (int i = 0; i < shown; ++i) {
        const Field &f = m_card.fields.at(i);
        auto *rowWidget = new QWidget(this);
        auto *row = new QHBoxLayout(rowWidget);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(10);

        auto *label = new QLabel(f.label, rowWidget);
        label->setTextFormat(Qt::PlainText);
        label->setStyleSheet(QStringLiteral("color: %1; background: transparent;")
                                 .arg(th.textSecondary.name()));
        QFont lf = label->font();
        lf.setPointSize(th.fontSizeSmall);
        label->setFont(lf);

        auto *value = new QLabel(f.value, rowWidget);
        value->setTextFormat(Qt::PlainText);
        value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        value->setStyleSheet(QStringLiteral("color: %1; background: transparent;")
                                 .arg(inkForStyle(f.style).name()));
        QFont vf = value->font();
        vf.setPointSize(th.fontSizeSmall);
        vf.setBold(f.style == QLatin1String("warning") || f.style == QLatin1String("danger"));
        value->setFont(vf);

        row->addWidget(label);
        row->addStretch(1);
        row->addWidget(value);
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
