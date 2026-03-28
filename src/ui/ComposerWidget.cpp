#include "ComposerWidget.h"
#include "core/SignalingClient.h"
#include "models/MessageListModel.h"
#include <QFileDialog>
#include <QKeyEvent>
#include <QTimer>

// Custom text edit that sends on Enter, newline on Shift+Enter
class ComposeTextEdit : public QTextEdit
{
public:
    ComposeTextEdit(ComposerWidget *parent) : QTextEdit(parent), m_owner(parent) {
        setAcceptRichText(false);
        setTabChangesFocus(true);
        document()->setDocumentMargin(4);
    }

    QSize sizeHint() const override { return QSize(200, 36); }
    QSize minimumSizeHint() const override { return QSize(100, 36); }

protected:
    void keyPressEvent(QKeyEvent *e) override {
        if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) {
            if (e->modifiers() & Qt::ShiftModifier) {
                QTextEdit::keyPressEvent(e);
            } else {
                e->accept();
                QMetaObject::invokeMethod(m_owner, "sendAction", Qt::QueuedConnection);
            }
            return;
        }
        QTextEdit::keyPressEvent(e);
    }

private:
    ComposerWidget *m_owner;
};

ComposerWidget::ComposerWidget(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(8, 4, 8, 4);
    layout->setSpacing(6);

    m_attachBtn = new QPushButton(this);
    m_attachBtn->setText(QString::fromUtf8("\xF0\x9F\x93\x8E"));  // clip
    m_attachBtn->setFixedSize(32, 32);
    m_attachBtn->setFlat(true);
    m_attachBtn->setToolTip("Attach file");
    layout->addWidget(m_attachBtn);

    m_input = new ComposeTextEdit(this);
    m_input->setPlaceholderText("Message...");
    m_input->setMaximumHeight(120);
    m_input->setMinimumHeight(36);
    layout->addWidget(m_input, 1);

    m_sendBtn = new QPushButton(this);
    m_sendBtn->setText(QString::fromUtf8("\xE2\x9E\xA4"));  // arrow
    m_sendBtn->setFixedSize(36, 36);
    m_sendBtn->setToolTip("Send message (Enter)");
    layout->addWidget(m_sendBtn);

    connect(m_sendBtn, &QPushButton::clicked, this, &ComposerWidget::sendAction);
    connect(m_attachBtn, &QPushButton::clicked, this, [this]() {
        if (!m_model) return;
        QString file = QFileDialog::getOpenFileName(this, "Send file");
        if (!file.isEmpty()) {
            m_model->promptFileSend(QUrl::fromLocalFile(file).toString());
        }
    });

    setMaximumHeight(140);
}

void ComposerWidget::setTopicName(const QString &name)
{
    m_topicName = name;
    if (name.isEmpty())
        m_input->setPlaceholderText("Message...");
    else
        m_input->setPlaceholderText("Reply in " + name + "...");
}

void ComposerWidget::setSignaling(SignalingClient *sig)
{
    m_signaling = sig;
}

void ComposerWidget::setMessageModel(MessageListModel *model)
{
    m_model = model;
}

void ComposerWidget::setInputFont(const QFont &font)
{
    m_input->setFont(font);
}

void ComposerWidget::sendAction()
{
    QString text = m_input->toPlainText().trimmed();
    if (text.isEmpty()) return;
    if (m_signaling) m_signaling->sendStoppedTyping();
    emit sendMessage(text);
    m_input->clear();
}
