#include "NextcloudFilePickerDialog.h"

#include "core/ApiClient.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QLocale>
#include <QMouseEvent>
#include <algorithm>

namespace {

constexpr int kIconCol = 0;
constexpr int kPathRole = Qt::UserRole + 1;
constexpr int kIsDirRole = Qt::UserRole + 2;

QString iconForEntry(const NcFileEntry &e)
{
    if (e.isDir) return QStringLiteral("\U0001F4C1"); // folder
    const QString &m = e.mimeType;
    if (m.startsWith(QStringLiteral("image/")))  return QStringLiteral("\U0001F5BC");  // framed picture
    if (m.startsWith(QStringLiteral("video/")))  return QStringLiteral("\U0001F3AC");  // clapperboard
    if (m.startsWith(QStringLiteral("audio/")))  return QStringLiteral("\U0001F3B5");  // music
    if (m == QStringLiteral("application/pdf"))  return QStringLiteral("\U0001F4D5");
    return QStringLiteral("\U0001F4C4"); // page facing up
}

} // namespace

NextcloudFilePickerDialog::NextcloudFilePickerDialog(ApiClient *api, QWidget *parent)
    : QDialog(parent), m_api(api)
{
    setWindowTitle(tr("Share from Nextcloud"));
    resize(640, 480);
    // Chrome inherited from the app-wide AppStyle sheet (theme-driven).

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(16, 16, 16, 16);
    outer->setSpacing(12);

    auto *crumbRow = new QWidget(this);
    m_crumbLayout = new QHBoxLayout(crumbRow);
    m_crumbLayout->setContentsMargins(0, 0, 0, 0);
    m_crumbLayout->setSpacing(4);
    m_crumbLayout->addStretch();
    outer->addWidget(crumbRow);

    m_list = new QListWidget(this);
    m_list->setUniformItemSizes(true);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    outer->addWidget(m_list, 1);

    m_status = new QLabel(tr("Loading\u2026"), this);
    m_status->setProperty("role", "secondary");
    outer->addWidget(m_status);

    auto *btnRow = new QHBoxLayout();
    btnRow->setContentsMargins(0, 4, 0, 0);
    btnRow->setSpacing(8);
    btnRow->addStretch();
    m_cancelBtn = new QPushButton(tr("Cancel"), this);
    m_cancelBtn->setProperty("variant", "default");
    m_shareBtn  = new QPushButton(tr("Share"), this);
    m_shareBtn->setProperty("variant", "primary");
    m_shareBtn->setEnabled(false);
    m_shareBtn->setDefault(true);
    btnRow->addWidget(m_cancelBtn);
    btnRow->addWidget(m_shareBtn);
    outer->addLayout(btnRow);

    connect(m_list, &QListWidget::itemActivated,
            this, &NextcloudFilePickerDialog::onItemActivated);
    connect(m_list, &QListWidget::currentItemChanged,
            this, &NextcloudFilePickerDialog::onCurrentChanged);
    connect(m_shareBtn,  &QPushButton::clicked, this, &NextcloudFilePickerDialog::onShareClicked);
    connect(m_cancelBtn, &QPushButton::clicked, this, &QDialog::reject);

    navigateTo(QStringLiteral("/"));
}

void NextcloudFilePickerDialog::navigateTo(const QString &path)
{
    m_currentPath = path.isEmpty() ? QStringLiteral("/") : path;
    m_list->clear();
    m_shareBtn->setEnabled(false);
    m_status->setText(tr("Loading\u2026"));
    rebuildBreadcrumb();

    m_api->listNextcloudFolder(m_currentPath, this,
        [this](bool ok, const QVector<NcFileEntry> &entries,
               int status, const QString &error) {
            if (ok) { populate(entries); return; }
            QString msg;
            if (status == 0)          msg = tr("Couldn\u2019t reach Nextcloud \u2014 check your connection.");
            else if (status == 401)   msg = tr("Your Nextcloud session has expired. Please sign in again.");
            else if (status == 403)   msg = tr("This app password doesn\u2019t have permission to list files.");
            else if (status == 404)   msg = tr("That folder no longer exists.");
            else if (status == 503)   msg = tr("Nextcloud is in maintenance mode.");
            else if (!error.isEmpty()) msg = tr("Couldn\u2019t load folder: %1").arg(error);
            else                       msg = tr("Couldn\u2019t load folder (HTTP %1).").arg(status);
            m_status->setText(msg);
        });
}

