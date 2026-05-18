#pragma once

#include <QWidget>
#include <QDialog>
#include <QColor>
#include "core/UserStatusManager.h"

class QLineEdit;
class QComboBox;
class QToolButton;
class QLabel;
class QVBoxLayout;

/**
 * Small presence dot. Reused both on the profile avatar and inside the
 * status popover's type rows so own-status matches the contacts' dots.
 */
class StatusDot : public QWidget
{
    Q_OBJECT
public:
    explicit StatusDot(QWidget *parent = nullptr);
    void setColor(const QColor &c);
    void setRingColor(const QColor &c);   // background ring (separates from avatar)
    QSize sizeHint() const override { return {14, 14}; }

protected:
    void paintEvent(QPaintEvent *) override;

private:
    QColor m_color{0x8a, 0x86, 0x80};
    QColor m_ring{0x1b, 0x1a, 0x17};
};

/**
 * Frameless modeless dialog to set your own Nextcloud status: the four
 * status types, server-provided presets, a custom message (emoji + text),
 * and a "clear after" timer. Talks only to UserStatusManager.
 */
class StatusPopover : public QDialog
{
    Q_OBJECT
public:
    explicit StatusPopover(UserStatusManager *mgr, QWidget *parent = nullptr);

    // Position above/below an anchor (the profile bar) and show.
    void popupNear(const QRect &anchorGlobal);

protected:
    // Dismiss on click-away (lost activation), like a real dropdown —
    // except while the emoji picker child popup is the one taking focus.
    bool event(QEvent *e) override;
    void changeEvent(QEvent *e) override;   // re-theme on palette change

private:
    void applyChrome();   // popover card frame, palette-driven
    void buildUi();
    void rebuildPresets();
    void refreshFromManager();
    void openEmojiPicker();
    qint64 selectedClearAt() const;
    static QString codepointsToEmoji(const QString &cp);

    UserStatusManager *m_mgr;

    QList<QWidget *> m_typeRows;          // index aligns with kTypes
    QToolButton  *m_emojiBtn = nullptr;
    QLineEdit    *m_msg = nullptr;
    QComboBox    *m_clear = nullptr;
    QWidget      *m_presetHost = nullptr;
    QVBoxLayout  *m_presetLay = nullptr;
    QLabel       *m_err = nullptr;
    QString       m_icon;                 // chosen custom emoji
    bool          m_emojiOpen = false;    // emoji picker is the focus thief
};
