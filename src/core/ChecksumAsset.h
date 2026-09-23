#pragma once

// Reading the installer digest out of a GitHub release's companion ".sha256"
// asset (the generic build's update channel).
//
// WHY THIS EXISTS: UpdateChecker used to accept the asset only when its whole
// body was 64 hex characters. The release runbook published it with
// `sha256sum TalQ-vX-Setup.exe > TalQ-vX-Setup.exe.sha256`, which writes
// "<digest>  <name>" -- or "<digest> *<name>" from Git Bash on Windows, which
// marks binary mode with an asterisk. Every such body was rejected, so the
// generic channel quietly skipped checksum verification and trusted HTTPS alone
// on every update (found on v0.72.3 and v0.73.0, 2026-09-23).
//
// Accepted:
//   - a bare digest:                            "<64 hex>"
//   - GNU sha256sum, text or binary mode:       "<64 hex>  <name>" / "<64 hex> *<name>"
//   - GNU's escaped form, used when the name holds a backslash or a newline:
//     "\<64 hex> *C:\\dir\\X.exe" (Git Bash's sha256sum on a Windows path)
//   - several such lines (a SHA256SUMS list):   the line naming `assetName` wins
// A name is compared by its last path component, so "dist/X.exe" names X.exe,
// and without regard to case: the asset is a Windows file. Hex may be upper-case
// (PowerShell, CertUtil); the result is always lower-case, as verifySha256 wants.
//
// Anything that cannot be trusted yields an EMPTY string, and the caller keeps
// the old fallback (warn, then rely on HTTPS): a malformed line, a file that only
// names OTHER files, two different digests for our file -- a bare digest counts
// as one, since the asset is "<our file>.sha256" -- or more than one bare
// digest. It never guesses. Guessing wrong would not open a hole -- the asset
// comes from the same release as the installer -- but it would make a good
// download fail verification and block the update.

#include <QByteArray>
#include <QString>
#include <QStringList>

namespace talq {

inline bool isSha256Hex(const QString &s)
{
    if (s.size() != 64) return false;
    for (const QChar c : s) {
        const char16_t u = c.unicode();
        const bool hex = (u >= u'0' && u <= u'9') || (u >= u'a' && u <= u'f')
                      || (u >= u'A' && u <= u'F');
        if (!hex) return false;
    }
    return true;
}

inline QString sha256FromChecksumAsset(const QByteArray &body, const QString &assetName)
{
    QByteArray bytes = body;
    if (bytes.startsWith("\xEF\xBB\xBF"))            // UTF-8 BOM (an editor save)
        bytes.remove(0, 3);

    QStringList bare;                                 // digests with no name
    QStringList forUs;                                // digests naming assetName
    bool namesOthers = false;

    const QStringList lines = QString::fromUtf8(bytes).split(QLatin1Char('\n'));
    for (const QString &raw : lines) {
        QString line = raw.trimmed();                 // also drops a CRLF's '\r'
        if (line.isEmpty()) continue;

        // GNU coreutils prefixes a line with '\' when it had to escape the name
        // ('\\' for a backslash, '\n' for a newline, '\r' for a CR).
        const bool escaped = line.startsWith(QLatin1Char('\\'));
        if (escaped) line.remove(0, 1);

        const QString digest = line.left(64);
        if (!isSha256Hex(digest)) return {};          // malformed line: trust nothing

        if (line.size() == 64) { bare << digest.toLower(); continue; }

        // sha256sum: the digest, one separator, then ' ' (text) or '*' (binary),
        // then the name. The separator is REQUIRED, so 65+ hex characters in a
        // row is malformed rather than "a digest named by its 65th character".
        const QChar sep = line.at(64);
        if (sep != QLatin1Char(' ') && sep != QLatin1Char('\t')) return {};
        QString name = line.mid(65);
        if (name.startsWith(QLatin1Char(' ')) || name.startsWith(QLatin1Char('*')))
            name.remove(0, 1);
        name = name.trimmed();
        if (name.isEmpty()) { bare << digest.toLower(); continue; }

        if (escaped) {
            QString un;
            un.reserve(name.size());
            for (int i = 0; i < name.size(); ++i) {
                if (name.at(i) == QLatin1Char('\\') && i + 1 < name.size()) {
                    const QChar n = name.at(++i);
                    un += n == QLatin1Char('n') ? QChar(u'\n')
                        : n == QLatin1Char('r') ? QChar(u'\r') : n;
                } else {
                    un += name.at(i);
                }
            }
            name = un;
        }

        const int slash = qMax(name.lastIndexOf(QLatin1Char('/')),
                               name.lastIndexOf(QLatin1Char('\\')));
        const QString base = slash >= 0 ? name.mid(slash + 1) : name;
        if (base.compare(assetName, Qt::CaseInsensitive) == 0) forUs << digest.toLower();
        else                                                    namesOthers = true;
    }

    if (!forUs.isEmpty()) {
        for (const QString &d : forUs)
            if (d != forUs.first()) return {};        // conflicting digests for our file
        for (const QString &d : bare)
            if (d != forUs.first()) return {};        // a bare digest that disagrees
        return forUs.first();
    }
    if (namesOthers) return {};                       // a list without our file
    if (bare.size() == 1) return bare.first();
    return {};                                        // empty, or several bare digests
}

} // namespace talq
