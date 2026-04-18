#include "EmojiData.h"

#include <QSettings>
#include <algorithm>

namespace EmojiData {

namespace {
QVector<EmojiEntry> g_entries;
QHash<QString, const EmojiEntry*> g_byShortcode;
QHash<QString, const EmojiEntry*> g_byShortForm;
QHash<QString, QPixmap> g_pixmapCache;
QStringList g_recentCodepoints;
const int kRecentMax = 24;
} // namespace

// Defined in EmojiData_generated.cpp; fills g_entries when called.
void loadGeneratedData(QVector<EmojiEntry> &out);

void initialize()
{
    loadGeneratedData(g_entries);
    for (const auto &e : g_entries) {
        for (const QString &sc : e.shortcodes) g_byShortcode.insert(sc, &e);
        for (const QString &sf : e.shortForms) g_byShortForm.insert(sf, &e);
    }
    g_recentCodepoints = QSettings().value("emoji/recent").toStringList();
}

const QVector<EmojiEntry> &allEntries() { return g_entries; }
const EmojiEntry *findByShortcode(const QString &s) { return g_byShortcode.value(s, nullptr); }
const EmojiEntry *findByShortForm(const QString &s) { return g_byShortForm.value(s, nullptr); }
QVector<const EmojiEntry*> inCategory(Category c)
{
    QVector<const EmojiEntry*> out;
    for (const auto &e : g_entries)
        if (e.category == c) out.append(&e);
    return out;
}

QVector<const EmojiEntry*> search(const QString &query)
{
    QString q = query.trimmed().toLower();
    if (q.isEmpty()) return {};

    QVector<QPair<int, const EmojiEntry*>> ranked;
    for (const auto &e : g_entries) {
        int score = 0;
        if (e.name.contains(q)) score += (e.name.startsWith(q) ? 10 : 5);
        for (const QString &k : e.keywords)
            if (k.contains(q)) score += (k == q ? 8 : 2);
        for (const QString &sc : e.shortcodes)
            if (sc.contains(q)) score += (sc == ":" + q + ":" ? 15 : 3);
        if (score > 0) ranked.append({score, &e});
    }

    std::sort(ranked.begin(), ranked.end(),
              [](const auto &a, const auto &b) { return a.first > b.first; });

    QVector<const EmojiEntry*> out;
    out.reserve(ranked.size());
    for (const auto &p : ranked) out.append(p.second);
    return out;
}
bool isEmojiCluster(const QString &cluster)
{
    // Heuristic: any codepoint in a known emoji range makes this an emoji cluster.
    for (int i = 0; i < cluster.size(); ) {
        uint cp = cluster[i].unicode();
        if (QChar::isHighSurrogate(cp) && i + 1 < cluster.size()) {
            cp = QChar::surrogateToUcs4(cluster[i], cluster[i + 1]);
            i += 2;
        } else ++i;
        if ((cp >= 0x1F300 && cp <= 0x1FAFF) ||   // Misc + Supplemental Symbols
            (cp >= 0x2600  && cp <= 0x27BF)  ||   // Misc Symbols + Dingbats
            (cp >= 0x1F1E6 && cp <= 0x1F1FF) ||   // Regional Indicators (flag halves)
            cp == 0x00A9 || cp == 0x00AE)         // © ®
            return true;
    }
    return false;
}

QPixmap pixmapFor(const QString &codepoints, int sizePx)
{
    QString key = codepoints + QLatin1Char(':') + QString::number(sizePx);
    auto it = g_pixmapCache.constFind(key);
    if (it != g_pixmapCache.constEnd())
        return it.value();

    // Build the hex filename used by Twemoji: codepoints joined by '-', lowercased,
    // U+FE0F (variation selector) stripped because Twemoji filenames omit it.
    QString hex;
    for (int i = 0; i < codepoints.size(); ) {
        uint cp = codepoints[i].unicode();
        if (QChar::isHighSurrogate(cp) && i + 1 < codepoints.size()) {
            cp = QChar::surrogateToUcs4(codepoints[i], codepoints[i+1]);
            i += 2;
        } else {
            ++i;
        }
        if (cp == 0xFE0F) continue;  // variation selector — omitted in filenames
        if (!hex.isEmpty()) hex += '-';
        hex += QString::number(cp, 16);
    }

    QString path = QStringLiteral(":/twemoji/72x72/%1.png").arg(hex);
    QPixmap pm(path);
    if (!pm.isNull() && pm.height() != sizePx)
        pm = pm.scaled(sizePx, sizePx, Qt::KeepAspectRatio, Qt::SmoothTransformation);

    g_pixmapCache.insert(key, pm);
    return pm;
}
QVector<const EmojiEntry*> recent()
{
    QVector<const EmojiEntry*> out;
    for (const QString &cp : g_recentCodepoints) {
        for (const auto &e : g_entries)
            if (e.codepoints == cp) { out.append(&e); break; }
    }
    return out;
}

void pushRecent(const EmojiEntry *e)
{
    if (!e) return;
    g_recentCodepoints.removeAll(e->codepoints);
    g_recentCodepoints.prepend(e->codepoints);
    while (g_recentCodepoints.size() > kRecentMax)
        g_recentCodepoints.removeLast();
    QSettings().setValue("emoji/recent", g_recentCodepoints);
}

} // namespace EmojiData
