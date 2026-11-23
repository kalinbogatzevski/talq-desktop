#include "PhoneAppDialog.h"

#include "core/AppSettings.h"   // TalQUpdates::kPhoneAppUrl / kPhoneAppHint

#include <QClipboard>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QEvent>
#include <QGuiApplication>
#include <QLabel>
#include <QPixmap>
#include <QPointer>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

namespace {
// The code's side in logical pixels: big enough for a phone at arm's length, small
// enough that the dialog stays a small dialog. The module size is rounded DOWN to
// whole device pixels, so the code comes out at or a little under this.
constexpr int kCodeSidePx = 240;
}

QUrl PhoneAppDialog::link()
{
    return talq::phoneapp::phoneAppLink(TalQUpdates::kPhoneAppUrl);
}

PhoneAppDialog::PhoneAppDialog(QWidget *parent)
    : QDialog(parent)
    , m_link(link())
{
    setWindowTitle(tr("TalQ for your phone"));

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(24, 22, 24, 16);
    root->setSpacing(14);

    m_code = new QLabel(this);
    m_code->setAlignment(Qt::AlignCenter);
    m_code->setAccessibleName(tr("QR code of the link to the TalQ app for Android"));
    root->addWidget(m_code, 0, Qt::AlignHCenter);

    auto *hint = new QLabel(tr(TalQUpdates::kPhoneAppHint), this);
    hint->setProperty("role", "secondary");
    hint->setAlignment(Qt::AlignCenter);
    hint->setWordWrap(true);
    root->addWidget(hint);

    auto *buttons = new QDialogButtonBox(this);
    m_copyBtn = buttons->addButton(tr("Copy link"), QDialogButtonBox::ActionRole);
    m_copyBtn->setProperty("variant", "default");
    auto *openBtn = buttons->addButton(tr("Open in browser"), QDialogButtonBox::ActionRole);
    openBtn->setProperty("variant", "primary");
    buttons->addButton(QDialogButtonBox::Close);
    root->addWidget(buttons);

    // Both act on m_link, which link() has already limited to http/https.
    const bool haveLink = !m_link.isEmpty();
    m_copyBtn->setEnabled(haveLink);
    openBtn->setEnabled(haveLink);

    connect(m_copyBtn, &QPushButton::clicked, this, [this]() {
        if (m_link.isEmpty()) return;
        if (QClipboard *cb = QGuiApplication::clipboard())
            cb->setText(m_link.toString());
        m_copyBtn->setText(tr("Copied"));
        QPointer<QPushButton> btn = m_copyBtn;
        QTimer::singleShot(1500, this, [btn]() {
            if (btn) btn->setText(tr("Copy link"));
        });
    });
    connect(openBtn, &QPushButton::clicked, this, [this]() {
        if (!m_link.isEmpty())
            QDesktopServices::openUrl(m_link);
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    m_symbol = talq::phoneapp::encode(m_link.toString(QUrl::FullyEncoded));
    updateCode();
}

void PhoneAppDialog::updateCode()
{
    if (!m_code) return;
    if (m_symbol.isNull()) {
        // Never expected for a link of normal length; the buttons still work.
        m_code->setPixmap(QPixmap());
        m_code->setProperty("role", "secondary");
        m_code->setWordWrap(true);
        m_code->setTextInteractionFlags(Qt::TextSelectableByMouse);
        m_code->setText(m_link.isEmpty()
                            ? tr("No link is set for this build.")
                            : tr("Couldn't draw the code. Open this link on your phone:\n%1")
                                  .arg(m_link.toString()));
        return;
    }

    const qreal dpr = devicePixelRatioF() > 0 ? devicePixelRatioF() : 1.0;
    if (qFuzzyCompare(dpr, m_renderedDpr)) return;
    m_renderedDpr = dpr;

    // Work in DEVICE pixels: a whole number of them per module, then tell the
    // pixmap its ratio so the label shows it unscaled.
    const int maxDevicePx = qRound(kCodeSidePx * dpr);
    const int modulePx = talq::phoneapp::modulePixelsFor(m_symbol.size, maxDevicePx);
    QPixmap pm = QPixmap::fromImage(talq::phoneapp::render(m_symbol, modulePx));
    pm.setDevicePixelRatio(dpr);
    m_code->setPixmap(pm);
}

bool PhoneAppDialog::event(QEvent *e)
{
    // Moved to a screen with another scale: re-render rather than let Qt stretch it.
    if (e->type() == QEvent::DevicePixelRatioChange)
        updateCode();
    return QDialog::event(e);
}

void PhoneAppDialog::showEvent(QShowEvent *e)
{
    QDialog::showEvent(e);
    // Before the first show the ratio can be the primary screen's, not this one's.
    updateCode();
}
