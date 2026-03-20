#include "core/DebugMonitor.h"
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

    // Keep log bounded
    if (m_log.count('\n') >= MAX_LOG_LINES) {
        int firstNewline = m_log.indexOf('\n');
        if (firstNewline >= 0)
            m_log = m_log.mid(firstNewline + 1);
    }

    m_log += line + "\n";
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
