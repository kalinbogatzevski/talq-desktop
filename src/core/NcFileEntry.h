#pragma once

#include <QString>
#include <QDateTime>

struct NcFileEntry {
    QString   name;
    QString   path;           // absolute, relative to user root ("/Documents/report.pdf")
    bool      isDir = false;
    qint64    size = 0;
    QDateTime lastModified;
    QString   mimeType;
    qint64    fileId = 0;     // NC fileids routinely exceed 2^31 on busy instances
};
