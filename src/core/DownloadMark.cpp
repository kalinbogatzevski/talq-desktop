#include "core/DownloadMark.h"
#include "core/DownloadMarkLogic.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>
#include <QObject>
#include <QPointer>

#include <thread>
#include <utility>

#if defined(_WIN32)

#include <windows.h>
#include <objbase.h>
#include <shobjidl.h>
#include <urlmon.h>

#include <string>

namespace talq {

namespace {

using downloadmark::SaveAnswer;
using downloadmark::Verdict;

static_assert(downloadmark::kHresultEFail == static_cast<qint32>(E_FAIL));
static_assert(downloadmark::kHresultInetSecurityProblem
              == static_cast<qint32>(INET_E_SECURITY_PROBLEM));

// TalQ's own client id for Attachment Services (SetClientGuid). Fixed for good: Windows keys what
// it remembers about a client (a per-client prompt state) on it.
// {deb49b29-97aa-4fab-bfc9-dd7c8070a056}
constexpr GUID kTalqAttachmentClient = {
    0xdeb49b29, 0x97aa, 0x4fab, {0xbf, 0xc9, 0xdd, 0x7c, 0x80, 0x70, 0xa0, 0x56}};

std::wstring zoneStreamOf(const std::wstring &path)
{
    return path + L":Zone.Identifier";
}

bool zoneStreamExists(const std::wstring &path)
{
    const HANDLE h = CreateFileW(zoneStreamOf(path).c_str(), GENERIC_READ,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    CloseHandle(h);
    return true;
}

// The fallback: the stream written directly. On a volume without streams (FAT, exFAT, some shares)
// the create fails, and that is the whole of the failure.
bool writeZoneStream(const std::wstring &path, const QByteArray &text, DWORD *error)
{
    const HANDLE h = CreateFileW(zoneStreamOf(path).c_str(), GENERIC_WRITE,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        *error = GetLastError();
        return false;
    }
    DWORD written = 0;
    const bool ok = WriteFile(h, text.constData(), static_cast<DWORD>(text.size()), &written, nullptr)
                    && written == static_cast<DWORD>(text.size());
    if (!ok)
        *error = GetLastError();
    CloseHandle(h);
    return ok;
}

// Attachment Services, on the calling thread. The class is apartment-threaded, so the thread is
// made a single-threaded apartment for the call (ours is a fresh thread, so this init is its own).
SaveAnswer askAttachmentServices(const std::wstring &path, const std::wstring &source, HRESULT *hr)
{
    const HRESULT coInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool weInitedCom = SUCCEEDED(coInit);

    SaveAnswer answer = SaveAnswer::NotAsked;
    IAttachmentExecute *ae = nullptr;
    *hr = CoCreateInstance(__uuidof(AttachmentServices), nullptr, CLSCTX_INPROC_SERVER,
                           __uuidof(IAttachmentExecute), reinterpret_cast<void **>(&ae));
    if (SUCCEEDED(*hr) && ae) {
        *hr = ae->SetClientGuid(kTalqAttachmentClient);
        if (SUCCEEDED(*hr))
            *hr = ae->SetLocalPath(path.c_str());
        if (SUCCEEDED(*hr))
            *hr = ae->SetSource(source.c_str());
        if (SUCCEEDED(*hr)) {
            // Only Save()'s own answer is read for a finding: an E_FAIL from the set-up calls
            // above leaves the answer NotAsked.
            *hr = ae->Save();
            answer = downloadmark::saveAnswerFor(static_cast<qint32>(*hr));
        }
        ae->Release();
    }
    if (weInitedCom)
        CoUninitialize();
    return answer;
}

QString hex(long value)
{
    return QStringLiteral("0x%1").arg(static_cast<quint32>(value), 8, 16, QLatin1Char('0'));
}

} // namespace

DownloadMarkOutcome markDownloadedFile(const QString &path, const QUrl &source)
{
    const QString name = QFileInfo(path).fileName();
    const std::wstring native =
        QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath()).toStdWString();
    const std::wstring sourceW = downloadmark::sourceUrlFor(source).toStdWString();

