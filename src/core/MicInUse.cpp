#include "core/MicInUse.h"
#include "core/MicInUsePolicy.h"

#if defined(_WIN32)

#include <QDebug>
#include <QFileInfo>
#include <windows.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <objbase.h>

namespace talq {

namespace {

QString processName(DWORD pid)
{
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return QStringLiteral("pid %1").arg(pid);
    wchar_t path[MAX_PATH];
    DWORD len = MAX_PATH;
    QString name = QStringLiteral("pid %1").arg(pid);
    if (QueryFullProcessImageNameW(h, 0, path, &len))
        name = QFileInfo(QString::fromWCharArray(path, int(len))).fileName();
    CloseHandle(h);
    return name;
}

} // namespace

bool anotherAppUsingMic(QString *who)
{
    // Same COM discipline as WasapiDucking: Qt already initialised COM on the GUI
    // thread; balance CoUninitialize only when this call's init SUCCEEDED.
    const HRESULT coInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool weInitedCom = SUCCEEDED(coInit);

    std::vector<CaptureSession> sessions;
    IMMDeviceEnumerator *enumerator = nullptr;
    IMMDeviceCollection *devices = nullptr;
    UINT deviceCount = 0;

    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
                                __uuidof(IMMDeviceEnumerator),
                                reinterpret_cast<void **>(&enumerator))) || !enumerator)
        goto done;
    // Every active capture endpoint, not only the default: a headset mic in a
    // Zoom call is often not the Windows default device.
    if (FAILED(enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &devices)) || !devices)
        goto done;
    devices->GetCount(&deviceCount);

    for (UINT i = 0; i < deviceCount; ++i) {
        IMMDevice *device = nullptr;
        IAudioSessionManager2 *mgr = nullptr;
        IAudioSessionEnumerator *list = nullptr;
        int count = 0;
        if (FAILED(devices->Item(i, &device)) || !device) continue;
        if (SUCCEEDED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_INPROC_SERVER,
                                       nullptr, reinterpret_cast<void **>(&mgr))) && mgr
            && SUCCEEDED(mgr->GetSessionEnumerator(&list)) && list) {
            list->GetCount(&count);
            for (int j = 0; j < count; ++j) {
                IAudioSessionControl *ctl = nullptr;
                if (FAILED(list->GetSession(j, &ctl)) || !ctl) continue;
                CaptureSession s;
                AudioSessionState state = AudioSessionStateInactive;
                if (SUCCEEDED(ctl->GetState(&state)))
                    s.active = (state == AudioSessionStateActive);
                IAudioSessionControl2 *ctl2 = nullptr;
                if (SUCCEEDED(ctl->QueryInterface(__uuidof(IAudioSessionControl2),
                                                  reinterpret_cast<void **>(&ctl2))) && ctl2) {
                    DWORD pid = 0;
                    ctl2->GetProcessId(&pid);   // S_FALSE for a multi-process session; pid still set
                    s.pid = pid;
                    ctl2->Release();
                }
                ctl->Release();
                sessions.push_back(s);
            }
        }
        if (list) list->Release();
        if (mgr) mgr->Release();
        device->Release();
    }

done:
    if (devices) devices->Release();
    if (enumerator) enumerator->Release();
    if (weInitedCom) CoUninitialize();

    const unsigned long self = GetCurrentProcessId();
    if (!otherAppCapturing(sessions, self)) return false;
    if (who) {
        for (const CaptureSession &s : sessions)
            if (s.active && s.pid != 0 && s.pid != self) { *who = processName(s.pid); break; }
    }
    return true;
}

} // namespace talq

#else

namespace talq {
bool anotherAppUsingMic(QString *) { return false; }
} // namespace talq

#endif
