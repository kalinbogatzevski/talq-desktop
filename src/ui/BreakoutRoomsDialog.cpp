#include "BreakoutRoomsDialog.h"

#include "core/ApiClient.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QStyle>
#include <QVBoxLayout>

BreakoutRoomsDialog::BreakoutRoomsDialog(ApiClient *api, const QString &token, QWidget *parent)
    : QDialog(parent)
    , m_api(api)
    , m_token(token)
{
    setWindowTitle(tr("Breakout rooms"));
    setMinimumWidth(440);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(20, 18, 20, 14);
    root->setSpacing(12);

    auto *form = new QFormLayout();
    form->setSpacing(8);

    m_mode = new QComboBox(this);
    // Talk's BreakoutRoom modes: 1 automatic, 2 manual, 3 free.
    // Manual needs a per-attendee map, which this dialog does not collect — so
    // it is deliberately not offered rather than sent with an empty map, which
    // would create rooms with nobody in them.
    m_mode->addItem(tr("Split people up automatically"), 1);
    m_mode->addItem(tr("Let people choose their own room"), 3);
    form->addRow(tr("How"), m_mode);

    m_amount = new QSpinBox(this);
    m_amount->setRange(1, 20);
    m_amount->setValue(2);
    form->addRow(tr("How many rooms"), m_amount);
    root->addLayout(form);

    m_createBtn = new QPushButton(tr("Create rooms"), this);
    m_startBtn  = new QPushButton(tr("Open them"), this);
    m_stopBtn   = new QPushButton(tr("Bring everyone back"), this);
    m_removeBtn = new QPushButton(tr("Delete rooms"), this);
    m_removeBtn->setProperty("variant", "danger");
    root->addWidget(m_createBtn);
    root->addWidget(m_startBtn);
    root->addWidget(m_stopBtn);
    root->addWidget(m_removeBtn);

    auto *bcRow = new QFormLayout();
    m_broadcast = new QLineEdit(this);
    m_broadcast->setPlaceholderText(tr("Send a message to every room…"));
    bcRow->addRow(tr("Announce"), m_broadcast);
    root->addLayout(bcRow);

    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    m_status->setProperty("role", "secondary");
    root->addWidget(m_status);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    root->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    connect(m_createBtn, &QPushButton::clicked, this, &BreakoutRoomsDialog::configure);
    connect(m_startBtn,  &QPushButton::clicked, this, &BreakoutRoomsDialog::start);
    connect(m_stopBtn,   &QPushButton::clicked, this, &BreakoutRoomsDialog::stop);
    connect(m_removeBtn, &QPushButton::clicked, this, &BreakoutRoomsDialog::remove);
    connect(m_broadcast, &QLineEdit::returnPressed, this, &BreakoutRoomsDialog::broadcast);

    say(tr("Create the rooms first, then open them when you are ready."));
}

void BreakoutRoomsDialog::say(const QString &text, bool bad)
{
    m_status->setProperty("role", bad ? "danger" : "secondary");
    m_status->setText(text);
    m_status->style()->unpolish(m_status);
    m_status->style()->polish(m_status);
}

void BreakoutRoomsDialog::configure()
{
    const int mode   = m_mode->currentData().toInt();
    const int amount = m_amount->value();
    m_api->configureBreakoutRooms(m_token, mode, amount, QStringLiteral("[]"),
        [this, amount](bool ok, const QJsonObject &data, int) {
            if (!ok) {
                const QString err = data.value(QStringLiteral("error")).toString();
                say(err.isEmpty() ? tr("The rooms could not be created.")
                                  : tr("The rooms could not be created: %1").arg(err), true);
                return;
            }
            say(tr("%n room(s) created. Open them when you are ready.", nullptr, amount));
        });
}

void BreakoutRoomsDialog::start()
{
    m_api->startBreakoutRooms(m_token, [this](bool ok, const QJsonObject &, int) {
        // Opening the rooms is what moves people: the server tells each client
        // which conversation it is now in. Nothing this dialog does moves them.
        say(ok ? tr("Rooms are open — everyone has been moved.")
               : tr("The rooms could not be opened."), !ok);
    });
}

void BreakoutRoomsDialog::stop()
{
    m_api->stopBreakoutRooms(m_token, [this](bool ok, const QJsonObject &, int) {
        say(ok ? tr("Everyone has been brought back.")
               : tr("The rooms could not be closed."), !ok);
    });
}

void BreakoutRoomsDialog::remove()
{
    m_api->removeBreakoutRooms(m_token, [this](bool ok, const QJsonObject &, int) {
        say(ok ? tr("Rooms deleted.") : tr("The rooms could not be deleted."), !ok);
    });
}

void BreakoutRoomsDialog::broadcast()
{
    const QString msg = m_broadcast->text().trimmed();
    if (msg.isEmpty()) return;
    m_api->broadcastToBreakoutRooms(m_token, msg, [this](bool ok, const QJsonObject &, int) {
        if (ok) m_broadcast->clear();
        say(ok ? tr("Sent to every room.") : tr("The message was not sent."), !ok);
    });
}
