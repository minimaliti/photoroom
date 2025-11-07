#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include "librarymanager.h"

#include <QList>
#include <QMainWindow>
#include <QVector>

QT_BEGIN_NAMESPACE
namespace Ui {
class MainWindow;
}
QT_END_NAMESPACE

class LibraryGridView;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

private slots:
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
    void openAssetInDevelop(qint64 assetId, const QString &filePath);
    void handleSelectionChanged(const QList<qint64> &selection);

private:
    Ui::MainWindow *ui;
    LibraryGridView *m_libraryGridView = nullptr;

    QString currentLibraryPath;
    QVector<LibraryAsset> m_assets;

    void clearLibrary();
    void bindLibrarySignals();
    void refreshLibraryView();
    void updateThumbnailPreview(qint64 assetId, const QString &previewPath);
    QString assetPreviewPath(const LibraryAsset &asset) const;
    QString assetOriginalPath(const LibraryAsset &asset) const;
    void showStatusMessage(const QString &message, int timeoutMs = 3000);

    LibraryManager *m_libraryManager = nullptr;
};

#endif // MAINWINDOW_H
