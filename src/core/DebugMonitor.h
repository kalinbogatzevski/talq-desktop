#pragma once

#include <QObject>
#include <QTimer>
#include <QElapsedTimer>
#include <QStringList>

class DebugMonitor : public QObject
{
    Q_OBJECT
    Q_PROPERTY(qint64 memoryMB READ memoryMB NOTIFY updated)
    Q_PROPERTY(int messageCount READ messageCount WRITE setMessageCount NOTIFY updated)
    Q_PROPERTY(int avatarCacheCount READ avatarCacheCount WRITE setAvatarCacheCount NOTIFY updated)
    Q_PROPERTY(int previewCacheCount READ previewCacheCount WRITE setPreviewCacheCount NOTIFY updated)
    Q_PROPERTY(qint64 previewCacheBytes READ previewCacheBytes WRITE setPreviewCacheBytes NOTIFY updated)
    Q_PROPERTY(int pendingRequests READ pendingRequests WRITE setPendingRequests NOTIFY updated)
    Q_PROPERTY(int conversationCount READ conversationCount WRITE setConversationCount NOTIFY updated)
    Q_PROPERTY(QString log READ log NOTIFY logChanged)
    Q_PROPERTY(bool visible READ visible WRITE setVisible NOTIFY visibleChanged)

public:
    explicit DebugMonitor(QObject *parent = nullptr);

    qint64 memoryMB() const { return m_memoryMB; }
    int messageCount() const { return m_messageCount; }
    int avatarCacheCount() const { return m_avatarCacheCount; }
    int previewCacheCount() const { return m_previewCacheCount; }
    qint64 previewCacheBytes() const { return m_previewCacheBytes; }
    int pendingRequests() const { return m_pendingRequests; }
    int conversationCount() const { return m_conversationCount; }
    QString log() const { return m_logLines.join('\n'); }
    bool visible() const { return m_visible; }

    void setMessageCount(int v) { m_messageCount = v; }
    void setAvatarCacheCount(int v) { m_avatarCacheCount = v; }
    void setPreviewCacheCount(int v) { m_previewCacheCount = v; }
    void setPreviewCacheBytes(qint64 v) { m_previewCacheBytes = v; }
    void setPendingRequests(int v) { m_pendingRequests = v; }
    void setConversationCount(int v) { m_conversationCount = v; }
    void setVisible(bool v) { if (m_visible != v) { m_visible = v; emit visibleChanged(); } }

    // Call from anywhere to log a debug event
    Q_INVOKABLE void addLog(const QString &msg);

    // Snapshot: force an immediate memory reading + emit
    Q_INVOKABLE void snapshot(const QString &label = {});

signals:
    void updated();
    void logChanged();
    void visibleChanged();
    void memoryAlert(qint64 currentMB, qint64 deltaMB);  // fired when growth > 100MB in one tick

private:
    void tick();
    qint64 readProcessMemoryMB();

    QTimer m_timer;
    QElapsedTimer m_uptime;

    qint64 m_memoryMB = 0;
    qint64 m_prevMemoryMB = 0;
    int m_messageCount = 0;
    int m_avatarCacheCount = 0;
    int m_previewCacheCount = 0;
    qint64 m_previewCacheBytes = 0;
    int m_pendingRequests = 0;
    int m_conversationCount = 0;
    bool m_visible = false;

    QStringList m_logLines;
    static const int MAX_LOG_LINES = 200;
};
