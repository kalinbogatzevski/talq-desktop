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
    explicit ImageViewerDialog(ApiClient *api, QWidget *parent = nullptr);
    void setImage(int fileId, const QString &fileName, const QImage &placeholder);

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
    QGraphicsScene *m_scene;
    QGraphicsView *m_view;
    QGraphicsPixmapItem *m_item = nullptr;
    QLabel *m_titleBar;
    QPushButton *m_menuBtn = nullptr;
    QImage m_currentImage;
    QString m_currentFileName;   // source of truth for the filename; title bar may show a toast suffix
    int m_currentFileId = 0;
    bool m_at100 = false;
    QLabel *m_toast = nullptr;   // centered confirmation pill, hidden by default
    QTimer *m_toastHideTimer = nullptr;
};
