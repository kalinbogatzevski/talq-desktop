#pragma once

#include <QWidget>

class QLineEdit;
class QPushButton;
class QLabel;
class AuthManager;

class LoginWidget : public QWidget
{
    Q_OBJECT

public:
    explicit LoginWidget(AuthManager *auth, QWidget *parent = nullptr);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    void updateState();

    AuthManager *m_auth;
    QLineEdit *m_serverInput = nullptr;
    QPushButton *m_connectBtn = nullptr;
    QLabel *m_statusLabel = nullptr;
    QLabel *m_errorLabel = nullptr;
    QLabel *m_versionLabel = nullptr;
    bool m_isBranded = false;
    QString m_brandServer;
    QString m_brandName;
};
