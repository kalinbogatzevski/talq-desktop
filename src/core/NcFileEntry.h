#pragma once

#include <QString>
#include <QDateTime>

struct NcFileEntry {
    QString   name;           // display name ("report.pdf")
    QString   path;           // absolute, relative to user root ("/Documents/report.pdf")
    bool      isDir = false;
    qint64    size = 0;
    QDateTime lastModified;
    QString   mimeType;
    int       fileId = 0;
};
