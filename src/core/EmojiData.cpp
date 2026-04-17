#include "EmojiData.h"

#include <QSettings>

namespace EmojiData {

namespace {
QVector<EmojiEntry> g_entries;
QHash<QString, const EmojiEntry*> g_byShortcode;
QHash<QString, const EmojiEntry*> g_byShortForm;
QHash<QString, QPixmap> g_pixmapCache;
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
}

const QVector<EmojiEntry> &allEntries() { return g_entries; }
const EmojiEntry *findByShortcode(const QString &s) { return g_byShortcode.value(s, nullptr); }
const EmojiEntry *findByShortForm(const QString &s) { return g_byShortForm.value(s, nullptr); }
QVector<const EmojiEntry*> search(const QString &) { return {}; }
QVector<const EmojiEntry*> inCategory(Category) { return {}; }
QPixmap pixmapFor(const QString &, int) { return QPixmap(); }
QVector<const EmojiEntry*> recent() { return {}; }
void pushRecent(const EmojiEntry *) {}

} // namespace EmojiData
