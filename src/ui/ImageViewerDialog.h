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
    // A person's or a group's PICTURE, not a chat file (the avatar viewer). `path` is the
    // server path of the full-size picture (core/AvatarPaths.h builds it); the viewer
    // fetches it itself, with a timeout, and an answer for a picture the user has since
    // replaced is ignored. There is no file behind it: Copy and Save use what is on
    // screen, which is all there is. A freshly opened viewer is sized for the picture
    // (an avatar is a few hundred pixels; the photo-sized window would blow it up), and
    // that size is never saved over the geometry the user chose for photos.
    void showPicture(const QString &title, const QString &path);

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
    void showEvent(QShowEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void restoreSavedGeometry();
    void rememberPhotoGeometry();                   // the restored size, never the maximized one
    void sizeForPicture(const QSize &imageSize);    // once, for a window that was not open yet
    void resizeForPicture(const QSize &imageSize);  // the size itself, kept inside the screen
    void keepFrameOnScreen();                       // the title bar with the close button too
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
    QPushButton *m_closeBtn = nullptr;
    QImage m_currentImage;
    QString m_currentFileName;   // source of truth for the filename; title bar may show a toast suffix
    QString m_currentMime;
    int m_currentFileId = 0;
    // Picture mode (showPicture). m_pictureSeq numbers the pictures asked for so a slow
    // answer for an earlier one is dropped; m_pictureFresh = the window was not on screen
    // when the picture was asked for, so it may still be sized for it.
    int m_pictureSeq = 0;
    bool m_showingPicture = false;
    bool m_pictureFresh = false;
    bool m_at100 = false;
    QLabel *m_toast = nullptr;   // centered confirmation pill, hidden by default
    QTimer *m_toastHideTimer = nullptr;
};
