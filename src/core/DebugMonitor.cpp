#include "core/DebugMonitor.h"
#include "core/TalqLog.h"
#include <QDebug>
#include <QDateTime>

#ifdef Q_OS_WIN
#include <windows.h>
#include <psapi.h>
#endif

DebugMonitor::DebugMonitor(QObject *parent)
    : QObject(parent)
{
    m_uptime.start();

    // Tick every 2 seconds
    connect(&m_timer, &QTimer::timeout, this, &DebugMonitor::tick);
    m_timer.start(2000);

    // Initial reading
    m_memoryMB = readProcessMemoryMB();
    m_prevMemoryMB = m_memoryMB;
    addLog("Monitor started, baseline " + QString::number(m_memoryMB) + " MB");
}

void DebugMonitor::tick()
{
    m_prevMemoryMB = m_memoryMB;
    m_memoryMB = readProcessMemoryMB();

    emit updated();

    qint64 delta = m_memoryMB - m_prevMemoryMB;

    // Log every tick to console (compact)
    qDebug().noquote() << QString("[MEM] %1 MB (%2%3) msgs:%4 av:%5 pv:%6(%7KB) net:%8")
        .arg(m_memoryMB)
        .arg(delta >= 0 ? "+" : "").arg(delta)
        .arg(m_messageCount)
        .arg(m_avatarCacheCount)
        .arg(m_previewCacheCount)
        .arg(m_previewCacheBytes / 1024)
        .arg(m_pendingRequests);

    // Alert on large growth (>100MB in 2 seconds)
    if (delta > 100) {
        QString alert = QString("MEMORY ALERT: +%1 MB in 2s (now %2 MB)").arg(delta).arg(m_memoryMB);
        addLog(alert);
        qWarning().noquote() << alert;
        emit memoryAlert(m_memoryMB, delta);
    }

    // Alert on absolute threshold
    if (m_memoryMB > 1000 && m_prevMemoryMB <= 1000) {
        addLog("WARNING: Memory exceeded 1 GB");
    }

    // Every 30 ticks (60s at 2s cadence) emit a detailed cache breakdown.
    // This is the line you scan when chasing a memory growth bug.
    if (++m_summaryTickCounter >= 30) {
        m_summaryTickCounter = 0;
        summaryLog();
    }

    // Request a physical-disk flush (performed on the background flusher thread,
    // NOT here on the GUI thread — FlushFileBuffers can block for seconds on a
    // busy disk, e.g. Defender scanning cold DLLs during a first Settings-open,
    // which froze the UI). The flusher's ~2s cadence still bounds durability.
    TalqLog::requestSync();
}

void DebugMonitor::summaryLog()
{
    auto kb = [](qint64 b) { return b / 1024; };
    QString line = QString("[MEM-DETAIL] rss=%1MB | sidebar-av:%2(%3KB) chat-av:%4(%5KB) "
                           "preview:%6(%7KB) layout:%8(%9KB) emoji:%10(%11KB) "
                           "msgs:%12 convos:%13 netreq:%14")
        .arg(m_memoryMB)
        .arg(m_sidebarAvatarCount).arg(kb(m_sidebarAvatarBytes))
        .arg(m_chatAvatarCount).arg(kb(m_chatAvatarBytes))
        .arg(m_previewCacheCount).arg(kb(m_previewCacheBytes))
        .arg(m_layoutCacheCount).arg(kb(m_layoutCacheBytes))
        .arg(m_emojiCacheCount).arg(kb(m_emojiCacheBytes))
        .arg(m_messageCount).arg(m_conversationCount).arg(m_pendingRequests);
    addLog(line);
    qDebug().noquote() << line;
}

void DebugMonitor::snapshot(const QString &label)
{
    m_memoryMB = readProcessMemoryMB();
    qint64 delta = m_memoryMB - m_prevMemoryMB;

    QString msg = QString("%1: %2 MB (%3%4)")
        .arg(label.isEmpty() ? "snapshot" : label)
        .arg(m_memoryMB)
        .arg(delta >= 0 ? "+" : "").arg(delta);

    addLog(msg);
    qDebug().noquote() << "[MEM] " << msg;
    emit updated();
}

void DebugMonitor::addLog(const QString &msg)
{
    QString ts = QDateTime::currentDateTime().toString("HH:mm:ss");
    QString line = ts + " " + msg;

    // O(1) append + trim from front (QStringList, not O(n) QString::mid)
    m_logLines.append(line);
    while (m_logLines.size() > MAX_LOG_LINES)
        m_logLines.removeFirst();

    emit logChanged();
}

qint64 DebugMonitor::readProcessMemoryMB()
{
#ifdef Q_OS_WIN
    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        return static_cast<qint64>(pmc.WorkingSetSize / 1024 / 1024);
    }
#endif
    return 0;
}
