#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include "librarymanager.h"

#include <QGraphicsPixmapItem>
#include <QFutureWatcher>
#include <QGraphicsBlurEffect>
#include <QGraphicsScene>
#include <QImage>
#include <QList>
#include <QMainWindow>
#include <QUuid>
#include <QString>
#include <QVector>
#include <QTimer>
#include <functional>

#include "developtypes.h"
#include "developadjustmentengine.h"

QT_BEGIN_NAMESPACE
namespace Ui {
class MainWindow;
}
QT_END_NAMESPACE

class LibraryGridView;
class HistogramWidget;
class JobManager;
class JobsWindow;
class QAction;

struct DevelopImageLoadResult
{
    int requestId = 0;
    qint64 assetId = -1;
    QString filePath;
    QImage image;
    DevelopMetadata metadata;
    QString errorMessage;
};

struct HistogramTaskResult
{
    int requestId = 0;
    HistogramData histogram;
};

struct AdjustmentTaskResult
{
    int requestId = 0;
    DevelopAdjustmentRenderResult renderResult;
};

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
    void on_actionExport_triggered();
    void openAssetInDevelop(qint64 assetId, const QString &filePath);
    void handleSelectionChanged(const QList<qint64> &selection);

    void toggleJobsWindow();
    void handleImportProgress(int imported, int total);
    void handleImportCompleted();
    void handleLibraryError(const QString &message);
    void handleJobListChanged();

private:
    Ui::MainWindow *ui;
    QGraphicsScene *m_developScene = nullptr;
    QGraphicsPixmapItem *m_developPixmapItem = nullptr;
    QGraphicsBlurEffect *m_developBlurEffect = nullptr;
    LibraryGridView *m_libraryGridView = nullptr;
    HistogramWidget *m_histogramWidget = nullptr;

    QString currentLibraryPath;
    QVector<LibraryAsset> m_assets;
    qint64 m_currentDevelopAssetId = -1;
    double m_developZoom = 1.0;
    bool m_developFitMode = true;
    int m_pendingDevelopRequestId = 0;
    QString m_pendingDevelopFilePath;
    QFutureWatcher<DevelopImageLoadResult> *m_imageLoadWatcher = nullptr;
    QFutureWatcher<HistogramTaskResult> *m_histogramWatcher = nullptr;
    int m_activeHistogramRequestId = 0;

    void clearLibrary();
    void clearDevelopView();
    void ensureDevelopViewInitialized();
    void initializeDevelopHistogram();
    void bindLibrarySignals();
    void refreshLibraryView();
    void updateThumbnailPreview(qint64 assetId, const QString &previewPath);
    QString assetPreviewPath(const LibraryAsset &asset) const;
    QString assetOriginalPath(const LibraryAsset &asset) const;
    void showStatusMessage(const QString &message, int timeoutMs = 3000);
    void updateDevelopFilmstrip();
    void populateDevelopMetadata(const QImage &image, const QString &filePath, const DevelopMetadata &metadata);
    const LibraryAsset *assetById(qint64 assetId) const;
    void applyDevelopZoomPreset(const QString &preset);
    void fitDevelopViewToImage();
    void showDevelopPreview(const QPixmap &pixmap);
    void showDevelopLoadingState(const QString &message);
    void handleDevelopImageLoaded();
    void updateHistogram(const HistogramData &histogram);
    void handleHistogramReady();
    void requestHistogramComputation(const QImage &image, int requestId);
    void setupJobSystem();
    void updateJobsActionBadge();
    void schedulePreviewRegeneration(qint64 assetId, const QImage &sourceImage);
    void resetHistogram();
    void selectFilmstripItem(qint64 assetId);
    void setupAdjustmentEngine();
    void initializeAdjustmentControls();
    void bindAdjustmentControl(QWidget *slider,
                               QWidget *spinBox,
                               const std::function<void(double)> &setter,
                               const std::function<double()> &getter,
                               const std::function<double(int)> &sliderToValue,
                               const std::function<int(double)> &valueToSlider);
    void handleAdjustmentChanged();
    void requestAdjustmentRender(bool forceImmediate = false);
    void handleAdjustmentRenderResult(const DevelopAdjustmentRenderResult &result);
    void startPreviewRender();
    void startFullRender();
    bool shouldUsePreviewRender() const;
    void ensurePreviewImageReady();
    bool adjustmentsAreIdentity(const DevelopAdjustments &adjustments) const;
    void syncAdjustmentControls(const DevelopAdjustments &adjustments);
    void applyDevelopImage(const QImage &image,
                           bool updateHistogram = true,
                           bool isPreview = false,
                           double displayScale = 1.0);
    void persistCurrentAdjustments();
    void scheduleAdjustmentPersist();
    void loadAdjustmentsForAsset(qint64 assetId);
    void resetAdjustmentsToDefault();

    LibraryManager *m_libraryManager = nullptr;
    JobManager *m_jobManager = nullptr;
    JobsWindow *m_jobsWindow = nullptr;
    QAction *m_toggleJobsAction = nullptr;
    void openOrCreateDefaultLibrary();

    QUuid m_activeImportJobId;
    QUuid m_activeExportJobId;
    int m_activeImportTotal = 0;
    bool m_importJobActive = false;
    bool m_exportInProgress = false;
    QString m_lastExportDirectory;

    QUuid m_activeDevelopJobId;
    QUuid m_activeHistogramJobId;

    DevelopAdjustments m_currentAdjustments;
    QImage m_currentDevelopOriginalImage;
    QImage m_currentDevelopAdjustedImage;
    bool m_currentDevelopAdjustedValid = false;
    DevelopAdjustmentEngine *m_adjustmentEngine = nullptr;
    QTimer m_fullRenderTimer;
    QImage m_currentDevelopPreviewImage;
    double m_currentDevelopPreviewScale = 1.0;
    int m_nextAdjustmentRequestId = 0;
    int m_latestPreviewRequestId = 0;
    int m_latestFullRequestId = 0;
    bool m_previewRenderEnabled = false;
    bool m_savingAdjustmentsPending = false;
    QTimer m_adjustmentPersistTimer;

protected:
    void resizeEvent(QResizeEvent *event) override;
};

#endif // MAINWINDOW_H
