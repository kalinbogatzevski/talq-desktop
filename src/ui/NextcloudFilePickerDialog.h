#pragma once

#include <QDialog>
#include <QString>
#include <QVector>
#include "core/NcFileEntry.h"

class ApiClient;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QHBoxLayout;

/**
 * Modal file picker that lists the user's Nextcloud Files via WebDAV and
 * returns the selected file path on accept. Folder navigation is in-place:
 * double-click a folder to enter it; double-click a file (or press Share)
 * to accept.
 */
class NextcloudFilePickerDialog : public QDialog
{
    Q_OBJECT
public:
    explicit NextcloudFilePickerDialog(ApiClient *api, QWidget *parent = nullptr);

    // After exec() returns Accepted, this holds the picked file's
    // absolute path relative to the user's NC root (e.g. "/Documents/foo.pdf").
    QString pickedPath() const { return m_pickedPath; }

private slots:
    void onItemActivated(QListWidgetItem *item);
    void onCurrentChanged(QListWidgetItem *cur, QListWidgetItem *prev);
    void onShareClicked();

private:
    void navigateTo(const QString &path);
    void rebuildBreadcrumb();
    void populate(const QVector<NcFileEntry> &entries);
    static QString formatSize(qint64 bytes);

    ApiClient    *m_api = nullptr;
    QLabel       *m_breadcrumb = nullptr;
    QHBoxLayout  *m_crumbLayout = nullptr;
    QListWidget  *m_list = nullptr;
    QPushButton  *m_shareBtn = nullptr;
    QPushButton  *m_cancelBtn = nullptr;
    QLabel       *m_status = nullptr;

    QString       m_currentPath = QStringLiteral("/");
    QString       m_pickedPath;
};
