#pragma once
// Sidebar folders: every data rule, in one header with no I/O and no QObject (Qt Core value
// types only), so a test can pin all of it and the store/painter/glue cannot each invent a
// slightly different version. Spec: docs/superpowers/specs/2026-11-02-sidebar-folders-design.md
//
// WHAT A FOLDER IS. A named, ordered, collapsible section the user files conversations into.
// Local to this PC and this account by default, never sent to the Talk server (the Talk-24 tag
// code is a separate, server-synced feature and is untouched). With the opt-in sync switch on, a
// copy also lives in the user's OWN Nextcloud Files (see
// docs/superpowers/specs/2026-11-03-folder-sync-design.md). A conversation is in EXACTLY ONE
// folder, or in none ("Other"). That invariant holds by DATA SHAPE: `assign` is a token -> folder
// id map, so a token cannot name two folders; the functions below keep every value pointing at a
// folder that exists.
//
// TRAPS THIS FILE ALREADY ACCOUNTS FOR
//  - A folder id is "f_" + 12 lowercase hex. It never starts with '!' and is never numeric, so
//    it cannot be mistaken for a Talk tag id (numeric strings) or the client pseudo-ids
//    ("!favorites", "!other") by code that keys on a leading '!'.
//  - A name is PAINTED in the sidebar, so a bidi override (U+202A..202E, U+2066..2069), a
//    zero-width or any other control/format character is stripped, never stored. A name is
//    measured in CODE POINTS (an emoji outside the BMP is two UTF-16 units): do NOT put a
//    maxLength(40) on a QLineEdit/QInputDialog for it -- that counts UTF-16 units and cuts
//    short or splits a surrogate pair.
//  - A name that is too long is TRUNCATED, not refused. The same function repairs a file read
//    from disk, where refusing would drop the user's folder and every chat filed in it.
//  - A token is checked against Talk's own route pattern (TalkToken.h) both when assigned and
//    when loaded.
//  - A caller must never lose a user's folders to a hostile or damaged file: decodeFolders()
//    reports Corrupt/FutureVersion and the STORE decides what to do with the bytes; this header
//    never throws, never allocates beyond the caps below, and never trusts a JSON type.
//  - The caps are one set of constants used by both sides: the number encode can write is the
//    number decode accepts, so the store can never write a file it would later set aside as
//    corrupt (tested: "the largest state encode can write decodes back").

#include "core/TalkToken.h"

#include <QByteArray>
#include <QChar>
#include <QCryptographicHash>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QRegularExpression>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

#include <algorithm>
#include <cmath>
#include <limits>

