#include "core/PowerStateInhibitor.h"

#include <QDebug>
#include <QtGlobal>
#ifdef Q_OS_WIN
#  include <windows.h>
#endif

namespace talq {

PowerStateInhibitor::~PowerStateInhibitor()
{
    // Release on the way out: a request left registered would keep the machine
    // awake for the rest of the process's life.
    if (m_inCall || m_display) apply(false, false);
}

void PowerStateInhibitor::update(bool inCall, bool needsDisplay)
{
    if (inCall == m_inCall && needsDisplay == m_display) return;   // idempotent
    apply(inCall, needsDisplay);
    m_inCall  = inCall;
    m_display = needsDisplay;
}

void PowerStateInhibitor::apply(bool inCall, bool needsDisplay)
{
#ifdef Q_OS_WIN
    // ES_CONTINUOUS alone (the not-in-a-call case) CLEARS any previous request
    // rather than adding one, which is exactly how the release works.
    EXECUTION_STATE es = ES_CONTINUOUS;
    if (inCall) {
        es |= ES_SYSTEM_REQUIRED;
        if (needsDisplay) es |= ES_DISPLAY_REQUIRED;
    }
    if (SetThreadExecutionState(es) == 0)
        qWarning() << "PowerStateInhibitor: SetThreadExecutionState failed";
    else
        qInfo() << "PowerStateInhibitor: inCall=" << inCall
                << "keepDisplayOn=" << (inCall && needsDisplay);
#else
    Q_UNUSED(inCall); Q_UNUSED(needsDisplay);
#endif
}

} // namespace talq
