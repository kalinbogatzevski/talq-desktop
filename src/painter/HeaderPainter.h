#pragma once

#include <QQuickPaintedItem>
#include <QImage>
#include <QSet>
#include "PainterTheme.h"

class ApiClient;
class QNetworkReply;

/**
 * QQuickPaintedItem that renders the chat header bar via QPainter.
 * Replaces the QML Page header in ChatView.qml.
 *
 * Layout (left to right):
 *   [back-expand] [back-thread] [topic-dot] [avatar] [name + status] [call-buttons] [loading]
 */
class HeaderPainter : public QQuickPaintedItem
{
    Q_OBJECT

    // ── Data properties (bound from QML) ──
    Q_PROPERTY(QString conversationName READ conversationName WRITE setConversationName NOTIFY conversationNameChanged)
    Q_PROPERTY(QString conversationUserId READ conversationUserId WRITE setConversationUserId NOTIFY conversationUserIdChanged)
    Q_PROPERTY(int conversationType READ conversationType WRITE setConversationType NOTIFY conversationTypeChanged)
    Q_PROPERTY(QString peerStatus READ peerStatus WRITE setPeerStatus NOTIFY peerStatusChanged)
    Q_PROPERTY(int activeThreadId READ activeThreadId WRITE setActiveThreadId NOTIFY activeThreadIdChanged)
    Q_PROPERTY(QString activeThreadTitle READ activeThreadTitle WRITE setActiveThreadTitle NOTIFY activeThreadTitleChanged)
    Q_PROPERTY(int activeThreadColor READ activeThreadColor WRITE setActiveThreadColor NOTIFY activeThreadColorChanged)
    Q_PROPERTY(bool isInTopicMode READ isInTopicMode WRITE setIsInTopicMode NOTIFY isInTopicModeChanged)
    Q_PROPERTY(bool sidebarSqueezed READ sidebarSqueezed WRITE setSidebarSqueezed NOTIFY sidebarSqueezedChanged)
    Q_PROPERTY(QString conversationToken READ conversationToken WRITE setConversationToken NOTIFY conversationTokenChanged)
    Q_PROPERTY(int messageCount READ messageCount WRITE setMessageCount NOTIFY messageCountChanged)
    Q_PROPERTY(bool loading READ loading WRITE setLoading NOTIFY loadingChanged)

    // ── Typing indicator ──
    Q_PROPERTY(QString typingUser READ typingUser WRITE setTypingUser NOTIFY typingUserChanged)
    Q_PROPERTY(bool isTyping READ isTyping WRITE setIsTyping NOTIFY isTypingChanged)

    // ── Call state ──
    Q_PROPERTY(int callState READ callState WRITE setCallState NOTIFY callStateChanged)
    Q_PROPERTY(int callDuration READ callDuration WRITE setCallDuration NOTIFY callDurationChanged)
    Q_PROPERTY(bool callsAvailable READ callsAvailable WRITE setCallsAvailable NOTIFY callsAvailableChanged)
    Q_PROPERTY(QString callsUnavailableReason READ callsUnavailableReason WRITE setCallsUnavailableReason NOTIFY callsUnavailableReasonChanged)

    // ── Theme ──
    Q_PROPERTY(bool darkMode READ darkMode WRITE setDarkMode NOTIFY darkModeChanged)
    Q_PROPERTY(QObject* api READ apiObject WRITE setApiObject NOTIFY apiChanged)

public:
    explicit HeaderPainter(QQuickItem *parent = nullptr);

    void paint(QPainter *painter) override;

    // ── Accessors ──
    QString conversationName() const { return m_conversationName; }
    void setConversationName(const QString &v);

    QString conversationUserId() const { return m_conversationUserId; }
    void setConversationUserId(const QString &v);

    int conversationType() const { return m_conversationType; }
    void setConversationType(int v);

    QString peerStatus() const { return m_peerStatus; }
    void setPeerStatus(const QString &v);

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

    QObject *apiObject() const;
    void setApiObject(QObject *obj);

signals:
    void conversationNameChanged();
    void conversationUserIdChanged();
    void conversationTypeChanged();
    void peerStatusChanged();
    void activeThreadIdChanged();
    void activeThreadTitleChanged();
    void activeThreadColorChanged();
    void isInTopicModeChanged();
    void sidebarSqueezedChanged();
    void conversationTokenChanged();
    void messageCountChanged();
    void loadingChanged();
    void typingUserChanged();
    void isTypingChanged();
    void callStateChanged();
    void callDurationChanged();
    void callsAvailableChanged();
    void callsUnavailableReasonChanged();
    void darkModeChanged();
    void apiChanged();

    // ── Action signals (connected in ChatView.qml) ──
    void expandSidebarClicked();
    void backClicked();            // close thread
    void audioCallClicked();
    void videoCallClicked();

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void hoverMoveEvent(QHoverEvent *event) override;
    void hoverLeaveEvent(QHoverEvent *event) override;

private:
    // ── Layout constants ──
    static constexpr int HeaderHeight = 54;
    static constexpr int AvatarSize = 30;
    static constexpr int ButtonSize = 34;
    static constexpr int TopicDotSize = 10;

    // ── Hit-test rectangles (set during paint) ──
    QRectF m_expandBtnRect;
    QRectF m_backBtnRect;
    QRectF m_audioCallRect;
    QRectF m_videoCallRect;
    int m_hoveredButton = -1;   // 0=expand, 1=back, 2=audio, 3=video

    int buttonAtPos(const QPointF &pos) const;

    // ── Topic color palette (mirrors Theme.qml) ──
    static QColor topicColor(int index);

    // ── Avatar loading ──
    QImage fetchAvatar(const QString &userId);
    void requestAvatar(const QString &userId);
    QHash<QString, QImage> m_avatarCache;
    QSet<QString> m_avatarPending;

    // ── Painting helpers ──
    void paintBackButton(QPainter *p, const QRectF &rect, bool hovered);
    void paintCallButton(QPainter *p, const QRectF &rect, const QColor &bg,
                         const QString &icon, bool hovered);
    void paintAvatar(QPainter *p, const QRectF &rect);

    // ── State ──
    QString m_conversationName;
    QString m_conversationUserId;
    int m_conversationType = 0;
    QString m_peerStatus;
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

    ApiClient *m_api = nullptr;
    PainterTheme m_theme;
};