namespace talq {

constexpr int kMaxFolders = 50;
constexpr int kMaxFolderNameChars = 40;          // CODE POINTS, not UTF-16 units
constexpr int kMaxAssignments = 20000;           // filed chats per account; mutation AND decode agree
constexpr int kMaxFolderEntriesExamined = kMaxFolders * 20;   // decode work cap on the folders array
constexpr qint64 kMaxFolderFileBytes = 4 * 1024 * 1024;       // decode refuses more (largest legal file is ~1.2 MB: tested)
constexpr int kFolderFormatVersion = 1;

struct Folder {
    QString id;
    QString name;
    bool collapsed = false;
};

struct FolderState {
    QVector<Folder> folders;               // order = display order
    bool otherCollapsed = false;           // the built-in "Other" section
    QHash<QString, QString> assign;        // conversation token -> folder id
};

// What a closed folder shows at its right edge. One input per member row.
struct FolderRollup {
    int count = 0;
    int unread = 0;
    bool mention = false;
    bool directMention = false;
};

// count++, unread += max(0, unreadCount) (saturating, so an absurd count cannot wrap negative and
// hide a badge), flags OR-ed. A direct mention IS a mention, so mention is never false while
// directMention is true.
inline void addToRollup(FolderRollup &r, int unreadCount, bool mention, bool directMention)
{
    if (r.count < std::numeric_limits<int>::max())
        ++r.count;
    const int u = std::max(0, unreadCount);
    r.unread = (r.unread > std::numeric_limits<int>::max() - u) ? std::numeric_limits<int>::max()
                                                                : r.unread + u;
    r.mention = r.mention || mention || directMention;
    r.directMention = r.directMention || directMention;
}

// The badge is painted only when the sum is above zero (a mention flag with a zero sum has
// nothing to show a number for). SidebarPainter::paintFolderHeader asks this for a CLOSED folder.
// The badge's colour (any direct mention = danger, else any mention = amber, else plain) is not
// decided here: paintUnreadBadge does it from the two flags, and a conversation row shares it.
inline bool rollupShowsBadge(const FolderRollup &r) { return r.unread > 0; }

// ---------------------------------------------------------------------------------------------
// Mutation results. Total and small. A mutation that returns anything but Ok leaves the state
// BYTE-FOR-BYTE as it was (tested for every refusal). NotAttached and ReadOnly are produced by
// FolderStore only; they live here so the glue switches over one enum.
enum class FolderResult {
    Ok,             // changed
    Unchanged,      // already so; nothing to do, nothing to write
    EmptyName,      // nothing left after cleaning the name
    DuplicateName,  // another folder already has that name (ignoring case)
    TooMany,        // kMaxFolders folders, or kMaxAssignments filed chats, already
    NotFound,       // no folder with that id
    BadToken,       // not a Talk conversation token
    NotAttached,    // (store) no account is attached
    ReadOnly        // (store) the file on disk is a newer format, or could not be set aside
};

inline bool isRefusal(FolderResult r) { return r != FolderResult::Ok && r != FolderResult::Unchanged; }

// ---------------------------------------------------------------------------------------------
// Names

namespace folder_detail {

inline void appendCodePoint(QString &out, char32_t cp)
{
    if (QChar::requiresSurrogates(cp)) {
        out.append(QChar(QChar::highSurrogate(cp)));
        out.append(QChar(QChar::lowSurrogate(cp)));
    } else {
        out.append(QChar(static_cast<char16_t>(cp)));
    }
}

// Characters that are never part of a painted name: controls (Cc), format characters (Cf: bidi
// overrides/isolates/marks, zero-width space/joiners, BOM, soft hyphen, word joiner, the tags
// block), surrogates and private use, plus the noncharacters. Whitespace-class controls are
// mapped to a space BEFORE this test, or "a\nb" would collapse to "ab".
inline bool isStrippedCodePoint(char32_t cp)
{
    switch (QChar::category(cp)) {
    case QChar::Other_Control:
    case QChar::Other_Format:
    case QChar::Other_Surrogate:
    case QChar::Other_PrivateUse:
        return true;
    default:
        break;
    }
    if (QChar::isNonCharacter(cp))
        return true;
    // Fillers that paint as NOTHING but are letters/marks, so no category test catches them: the
    // combining grapheme joiner, the Hangul and Khmer fillers, the Mongolian free variation
    // selectors. A name made only of these would pass the "not empty" test and paint as a blank
    // header. (Emoji variation selectors U+FE00..FE0F are kept: they belong to the emoji before them.)
    return cp == 0x034F || cp == 0x115F || cp == 0x1160 || cp == 0x17B4 || cp == 0x17B5
        || (cp >= 0x180B && cp <= 0x180F) || cp == 0x3164 || cp == 0xFFA0;
}

// A name needs at least one character that paints something by itself: marks (accents, variation
// selectors, enclosing marks) alone are not a name.
inline bool isMarkCategory(char32_t cp)
{
    const QChar::Category c = QChar::category(cp);
    return c == QChar::Mark_NonSpacing || c == QChar::Mark_SpacingCombining || c == QChar::Mark_Enclosing;
}

// True when at least one code point is neither a space nor a mark.
inline bool hasPaintingBase(const QString &s)
{
    for (qsizetype i = 0; i < s.size();) {   // a cleaned string has no lone surrogate
        char32_t cp = s.at(i).unicode();
        if (QChar::isHighSurrogate(cp) && i + 1 < s.size()) {
            cp = QChar::surrogateToUcs4(s.at(i).unicode(), s.at(i + 1).unicode());
            i += 2;
        } else {
            i += 1;
        }
        if (cp != 0x20 && !isMarkCategory(cp))
            return true;
    }
    return false;
}

} // namespace folder_detail

// Clean a name for storage and painting. Returns an EMPTY string when nothing usable is left
// (the caller refuses).
//   1. whitespace-class characters (\n, \t, NBSP, U+2028, ...) become a space
//   2. control / format / bidi / zero-width / private-use / lone-surrogate characters and the
//      blank fillers (U+3164, U+115F, ...) are dropped; a name of only combining marks is empty
//   3. trim and collapse runs of whitespace to one space
//   4. NFC, so "e" + U+0301 and the precomposed e-acute are ONE name for the duplicate check
//      (NFC can LENGTHEN a string: U+0344 becomes two code points, so it runs before the cut)
//   5. cut to kMaxFolderNameChars CODE POINTS (a surrogate pair is never split), then trim again
//      in case the cut landed after a space
//   6. refuse (empty) if what is left is only marks: tested on the FINAL string so the function is
//      idempotent -- a stored name must come back unchanged when it is cleaned again on load
// Removing the zero-width joiner splits an emoji ZWJ sequence into its parts; that is the price of
// not letting an invisible character into a painted label.
inline QString normaliseFolderName(const QString &raw)
{
    QString cleaned;
    cleaned.reserve(raw.size());
    for (qsizetype i = 0; i < raw.size();) {
        char32_t cp = raw.at(i).unicode();
        qsizetype step = 1;
        if (QChar::isHighSurrogate(cp)) {
            if (i + 1 < raw.size() && QChar::isLowSurrogate(raw.at(i + 1).unicode())) {
                cp = QChar::surrogateToUcs4(raw.at(i).unicode(), raw.at(i + 1).unicode());
                step = 2;
            } else {
                i += 1;   // lone high surrogate: not a character
                continue;
            }
        } else if (QChar::isLowSurrogate(cp)) {
            i += 1;       // lone low surrogate
            continue;
        }
        i += step;
        if (QChar::isSpace(cp))
            cleaned.append(QLatin1Char(' '));
        else if (!folder_detail::isStrippedCodePoint(cp))
            folder_detail::appendCodePoint(cleaned, cp);
    }
    cleaned = cleaned.simplified();
    if (cleaned.isEmpty())
        return QString();
    cleaned = cleaned.normalized(QString::NormalizationForm_C);

    int codePoints = 0;
    qsizetype cut = 0;
    while (cut < cleaned.size() && codePoints < kMaxFolderNameChars) {
        cut += (QChar::isHighSurrogate(cleaned.at(cut).unicode()) && cut + 1 < cleaned.size()) ? 2 : 1;
        ++codePoints;
    }
    cleaned.truncate(cut);
    cleaned = cleaned.trimmed();
    // The "is there a base character" test runs on the FINAL string, after the cut: testing before it
    // let 40 combining marks + "a" through, the cut then kept only the marks, and the stored name
    // normalised to EMPTY on the next load -- which drops the folder and every chat filed in it. The
    // function must be idempotent: normaliseFolderName(normaliseFolderName(x)) == normaliseFolderName(x).
    return folder_detail::hasPaintingBase(cleaned) ? cleaned : QString();
}

// ---------------------------------------------------------------------------------------------
// Ids

inline bool isFolderId(const QString &id)
{
    static const QRegularExpression re(QStringLiteral("\\Af_[0-9a-f]{12}\\z"));
    return re.match(id).hasMatch();
}

// "f_" + the low 48 bits of `bits` as 12 lowercase hex characters.
inline QString folderIdFromBits(quint64 bits)
{
    return QStringLiteral("f_")
        + QString::number(bits & 0xFFFFFFFFFFFFULL, 16).rightJustified(12, QLatin1Char('0'));
}

// The deterministic "derive again" step used on a clash (the splitmix64 finalizer).
inline quint64 deriveFolderEntropy(quint64 x)
{
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

// A fresh id from caller-supplied entropy (the store feeds QRandomGenerator; a test feeds a
// constant). On a clash with `existing` it derives again: eight mixed re-derivations, then a
// linear probe over the 48-bit space, which MUST terminate because `existing` is finite (it finds
// a free value within existing.size() + 1 steps).
inline QString makeFolderId(quint64 entropy, const QVector<Folder> &existing)
{
    quint64 bits = entropy;
    for (int attempt = 0;; ++attempt) {
        const QString id = folderIdFromBits(bits);
        const bool clash = std::any_of(existing.begin(), existing.end(),
                                       [&](const Folder &f) { return f.id == id; });
        if (!clash)
            return id;
        bits = attempt < 8 ? deriveFolderEntropy(bits) : bits + 1;
    }
}

// ---------------------------------------------------------------------------------------------
// Lookups

inline int folderIndexOf(const FolderState &s, const QString &id)
{
    if (id.isEmpty())
        return -1;
    for (int i = 0; i < s.folders.size(); ++i) {
        if (s.folders.at(i).id == id)
            return i;
    }
    return -1;
}

// The folder id a token is filed under, or EMPTY when it is unfiled -- also when the mapped
// folder no longer exists (an orphan is "Other", it is never an error).
inline QString folderOf(const FolderState &s, const QString &token)
{
    const auto it = s.assign.constFind(token);
    if (it == s.assign.constEnd())
        return QString();
    return folderIndexOf(s, it.value()) >= 0 ? it.value() : QString();
}

// Is `name` (already normalised) used by a folder other than `exceptId`? Case-insensitive.
inline bool folderNameTaken(const FolderState &s, const QString &name, const QString &exceptId = QString())
{
    for (const Folder &f : s.folders) {
        if (f.id != exceptId && QString::compare(f.name, name, Qt::CaseInsensitive) == 0)
            return true;
    }
    return false;
}

// ---------------------------------------------------------------------------------------------
// Mutations. Each returns Ok only when it changed the state; every other outcome leaves it
// untouched.

// New folders go at the END (display order = creation order, D14). The check order is
// EmptyName, TooMany, DuplicateName: at the limit no name would work, so say that first.
inline FolderResult createFolder(FolderState &s, const QString &rawName, quint64 entropy,
                                 QString *outId = nullptr)
{
    const QString name = normaliseFolderName(rawName);
    if (name.isEmpty())
        return FolderResult::EmptyName;
    if (s.folders.size() >= kMaxFolders)
        return FolderResult::TooMany;
    if (folderNameTaken(s, name))
        return FolderResult::DuplicateName;
    Folder f;
    f.id = makeFolderId(entropy, s.folders);
    f.name = name;
    s.folders.append(f);
    if (outId)
        *outId = f.id;
    return FolderResult::Ok;
}

// Renaming to a name that differs only by case from the folder's OWN name is allowed (that is a
// case fix, not a duplicate).
inline FolderResult renameFolder(FolderState &s, const QString &id, const QString &rawName)
{
    const int idx = folderIndexOf(s, id);
    if (idx < 0)
        return FolderResult::NotFound;
    const QString name = normaliseFolderName(rawName);
    if (name.isEmpty())
        return FolderResult::EmptyName;
    // Unchanged first: duplicate NAMES are tolerated on load (ids differ), so a folder may have a twin,
    // and "rename to what it already is" must stay a no-op, not read as a refusal.
    if (s.folders.at(idx).name == name)
        return FolderResult::Unchanged;
    if (folderNameTaken(s, name, id))
        return FolderResult::DuplicateName;
    s.folders[idx].name = name;
    return FolderResult::Ok;
}

// Deleting returns the folder's chats to Other: exactly its assignments are dropped.
inline FolderResult deleteFolder(FolderState &s, const QString &id)
{
    const int idx = folderIndexOf(s, id);
    if (idx < 0)
        return FolderResult::NotFound;
    s.folders.removeAt(idx);
    for (auto it = s.assign.begin(); it != s.assign.end();) {
        if (it.value() == id)
            it = s.assign.erase(it);
        else
            ++it;
    }
    return FolderResult::Ok;
}

// delta -1 = up, +1 = down; any other value is clamped into the list (no overflow for INT_MIN /
// INT_MAX). At the ends, or delta 0, it is a no-op: Unchanged.
inline FolderResult moveFolder(FolderState &s, const QString &id, int delta)
{
    const int idx = folderIndexOf(s, id);
    if (idx < 0)
        return FolderResult::NotFound;
    const qint64 wanted = static_cast<qint64>(idx) + static_cast<qint64>(delta);
    const int target = static_cast<int>(std::clamp<qint64>(wanted, 0, s.folders.size() - 1));
    if (target == idx)
        return FolderResult::Unchanged;
    s.folders.move(idx, target);
    return FolderResult::Ok;
}

// File `token` under `folderId`; an EMPTY folderId unfiles it. Token first, then folder.
inline FolderResult assignToken(FolderState &s, const QString &token, const QString &folderId)
{
    if (!isTalkToken(token))
        return FolderResult::BadToken;
    if (folderId.isEmpty())
        return s.assign.remove(token) > 0 ? FolderResult::Ok : FolderResult::Unchanged;
    if (folderIndexOf(s, folderId) < 0)
        return FolderResult::NotFound;
    const auto it = s.assign.find(token);
    if (it != s.assign.end()) {
        if (it.value() == folderId)
            return FolderResult::Unchanged;
        it.value() = folderId;
        return FolderResult::Ok;
    }
    if (s.assign.size() >= kMaxAssignments)
        return FolderResult::TooMany;
    s.assign.insert(token, folderId);
    return FolderResult::Ok;
}

inline FolderResult setFolderCollapsed(FolderState &s, const QString &id, bool collapsed)
{
    const int idx = folderIndexOf(s, id);
    if (idx < 0)
        return FolderResult::NotFound;
    if (s.folders.at(idx).collapsed == collapsed)
        return FolderResult::Unchanged;
    s.folders[idx].collapsed = collapsed;
    return FolderResult::Ok;
}

inline FolderResult setOtherCollapsed(FolderState &s, bool collapsed)
{
    if (s.otherCollapsed == collapsed)
        return FolderResult::Unchanged;
    s.otherCollapsed = collapsed;
    return FolderResult::Ok;
}

// ---------------------------------------------------------------------------------------------
// Repair after decoding (or any state of unknown origin). Returns true when it changed anything.
//   - a folder whose id is not ^f_[0-9a-f]{12}$ is DROPPED with its assignments (an id is never
//     re-keyed: two files could then disagree about which folder a chat is in)
//   - duplicate ids: the first valid one is kept
//   - names are normalised; a folder with no name left is dropped
//   - at most kMaxFolders folders survive
//   - an assignment is dropped when its token is not a Talk token or its folder is gone
// Duplicate NAMES are tolerated on load (ids differ, nothing is lost); only create/rename enforce
// uniqueness. The assignment COUNT is capped by decode, not here (nothing to choose between).
inline bool sanitiseFolderState(FolderState &s)
{
    bool changed = false;
    QVector<Folder> kept;
    kept.reserve(std::min<int>(s.folders.size(), kMaxFolders));
    QSet<QString> ids;
    for (Folder f : s.folders) {
        if (!isFolderId(f.id) || ids.contains(f.id)) {
            changed = true;
            continue;
        }
        const QString name = normaliseFolderName(f.name);
        if (name.isEmpty() || kept.size() >= kMaxFolders) {
            changed = true;
            continue;
        }
        if (name != f.name) {
            changed = true;
            f.name = name;
        }
        ids.insert(f.id);
        kept.append(f);
    }
    s.folders = kept;
    for (auto it = s.assign.begin(); it != s.assign.end();) {
        if (!isTalkToken(it.key()) || !ids.contains(it.value())) {
            it = s.assign.erase(it);
            changed = true;
        } else {
            ++it;
        }
    }
    return changed;
}

// ---------------------------------------------------------------------------------------------
// Account scope: folders are per Nextcloud account, so the file name is derived from the server
// and the CANONICAL user id (not the login name).

// Trim, drop trailing slashes, lowercase the scheme and the HOST only (userinfo and the path keep
// their case: a path can be case-sensitive). Empty when there is no host.
inline QString normaliseServerUrl(const QString &raw)
{
    const QString trimmed = raw.trimmed();
    QString scheme;
    QString rest = trimmed;
    const qsizetype sep = trimmed.indexOf(QStringLiteral("://"));
    if (sep >= 0) {
        scheme = trimmed.left(sep).toLower();
        rest = trimmed.mid(sep + 3);
    }
    while (rest.endsWith(QLatin1Char('/')))
        rest.chop(1);
    qsizetype authorityEnd = rest.size();
    for (qsizetype i = 0; i < rest.size(); ++i) {
        const QChar c = rest.at(i);
        if (c == QLatin1Char('/') || c == QLatin1Char('?') || c == QLatin1Char('#')) {
            authorityEnd = i;
            break;
        }
    }
    QString authority = rest.left(authorityEnd);
    const QString tail = rest.mid(authorityEnd);
    const qsizetype at = authority.lastIndexOf(QLatin1Char('@'));
    const QString hostPart = authority.mid(at + 1);   // at == -1 -> the whole authority
    if (hostPart.isEmpty())
        return QString();
    authority = authority.left(at + 1) + hostPart.toLower();
    return (sep >= 0 ? scheme + QStringLiteral("://") : QString()) + authority + tail;
}

// The first 16 lowercase hex characters of SHA-1 over normalisedUrl + "\n" + userId (UTF-8).
// EMPTY when either input is empty: the store refuses to attach to an empty key.
inline QString accountScopeKey(const QString &serverUrl, const QString &userId)
{
    const QString url = normaliseServerUrl(serverUrl);
    const QString uid = userId.trimmed();
    if (url.isEmpty() || uid.isEmpty())
        return QString();
    const QByteArray material = (url + QLatin1Char('\n') + uid).toUtf8();
    const QByteArray digest = QCryptographicHash::hash(material, QCryptographicHash::Sha1).toHex();
    return QString::fromLatin1(digest.left(16));
}

// What the store accepts as a scope key: exactly what accountScopeKey produces. The key becomes
// a file name, so this refuses a path separator, "..", a drive colon and an empty string; being
// hex-only it also cannot spell a Windows device name (CON, NUL, AUX, ...).
inline bool isScopeKey(const QString &key)
{
    static const QRegularExpression re(QStringLiteral("\\A[0-9a-f]{16}\\z"));
    return re.match(key).hasMatch();
}

// ---------------------------------------------------------------------------------------------
// JSON codec
//   { "v": 1, "otherCollapsed": false,
//     "folders": [ { "id": "f_0a1b2c3d4e5f", "name": "HQ", "collapsed": false } ],
//     "assign":  { "ycy3ht4n": "f_0a1b2c3d4e5f" } }

// Deterministic: folders in display order, assign sorted by token. `version` is the "v" written:
// the local file always takes the default (kFolderFormatVersion); the sync code passes its own
// constant for the shared remote file, so the two formats can move apart.
inline QByteArray encodeFolders(const FolderState &s, int version = kFolderFormatVersion)
{
    QJsonArray folders;
    for (const Folder &f : s.folders) {
        QJsonObject o;
        o.insert(QStringLiteral("id"), f.id);
        o.insert(QStringLiteral("name"), f.name);
        o.insert(QStringLiteral("collapsed"), f.collapsed);
        folders.append(o);
    }
    QStringList tokens;
    tokens.reserve(s.assign.size());
    for (auto it = s.assign.constBegin(); it != s.assign.constEnd(); ++it)
        tokens.append(it.key());
    std::sort(tokens.begin(), tokens.end());
    QJsonObject assign;
    for (const QString &t : std::as_const(tokens))
        assign.insert(t, s.assign.value(t));
    QJsonObject root;
    root.insert(QStringLiteral("v"), version);
    root.insert(QStringLiteral("otherCollapsed"), s.otherCollapsed);
    root.insert(QStringLiteral("folders"), folders);
    root.insert(QStringLiteral("assign"), assign);
    return QJsonDocument(root).toJson(QJsonDocument::Indented);
}

enum class DecodeStatus {
    Ok,             // state is valid (possibly empty); `repaired` says whether entries were dropped
    Corrupt,        // not a file this version can read; state is EMPTY; the store sets the bytes aside
    FutureVersion   // "v" is newer than this build; state is EMPTY and untouched; the caller must NOT write it back
};

// How a store's attach found the file on disk. It lives here, not inside FolderStore, so a pure
// function (the folder-sync base-trust rule) can take it as an input.
enum class FolderLoadOutcome {
    Detached,       // no account attached
    Ok,             // the file was read, valid, nothing repaired
    Missing,        // no file
    Repaired,       // decoded, but entries were dropped or cleaned (DecodeResult::repaired)
    SetAside,       // corrupt, and renamed away
    ReadOnlyLoad    // newer format, or the corrupt file could not be set aside: the store is read-only
};

struct DecodeResult {
    DecodeStatus status = DecodeStatus::Ok;
    FolderState state;
    bool repaired = false;   // Ok, but something was dropped or cleaned (see sanitiseFolderState)
};

// Never throws, and its work is bounded by kMaxFolderFileBytes and the entry caps.
//   empty / whitespace-only bytes (after an optional UTF-8 BOM) -> Ok, empty state (a zero-byte
//   file is not damage)
//   not JSON, over the byte cap, a root that is not an object, "v" missing / not a number /
//   not an integer / below 1, or "folders" / "assign" present with the wrong type, or more
//   entries than the caps -> Corrupt
//   "v" above `maxVersion` (and finite and integral) -> FutureVersion
// Per-entry problems (a folder that is not an object, a non-string id, an assignment whose value
// is not a string) drop THAT entry and set `repaired`. Unknown fields are ignored.
// `maxVersion` is the newest "v" the caller can read (default kFolderFormatVersion, which is what
// the local file uses); the sync code passes kFolderSyncRemoteVersion for the shared file. The
// "v" below 1 rule does not depend on it.
inline DecodeResult decodeFolders(const QByteArray &bytes, int maxVersion = kFolderFormatVersion)
{
    DecodeResult out;
    const auto corrupt = [&out]() {
        out.status = DecodeStatus::Corrupt;
        out.state = FolderState();
        out.repaired = false;
        return out;
    };

    if (bytes.size() > kMaxFolderFileBytes)
        return corrupt();
    // A hand edit in an old Notepad prepends a UTF-8 byte-order mark; that is not damage.
    const QByteArray body = bytes.startsWith("\xEF\xBB\xBF") ? bytes.mid(3) : bytes;
    if (body.trimmed().isEmpty())
        return out;

    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject())
        return corrupt();
    const QJsonObject root = doc.object();

    const QJsonValue v = root.value(QStringLiteral("v"));
    if (!v.isDouble())
        return corrupt();
    const double version = v.toDouble();
    // 1e400 parses to infinity and floor(inf) == inf, so test finiteness first.
    if (!std::isfinite(version) || version != std::floor(version) || version < 1)
        return corrupt();
    if (version > maxVersion) {
        out.status = DecodeStatus::FutureVersion;
        return out;
    }

    out.state.otherCollapsed = root.value(QStringLiteral("otherCollapsed")).toBool(false);

    const QJsonValue foldersV = root.value(QStringLiteral("folders"));
    if (!foldersV.isUndefined()) {
        if (!foldersV.isArray())
            return corrupt();
        const QJsonArray arr = foldersV.toArray();
        if (arr.size() > kMaxFolderEntriesExamined)
            return corrupt();
        for (const QJsonValue &e : arr) {
            if (!e.isObject()) {
                out.repaired = true;
                continue;
            }
            const QJsonObject o = e.toObject();
            const QJsonValue idV = o.value(QStringLiteral("id"));
            const QJsonValue nameV = o.value(QStringLiteral("name"));
            if (!idV.isString() || !nameV.isString()) {
                out.repaired = true;
                continue;
            }
            Folder f;
            f.id = idV.toString();
            f.name = nameV.toString();
            f.collapsed = o.value(QStringLiteral("collapsed")).toBool(false);
            out.state.folders.append(f);
        }
    }

    const QJsonValue assignV = root.value(QStringLiteral("assign"));
    if (!assignV.isUndefined()) {
        if (!assignV.isObject())
            return corrupt();
        const QJsonObject obj = assignV.toObject();
        if (obj.size() > kMaxAssignments)
            return corrupt();
        for (auto it = obj.constBegin(); it != obj.constEnd(); ++it) {
            if (!it.value().isString()) {
                out.repaired = true;
                continue;
            }
            out.state.assign.insert(it.key(), it.value().toString());
        }
    }

    if (sanitiseFolderState(out.state))
        out.repaired = true;
    return out;
}

} // namespace talq