    HRESULT hr = S_OK;
    SaveAnswer answer = SaveAnswer::NotAsked;
    if (QFileInfo(path).size() > 0)
        answer = askAttachmentServices(native, sourceW, &hr);

    switch (downloadmark::judge(answer, QFileInfo::exists(path))) {
    case Verdict::Marked:
        // A success with no stream is Windows' own choice (its policy keeps no zone, or the zone
        // needs none); writing one over that choice would overrule the machine's settings.
        if (zoneStreamExists(native))
            qInfo().noquote() << "[MOTW] marked" << name << "through Attachment Services";
        else
            qInfo().noquote() << "[MOTW]" << name
                              << "checked by Attachment Services; Windows wrote no zone stream";
        return DownloadMarkOutcome::Marked;
    case Verdict::Blocked:
        if (QFileInfo::exists(path))
            QFile::remove(path);
        qWarning().noquote() << "[MOTW]" << name << "blocked by the zone's policy; not kept";
        return DownloadMarkOutcome::Blocked;
    case Verdict::VirusFound: {
        // Flagged, and the antivirus may have left it in place: never written a stream of ours
        // and opened. A removal the antivirus still blocks leaves it, unannounced and unopened.
        const bool wasThere = QFileInfo::exists(path);
        const bool gone = !wasThere || QFile::remove(path);
        qWarning().noquote() << "[MOTW]" << name << "flagged by the antivirus check, answer"
                             << hex(hr) << '-'
                             << (!wasThere ? QStringLiteral("already removed")
                                 : gone    ? QStringLiteral("removed")
                                           : QStringLiteral("could not be removed; not opened"));
        return DownloadMarkOutcome::VirusFound;
    }
    case Verdict::Removed:
        qWarning().noquote() << "[MOTW]" << name << "removed by the security check, answer"
                             << hex(hr);
        return DownloadMarkOutcome::Removed;
    case Verdict::WriteStream:
        break;
    }

    DWORD error = 0;
    if (writeZoneStream(native, downloadmark::zoneIdentifierText(source), &error)) {
        qInfo().noquote() << "[MOTW] marked" << name << "directly; Attachment Services"
                          << (answer == SaveAnswer::NotAsked ? QStringLiteral("not used")
                                                             : QStringLiteral("failed"))
                          << hex(hr);
        return DownloadMarkOutcome::MarkedDirectly;
    }
    qWarning().noquote() << "[MOTW] could not mark" << name << "(no stream on this drive?), error"
                         << error << "- the file is kept";
    return DownloadMarkOutcome::NotMarked;
}

} // namespace talq

#endif // _WIN32

namespace talq {

#if !defined(_WIN32)
DownloadMarkOutcome markDownloadedFile(const QString &, const QUrl &)
{
    return DownloadMarkOutcome::NotApplicable;
}
#endif

void markDownloadedFileAsync(const QString &path, const QUrl &source, QObject *context,
                             std::function<void(DownloadMarkOutcome)> done)
{
    QPointer<QObject> guard(context);
#if defined(_WIN32)
    std::thread([path, source, guard, done = std::move(done)]() mutable {
        const DownloadMarkOutcome outcome = markDownloadedFile(path, source);
        // Back on the GUI thread: qApp lives as long as the event loop that would run it, and the
        // guard drops the answer when the owner went away while the check ran.
        if (QCoreApplication *app = QCoreApplication::instance()) {
            QMetaObject::invokeMethod(app, [guard, done = std::move(done), outcome]() {
                if (guard)
                    done(outcome);
            }, Qt::QueuedConnection);
        }
    }).detach();
#else
    (void)path;
    (void)source;
    QMetaObject::invokeMethod(QCoreApplication::instance(), [guard, done = std::move(done)]() {
        if (guard)
            done(DownloadMarkOutcome::NotApplicable);
    }, Qt::QueuedConnection);
#endif
}

} // namespace talq
