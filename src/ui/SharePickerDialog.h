#pragma once

#include <QDialog>
#include <QListWidget>
#include <QVBoxLayout>
#include <QPushButton>
#include <QLabel>
#include <QTabWidget>
#include <QCheckBox>
#include <QTimer>

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

    // Ticked = this share may use the full quality level shown in the picker at
    // 30 fps. Unticked (default) = clamped to 1080p at 15 fps. Per-share only:
    // it never rewrites Video/screenShareQuality.
    bool presentationMode() const { return m_presentation && m_presentation->isChecked(); }

private:
    void populateMonitors();
    void populateWindows();
    void refreshThumbnails();   // live preview, ~1.5s while the picker is open

    QTabWidget *m_tabs = nullptr;
    QCheckBox *m_presentation = nullptr;
    QListWidget *m_monitorList = nullptr;
    QListWidget *m_windowList = nullptr;
    QPushButton *m_shareBtn = nullptr;
    QTimer *m_thumbTimer = nullptr;
    ShareTarget m_selected;

    QList<ShareTarget> m_monitors;
    QList<ShareTarget> m_windows;
};
