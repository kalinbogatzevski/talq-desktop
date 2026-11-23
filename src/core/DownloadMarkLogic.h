#pragma once

// Mark-of-the-Web for a file TalQ saves from a chat: the pure half (test: download-mark-logic-test).
// The Windows half, and the reason, is DownloadMark.h.
//
//   sourceUrlFor         the address written down as the file's source: http or https with a host,
//                        no user name, password or fragment, fully percent-encoded. Anything else
//                        is "about:internet", which Windows always places in the Internet zone. The
//                        result is printable ASCII and nothing else: the file name the SENDER chose
//                        is part of the path, and a raw line break in it would start a line of its
//                        own in the stream (a "ZoneId=0" of the sender's making).
//   zoneIdentifierText   the "Zone.Identifier" stream TalQ writes itself when Windows' Attachment
//                        Services cannot be used, as browsers write it: an INI section of CRLF
//                        lines, the Internet zone (3), the source as HostUrl. (Attachment Services
//                        itself ends its HostUrl line with a NUL instead of CRLF; Windows reads
//                        both.)
//   saveAnswerFor        the HRESULT Save() returned, reduced to a SaveAnswer. E_FAIL is the
//                        antivirus finding: browsers read it so, and a flagged file Windows left on
//                        disk must never be written a stream of TalQ's own and opened.
//   judge                what an answer from Attachment Services' Save() means for the file, from
//                        that answer and whether the file is still there afterwards (its check can
//                        delete the file).

#include <QByteArray>
#include <QString>
#include <QUrl>
#include <QtGlobal>

namespace talq::downloadmark {

inline QString sourceUrlFor(const QUrl &url)
{
    const QString fallback = QStringLiteral("about:internet");
    if (!url.isValid() || url.host().isEmpty())
        return fallback;
    const QString scheme = url.scheme().toLower();
    if (scheme != QLatin1String("http") && scheme != QLatin1String("https"))
        return fallback;
    const QByteArray encoded =
        url.adjusted(QUrl::RemoveUserInfo | QUrl::RemoveFragment).toEncoded(QUrl::FullyEncoded);
    if (encoded.isEmpty())
        return fallback;
    for (const char c : encoded) {
        const auto u = static_cast<unsigned char>(c);
        if (u <= 0x20 || u >= 0x7F)
            return fallback;
    }
    return QString::fromLatin1(encoded);
}

inline QByteArray zoneIdentifierText(const QUrl &source)
{
    QByteArray text("[ZoneTransfer]\r\nZoneId=3\r\nHostUrl=");
    text += sourceUrlFor(source).toLatin1();
    text += "\r\n";
    return text;
}

// What IAttachmentExecute::Save() answered, reduced to what matters here.
enum class SaveAnswer {
    NotAsked,          // the interface could not be created or set up, or the file is empty
                       // (Save may delete an empty file, and there is nothing in it to check)
    Saved,             // a success code: Windows wrote what its zone mapping and policy call for
    BlockedByPolicy,   // INET_E_SECURITY_PROBLEM: the policy for that zone does not allow the file
    VirusFound,        // E_FAIL: the antivirus check flagged the file (it may have left it there)
    Failed,            // any other failure
};

// The two failure codes Save() is read for, by value (this header does not include windows.h;
// DownloadMark.cpp checks them against the SDK's own definitions at compile time).
constexpr qint32 kHresultEFail = static_cast<qint32>(0x80004005u);                // E_FAIL
constexpr qint32 kHresultInetSecurityProblem = static_cast<qint32>(0x800C000Eu);  // INET_E_SECURITY_PROBLEM

inline SaveAnswer saveAnswerFor(qint32 hr)
{
    if (hr >= 0)   // SUCCEEDED
        return SaveAnswer::Saved;
    if (hr == kHresultInetSecurityProblem)
        return SaveAnswer::BlockedByPolicy;
    if (hr == kHresultEFail)
        return SaveAnswer::VirusFound;
    return SaveAnswer::Failed;
}

enum class Verdict {
    Marked,        // Windows did it (when its policy says to keep no zone, it keeps none): go on
    WriteStream,   // TalQ writes the stream itself, then goes on whether or not that works
    Blocked,       // refused by policy: not kept, not opened, and the user is told
    VirusFound,    // flagged by the antivirus: not kept, not opened, and the user is told
    Removed,       // gone after the check (an antivirus took it): not opened, and the user is told
};

inline Verdict judge(SaveAnswer answer, bool fileStillThere)
{
    if (answer == SaveAnswer::BlockedByPolicy)
        return Verdict::Blocked;
    if (answer == SaveAnswer::VirusFound)
        return Verdict::VirusFound;
    if (!fileStillThere)
        return Verdict::Removed;
    return answer == SaveAnswer::Saved ? Verdict::Marked : Verdict::WriteStream;
}

} // namespace talq::downloadmark
