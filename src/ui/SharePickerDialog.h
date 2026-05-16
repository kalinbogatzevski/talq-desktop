#pragma once

#include <QDialog>
#include <QListWidget>
#include <QVBoxLayout>
#include <QPushButton>
#include <QLabel>
#include <QTabWidget>

struct ShareTarget {
    enum Type { Monitor, Window };
    Type type;
    int monitorIndex = 0;          // for Monitor type
    quintptr windowHandle = 0;     // HWND for Window type
    QString name;
};

class SharePickerDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SharePickerDialog(QWidget *parent = nullptr);

    ShareTarget selectedTarget() const { return m_selected; }

private:
    void populateMonitors();
    void populateWindows();

    QTabWidget *m_tabs = nullptr;
    QListWidget *m_monitorList = nullptr;
    QListWidget *m_windowList = nullptr;
    QPushButton *m_shareBtn = nullptr;
    ShareTarget m_selected;

    QList<ShareTarget> m_monitors;
    QList<ShareTarget> m_windows;
};
