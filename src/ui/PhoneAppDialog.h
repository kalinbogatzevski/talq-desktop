#pragma once

#include <QDialog>
#include <QUrl>

#include "core/PhoneAppCode.h"

class QLabel;
class QPushButton;

// "TalQ for your phone": a QR code of the one link that installs the Android app,
// one line of instructions, and Copy link / Open in browser. Opened from
// Settings > Account (SettingsDialog::buildAccountTab). The code itself is black on
// white whatever the theme (PhoneAppCode.h says why); everything else is AppStyle.
class PhoneAppDialog : public QDialog
{
    Q_OBJECT

public:
    // The link for this build (TalQUpdates::kPhoneAppUrl), or empty when the build
    // has none or it is not an http/https address. Empty = the entry is not shown.
    static QUrl link();

    explicit PhoneAppDialog(QWidget *parent = nullptr);

protected:
    bool event(QEvent *e) override;
    void showEvent(QShowEvent *e) override;

private:
    // Re-renders the code for the current device pixel ratio, so every module is a
    // whole number of physical pixels (a scaled-up pixmap blurs the module edges).
    void updateCode();

    QUrl m_link;
    talq::phoneapp::Symbol m_symbol;
    qreal m_renderedDpr = 0;
    QLabel *m_code = nullptr;
    QPushButton *m_copyBtn = nullptr;
};
