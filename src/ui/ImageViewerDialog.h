#pragma once

#include <QImage>
#include <QWidget>
#include <QPointer>

class QGraphicsScene;
class QGraphicsView;
class QGraphicsPixmapItem;
class QLabel;
class QPushButton;
class QMenu;
class ApiClient;
class ImageClipboard;
class QKeyEvent;
class QMouseEvent;
class QCloseEvent;
class QContextMenuEvent;
class QResizeEvent;
class QTimer;

class ImageViewerDialog : public QWidget
{
    Q_OBJECT
public:
    // `clipboard` is the shared copy route (ImageClipboard.h); null leaves Copy
    // with only what is on screen.
    ImageViewerDialog(ApiClient *api, ImageClipboard *clipboard, QWidget *parent = nullptr);
    void setImage(int fileId, const QString &fileName, const QString &mime,
                  const QImage &placeholder);

signals:
    // "Save as..." wants the ORIGINAL file, which this dialog has never held --
    // what it displays is /core/preview's re-render, capped to the screen. The
    // owner routes this to the model, which is the one place that knows how to
    // fetch an attachment's real bytes.
    void saveOriginalRequested(int fileId, const QString &fileName,
                               const QString &destPath);
    // A Copy finished after the viewer was closed or moved on to another
    // picture, so its own toast would land where nobody is looking. The owner
    // shows `text` instead.
    void copyAnnouncement(const QString &text);

protected:
    void keyPressEvent(QKeyEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void applyPixmap(const QImage &img);
    void fitToWindow();
    void actualSize();
    void zoomByStep(bool zoomIn);
    void copyImage();
    void saveAs();
    void showToast(const QString &text);
    void positionToast();

    ApiClient *m_api;
    ImageClipboard *m_clipboard;
    QGraphicsScene *m_scene;
    QGraphicsView *m_view;
    QGraphicsPixmapItem *m_item = nullptr;
    QLabel *m_titleBar;
    QPushButton *m_menuBtn = nullptr;
    QImage m_currentImage;
    QString m_currentFileName;   // source of truth for the filename; title bar may show a toast suffix
    QString m_currentMime;
    int m_currentFileId = 0;
    bool m_at100 = false;
    QLabel *m_toast = nullptr;   // centered confirmation pill, hidden by default
    QTimer *m_toastHideTimer = nullptr;
};
