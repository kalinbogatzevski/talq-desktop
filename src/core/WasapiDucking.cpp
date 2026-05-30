#include "core/WasapiDucking.h"

#if defined(_WIN32)

#include <QDebug>
// INITGUID makes the DEFINE_GUID declarations in the COM headers below allocate
// the GUID constants (CLSID_MMDeviceEnumerator, IID_IMMDeviceEnumerator,
// IID_IAudioSessionManager2, IID_IAudioSessionControl2, …) locally in this
// translation unit, so we don't depend on which import lib happens to carry the
// WASAPI GUIDs under MinGW. Must precede the headers that DEFINE_GUID them.
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <objbase.h>

namespace talq {

void disableCommunicationsDucking()
{
    // Qt's Windows platform plugin already initialises COM on the GUI thread
    // (apartment-threaded), like NotificationManager relies on. Still call
    // CoInitializeEx defensively in case we run on a thread that hasn't: a
    // SUCCEEDED result (S_OK or S_FALSE) must be balanced with CoUninitialize;
    // RPC_E_CHANGED_MODE (already inited differently) must NOT be.
    const HRESULT coInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool weInitedCom = SUCCEEDED(coInit);

    IMMDeviceEnumerator    *enumerator = nullptr;
    IMMDevice              *device     = nullptr;
    IAudioSessionManager2  *sessionMgr = nullptr;
    IAudioSessionControl   *sessionCtl = nullptr;
    IAudioSessionControl2  *sessionCtl2 = nullptr;

    HRESULT hr = CoCreateInstance(CLSID_MMDeviceEnumerator, nullptr,
                                  CLSCTX_INPROC_SERVER, IID_IMMDeviceEnumerator,
                                  reinterpret_cast<void **>(&enumerator));
    if (FAILED(hr) || !enumerator) {
        qWarning() << "WasapiDucking: no device enumerator, hr=" << Qt::hex << hr;
        goto cleanup;
    }

    // Default render (playback) endpoint, console role — the device GStreamer's
    // wasapi sink plays our call audio on by default.
    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    if (FAILED(hr) || !device) {
        qWarning() << "WasapiDucking: no default render endpoint, hr=" << Qt::hex << hr;
        goto cleanup;
    }

    hr = device->Activate(IID_IAudioSessionManager2, CLSCTX_INPROC_SERVER,
                          nullptr, reinterpret_cast<void **>(&sessionMgr));
    if (FAILED(hr) || !sessionMgr) {
        qWarning() << "WasapiDucking: no session manager, hr=" << Qt::hex << hr;
        goto cleanup;
    }

    // NULL session GUID = this process's default audio session on that device.
    hr = sessionMgr->GetAudioSessionControl(nullptr, 0, &sessionCtl);
    if (FAILED(hr) || !sessionCtl) {
        qWarning() << "WasapiDucking: no session control, hr=" << Qt::hex << hr;
        goto cleanup;
    }

    hr = sessionCtl->QueryInterface(IID_IAudioSessionControl2,
                                    reinterpret_cast<void **>(&sessionCtl2));
    if (FAILED(hr) || !sessionCtl2) {
        qWarning() << "WasapiDucking: IAudioSessionControl2 unavailable, hr=" << Qt::hex << hr;
        goto cleanup;
    }

    // TRUE = opt OUT of the system's default ducking experience for this
    // session, so our call audio is not attenuated by other apps' comms streams.
    hr = sessionCtl2->SetDuckingPreference(TRUE);
    if (FAILED(hr))
        qWarning() << "WasapiDucking: SetDuckingPreference failed, hr=" << Qt::hex << hr;
    else
        qInfo() << "WasapiDucking: opted call audio out of communications ducking";

cleanup:
    if (sessionCtl2) sessionCtl2->Release();
    if (sessionCtl)  sessionCtl->Release();
    if (sessionMgr)  sessionMgr->Release();
    if (device)      device->Release();
    if (enumerator)  enumerator->Release();
    if (weInitedCom) CoUninitialize();
}

} // namespace talq

#else  // non-Windows

namespace talq {
void disableCommunicationsDucking() {}
} // namespace talq

#endif
