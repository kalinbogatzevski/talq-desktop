#pragma once

#include <QImage>
#include <QWidget>
#include <QPointer>

class QGraphicsScene;
class QGraphicsView;
class QGraphicsPixmapItem;
class QLabel;
class ApiClient;
class QKeyEvent;
class QMouseEvent;
class QCloseEvent;

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
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void applyPixmap(const QImage &img);
    void fitToWindow();
    void actualSize();
    void zoomByStep(bool zoomIn);
    void copyImage();
    void saveAs();

    ApiClient *m_api;
    QGraphicsScene *m_scene;
    QGraphicsView *m_view;
    QGraphicsPixmapItem *m_item = nullptr;
    QLabel *m_titleBar;
    QImage m_currentImage;
    int m_currentFileId = 0;
    bool m_at100 = false;
};