void NextcloudFilePickerDialog::rebuildBreadcrumb()
{
    // Remove prior crumb widgets (keep the trailing stretch).
    while (m_crumbLayout->count() > 1) {
        QLayoutItem *it = m_crumbLayout->takeAt(0);
        if (it->widget()) it->widget()->deleteLater();
        delete it;
    }
    auto addCrumb = [&](const QString &label, const QString &target, bool active) {
        auto *btn = new QPushButton(label, this);
        btn->setFlat(true);
        btn->setProperty("variant", "ghost");
        btn->setCursor(active ? Qt::ArrowCursor : Qt::PointingHandCursor);
        btn->setEnabled(!active);
        if (!active) {
            connect(btn, &QPushButton::clicked, this, [this, target]() { navigateTo(target); });
        }
        m_crumbLayout->insertWidget(m_crumbLayout->count() - 1, btn);
    };

    addCrumb(QStringLiteral("\U0001F3E0 Home"), QStringLiteral("/"),
             m_currentPath == QStringLiteral("/"));
    QStringList segments = m_currentPath.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    QString acc;
    for (int i = 0; i < segments.size(); ++i) {
        acc += QStringLiteral("/") + segments[i];
        auto *sep = new QLabel(QStringLiteral("\u203A"), this);
        sep->setProperty("role", "muted");
        m_crumbLayout->insertWidget(m_crumbLayout->count() - 1, sep);
        addCrumb(segments[i], acc, i == segments.size() - 1);
    }
}

void NextcloudFilePickerDialog::populate(const QVector<NcFileEntry> &entries)
{
    QVector<NcFileEntry> sorted = entries;
    std::sort(sorted.begin(), sorted.end(), [](const NcFileEntry &a, const NcFileEntry &b) {
        if (a.isDir != b.isDir) return a.isDir;      // folders first
        return a.name.localeAwareCompare(b.name) < 0;
    });

    for (const NcFileEntry &e : sorted) {
        QString right;
        if (e.isDir) {
            right = e.lastModified.isValid()
                ? e.lastModified.toLocalTime().toString(QStringLiteral("yyyy-MM-dd"))
                : QString();
        } else {
            right = formatSize(e.size);
            if (e.lastModified.isValid())
                right += QStringLiteral("  \u00b7  ")
                       + e.lastModified.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm"));
        }
        QString line = QStringLiteral("%1  %2").arg(iconForEntry(e), e.name);
        if (!right.isEmpty())
            line += QStringLiteral("   \u2014  ") + right;
        auto *item = new QListWidgetItem(line);
        item->setData(kPathRole,  e.path);
        item->setData(kIsDirRole, e.isDir);
        m_list->addItem(item);
    }
    m_status->setText(sorted.isEmpty()
        ? tr("Empty folder.")
        : tr("%n item(s)", nullptr, sorted.size()));
}

void NextcloudFilePickerDialog::onCurrentChanged(QListWidgetItem *cur, QListWidgetItem *)
{
    if (!cur) { m_shareBtn->setEnabled(false); return; }
    m_shareBtn->setEnabled(!cur->data(kIsDirRole).toBool());
}

void NextcloudFilePickerDialog::onItemActivated(QListWidgetItem *item)
{
    if (!item) return;
    if (item->data(kIsDirRole).toBool()) {
        navigateTo(item->data(kPathRole).toString());
    } else {
        m_pickedPath = item->data(kPathRole).toString();
        accept();
    }
}

void NextcloudFilePickerDialog::onShareClicked()
{
    QListWidgetItem *cur = m_list->currentItem();
    if (!cur || cur->data(kIsDirRole).toBool()) return;
    m_pickedPath = cur->data(kPathRole).toString();
    accept();
}

QString NextcloudFilePickerDialog::formatSize(qint64 bytes)
{
    static const char *units[] = { "B", "KB", "MB", "GB", "TB" };
    double v = bytes;
    int u = 0;
    while (v >= 1024.0 && u < 4) { v /= 1024.0; ++u; }
    return QStringLiteral("%1 %2").arg(v, 0, 'f', u == 0 ? 0 : 1).arg(QString::fromLatin1(units[u]));
}
