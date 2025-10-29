#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include "imagelabel.h"
#include "imageprocessor.h"
#include "imageadjustments.h"
#include <QMainWindow>
#include <QPixmap>
#include <qstackedwidget.h>
#include <QGridLayout>
#include <QFileInfoList>
#include <QFuture>
#include <QFutureWatcher>
#include <QMouseEvent>
#include "librarymanager.h"

QT_BEGIN_NAMESPACE
namespace Ui {
class MainWindow;
}
QT_END_NAMESPACE

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

    static QPixmap loadPixmapFromFile(const QString &filePath, bool isThumbnail, LibraryManager *libraryManager);

public slots:
    void on_actionExit_triggered();
    void on_actionNew_Library_triggered();
    void on_actionOpen_Library_triggered();
    void on_actionClear_recents_triggered();
    void on_actionUndo_triggered();
    void on_actionRedo_triggered();
    void on_actionCut_triggered();
    void on_actionCopy_triggered();
    void on_actionPaste_triggered();
    void on_actionSelect_All_triggered();
    void on_actionSelect_None_triggered();
    void on_actionInverse_Selection_triggered();
    void on_actionPreferences_triggered();
    void on_actionImport_triggered();
    void on_actionRefresh_triggered();
    void on_actionSettings_triggered();
    void on_actionBackup_triggered();

    void onThumbnailClicked(QMouseEvent *event);
    void onThumbnailDoubleClicked();
    void onThumbnailReady(const QString &filePath, const QPixmap &pixmap);
    void applyAdjustments();

    void showLibraryPage();
    void showDevelopPage();
    void onResizeTimerTimeout();

signals:
    void thumbnailLoaded(const QString &filePath, const QPixmap &pixmap);

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    Ui::MainWindow *ui;
    QStackedWidget *stackedWidget;
    QWidget *libraryPage;
    QWidget *developPage;

    QFileInfoList imageFiles;
    QString currentLibraryPath;

    QList<ImageLabel*> thumbnailWidgets;
    QList<ImageLabel*> m_imageStripThumbnails;
    QTimer *resizeTimer;

    ImageLabel* m_lastSelected = nullptr;

    QPixmap m_currentPixmap;
    QPixmap m_originalPixmap;
    QString m_currentDevelopImagePath;
    ImageProcessor* m_imageProcessor;
    ImageAdjustments m_adjustments;
    QTimer* m_adjustmentTimer;

    // For asynchronous loading
    QFutureWatcher<void> m_imageLoadingWatcher;

private slots:
    void delayedApplyAdjustments();
    void onThreadCountSliderChanged(int value);

    void populateLibrary(const QString &folderPath);
    void updateImageGrid();
    void updateImageStrip();
    void clearLibrary();

    void updateDevelopImage();
    void onImageStripThumbnailClicked();

private slots:
    void onLibraryError(const QString &message);

private:
    LibraryManager *m_libraryManager;

    QStringList findImageFilesInDirectory(const QString &folderPath);
};

#endif // MAINWINDOW_H
