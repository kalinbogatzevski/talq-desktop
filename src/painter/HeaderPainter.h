#pragma once

#include <QWidget>
#include <QImage>
#include <QSet>
#include "PainterTheme.h"

class ApiClient;
class QNetworkReply;
class SignalingClient;

/**
 * QWidget that renders the chat header bar via QPainter.
 */
class HeaderPainter : public QWidget
{
    Q_OBJECT

public:
    explicit HeaderPainter(QWidget *parent = nullptr);

    // ── Accessors ──
    QString conversationName() const { return m_conversationName; }
    void setConversationName(const QString &v);

    QString conversationUserId() const { return m_conversationUserId; }
    void setConversationUserId(const QString &v);
    void setAvatarImage(const QImage &img);

    int conversationType() const { return m_conversationType; }
    void setConversationType(int v);

    QString peerStatus() const { return m_peerStatus; }
    void setPeerStatus(const QString &state, const QString &message, const QString &icon);

    int activeThreadId() const { return m_activeThreadId; }
    void setActiveThreadId(int v);

    QString activeThreadTitle() const { return m_activeThreadTitle; }
    void setActiveThreadTitle(const QString &v);

    int activeThreadColor() const { return m_activeThreadColor; }
    void setActiveThreadColor(int v);

    bool isInTopicMode() const { return m_isInTopicMode; }
    void setIsInTopicMode(bool v);

    bool sidebarSqueezed() const { return m_sidebarSqueezed; }
    void setSidebarSqueezed(bool v);

    QString conversationToken() const { return m_conversationToken; }
    void setConversationToken(const QString &v);

    int messageCount() const { return m_messageCount; }
    void setMessageCount(int v);

    bool loading() const { return m_loading; }
    void setLoading(bool v);

    QString typingUser() const { return m_typingUser; }
    void setTypingUser(const QString &v);

    bool isTyping() const { return m_isTyping; }
    void setIsTyping(bool v);

    int callState() const { return m_callState; }
    void setCallState(int v);

    int callDuration() const { return m_callDuration; }
    void setCallDuration(int v);

    bool callsAvailable() const { return m_callsAvailable; }
    void setCallsAvailable(bool v);

    QString callsUnavailableReason() const { return m_callsUnavailableReason; }
    void setCallsUnavailableReason(const QString &v);

    bool darkMode() const { return m_darkMode; }
    void setDarkMode(bool v);
    void setTheme(PainterTheme::Theme t);

    void setApi(ApiClient *api);
    void setSignaling(SignalingClient *signaling);

signals:
    void expandSidebarClicked();
    void backClicked();
    void audioCallClicked();
    void videoCallClicked();
    void searchRequested();
    void remindersRequested();
    void infoRequested();

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    bool event(QEvent *event) override;

private:
    // ── Layout constants ──
    static constexpr int HeaderHeight = 54;
    static constexpr int AvatarSize = 30;
    static constexpr int ButtonSize = 34;
    // Visual chrome (rounded pill) painted inside the 34px hit target, so
    // every action button has the same compact, consistent footprint.
    static constexpr int ButtonChrome = 30;
    static constexpr int TopicDotSize = 10;

    // ── Hit-test rectangles (set during paint) ──
    QRectF m_expandBtnRect;
    QRectF m_backBtnRect;
    QRectF m_audioCallRect;
    QRectF m_videoCallRect;
    QRectF m_searchBtnRect;
    QRectF m_remindersBtnRect;
    QRectF m_infoBtnRect;
    int m_hoveredButton = -1;   // 0=expand, 1=back, 2=audio, 3=video, 4=search, 5=reminders, 6=info
    int m_pressedButton = -1;   // armed button between press and release (same ids)

    int buttonAtPos(const QPointF &pos) const;
    QRectF buttonRect(int id) const;          // hit-test rect for a button id
    QString tooltipFor(int id) const;         // user-facing label for a button id
    void showTooltipFor(int id);              // anchor a tooltip under the button
    void clearHoverState();                   // drop hover + pressed + tooltip

    // ── Avatar loading ──
    QImage fetchAvatar(const QString &userId);
    void requestAvatar(const QString &userId);
    QHash<QString, QImage> m_avatarCache;
    QSet<QString> m_avatarPending;

    // ── Painting helpers ──
    // Shared rounded-pill background for every action button (hover/pressed).
    void paintButtonChrome(QPainter *p, const QRectF &rect,
                           bool hovered, bool pressed);
    void paintBackButton(QPainter *p, const QRectF &rect,
                         bool hovered, bool pressed);
    void paintCallButton(QPainter *p, const QRectF &rect, const QColor &accentColor,
                         const QString &icon, bool hovered, bool pressed);
    void paintAvatar(QPainter *p, const QRectF &rect);

    // ── State ──
    QString m_conversationName;
    QString m_conversationUserId;
    int m_conversationType = 0;
    QString m_peerStatus;
    QString m_peerStatusMessage;
    QString m_peerStatusIcon;
    int m_activeThreadId = 0;
    QString m_activeThreadTitle;
    int m_activeThreadColor = 0;
    bool m_isInTopicMode = false;
    bool m_sidebarSqueezed = false;
    QString m_conversationToken;
    int m_messageCount = 0;
    bool m_loading = false;
    QString m_typingUser;
    bool m_isTyping = false;
    int m_callState = 0;
    int m_callDuration = 0;
    bool m_callsAvailable = true;
    QString m_callsUnavailableReason;
    bool m_darkMode = true;
    PainterTheme::Theme m_themeId = PainterTheme::Theme::Vivid;

    ApiClient *m_api = nullptr;
    SignalingClient *m_signaling = nullptr;
    PainterTheme m_theme;
};
