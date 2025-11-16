#include "mainwindow.h"
#include "./ui_mainwindow.h"
#include "preferencesdialog.h"
#include "librarygridview.h"
#include "libraryfilterpane.h"
#include "histogramwidget.h"
#include "jobmanager.h"
#include "jobswindow.h"
#include "exportdialog.h"
#include "importpreviewdialog.h"
#include "imageloader.h"

#include <QAction>
#include <QAbstractItemView>
#include <QApplication>
#include <QColor>
#include <QComboBox>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QDirIterator>
#include <QFileDialog>
#include <QFileInfo>
#include <QGraphicsBlurEffect>
#include <QGraphicsView>
#include <QImage>
#include <QImageReader>
#include <QImageWriter>
#include <QLabel>
#include <QIcon>
#include <QLocale>
#include <QListWidget>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QStringList>
#include <QThreadPool>
#include <QThread>
#include <QTransform>
#include <QColorSpace>
#include <QtConcurrent/QtConcurrentRun>
#include <QtGlobal>
#include <QFuture>
#include <QFutureWatcher>
#include <algorithm>
#include <array>
#include <cmath>
#include <utility>
#include <memory>
#include <QFile>
#include <QPixmap>
#include <QPointer>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSharedPointer>
#include <QStandardPaths>
#include <QToolBar>
#include <QVBoxLayout>
#include <QWidget>
#include <QOpenGLWidget>
#include <QSlider>
#include <QDoubleSpinBox>
#include <QSet>
#include <QSurfaceFormat>

#include "imageloader.h"

namespace {

constexpr int kHistogramBins = 256;
constexpr int kHistogramTargetSampleCount = 750000;
constexpr int kPreviewMaxDimension = 960;

struct ExportTaskReport
{
    bool success = true;
    QString errorMessage;
    QString destinationDir;
    QStringList exportedFiles;
};

struct ExportItem
{
    qint64 assetId = -1;
    QString sourcePath;
    DevelopAdjustments adjustments;
    bool identity = true;
};

static QString exportExtensionForFormat(const QString &format)
{
    const QString lowered = format.toLower();
    if (lowered == QStringLiteral("jpeg")) {
        return QStringLiteral("jpg");
    }
    if (lowered == QStringLiteral("tiff")) {
        return QStringLiteral("tif");
    }
    return lowered;
}

static QString sanitizeFileName(const QString &name)
{
    QString sanitized = name.trimmed();
    static const QRegularExpression invalidChars(QStringLiteral(R"([\/\\\:\*\?"<>\|])"));
    sanitized.replace(invalidChars, QStringLiteral("_"));
    if (sanitized.isEmpty()) {
        sanitized = QStringLiteral("Exported");
    }
    return sanitized;
}

static QString generateExportBaseName(const QFileInfo &sourceInfo,
                                      int sequenceIndex,
                                      const QString &namingMode,
                                      const QString &customPattern,
                                      int sequenceStart,
                                      int sequencePadding,
                                      const QString &customSuffix)
{
    const QString originalName = sourceInfo.completeBaseName();
    if (namingMode == QStringLiteral("original-with-suffix")) {
        if (customSuffix.isEmpty()) {
            return originalName;
        }
        return originalName + customSuffix;
    }

    if (namingMode == QStringLiteral("custom-pattern")) {
        QString pattern = customPattern;
        if (pattern.isEmpty()) {
            pattern = QStringLiteral("Export_{index}");
        }

        const int value = sequenceStart + sequenceIndex;
        const int padding = qMax(1, sequencePadding);
        QString numberString = QString::number(value);
        if (numberString.size() < padding) {
            numberString = QString(padding - numberString.size(), QLatin1Char('0')) + numberString;
        }

        QString result = pattern;
        if (result.contains(QStringLiteral("{index}"))) {
            result.replace(QStringLiteral("{index}"), numberString);
        } else {
            result.append(numberString);
        }

        if (!customSuffix.isEmpty()) {
            result.append(customSuffix);
        }
        return result;
    }

    return originalName;
}

static QString ensureUniqueFileName(const QString &baseName,
                                    const QString &extension,
                                    QSet<QString> &usedBaseNames,
                                    const QDir &destinationDir)
{
    QString candidateBase = baseName;
    int attempt = 1;

    while (usedBaseNames.contains(candidateBase) ||
           destinationDir.exists(candidateBase + QLatin1Char('.') + extension)) {
        candidateBase = baseName + QStringLiteral("_%1").arg(attempt++);
    }

    usedBaseNames.insert(candidateBase);
    return candidateBase + QLatin1Char('.') + extension;
}

struct HistogramChunk
{
    std::array<int, kHistogramBins> red{};
    std::array<int, kHistogramBins> green{};
    std::array<int, kHistogramBins> blue{};
    std::array<int, kHistogramBins> luminance{};
    int totalSamples = 0;
};

static HistogramChunk computeHistogramChunk(const QImage &image,
                                            int startY,
                                            int endY,
                                            int strideStep,
                                            bool isRgb32,
                                            bool isRgb888)
{
    HistogramChunk chunk;
    const int width = image.width();

    for (int y = startY; y < endY; ++y) {
        if (strideStep > 1 && (y % strideStep) != 0) {
            continue;
        }

        const uchar *line = image.constScanLine(y);
        if (!line) {
            continue;
        }

        for (int x = 0; x < width; x += strideStep) {
            int r = 0;
            int g = 0;
            int b = 0;

            if (isRgb32) {
                const QRgb pixel = reinterpret_cast<const QRgb *>(line)[x];
                r = qRed(pixel);
                g = qGreen(pixel);
                b = qBlue(pixel);
            } else if (isRgb888) {
                const uchar *pixel = line + (x * 3);
                r = pixel[0];
                g = pixel[1];
                b = pixel[2];
            } else {
                const uchar *pixel = line + (x * 4);
                r = pixel[0];
                g = pixel[1];
                b = pixel[2];
            }

            const int luminance = qBound(0, qGray(r, g, b), 255);

            chunk.red[r]++;
            chunk.green[g]++;
            chunk.blue[b]++;
            chunk.luminance[luminance]++;
            ++chunk.totalSamples;
        }
    }

    return chunk;
}

HistogramData computeHistogram(const QImage &sourceImage)
{
    HistogramData histogram;
    histogram.red.fill(0, kHistogramBins);
    histogram.green.fill(0, kHistogramBins);
    histogram.blue.fill(0, kHistogramBins);
    histogram.luminance.fill(0, kHistogramBins);
    histogram.maxValue = 0;
    histogram.totalSamples = 0;

    if (sourceImage.isNull()) {
        return histogram;
    }

    QImage image = sourceImage;
    switch (image.format()) {
    case QImage::Format_RGB32:
    case QImage::Format_ARGB32:
    case QImage::Format_ARGB32_Premultiplied:
    case QImage::Format_RGB888:
    case QImage::Format_RGBA8888:
    case QImage::Format_RGBX8888:
        break;
    default:
        image = image.convertToFormat(QImage::Format_RGBA8888);
        break;
    }

    const int width = image.width();
    const int height = image.height();
    const int totalPixels = width * height;

    int strideStep = 1;
    if (totalPixels > kHistogramTargetSampleCount) {
        const double factor = std::sqrt(static_cast<double>(totalPixels) /
                                        static_cast<double>(kHistogramTargetSampleCount));
        strideStep = qBound(1, static_cast<int>(factor), 16);
    }

    const bool isRgb32 = image.format() == QImage::Format_RGB32 ||
                         image.format() == QImage::Format_ARGB32 ||
                         image.format() == QImage::Format_ARGB32_Premultiplied ||
                         image.format() == QImage::Format_RGBX8888;
    const bool isRgb888 = image.format() == QImage::Format_RGB888;

    const int effectiveRows = strideStep > 1 ? (height + strideStep - 1) / strideStep : height;
    const int maxThreads = qMax(1, QThreadPool::globalInstance()->maxThreadCount());
    const int chunkCount = qMax(1, qMin(effectiveRows, maxThreads * 2));
    const int rowsPerChunk = qMax(1, (height + chunkCount - 1) / chunkCount);

    QVector<QFuture<HistogramChunk>> futures;
    futures.reserve(chunkCount);

    for (int startY = 0; startY < height; startY += rowsPerChunk) {
        const int endY = qMin(height, startY + rowsPerChunk);
        futures.append(QtConcurrent::run([image, startY, endY, strideStep, isRgb32, isRgb888]() {
            return computeHistogramChunk(image, startY, endY, strideStep, isRgb32, isRgb888);
        }));
    }

    int totalSamples = 0;
    for (QFuture<HistogramChunk> &future : futures) {
        const HistogramChunk chunk = future.result();
        totalSamples += chunk.totalSamples;
        for (int i = 0; i < kHistogramBins; ++i) {
            histogram.red[i] += chunk.red[i];
            histogram.green[i] += chunk.green[i];
            histogram.blue[i] += chunk.blue[i];
            histogram.luminance[i] += chunk.luminance[i];
        }
    }

    int maxValue = 0;
    for (int value : histogram.red) {
        maxValue = std::max(maxValue, value);
    }
    for (int value : histogram.green) {
        maxValue = std::max(maxValue, value);
    }
    for (int value : histogram.blue) {
        maxValue = std::max(maxValue, value);
    }
    for (int value : histogram.luminance) {
        maxValue = std::max(maxValue, value);
    }

    histogram.totalSamples = totalSamples;
    histogram.maxValue = maxValue;
    return histogram;
}

inline bool almostEqual(double a, double b)
{
    return std::abs(a - b) < 1e-4;
}

DevelopImageLoadResult loadDevelopImageAsync(int requestId, qint64 assetId, const QString &filePath)
{
    DevelopImageLoadResult result;
    result.requestId = requestId;
    result.assetId = assetId;
    result.filePath = filePath;

    QString loadError;
    QImage image = ImageLoader::loadImageWithRawSupport(filePath, &loadError);
    if (image.isNull()) {
        result.errorMessage = loadError.isEmpty() ? QObject::tr("Failed to load image.") : loadError;
        return result;
    }

    result.image = image;
    ImageLoader::extractMetadata(filePath, &result.metadata, nullptr);
    return result;
}

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);

    m_currentAdjustments = defaultDevelopAdjustments();

    m_libraryManager = new LibraryManager(this);
    setupAdjustmentEngine();
    setupJobSystem();
    bindLibrarySignals();

    // Create filter pane
    m_libraryFilterPane = new LibraryFilterPane(this);
    connect(m_libraryFilterPane, &LibraryFilterPane::filterChanged,
            this, [this](const FilterOptions &options) {
                refreshLibraryView(options);
            });

    // Create library grid view
    m_libraryGridView = new LibraryGridView(this);
    connect(m_libraryGridView, &LibraryGridView::assetActivated,
            this, &MainWindow::openAssetInDevelop);
    connect(m_libraryGridView, &LibraryGridView::selectionChanged,
            this, &MainWindow::handleSelectionChanged);
    connect(m_libraryGridView, &LibraryGridView::folderDropped,
            this, &MainWindow::handleFolderDropped);

    // Add filter pane and grid view to layout
    if (ui->libraryPageLayout) {
        ui->libraryPageLayout->insertWidget(0, m_libraryFilterPane);
    }
    if (ui->libraryGridLayout) {
        ui->libraryGridLayout->addWidget(m_libraryGridView);
    }

    m_imageLoadWatcher = new QFutureWatcher<DevelopImageLoadResult>(this);
    connect(m_imageLoadWatcher, &QFutureWatcher<DevelopImageLoadResult>::finished,
            this, &MainWindow::handleDevelopImageLoaded);

    m_histogramWatcher = new QFutureWatcher<HistogramTaskResult>(this);
    connect(m_histogramWatcher, &QFutureWatcher<HistogramTaskResult>::finished,
            this, &MainWindow::handleHistogramReady);

    ensureDevelopViewInitialized();

    if (ui->developVerticalSplitter) {
        ui->developVerticalSplitter->setStretchFactor(0, 1);
        ui->developVerticalSplitter->setStretchFactor(1, 0);
        ui->developVerticalSplitter->setCollapsible(0, false);
        ui->developVerticalSplitter->setCollapsible(1, false);
    }

    if (ui->developFilmstripList) {
        ui->developFilmstripList->setViewMode(QListView::IconMode);
        ui->developFilmstripList->setFlow(QListView::LeftToRight);
        ui->developFilmstripList->setMovement(QListView::Static);
        ui->developFilmstripList->setWrapping(false);
        ui->developFilmstripList->setSpacing(8);
        ui->developFilmstripList->setResizeMode(QListView::Adjust);
        ui->developFilmstripList->setUniformItemSizes(true);
        ui->developFilmstripList->setSelectionMode(QAbstractItemView::SingleSelection);
        ui->developFilmstripList->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
        ui->developFilmstripList->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        ui->developFilmstripList->setIconSize(QSize(128, 80));
        ui->developFilmstripList->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

        connect(ui->developFilmstripList, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *item) {
            if (!item) {
                return;
            }
            const qint64 filmstripAssetId = item->data(Qt::UserRole).toLongLong();
            const LibraryAsset *asset = assetById(filmstripAssetId);
            if (!asset) {
                return;
            }
            const QString originalPath = assetOriginalPath(*asset);
            if (originalPath.isEmpty()) {
                QMessageBox::warning(this,
                                     tr("Unable to open image"),
                                     tr("The selected asset does not have an original file path."));
                return;
            }
            openAssetInDevelop(filmstripAssetId, originalPath);
        });
    }

    if (ui->developFilmstripContainer) {
        ui->developFilmstripContainer->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    }

    if (ui->developZoomCombo) {
        if (ui->developZoomCombo->count() == 0) {
            ui->developZoomCombo->addItems({tr("Fit"), QStringLiteral("50 %"), QStringLiteral("100 %"), QStringLiteral("200 %"), QStringLiteral("400 %")});
        }
        connect(ui->developZoomCombo, &QComboBox::currentTextChanged, this, &MainWindow::applyDevelopZoomPreset);
    }

    initializeAdjustmentControls();

    if (ui->developToggleBeforeAfterButton) {
        connect(ui->developToggleBeforeAfterButton, &QPushButton::clicked, this, [this]() {
            showStatusMessage(tr("Before/After toggle (not yet implemented)"), 2000);
        });
    }

    if (ui->developFitButton) {
        connect(ui->developFitButton, &QPushButton::clicked, this, [this]() {
            fitDevelopViewToImage();
        });
    }

    // Connect toolbar actions to switch pages
    connect(ui->actionShowLibrary, &QAction::triggered, [this]() {
        if(ui->stackedWidget) ui->stackedWidget->setCurrentIndex(0);
    });
    connect(ui->actionShowDevelop, &QAction::triggered, [this]() {
        if(ui->stackedWidget) ui->stackedWidget->setCurrentIndex(1);
    });

    if (ui->actionImport) {
        ui->actionImport->setEnabled(false);
    }

    openOrCreateDefaultLibrary();
}

void MainWindow::ensureDevelopViewInitialized()
{
    qDebug() << "ensureDevelopViewInitialized: Called from thread:" << QThread::currentThread()
             << "isMainThread:" << (QThread::currentThread() == QCoreApplication::instance()->thread());
    
    if (!ui->developImageView) {
        qDebug() << "ensureDevelopViewInitialized: developImageView is null, skipping initialization";
        initializeDevelopHistogram();
        return;
    }

    if (m_developScene) {
        qDebug() << "ensureDevelopViewInitialized: Scene already initialized";
        initializeDevelopHistogram();
        return;
    }

    qDebug() << "ensureDevelopViewInitialized: Creating new scene and OpenGL viewport";
    m_developScene = new QGraphicsScene(this);
    m_developScene->setItemIndexMethod(QGraphicsScene::NoIndex);
    ui->developImageView->setScene(m_developScene);
    
    // Ensure OpenGL viewport for GPU rendering
    QOpenGLWidget *glViewport = qobject_cast<QOpenGLWidget*>(ui->developImageView->viewport());
    if (!glViewport) {
        qDebug() << "ensureDevelopViewInitialized: Creating new QOpenGLWidget viewport";
        QSurfaceFormat format;
        format.setVersion(3, 3);
        format.setProfile(QSurfaceFormat::CoreProfile);
        format.setRenderableType(QSurfaceFormat::OpenGL);
        format.setSwapBehavior(QSurfaceFormat::DoubleBuffer);
        format.setSamples(0); // Disable MSAA for better performance
        
        qDebug() << "ensureDevelopViewInitialized: OpenGL format - Version:" << format.majorVersion() << format.minorVersion()
                 << "Profile:" << (format.profile() == QSurfaceFormat::CoreProfile ? "Core" : "Compatibility")
                 << "Renderable:" << (format.renderableType() == QSurfaceFormat::OpenGL ? "OpenGL" : "OpenGLES");
        
        glViewport = new QOpenGLWidget(ui->developImageView);
        glViewport->setFormat(format);
        glViewport->setUpdateBehavior(QOpenGLWidget::PartialUpdate);
        ui->developImageView->setViewport(glViewport);
        
        qDebug() << "ensureDevelopViewInitialized: QOpenGLWidget created, isValid:" << glViewport->isValid()
                 << "format:" << glViewport->format().majorVersion() << glViewport->format().minorVersion()
                 << "isVisible:" << glViewport->isVisible();
        
        // QOpenGLWidget may not be valid until it's shown, but we can still set it up
        // The context will be created when the widget is first painted
        if (!glViewport->isVisible()) {
            glViewport->show(); // Ensure widget is visible for context creation
        }
    } else {
        qDebug() << "ensureDevelopViewInitialized: Using existing QOpenGLWidget viewport, isValid:" << glViewport->isValid()
                 << "isVisible:" << glViewport->isVisible();
    }
    
    ui->developImageView->setViewportUpdateMode(QGraphicsView::FullViewportUpdate);
    ui->developImageView->setCacheMode(QGraphicsView::CacheNone); // Disable CPU caching for GPU rendering
    ui->developImageView->setRenderHint(QPainter::SmoothPixmapTransform, true);
    ui->developImageView->setRenderHint(QPainter::Antialiasing, true);
    ui->developImageView->setDragMode(QGraphicsView::ScrollHandDrag);
    ui->developImageView->setOptimizationFlags(QGraphicsView::DontSavePainterState | QGraphicsView::DontAdjustForAntialiasing);
    ui->developImageView->setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
    ui->developImageView->setResizeAnchor(QGraphicsView::AnchorUnderMouse);
    ui->developImageView->setBackgroundBrush(QColor(30, 30, 30));

    m_developPixmapItem = m_developScene->addPixmap(QPixmap());
    m_developPixmapItem->setVisible(false);
    m_developPixmapItem->setTransformationMode(Qt::SmoothTransformation);
    m_developPixmapItem->setCacheMode(QGraphicsItem::NoCache); // Force GPU rendering

    qDebug() << "ensureDevelopViewInitialized: Initialization complete, scene:" << m_developScene
             << "pixmapItem:" << m_developPixmapItem;
    initializeDevelopHistogram();
}

void MainWindow::initializeDevelopHistogram()
{
    if (!ui->developHistogramLayout) {
        return;
    }

    if (!m_histogramWidget) {
        m_histogramWidget = new HistogramWidget(this);
        m_histogramWidget->setMinimumHeight(180);
        ui->developHistogramLayout->insertWidget(0, m_histogramWidget);
    }

    if (ui->developHistogramPlaceholderLabel) {
        ui->developHistogramPlaceholderLabel->setVisible(m_histogramWidget == nullptr);
    }
}

void MainWindow::clearDevelopView()
{
    if (m_savingAdjustmentsPending && m_currentDevelopAssetId >= 0) {
        persistCurrentAdjustments();
    }

    m_adjustmentPersistTimer.stop();
    m_fullRenderTimer.stop();
    if (m_adjustmentEngine) {
        m_adjustmentEngine->cancelActive();
    }
    m_currentDevelopOriginalImage = QImage();
    m_currentDevelopAdjustedImage = QImage();
    m_currentDevelopAdjustedValid = false;
    m_currentDevelopPreviewImage = QImage();
    m_currentDevelopPreviewScale = 1.0;
    m_previewRenderEnabled = false;
    m_nextAdjustmentRequestId = 0;
    m_latestPreviewRequestId = 0;
    m_latestFullRequestId = 0;

    m_currentDevelopAssetId = -1;
    m_developZoom = 1.0;
    m_developFitMode = true;

    if (m_jobManager && !m_activeDevelopJobId.isNull()) {
        m_jobManager->cancelJob(m_activeDevelopJobId, tr("Develop view reset"));
        m_activeDevelopJobId = {};
    }
    if (m_jobManager && !m_activeHistogramJobId.isNull()) {
        m_jobManager->cancelJob(m_activeHistogramJobId, tr("Histogram reset"));
        m_activeHistogramJobId = {};
    }

    if (m_developPixmapItem) {
        m_developPixmapItem->setPixmap(QPixmap());
        m_developPixmapItem->setVisible(false);
    }

    if (m_developScene) {
        m_developScene->setSceneRect(QRectF());
    }

    if (ui->developViewerStack && ui->developPlaceholderWidget) {
        ui->developViewerStack->setCurrentWidget(ui->developPlaceholderWidget);
    }

    if (ui->developPlaceholderLabel) {
        ui->developPlaceholderLabel->setText(tr("Double-click an image in the Library to edit it here."));
    }

    if (ui->developImageInfoLabel) {
        ui->developImageInfoLabel->setText(tr("No image selected"));
    }
    auto resetLabel = [](QLabel *label) {
        if (label) {
            label->setText(QStringLiteral("—"));
        }
    };
    resetLabel(ui->developMetadataCameraValue);
    resetLabel(ui->developMetadataLensValue);
    resetLabel(ui->developMetadataIsoValue);
    resetLabel(ui->developMetadataShutterValue);
    resetLabel(ui->developMetadataApertureValue);
    resetLabel(ui->developMetadataFocalLengthValue);
    resetLabel(ui->developMetadataFlashValue);
    resetLabel(ui->developMetadataFocusDistanceValue);
    if (ui->developMetadataFileSizeValue) {
        ui->developMetadataFileSizeValue->setText(QStringLiteral("—"));
    }
    if (ui->developMetadataResolutionValue) {
        ui->developMetadataResolutionValue->setText(QStringLiteral("—"));
    }
    if (ui->developMetadataCaptureDateValue) {
        ui->developMetadataCaptureDateValue->setText(QStringLiteral("—"));
    }
    if (ui->developZoomCombo) {
        QSignalBlocker blocker(ui->developZoomCombo);
        ui->developZoomCombo->setCurrentIndex(0);
    }
    resetHistogram();
}

void MainWindow::resetHistogram()
{
    if (m_histogramWidget) {
        m_histogramWidget->clear();
    }
}

void MainWindow::showDevelopLoadingState(const QString &message)
{
    m_developFitMode = true;

    if (ui->developViewerStack && ui->developPlaceholderWidget) {
        ui->developViewerStack->setCurrentWidget(ui->developPlaceholderWidget);
    }
    if (ui->developPlaceholderLabel) {
        ui->developPlaceholderLabel->setText(message);
    }
    if (ui->developImageInfoLabel) {
        ui->developImageInfoLabel->setText(message);
    }
    if (m_histogramWidget) {
        m_histogramWidget->setStatusMessage(tr("Computing histogram…"));
    }

    auto resetLabel = [](QLabel *label) {
        if (label) {
            label->setText(QStringLiteral("—"));
        }
    };
    resetLabel(ui->developMetadataCameraValue);
    resetLabel(ui->developMetadataLensValue);
    resetLabel(ui->developMetadataIsoValue);
    resetLabel(ui->developMetadataShutterValue);
    resetLabel(ui->developMetadataApertureValue);
    resetLabel(ui->developMetadataFocalLengthValue);
    resetLabel(ui->developMetadataFlashValue);
    resetLabel(ui->developMetadataFocusDistanceValue);
    if (ui->developMetadataFileSizeValue) {
        ui->developMetadataFileSizeValue->setText(QStringLiteral("—"));
    }
    if (ui->developMetadataResolutionValue) {
        ui->developMetadataResolutionValue->setText(QStringLiteral("—"));
    }
    if (ui->developMetadataCaptureDateValue) {
        ui->developMetadataCaptureDateValue->setText(QStringLiteral("—"));
    }

    if (m_jobManager && !m_activeDevelopJobId.isNull()) {
        m_jobManager->updateDetail(m_activeDevelopJobId, message);
    }
}

void MainWindow::showDevelopPreview(const QPixmap &pixmap)
{
    if (!m_developScene || !m_developPixmapItem || pixmap.isNull()) {
        return;
    }

    if (!ui->developImageView) {
        return;
    }

    // Ensure the viewport is visible and has valid context
    QOpenGLWidget *glViewport = qobject_cast<QOpenGLWidget*>(ui->developImageView->viewport());
    if (glViewport && !glViewport->isVisible()) {
        glViewport->show();
    }

    m_developPixmapItem->setPixmap(pixmap);
    m_developPixmapItem->setVisible(true);
    m_developScene->setSceneRect(pixmap.rect());

    if (ui->developViewerStack && ui->developImageViewPage) {
        ui->developViewerStack->setCurrentWidget(ui->developImageViewPage);
    }

    if (ui->stackedWidget && ui->developPage) {
        ui->stackedWidget->setCurrentWidget(ui->developPage);
    }

    // Force viewport update for GPU rendering
    m_developScene->update();
    if (ui->developImageView) {
        ui->developImageView->viewport()->update();
        ui->developImageView->update();
    }

    fitDevelopViewToImage();

    if (m_jobManager && !m_activeDevelopJobId.isNull()) {
        m_jobManager->updateDetail(m_activeDevelopJobId, tr("Preview ready"));
    }
}

void MainWindow::updateHistogram(const HistogramData &histogram)
{
    if (!m_histogramWidget) {
        return;
    }

    if (!histogram.isValid()) {
        m_histogramWidget->setStatusMessage(tr("Histogram unavailable."));
        return;
    }

    m_histogramWidget->setHistogramData(histogram);

    if (ui->developHistogramHintLabel) {
        QStringList hints;
        if (histogram.totalSamples > 0) {
            auto sumRange = [](const QVector<int> &values, int start, int end) {
                int total = 0;
                for (int i = start; i <= end && i < values.size(); ++i) {
                    total += values.at(i);
                }
                return total;
            };

            const QLocale locale;
            const int shadowCount = sumRange(histogram.luminance, 0, 4);
            const int highlightCount = sumRange(histogram.luminance, 251, 255);
            const double shadowRatio = static_cast<double>(shadowCount) / static_cast<double>(histogram.totalSamples);
            const double highlightRatio = static_cast<double>(highlightCount) / static_cast<double>(histogram.totalSamples);

            if (highlightRatio > 0.05) {
                const double percent = highlightRatio * 100.0;
                hints << tr("Overexposed: ~%1% of pixels near white").arg(locale.toString(percent, 'f', percent >= 10.0 ? 0 : 1));
            }
            if (shadowRatio > 0.05) {
                const double percent = shadowRatio * 100.0;
                hints << tr("Underexposed: ~%1% of pixels near black").arg(locale.toString(percent, 'f', percent >= 10.0 ? 0 : 1));
            }
        }

        if (hints.isEmpty()) {
            ui->developHistogramHintLabel->setText(tr("Exposure looks balanced."));
        } else {
            ui->developHistogramHintLabel->setText(hints.join(QLatin1Char('\n')));
        }
    }
}

void MainWindow::handleHistogramReady()
{
    if (!m_histogramWatcher) {
        return;
    }

    const HistogramTaskResult result = m_histogramWatcher->result();
    if (result.requestId != m_activeHistogramRequestId) {
        return;
    }

    const bool valid = result.histogram.isValid();
    updateHistogram(result.histogram);

    if (m_jobManager && !m_activeHistogramJobId.isNull()) {
        if (valid) {
            m_jobManager->completeJob(m_activeHistogramJobId, tr("Histogram ready"));
        } else {
            m_jobManager->failJob(m_activeHistogramJobId, tr("Unable to compute histogram"));
        }
        m_activeHistogramJobId = {};
    }
}

void MainWindow::requestHistogramComputation(const QImage &image, int requestId)
{
    if (!m_histogramWatcher) {
        return;
    }

    if (image.isNull()) {
        resetHistogram();
        if (m_jobManager && !m_activeHistogramJobId.isNull()) {
            m_jobManager->cancelJob(m_activeHistogramJobId, tr("Histogram cancelled"));
            m_activeHistogramJobId = {};
        }
        return;
    }

    if (m_jobManager) {
        if (!m_activeHistogramJobId.isNull()) {
            m_jobManager->cancelJob(m_activeHistogramJobId, tr("Histogram superseded"));
        }
        m_activeHistogramJobId = m_jobManager->startJob(JobCategory::Histogram,
                                                        tr("Computing histogram"),
                                                        tr("Analyzing tonal data"));
        m_jobManager->setIndeterminate(m_activeHistogramJobId, true);
    }

    m_activeHistogramRequestId = requestId;

    if (m_histogramWidget) {
        m_histogramWidget->setStatusMessage(tr("Computing histogram…"));
    }

    auto future = QtConcurrent::run([requestId, image]() {
        HistogramTaskResult taskResult;
        taskResult.requestId = requestId;
        taskResult.histogram = computeHistogram(image);
        return taskResult;
    });

    m_histogramWatcher->setFuture(future);
}

void MainWindow::bindLibrarySignals()
{
    if (!m_libraryManager) {
        return;
    }

    connect(m_libraryManager, &LibraryManager::libraryOpened, this, [this](const QString &path) {
        updateFilterPaneOptions();
        currentLibraryPath = path;
        if (ui->actionImport) {
            ui->actionImport->setEnabled(true);
        }
        showStatusMessage(tr("Opened library: %1").arg(QDir(path).dirName()), 4000);
        refreshLibraryView();
    });

    connect(m_libraryManager, &LibraryManager::libraryClosed, this, [this]() {
        currentLibraryPath.clear();
        if (ui->actionImport) {
            ui->actionImport->setEnabled(false);
        }
        clearLibrary();
    });

    connect(m_libraryManager, &LibraryManager::assetsChanged, this, [this]() {
        updateFilterPaneOptions();
        if (m_libraryFilterPane) {
            refreshLibraryView(m_libraryFilterPane->currentFilterOptions());
        } else {
            refreshLibraryView();
        }
    });
    connect(m_libraryManager, &LibraryManager::assetPreviewUpdated, this, &MainWindow::updateThumbnailPreview);

    connect(m_libraryManager, &LibraryManager::importProgress, this, &MainWindow::handleImportProgress);
    connect(m_libraryManager, &LibraryManager::importCompleted, this, &MainWindow::handleImportCompleted);
    connect(m_libraryManager, &LibraryManager::errorOccurred, this, &MainWindow::handleLibraryError);
}

void MainWindow::setupJobSystem()
{
    if (!m_jobManager) {
        m_jobManager = new JobManager(this);
    }

    if (m_libraryManager) {
        m_libraryManager->setJobManager(m_jobManager);
    }

    if (!m_jobsWindow) {
        m_jobsWindow = new JobsWindow(this);
        connect(m_jobsWindow, &JobsWindow::visibilityChanged, this, [this](bool visible) {
            if (!m_toggleJobsAction) {
                return;
            }
            QSignalBlocker blocker(m_toggleJobsAction);
            m_toggleJobsAction->setChecked(visible);
        });
    }
    m_jobsWindow->setJobManager(m_jobManager);

    if (!m_toggleJobsAction) {
        m_toggleJobsAction = new QAction(tr("Jobs"), this);
        m_toggleJobsAction->setCheckable(true);
        connect(m_toggleJobsAction, &QAction::triggered, this, &MainWindow::toggleJobsWindow);
        if (ui->toolBar) {
            ui->toolBar->addSeparator();
            ui->toolBar->addAction(m_toggleJobsAction);
        }
    }

    connect(m_jobManager, &JobManager::jobAdded, this, &MainWindow::handleJobListChanged);
    connect(m_jobManager, &JobManager::jobRemoved, this, &MainWindow::handleJobListChanged);
    connect(m_jobManager, &JobManager::jobUpdated, this, &MainWindow::handleJobListChanged);

    updateJobsActionBadge();
}

void MainWindow::toggleJobsWindow()
{
    if (!m_jobsWindow || !m_toggleJobsAction) {
        return;
    }

    if (m_jobsWindow->isVisible()) {
        m_jobsWindow->hide();
        return;
    }

    QWidget *anchor = ui->toolBar ? ui->toolBar->widgetForAction(m_toggleJobsAction) : nullptr;
    m_jobsWindow->showRelativeTo(anchor ? anchor : this);
}

void MainWindow::handleJobListChanged()
{
    updateJobsActionBadge();

    if (!m_jobManager || !m_jobsWindow) {
        return;
    }
}

void MainWindow::updateJobsActionBadge()
{
    if (!m_toggleJobsAction || !m_jobManager) {
        return;
    }

    const int active = m_jobManager->activeJobCount();
    QString label = tr("Jobs");
    if (active > 0) {
        label = tr("Jobs (%1)").arg(active);
    }
    m_toggleJobsAction->setText(label);
    m_toggleJobsAction->setStatusTip(tr("Show current background activity"));
}

void MainWindow::handleImportProgress(int imported, int total)
{
    showStatusMessage(tr("Importing items %1/%2").arg(imported).arg(total), 0);

    if (!m_importJobActive || !m_jobManager || m_activeImportJobId.isNull()) {
        return;
    }

    m_jobManager->updateProgress(m_activeImportJobId, imported, total);
    m_jobManager->updateDetail(m_activeImportJobId,
                               tr("%1 of %2 photos processed").arg(imported).arg(total));
}

void MainWindow::handleImportCompleted()
{
    showStatusMessage(tr("Import completed"), 2000);

    if (m_jobManager && m_importJobActive && !m_activeImportJobId.isNull()) {
        m_jobManager->completeJob(m_activeImportJobId, tr("Import completed"));
    }

    m_importJobActive = false;
    m_activeImportJobId = {};
    m_activeImportTotal = 0;
}

void MainWindow::handleLibraryError(const QString &message)
{
    QMessageBox::warning(this, tr("Library error"), message);

    if (m_jobManager && m_importJobActive && !m_activeImportJobId.isNull()) {
        m_jobManager->failJob(m_activeImportJobId, message);
        m_importJobActive = false;
        m_activeImportJobId = {};
        m_activeImportTotal = 0;
    }
}

void MainWindow::refreshLibraryView()
{
    FilterOptions defaultOptions;
    defaultOptions.sortOrder = FilterOptions::SortByDateDesc;
    refreshLibraryView(defaultOptions);
}

void MainWindow::updateFilterPaneOptions()
{
    if (!m_libraryFilterPane || !m_libraryManager || !m_libraryManager->hasOpenLibrary()) {
        return;
    }

    MetadataCache *cache = m_libraryManager->metadataCache();
    if (!cache || !cache->hasOpenCache()) {
        return;
    }

    // Update available camera makes
    QStringList cameraMakes = cache->getAllCameraMakes();
    m_libraryFilterPane->setAvailableCameraMakes(cameraMakes);

    // Update ISO range
    int minIso = cache->getMinIso();
    int maxIso = cache->getMaxIso();
    if (minIso > 0 && maxIso > 0) {
        m_libraryFilterPane->setIsoRange(minIso, maxIso);
    }

    // Update available tags
    QStringList tags = cache->getAllTags();
    m_libraryFilterPane->setAvailableTags(tags);
}

void MainWindow::refreshLibraryView(const FilterOptions &filterOptions)
{
    if (!m_libraryGridView) {
        return;
    }

    if (!m_libraryManager || !m_libraryManager->hasOpenLibrary()) {
        m_assets.clear();
        m_libraryGridView->clear();
        updateDevelopFilmstrip();
        return;
    }

    m_assets = m_libraryManager->assets(filterOptions);

    QVector<LibraryGridItem> items;
    items.reserve(m_assets.size());
    for (const LibraryAsset &asset : m_assets) {
        LibraryGridItem item;
        item.assetId = asset.id;
        item.photoNumber = asset.photoNumber;
        item.fileName = asset.fileName;
        item.previewPath = assetPreviewPath(asset);
        item.originalPath = assetOriginalPath(asset);
        items.append(item);
    }

    m_libraryGridView->setItems(items);
    updateDevelopFilmstrip();

    QStringList preloadPaths;
    const int preloadCount = qMin(items.size(), 8);
    for (int i = 0; i < preloadCount; ++i) {
        const QString &path = items.at(i).originalPath;
        if (!path.isEmpty()) {
            preloadPaths.append(path);
        }
    }
    if (!preloadPaths.isEmpty()) {
        ImageLoader::preloadAsync(preloadPaths);
    }
}

void MainWindow::updateDevelopFilmstrip()
{
    if (!ui->developFilmstripList) {
        return;
    }

    QSignalBlocker blocker(ui->developFilmstripList);
    ui->developFilmstripList->clear();

    const QSize iconSize = ui->developFilmstripList->iconSize();

    int currentIndex = -1;
    for (int i = 0; i < m_assets.size(); ++i) {
        if (m_assets.at(i).id == m_currentDevelopAssetId) {
            currentIndex = i;
            break;
        }
    }

    QStringList developPreload;
    const int neighborRadius = 4;

    for (int index = 0; index < m_assets.size(); ++index) {
        const LibraryAsset &asset = m_assets.at(index);
        auto *item = new QListWidgetItem(asset.fileName);
        item->setData(Qt::UserRole, asset.id);
        item->setToolTip(asset.fileName);

        const QString previewPath = assetPreviewPath(asset);
        if (!previewPath.isEmpty()) {
            QImageReader reader(previewPath);
            reader.setAutoTransform(true);
            reader.setScaledSize(iconSize);
            const QImage thumbnail = reader.read();
            if (!thumbnail.isNull()) {
                item->setIcon(QIcon(QPixmap::fromImage(thumbnail)));
            } else {
                QPixmap fallback(previewPath);
                if (!fallback.isNull()) {
                    item->setIcon(QIcon(fallback.scaled(iconSize, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
                }
            }
        }

        ui->developFilmstripList->addItem(item);
        if (asset.id == m_currentDevelopAssetId) {
            item->setSelected(true);
            ui->developFilmstripList->scrollToItem(item, QAbstractItemView::PositionAtCenter);
        }

        if (currentIndex >= 0 && std::abs(index - currentIndex) <= neighborRadius) {
            const QString originalPath = assetOriginalPath(asset);
            if (!originalPath.isEmpty()) {
                developPreload.append(originalPath);
            }
        }
    }

    if (developPreload.isEmpty()) {
        const int preloadCount = qMin(m_assets.size(), 6);
        for (int i = 0; i < preloadCount; ++i) {
            const QString originalPath = assetOriginalPath(m_assets.at(i));
            if (!originalPath.isEmpty()) {
                developPreload.append(originalPath);
            }
        }
    }

    if (!developPreload.isEmpty()) {
        ImageLoader::preloadAsync(developPreload);
    }
}

void MainWindow::selectFilmstripItem(qint64 assetId)
{
    if (!ui->developFilmstripList) {
        return;
    }

    QSignalBlocker blocker(ui->developFilmstripList);
    for (int row = 0; row < ui->developFilmstripList->count(); ++row) {
        QListWidgetItem *item = ui->developFilmstripList->item(row);
        if (!item) {
            continue;
        }

        const bool isCurrent = item->data(Qt::UserRole).toLongLong() == assetId;
        item->setSelected(isCurrent);
        if (isCurrent) {
            ui->developFilmstripList->scrollToItem(item, QAbstractItemView::PositionAtCenter);
        }
    }
}

void MainWindow::setupAdjustmentEngine()
{
    if (!m_adjustmentEngine) {
        m_adjustmentEngine = new DevelopAdjustmentEngine(this);
        // Initialize GPU on the main thread before any rendering
        qDebug() << "setupAdjustmentEngine: Initializing GPU on main thread";
        m_adjustmentEngine->initializeGpuOnMainThread();
    }

    m_adjustmentPersistTimer.setSingleShot(true);
    m_adjustmentPersistTimer.setInterval(350);
    connect(&m_adjustmentPersistTimer, &QTimer::timeout,
            this, &MainWindow::persistCurrentAdjustments);

    m_fullRenderTimer.setSingleShot(true);
    m_fullRenderTimer.setInterval(300); // Slightly longer delay for full render after preview
    connect(&m_fullRenderTimer, &QTimer::timeout, this, [this]() {
        startFullRender();
    });
}

void MainWindow::bindAdjustmentControl(QWidget *sliderWidget,
                                       QWidget *spinWidget,
                                       const std::function<void(double)> &setter,
                                       const std::function<double()> &getter,
                                       const std::function<double(int)> &sliderToValue,
                                       const std::function<int(double)> &valueToSlider)
{
    auto *slider = qobject_cast<QSlider *>(sliderWidget);
    auto *spin = qobject_cast<QDoubleSpinBox *>(spinWidget);
    if (!slider || !spin) {
        return;
    }

    spin->setKeyboardTracking(false);

    connect(slider, &QSlider::valueChanged, this, [this, spin, setter, sliderToValue](int rawValue) {
        const double actualValue = sliderToValue(rawValue);
        setter(actualValue);
        const double clampedValue = std::clamp(actualValue, spin->minimum(), spin->maximum());
        QSignalBlocker spinBlock(spin);
        spin->setValue(clampedValue);
    });

    connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, slider, setter, valueToSlider](double value) {
        setter(value);
        const int sliderValue = valueToSlider(value);
        if (slider->value() != sliderValue) {
            QSignalBlocker sliderBlock(slider);
            slider->setValue(sliderValue);
        }
    });

    const double currentValue = getter();
    {
        QSignalBlocker sliderBlock(slider);
        slider->setValue(valueToSlider(currentValue));
    }
    {
        QSignalBlocker spinBlock(spin);
        spin->setValue(currentValue);
    }
}

void MainWindow::initializeAdjustmentControls()
{
    if (!ui) {
        return;
    }

    auto connectBasicControl = [this](QSlider *slider,
                                      QDoubleSpinBox *spin,
                                      std::function<double()> getter,
                                      std::function<void(double)> setter,
                                      double sliderScale) {
        if (!slider || !spin) {
            return;
        }
        slider->setTracking(true);
        auto sliderToValue = [sliderScale](int raw) {
            return static_cast<double>(raw) / sliderScale;
        };
        auto valueToSlider = [sliderScale, slider](double value) {
            const double clamped = std::clamp(value, slider->minimum() / sliderScale, slider->maximum() / sliderScale);
            return static_cast<int>(std::round(clamped * sliderScale));
        };
        bindAdjustmentControl(slider,
                              spin,
                              setter,
                              getter,
                              sliderToValue,
                              valueToSlider);
    };

    connectBasicControl(ui->exposureSlider,
                        ui->exposureSpinBox,
                        [this]() { return m_currentAdjustments.exposure; },
                        [this](double value) {
                            if (!almostEqual(m_currentAdjustments.exposure, value)) {
                                m_currentAdjustments.exposure = value;
                                handleAdjustmentChanged();
                            }
                        },
                        100.0);

    const auto identityGetter = [this](double DevelopAdjustments::*member) {
        return [this, member]() {
            return m_currentAdjustments.*member;
        };
    };
    const auto identitySetter = [this](double DevelopAdjustments::*member) {
        return [this, member](double value) {
            double &target = m_currentAdjustments.*member;
            if (!almostEqual(target, value)) {
                target = value;
                handleAdjustmentChanged();
            }
        };
    };

    auto connectHundredRange = [&](QSlider *slider,
                                   QDoubleSpinBox *spin,
                                   double DevelopAdjustments::*member) {
        connectBasicControl(slider,
                            spin,
                            identityGetter(member),
                            identitySetter(member),
                            1.0);
    };

    connectHundredRange(ui->contrastSlider, ui->contrastSpinBox, &DevelopAdjustments::contrast);
    connectHundredRange(ui->highlightsSlider, ui->highlightsSpinBox, &DevelopAdjustments::highlights);
    connectHundredRange(ui->shadowsSlider, ui->shadowsSpinBox, &DevelopAdjustments::shadows);
    connectHundredRange(ui->whitesSlider, ui->whitesSpinBox, &DevelopAdjustments::whites);
    connectHundredRange(ui->blacksSlider, ui->blacksSpinBox, &DevelopAdjustments::blacks);
    connectHundredRange(ui->claritySlider, ui->claritySpinBox, &DevelopAdjustments::clarity);
    connectHundredRange(ui->vibranceSlider, ui->vibranceSpinBox, &DevelopAdjustments::vibrance);
    connectHundredRange(ui->saturationSlider, ui->saturationSpinBox, &DevelopAdjustments::saturation);

    auto connectSliderOnly = [this](QSlider *slider, double DevelopAdjustments::*member, double scale = 1.0) {
        if (!slider) {
            return;
        }
        connect(slider, &QSlider::valueChanged, this, [this, slider, member, scale](int rawValue) {
            double value = static_cast<double>(rawValue) / scale;
            double &target = m_currentAdjustments.*member;
            if (!almostEqual(target, value)) {
                target = value;
                handleAdjustmentChanged();
            }
        });
        QSignalBlocker blocker(slider);
        slider->setValue(static_cast<int>(std::round((m_currentAdjustments.*member) * scale)));
    };

    connectSliderOnly(ui->toneCurveHighlightsSlider, &DevelopAdjustments::toneCurveHighlights);
    connectSliderOnly(ui->toneCurveLightsSlider, &DevelopAdjustments::toneCurveLights);
    connectSliderOnly(ui->toneCurveDarksSlider, &DevelopAdjustments::toneCurveDarks);
    connectSliderOnly(ui->toneCurveShadowsSlider, &DevelopAdjustments::toneCurveShadows);

    connectSliderOnly(ui->colorHueSlider, &DevelopAdjustments::hueShift);
    connectSliderOnly(ui->colorSaturationSlider, &DevelopAdjustments::saturationShift);
    connectSliderOnly(ui->colorLuminanceSlider, &DevelopAdjustments::luminanceShift);

    connectSliderOnly(ui->sharpeningSlider, &DevelopAdjustments::sharpening);
    connectSliderOnly(ui->noiseReductionSlider, &DevelopAdjustments::noiseReduction);
    connectSliderOnly(ui->vignetteSlider, &DevelopAdjustments::vignette);
    connectSliderOnly(ui->grainSlider, &DevelopAdjustments::grain);

    if (ui->toneCurvePresetCombo) {
        connect(ui->toneCurvePresetCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
            DevelopAdjustments preset = m_currentAdjustments;
            switch (index) {
            case 0: // Linear
                preset.toneCurveHighlights = 0.0;
                preset.toneCurveLights = 0.0;
                preset.toneCurveDarks = 0.0;
                preset.toneCurveShadows = 0.0;
                break;
            case 1: // Medium Contrast
                preset.toneCurveHighlights = 25.0;
                preset.toneCurveLights = 15.0;
                preset.toneCurveDarks = -15.0;
                preset.toneCurveShadows = -25.0;
                break;
            case 2: // Strong Contrast
                preset.toneCurveHighlights = 35.0;
                preset.toneCurveLights = 25.0;
                preset.toneCurveDarks = -25.0;
                preset.toneCurveShadows = -35.0;
                break;
            default:
                break;
            }
            m_currentAdjustments.toneCurveHighlights = preset.toneCurveHighlights;
            m_currentAdjustments.toneCurveLights = preset.toneCurveLights;
            m_currentAdjustments.toneCurveDarks = preset.toneCurveDarks;
            m_currentAdjustments.toneCurveShadows = preset.toneCurveShadows;
            syncAdjustmentControls(m_currentAdjustments);
            handleAdjustmentChanged();
        });
    }

    if (ui->developResetAdjustmentsButton) {
        connect(ui->developResetAdjustmentsButton, &QPushButton::clicked,
                this, &MainWindow::resetAdjustmentsToDefault);
    }

    if (ui->developCopyAdjustmentsButton) {
        connect(ui->developCopyAdjustmentsButton, &QPushButton::clicked,
                this, &MainWindow::on_actionCopy_triggered);
    }

    if (ui->developPasteAdjustmentsButton) {
        connect(ui->developPasteAdjustmentsButton, &QPushButton::clicked,
                this, &MainWindow::on_actionPaste_triggered);
    }

    syncAdjustmentControls(m_currentAdjustments);
}

void MainWindow::handleAdjustmentChanged()
{
    if (m_currentDevelopAssetId < 0) {
        return;
    }
    m_savingAdjustmentsPending = true;
    scheduleAdjustmentPersist();
    // Always use preview render for real-time feedback, then queue full render
    requestAdjustmentRender(false, false);
}

bool MainWindow::adjustmentsAreIdentity(const DevelopAdjustments &adjustments) const
{
    return almostEqual(adjustments.exposure, 0.0) &&
           almostEqual(adjustments.contrast, 0.0) &&
           almostEqual(adjustments.highlights, 0.0) &&
           almostEqual(adjustments.shadows, 0.0) &&
           almostEqual(adjustments.whites, 0.0) &&
           almostEqual(adjustments.blacks, 0.0) &&
           almostEqual(adjustments.clarity, 0.0) &&
           almostEqual(adjustments.vibrance, 0.0) &&
           almostEqual(adjustments.saturation, 0.0) &&
           almostEqual(adjustments.toneCurveHighlights, 0.0) &&
           almostEqual(adjustments.toneCurveLights, 0.0) &&
           almostEqual(adjustments.toneCurveDarks, 0.0) &&
           almostEqual(adjustments.toneCurveShadows, 0.0) &&
           almostEqual(adjustments.hueShift, 0.0) &&
           almostEqual(adjustments.saturationShift, 0.0) &&
           almostEqual(adjustments.luminanceShift, 0.0) &&
           almostEqual(adjustments.sharpening, 0.0) &&
           almostEqual(adjustments.noiseReduction, 0.0) &&
           almostEqual(adjustments.vignette, 0.0) &&
           almostEqual(adjustments.grain, 0.0);
}

void MainWindow::requestAdjustmentRender(bool forceImmediate, bool skipCancel)
{
    if (!m_adjustmentEngine || m_currentDevelopAssetId < 0 || m_currentDevelopOriginalImage.isNull()) {
        qDebug() << "requestAdjustmentRender: Cannot render - engine:" << (m_adjustmentEngine != nullptr)
                 << "assetId:" << m_currentDevelopAssetId << "image null:" << m_currentDevelopOriginalImage.isNull();
        return;
    }

    const bool identity = adjustmentsAreIdentity(m_currentAdjustments);
    if (identity) {
        qDebug() << "requestAdjustmentRender: Adjustments are identity, using original image";
        if (!skipCancel) {
            m_adjustmentEngine->cancelActive();
        }
        m_fullRenderTimer.stop();
        m_currentDevelopAdjustedImage = m_currentDevelopOriginalImage;
        m_currentDevelopAdjustedValid = true;
        applyDevelopImage(m_currentDevelopOriginalImage, true, false, 1.0);
        return;
    }

    qDebug() << "requestAdjustmentRender: Starting render, forceImmediate:" << forceImmediate << "skipCancel:" << skipCancel;
    m_currentDevelopAdjustedValid = false;
    m_currentDevelopAdjustedImage = QImage();

    m_fullRenderTimer.stop();

    if (forceImmediate) {
        m_previewRenderEnabled = false;
        startFullRender(skipCancel);
        return;
    }

    // For real-time feedback: always start with preview render immediately
    // Then queue full render after a delay
    ensurePreviewImageReady();
    m_previewRenderEnabled = true;
    startPreviewRender();
    
    // Queue full render after delay (only if image is small enough for real-time)
    if (!shouldUsePreviewRender()) {
        // For smaller images, also queue a full render after delay
        m_fullRenderTimer.start();
    }
}

void MainWindow::handleAdjustmentRenderResult(const DevelopAdjustmentRenderResult &result)
{
    if (result.cancelled || result.image.isNull()) {
        qDebug() << "Render result cancelled or null, requestId:" << result.requestId;
        return;
    }

    if (result.isPreview) {
        if (result.requestId != m_latestPreviewRequestId) {
            qDebug() << "Preview render ID mismatch:" << result.requestId << "vs" << m_latestPreviewRequestId;
            return;
        }
        applyDevelopImage(result.image, false, true, result.displayScale);
        return;
    }

    if (result.requestId != m_latestFullRequestId) {
        qDebug() << "Full render ID mismatch:" << result.requestId << "vs" << m_latestFullRequestId;
        return;
    }

    qDebug() << "Applying render result to viewport, requestId:" << result.requestId;
    m_currentDevelopAdjustedImage = result.image;
    m_currentDevelopAdjustedValid = true;
    applyDevelopImage(result.image, true, false, 1.0);

    // Update thumbnail with adjusted image
    if (m_currentDevelopAssetId >= 0) {
        schedulePreviewRegeneration(m_currentDevelopAssetId, result.image);
    }
}

void MainWindow::startPreviewRender()
{
    if (!m_adjustmentEngine) {
        return;
    }

    m_adjustmentEngine->cancelActive();

    DevelopAdjustmentRequest request;
    request.requestId = ++m_nextAdjustmentRequestId;
    request.image = m_currentDevelopPreviewImage.isNull() ? m_currentDevelopOriginalImage : m_currentDevelopPreviewImage;
    request.adjustments = m_currentAdjustments;
    if (m_previewRenderEnabled) {
        request.adjustments.sharpening = 0.0;
        request.adjustments.noiseReduction = 0.0;
    }
    request.isPreview = m_previewRenderEnabled;
    request.displayScale = m_previewRenderEnabled ? m_currentDevelopPreviewScale : 1.0;

    m_latestPreviewRequestId = request.requestId;

    auto future = m_adjustmentEngine->renderAsync(std::move(request));
    auto *watcher = new QFutureWatcher<DevelopAdjustmentRenderResult>(this);
    connect(watcher, &QFutureWatcher<DevelopAdjustmentRenderResult>::finished, this, [this, watcher]() {
        DevelopAdjustmentRenderResult result = watcher->result();
        watcher->deleteLater();
        handleAdjustmentRenderResult(result);
    });
    watcher->setFuture(future);
}

void MainWindow::startFullRender(bool skipCancel)
{
    if (!m_adjustmentEngine || m_currentDevelopOriginalImage.isNull()) {
        return;
    }

    // Cancel any previous active renders before starting a new one
    // Skip if we already cancelled (e.g., when pasting)
    if (!skipCancel) {
        m_adjustmentEngine->cancelActive();
    }

    DevelopAdjustmentRequest request;
    request.requestId = ++m_nextAdjustmentRequestId;
    request.image = m_currentDevelopOriginalImage;
    request.adjustments = m_currentAdjustments;
    request.isPreview = false;
    request.displayScale = 1.0;
    m_latestFullRequestId = request.requestId;

    qDebug() << "startFullRender: Starting render with requestId:" << request.requestId;

    auto future = m_adjustmentEngine->renderAsync(std::move(request));
    auto *watcher = new QFutureWatcher<DevelopAdjustmentRenderResult>(this);
    connect(watcher, &QFutureWatcher<DevelopAdjustmentRenderResult>::finished, this, [this, watcher, expectedId = request.requestId]() {
        DevelopAdjustmentRenderResult result = watcher->result();
        qDebug() << "startFullRender: Render finished, requestId:" << result.requestId << "expected:" << expectedId
                 << "cancelled:" << result.cancelled << "image null:" << result.image.isNull();
        watcher->deleteLater();
        handleAdjustmentRenderResult(result);
    });
    watcher->setFuture(future);
}

bool MainWindow::shouldUsePreviewRender() const
{
    if (m_currentDevelopOriginalImage.isNull()) {
        return false;
    }
    const int maxDimension = qMax(m_currentDevelopOriginalImage.width(), m_currentDevelopOriginalImage.height());
    const qint64 totalPixels = static_cast<qint64>(m_currentDevelopOriginalImage.width()) *
                               static_cast<qint64>(m_currentDevelopOriginalImage.height());
    // Use preview for images larger than 8MP for real-time feedback
    if (totalPixels <= 8'000'000) { // ~8 MP
        return false;
    }
    return maxDimension > 2048;
}

void MainWindow::ensurePreviewImageReady()
{
    if (m_currentDevelopOriginalImage.isNull()) {
        m_currentDevelopPreviewImage = QImage();
        m_currentDevelopPreviewScale = 1.0;
        m_previewRenderEnabled = false;
        return;
    }

    if (!m_currentDevelopPreviewImage.isNull()) {
        return;
    }

    const int maxDimension = qMax(m_currentDevelopOriginalImage.width(), m_currentDevelopOriginalImage.height());
    if (maxDimension <= kPreviewMaxDimension) {
        m_currentDevelopPreviewImage = m_currentDevelopOriginalImage;
        m_currentDevelopPreviewScale = 1.0;
        m_previewRenderEnabled = false;
        return;
    }

    const double scale = static_cast<double>(kPreviewMaxDimension) / static_cast<double>(maxDimension);
    const int previewWidth = qMax(1, static_cast<int>(std::round(m_currentDevelopOriginalImage.width() * scale)));
    const int previewHeight = qMax(1, static_cast<int>(std::round(m_currentDevelopOriginalImage.height() * scale)));
    const QSize previewSize(previewWidth, previewHeight);
    m_currentDevelopPreviewImage = m_currentDevelopOriginalImage.scaled(previewSize,
                                                                        Qt::KeepAspectRatio,
                                                                        Qt::FastTransformation);
    if (m_currentDevelopPreviewImage.isNull()) {
        m_currentDevelopPreviewScale = 1.0;
        m_previewRenderEnabled = false;
    } else {
        m_currentDevelopPreviewScale = static_cast<double>(m_currentDevelopOriginalImage.width()) /
                                       static_cast<double>(m_currentDevelopPreviewImage.width());
        m_previewRenderEnabled = !qFuzzyCompare(m_currentDevelopPreviewScale, 1.0);
    }
}

void MainWindow::applyDevelopImage(const QImage &image,
                                   bool updateHistogram,
                                   bool isPreview,
                                   double displayScale)
{
    if (!m_developScene || !m_developPixmapItem || image.isNull()) {
        qDebug() << "applyDevelopImage: Cannot apply - scene:" << (m_developScene != nullptr)
                 << "pixmapItem:" << (m_developPixmapItem != nullptr) << "image null:" << image.isNull();
        return;
    }

    if (!ui->developImageView) {
        qDebug() << "applyDevelopImage: developImageView is null";
        return;
    }

    // Convert image to GPU-friendly format for OpenGL rendering
    QImage gpuImage = image;
    if (gpuImage.format() != QImage::Format_RGB32 && gpuImage.format() != QImage::Format_ARGB32 && 
        gpuImage.format() != QImage::Format_RGBA8888 && gpuImage.format() != QImage::Format_RGBX8888) {
        gpuImage = gpuImage.convertToFormat(QImage::Format_RGBA8888);
    }

    // Create pixmap - QPixmap will use GPU when OpenGL viewport is active
    const QPixmap pixmap = QPixmap::fromImage(gpuImage);
    if (pixmap.isNull()) {
        qDebug() << "applyDevelopImage: Failed to create pixmap from image";
        return;
    }

    qDebug() << "applyDevelopImage: Applying image to viewport, size:" << image.size() << "isPreview:" << isPreview;
    
    // Ensure the viewport is visible and has valid context
    QOpenGLWidget *glViewport = qobject_cast<QOpenGLWidget*>(ui->developImageView->viewport());
    if (glViewport && !glViewport->isVisible()) {
        glViewport->show();
    }

    m_developPixmapItem->setPixmap(pixmap);
    m_developPixmapItem->setVisible(true);
    
    if (isPreview && !qFuzzyCompare(displayScale, 1.0)) {
        const QSizeF scaledSize = QSizeF(image.width() * displayScale,
                                         image.height() * displayScale);
        m_developScene->setSceneRect(QRectF(QPointF(0, 0), scaledSize));
        m_developPixmapItem->setScale(displayScale);
    } else {
        m_developScene->setSceneRect(pixmap.rect());
        m_developPixmapItem->setScale(1.0);
    }

    // Ensure the viewer stack shows the image view
    if (ui->developViewerStack && ui->developImageViewPage) {
        ui->developViewerStack->setCurrentWidget(ui->developImageViewPage);
    }
    if (ui->stackedWidget && ui->developPage) {
        ui->stackedWidget->setCurrentWidget(ui->developPage);
    }

    // Force comprehensive viewport update for GPU rendering
    m_developScene->update();
    if (ui->developImageView) {
        ui->developImageView->viewport()->update();
        ui->developImageView->update();
        // Force immediate repaint
        QApplication::processEvents();
    }

    if (m_developFitMode) {
        fitDevelopViewToImage();
    }

    if (updateHistogram) {
        const int histogramRequestId = ++m_activeHistogramRequestId;
        requestHistogramComputation(image, histogramRequestId);
    }
}

void MainWindow::persistCurrentAdjustments()
{
    m_adjustmentPersistTimer.stop();
    if (!m_savingAdjustmentsPending || !m_libraryManager || m_currentDevelopAssetId < 0) {
        return;
    }

    QString error;
    if (!m_libraryManager->saveDevelopAdjustments(m_currentDevelopAssetId, m_currentAdjustments, &error)) {
        qWarning() << "Failed to persist adjustments:" << error;
        showStatusMessage(tr("Unable to save adjustments: %1").arg(error), 4000);
    } else {
        m_savingAdjustmentsPending = false;
    }
}

void MainWindow::scheduleAdjustmentPersist()
{
    if (!m_libraryManager || m_currentDevelopAssetId < 0) {
        return;
    }
    m_adjustmentPersistTimer.start();
}

void MainWindow::loadAdjustmentsForAsset(qint64 assetId)
{
    if (!m_libraryManager || assetId < 0) {
        m_currentAdjustments = defaultDevelopAdjustments();
    } else {
        m_currentAdjustments = m_libraryManager->loadDevelopAdjustments(assetId);
    }
    syncAdjustmentControls(m_currentAdjustments);
    m_savingAdjustmentsPending = false;
}

void MainWindow::syncAdjustmentControls(const DevelopAdjustments &adjustments)
{
    auto setSliderSpin = [](QSlider *slider, QDoubleSpinBox *spin, double value, double sliderScale) {
        if (!slider || !spin) {
            return;
        }
        const int sliderMin = slider->minimum();
        const int sliderMax = slider->maximum();
        const double scaled = value * sliderScale;
        const double clampedScaled = std::clamp(scaled,
                                                static_cast<double>(sliderMin),
                                                static_cast<double>(sliderMax));
        const int sliderValue = static_cast<int>(std::round(clampedScaled));
        const double spinMin = spin->minimum();
        const double spinMax = spin->maximum();
        const double spinValue = std::clamp(clampedScaled / sliderScale, spinMin, spinMax);
        {
            QSignalBlocker blockSlider(slider);
            slider->setValue(sliderValue);
        }
        {
            QSignalBlocker blockSpin(spin);
            spin->setValue(spinValue);
        }
    };

    setSliderSpin(ui->exposureSlider, ui->exposureSpinBox, adjustments.exposure, 100.0);
    setSliderSpin(ui->contrastSlider, ui->contrastSpinBox, adjustments.contrast, 1.0);
    setSliderSpin(ui->highlightsSlider, ui->highlightsSpinBox, adjustments.highlights, 1.0);
    setSliderSpin(ui->shadowsSlider, ui->shadowsSpinBox, adjustments.shadows, 1.0);
    setSliderSpin(ui->whitesSlider, ui->whitesSpinBox, adjustments.whites, 1.0);
    setSliderSpin(ui->blacksSlider, ui->blacksSpinBox, adjustments.blacks, 1.0);
    setSliderSpin(ui->claritySlider, ui->claritySpinBox, adjustments.clarity, 1.0);
    setSliderSpin(ui->vibranceSlider, ui->vibranceSpinBox, adjustments.vibrance, 1.0);
    setSliderSpin(ui->saturationSlider, ui->saturationSpinBox, adjustments.saturation, 1.0);

    auto setSliderOnly = [](QSlider *slider, double value, double scale) {
        if (!slider) {
            return;
        }
        const int sliderMin = slider->minimum();
        const int sliderMax = slider->maximum();
        const double scaled = value * scale;
        const double clampedScaled = std::clamp(scaled,
                                                static_cast<double>(sliderMin),
                                                static_cast<double>(sliderMax));
        const int sliderValue = static_cast<int>(std::round(clampedScaled));
        QSignalBlocker blocker(slider);
        slider->setValue(sliderValue);
    };

    setSliderOnly(ui->toneCurveHighlightsSlider, adjustments.toneCurveHighlights, 1.0);
    setSliderOnly(ui->toneCurveLightsSlider, adjustments.toneCurveLights, 1.0);
    setSliderOnly(ui->toneCurveDarksSlider, adjustments.toneCurveDarks, 1.0);
    setSliderOnly(ui->toneCurveShadowsSlider, adjustments.toneCurveShadows, 1.0);

    setSliderOnly(ui->colorHueSlider, adjustments.hueShift, 1.0);
    setSliderOnly(ui->colorSaturationSlider, adjustments.saturationShift, 1.0);
    setSliderOnly(ui->colorLuminanceSlider, adjustments.luminanceShift, 1.0);

    setSliderOnly(ui->sharpeningSlider, adjustments.sharpening, 1.0);
    setSliderOnly(ui->noiseReductionSlider, adjustments.noiseReduction, 1.0);
    setSliderOnly(ui->vignetteSlider, adjustments.vignette, 1.0);
    setSliderOnly(ui->grainSlider, adjustments.grain, 1.0);

    if (ui->toneCurvePresetCombo) {
        int presetIndex = 0;
        if (almostEqual(adjustments.toneCurveHighlights, 25.0) &&
            almostEqual(adjustments.toneCurveLights, 15.0) &&
            almostEqual(adjustments.toneCurveDarks, -15.0) &&
            almostEqual(adjustments.toneCurveShadows, -25.0)) {
            presetIndex = 1;
        } else if (almostEqual(adjustments.toneCurveHighlights, 35.0) &&
                   almostEqual(adjustments.toneCurveLights, 25.0) &&
                   almostEqual(adjustments.toneCurveDarks, -25.0) &&
                   almostEqual(adjustments.toneCurveShadows, -35.0)) {
            presetIndex = 2;
        }
        QSignalBlocker comboBlock(ui->toneCurvePresetCombo);
        ui->toneCurvePresetCombo->setCurrentIndex(presetIndex);
    }
}

void MainWindow::resetAdjustmentsToDefault()
{
    if (adjustmentsAreIdentity(m_currentAdjustments)) {
        return;
    }
    m_currentAdjustments = defaultDevelopAdjustments();
    syncAdjustmentControls(m_currentAdjustments);
    handleAdjustmentChanged();
    showStatusMessage(tr("Adjustments reset"), 2000);
}

void MainWindow::processNextPreviewRegeneration()
{
    if (m_pendingPreviewRegenerations.isEmpty()) {
        // All done - complete the job
        if (m_jobManager && !m_pastePreviewJobId.isNull()) {
            m_jobManager->completeJob(m_pastePreviewJobId, tr("All previews updated"));
            m_pastePreviewJobId = {};
            m_pastePreviewCompleted = 0;
            m_pastePreviewTotal = 0;
        }
        return;
    }

    qint64 assetId = m_pendingPreviewRegenerations.takeFirst();
    const LibraryAsset *asset = assetById(assetId);
    if (!asset) {
        // Skip to next
        m_pastePreviewCompleted++;
        if (m_jobManager && !m_pastePreviewJobId.isNull()) {
            m_jobManager->updateProgress(m_pastePreviewJobId, m_pastePreviewCompleted, m_pastePreviewTotal);
        }
        QTimer::singleShot(0, this, &MainWindow::processNextPreviewRegeneration);
        return;
    }

    const QString originalPath = assetOriginalPath(*asset);
    if (originalPath.isEmpty()) {
        // Skip to next
        m_pastePreviewCompleted++;
        if (m_jobManager && !m_pastePreviewJobId.isNull()) {
            m_jobManager->updateProgress(m_pastePreviewJobId, m_pastePreviewCompleted, m_pastePreviewTotal);
        }
        QTimer::singleShot(0, this, &MainWindow::processNextPreviewRegeneration);
        return;
    }

    // Update job detail with current file
    if (m_jobManager && !m_pastePreviewJobId.isNull()) {
        m_jobManager->updateDetail(m_pastePreviewJobId, tr("Processing %1 (%2 of %3)")
                                   .arg(asset->fileName)
                                   .arg(m_pastePreviewCompleted + 1)
                                   .arg(m_pastePreviewTotal));
    }

    // Process this image asynchronously, then move to next
    QThreadPool::globalInstance()->start([this, assetId, originalPath, fileName = asset->fileName]() {
        QString loadError;
        QImage originalImage = ImageLoader::loadImageWithRawSupport(originalPath, &loadError);
        if (originalImage.isNull()) {
            // Move to next
            QMetaObject::invokeMethod(this, [this]() {
                m_pastePreviewCompleted++;
                if (m_jobManager && !m_pastePreviewJobId.isNull()) {
                    m_jobManager->updateProgress(m_pastePreviewJobId, m_pastePreviewCompleted, m_pastePreviewTotal);
                }
                QTimer::singleShot(0, this, &MainWindow::processNextPreviewRegeneration);
            }, Qt::QueuedConnection);
            return;
        }

        // Apply adjustments to generate preview
        if (!adjustmentsAreIdentity(m_copiedAdjustments)) {
            DevelopAdjustmentRequest request;
            request.requestId = 0;
            request.image = originalImage;
            request.adjustments = m_copiedAdjustments;
            request.isPreview = false;
            request.displayScale = 1.0;

            QFuture<DevelopAdjustmentRenderResult> renderFuture = m_adjustmentEngine->renderAsync(std::move(request));
            renderFuture.waitForFinished();
            const DevelopAdjustmentRenderResult result = renderFuture.result();
            if (!result.cancelled && !result.image.isNull()) {
                QMetaObject::invokeMethod(this, [this, assetId, result]() {
                    schedulePreviewRegeneration(assetId, result.image, m_pastePreviewJobId, [this]() {
                        // Preview saved - update progress and move to next
                        m_pastePreviewCompleted++;
                        if (m_jobManager && !m_pastePreviewJobId.isNull()) {
                            m_jobManager->updateProgress(m_pastePreviewJobId, m_pastePreviewCompleted, m_pastePreviewTotal);
                        }
                        QTimer::singleShot(0, this, &MainWindow::processNextPreviewRegeneration);
                    });
                }, Qt::QueuedConnection);
            } else {
                // Move to next on failure
                QMetaObject::invokeMethod(this, [this]() {
                    m_pastePreviewCompleted++;
                    if (m_jobManager && !m_pastePreviewJobId.isNull()) {
                        m_jobManager->updateProgress(m_pastePreviewJobId, m_pastePreviewCompleted, m_pastePreviewTotal);
                    }
                    QTimer::singleShot(0, this, &MainWindow::processNextPreviewRegeneration);
                }, Qt::QueuedConnection);
            }
        } else {
            // No adjustments, just use original
            QMetaObject::invokeMethod(this, [this, assetId, originalImage]() {
                schedulePreviewRegeneration(assetId, originalImage, m_pastePreviewJobId, [this]() {
                    // Preview saved - update progress and move to next
                    m_pastePreviewCompleted++;
                    if (m_jobManager && !m_pastePreviewJobId.isNull()) {
                        m_jobManager->updateProgress(m_pastePreviewJobId, m_pastePreviewCompleted, m_pastePreviewTotal);
                    }
                    QTimer::singleShot(0, this, &MainWindow::processNextPreviewRegeneration);
                });
            }, Qt::QueuedConnection);
        }
    });
}

void MainWindow::updateThumbnailPreview(qint64 assetId, const QString &previewPath)
{
    if (!m_libraryGridView) {
        return;
    }

    if (!m_assets.isEmpty() && m_libraryManager) {
        const QString libraryPath = m_libraryManager->libraryPath();
        for (LibraryAsset &asset : m_assets) {
            if (asset.id == assetId) {
                if (!libraryPath.isEmpty() && QFileInfo(previewPath).isAbsolute()) {
                    QDir root(libraryPath);
                    asset.previewRelativePath = root.relativeFilePath(previewPath);
                } else {
                    asset.previewRelativePath.clear();
                }
                break;
            }
        }
    }

    m_libraryGridView->updateItemPreview(assetId, previewPath);

    if (ui->developFilmstripList && !previewPath.isEmpty()) {
        for (int row = 0; row < ui->developFilmstripList->count(); ++row) {
            QListWidgetItem *item = ui->developFilmstripList->item(row);
            if (!item) {
                continue;
            }
            if (item->data(Qt::UserRole).toLongLong() == assetId) {
                QImageReader reader(previewPath);
                reader.setAutoTransform(true);
                reader.setScaledSize(ui->developFilmstripList->iconSize());
                const QImage thumbnail = reader.read();
                if (!thumbnail.isNull()) {
                    item->setIcon(QIcon(QPixmap::fromImage(thumbnail)));
                }
                break;
            }
        }
    }
}

void MainWindow::schedulePreviewRegeneration(qint64 assetId, const QImage &sourceImage, const QUuid &parentJobId, std::function<void()> onComplete)
{
    if (!m_libraryManager || sourceImage.isNull()) {
        return;
    }

    const LibraryAsset *asset = assetById(assetId);
    if (!asset) {
        return;
    }

    const QString previewPath = assetPreviewPath(*asset);
    if (previewPath.isEmpty()) {
        return;
    }

    QImage previewImage = sourceImage;
    const int targetSize = 512;
    if (previewImage.width() > targetSize || previewImage.height() > targetSize) {
        previewImage = previewImage.scaled(targetSize,
                                           targetSize,
                                           Qt::KeepAspectRatio,
                                           Qt::SmoothTransformation);
    }

    if (previewImage.isNull()) {
        return;
    }

    // Only create a job if not part of a parent job (e.g., paste operation)
    QUuid jobId;
    if (m_jobManager && parentJobId.isNull()) {
        QString jobDetail = QFileInfo(previewPath).fileName();
        jobId = m_jobManager->startJob(JobCategory::PreviewGeneration,
                                       tr("Updating preview"),
                                       jobDetail);
        m_jobManager->setIndeterminate(jobId, true);
    }

    [[maybe_unused]] QFuture<void> previewFuture = QtConcurrent::run([this,
                                                                      assetId,
                                                                      previewPath,
                                                                      previewImage,
                                                                      jobId,
                                                                      onComplete]() {
        bool success = false;
        QString errorMessage;

        QDir dir = QFileInfo(previewPath).dir();
        if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
            errorMessage = tr("Unable to create preview directory.");
        } else {
            success = previewImage.save(previewPath, "JPG", 90);
            if (!success) {
                errorMessage = tr("Failed to save preview.");
            }
        }

        QMetaObject::invokeMethod(this, [this, assetId, previewPath, success, errorMessage, jobId, onComplete]() {
            // Only complete/fail job if it's a standalone job (not part of paste operation)
            if (m_jobManager && !jobId.isNull()) {
                if (success) {
                    m_jobManager->completeJob(jobId, tr("Preview updated"));
                    qDebug() << "Preview updated";
                } else {
                    m_jobManager->failJob(jobId, errorMessage.isEmpty()
                                                    ? tr("Failed to update preview")
                                                    : errorMessage);
                    qDebug() << "Failed to update preview" << errorMessage;
                }
            }

            if (success) {
                updateThumbnailPreview(assetId, previewPath);
                qDebug() << "Updated Thumbnail";
            }
            
            // Call completion callback if provided
            if (onComplete) {
                onComplete();
            }
        }, Qt::QueuedConnection);
    });
}

void MainWindow::populateDevelopMetadata(const QImage &image, const QString &filePath, const DevelopMetadata &metadata)
{
    QFileInfo info(filePath);
    const QLocale locale;

    if (ui->developImageInfoLabel) {
        ui->developImageInfoLabel->setText(tr("%1 • %2 x %3 • %4")
                                               .arg(info.fileName())
                                               .arg(image.width())
                                               .arg(image.height())
                                               .arg(locale.formattedDataSize(info.size())));
    }

    auto setLabelText = [](QLabel *label, const QString &value) {
        if (label) {
            label->setText(value.isEmpty() ? QStringLiteral("—") : value);
        }
    };

    QString cameraDisplay;
    if (!metadata.cameraMake.isEmpty()) {
        cameraDisplay = metadata.cameraMake.trimmed();
    }
    if (!metadata.cameraModel.isEmpty()) {
        cameraDisplay = cameraDisplay.isEmpty()
                ? metadata.cameraModel.trimmed()
                : QStringLiteral("%1 %2").arg(cameraDisplay, metadata.cameraModel.trimmed());
    }
    setLabelText(ui->developMetadataCameraValue, cameraDisplay);
    setLabelText(ui->developMetadataLensValue, metadata.lens);
    setLabelText(ui->developMetadataIsoValue, metadata.iso);
    setLabelText(ui->developMetadataShutterValue, metadata.shutterSpeed);
    setLabelText(ui->developMetadataApertureValue, metadata.aperture);
    setLabelText(ui->developMetadataFocalLengthValue, metadata.focalLength);
    setLabelText(ui->developMetadataFlashValue, metadata.flash);
    setLabelText(ui->developMetadataFocusDistanceValue, metadata.focusDistance);

    if (ui->developMetadataFileSizeValue) {
        ui->developMetadataFileSizeValue->setText(locale.formattedDataSize(info.size()));
    }
    if (ui->developMetadataResolutionValue) {
        ui->developMetadataResolutionValue->setText(tr("%1 x %2").arg(image.width()).arg(image.height()));
    }
    if (ui->developMetadataCaptureDateValue) {
        QDateTime captured;
        // Use EXIF capture date if available, otherwise fall back to file created date
        if (metadata.captureDateTime.isValid()) {
            captured = metadata.captureDateTime;
        } else {
            captured = info.birthTime().isValid() ? info.birthTime() : info.lastModified();
        }
        ui->developMetadataCaptureDateValue->setText(locale.toString(captured, QLocale::ShortFormat));
    }
}

const LibraryAsset *MainWindow::assetById(qint64 assetId) const
{
    auto it = std::find_if(m_assets.cbegin(), m_assets.cend(), [assetId](const LibraryAsset &asset) {
        return asset.id == assetId;
    });
    if (it == m_assets.cend()) {
        return nullptr;
    }
    return &(*it);
}

void MainWindow::fitDevelopViewToImage()
{
    if (!ui->developImageView || !m_developPixmapItem) {
        return;
    }
    if (m_developPixmapItem->pixmap().isNull()) {
        return;
    }

    m_developFitMode = true;
    ui->developImageView->fitInView(m_developPixmapItem, Qt::KeepAspectRatio);
    m_developZoom = ui->developImageView->transform().m11();

    // Force viewport update after fit
    if (ui->developImageView) {
        ui->developImageView->viewport()->update();
        ui->developImageView->update();
    }

    if (ui->developZoomCombo) {
        QSignalBlocker blocker(ui->developZoomCombo);
        ui->developZoomCombo->setCurrentText(tr("Fit"));
    }
}

void MainWindow::applyDevelopZoomPreset(const QString &preset)
{
    if (!ui->developImageView || !m_developPixmapItem) {
        return;
    }
    if (m_developPixmapItem->pixmap().isNull()) {
        return;
    }

    if (preset.compare(tr("Fit"), Qt::CaseInsensitive) == 0) {
        fitDevelopViewToImage();
        return;
    }

    QString normalized = preset;
    normalized.remove(QRegularExpression(QStringLiteral("[^0-9\\.]")));

    bool ok = false;
    const double percentage = normalized.toDouble(&ok);
    if (!ok || percentage <= 0.0) {
        return;
    }

    m_developFitMode = false;
    m_developZoom = percentage / 100.0;

    QTransform transform;
    transform.scale(m_developZoom, m_developZoom);
    ui->developImageView->setTransform(transform);

    // Force viewport update after zoom
    if (ui->developImageView) {
        ui->developImageView->viewport()->update();
        ui->developImageView->update();
    }

    if (ui->developViewerStack && ui->developImageViewPage) {
        ui->developViewerStack->setCurrentWidget(ui->developImageViewPage);
    }
}

QString MainWindow::assetPreviewPath(const LibraryAsset &asset) const
{
    if (!m_libraryManager || asset.previewRelativePath.isEmpty()) {
        return {};
    }
    return m_libraryManager->resolvePath(asset.previewRelativePath);
}

QString MainWindow::assetOriginalPath(const LibraryAsset &asset) const
{
    if (!m_libraryManager || asset.originalRelativePath.isEmpty()) {
        return {};
    }
    return m_libraryManager->resolvePath(asset.originalRelativePath);
}

void MainWindow::showStatusMessage(const QString &message, int timeoutMs)
{
    if (statusBar()) {
        statusBar()->showMessage(message, timeoutMs);
    }
}

void MainWindow::openOrCreateDefaultLibrary()
{
    if (!m_libraryManager) {
        return;
    }

    const QString picturesDir = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    if (picturesDir.isEmpty()) {
        qWarning() << "Unable to determine Pictures directory for default library.";
        showStatusMessage(tr("Unable to determine default library location."), 4000);
        return;
    }

    QDir picturesPath(picturesDir);
    if (!picturesPath.exists()) {
        if (!picturesPath.mkpath(QStringLiteral("."))) {
            qWarning() << "Unable to create Pictures directory for default library at" << picturesDir;
            showStatusMessage(tr("Unable to prepare default library directory."), 4000);
            return;
        }
    }

    const QString defaultLibraryName = tr("Photoroom Library");
    const QString defaultLibraryPath = picturesPath.filePath(defaultLibraryName);

    QString openError;
    if (m_libraryManager->openLibrary(defaultLibraryPath, &openError)) {
        if (ui->stackedWidget && ui->libraryPage) {
            ui->stackedWidget->setCurrentWidget(ui->libraryPage);
        }
        return;
    }

    QString createError;
    if (m_libraryManager->createLibrary(defaultLibraryPath, &createError)) {
        showStatusMessage(tr("Created default library at %1")
                              .arg(QDir::toNativeSeparators(defaultLibraryPath)), 5000);
        if (ui->stackedWidget && ui->libraryPage) {
            ui->stackedWidget->setCurrentWidget(ui->libraryPage);
        }
        return;
    }

    const QString failureReason = !createError.isEmpty() ? createError : openError;
    if (!failureReason.isEmpty()) {
        qWarning() << "Failed to prepare default library:" << failureReason;
        QMessageBox::warning(this,
                             tr("Default library unavailable"),
                             tr("Could not prepare the default library.\n%1").arg(failureReason));
    }
}

void MainWindow::clearLibrary()
{
    m_assets.clear();
    if (m_libraryGridView) {
        m_libraryGridView->clear();
    }
    if (ui->developFilmstripList) {
        ui->developFilmstripList->clear();
    }
    clearDevelopView();
}

void MainWindow::openAssetInDevelop(qint64 assetId, const QString &filePath)
{
    if (filePath.isEmpty()) {
        QMessageBox::warning(this,
                             tr("Unable to open image"),
                             tr("The selected asset does not have an original file path."));
        return;
    }

    if (m_savingAdjustmentsPending && m_currentDevelopAssetId >= 0) {
        persistCurrentAdjustments();
    }
    m_adjustmentPersistTimer.stop();
    m_fullRenderTimer.stop();
    if (m_adjustmentEngine) {
        m_adjustmentEngine->cancelActive();
    }
    m_currentDevelopAdjustedValid = false;
    m_currentDevelopOriginalImage = QImage();
    m_currentDevelopAdjustedImage = QImage();
    m_currentDevelopPreviewImage = QImage();
    m_currentDevelopPreviewScale = 1.0;
    m_previewRenderEnabled = false;
    m_latestPreviewRequestId = 0;
    m_latestFullRequestId = 0;

    ensureDevelopViewInitialized();

    if (!m_developScene || !m_developPixmapItem) {
        qWarning() << "Develop view is not initialized.";
        return;
    }

    selectFilmstripItem(assetId);

    const QString displayName = QFileInfo(filePath).fileName();
    showDevelopLoadingState(tr("Loading %1…").arg(displayName));

    if (m_jobManager) {
        if (!m_activeDevelopJobId.isNull()) {
            m_jobManager->cancelJob(m_activeDevelopJobId, tr("Superseded by a new selection"));
        }
        m_activeDevelopJobId = m_jobManager->startJob(JobCategory::Develop,
                                                      tr("Preparing %1").arg(displayName),
                                                      tr("Loading original file"));
        m_jobManager->setIndeterminate(m_activeDevelopJobId, true);
    }

    if (const LibraryAsset *asset = assetById(assetId)) {
        const QString previewPath = assetPreviewPath(*asset);
        if (!previewPath.isEmpty()) {
            QPixmap previewPixmap;

            QImageReader previewReader(previewPath);
            previewReader.setAutoTransform(true);
            const QImage previewImage = previewReader.read();
            if (!previewImage.isNull()) {
                previewPixmap = QPixmap::fromImage(previewImage);
            } else {
                previewPixmap.load(previewPath);
            }

            if (!previewPixmap.isNull()) {
                showDevelopPreview(previewPixmap);
            }
        }
    }

    m_pendingDevelopFilePath = filePath;
    const int requestId = ++m_pendingDevelopRequestId;

    QStringList preloadTargets;
    preloadTargets.append(filePath);
    int currentIndex = -1;
    for (int i = 0; i < m_assets.size(); ++i) {
        if (m_assets.at(i).id == assetId) {
            currentIndex = i;
            break;
        }
    }
    if (currentIndex >= 0) {
        for (int offset = -3; offset <= 3; ++offset) {
            if (offset == 0) {
                continue;
            }
            const int neighborIndex = currentIndex + offset;
            if (neighborIndex < 0 || neighborIndex >= m_assets.size()) {
                continue;
            }
            const QString neighborPath = assetOriginalPath(m_assets.at(neighborIndex));
            if (!neighborPath.isEmpty()) {
                preloadTargets.append(neighborPath);
            }
        }
    }
    ImageLoader::preloadAsync(preloadTargets);

    auto future = QtConcurrent::run(loadDevelopImageAsync, requestId, assetId, filePath);
    m_imageLoadWatcher->setFuture(future);
}

void MainWindow::handleDevelopImageLoaded()
{
    if (!m_imageLoadWatcher) {
        return;
    }

    const DevelopImageLoadResult result = m_imageLoadWatcher->result();
    if (result.requestId != m_pendingDevelopRequestId ||
        result.filePath != m_pendingDevelopFilePath) {
        return;
    }

    if (!result.errorMessage.isEmpty() || result.image.isNull()) {
        if (m_jobManager && !m_activeDevelopJobId.isNull()) {
            const QString errorText = result.errorMessage.isEmpty()
                    ? tr("Failed to load image.")
                    : result.errorMessage;
            m_jobManager->failJob(m_activeDevelopJobId, errorText);
            m_activeDevelopJobId = {};
        }
        QMessageBox::warning(this,
                             tr("Unable to open image"),
                             tr("Could not open %1.\n%2")
                                 .arg(QFileInfo(result.filePath).fileName(),
                                      result.errorMessage));
        clearDevelopView();
        return;
    }

    m_currentDevelopAssetId = result.assetId;
    selectFilmstripItem(result.assetId);

    m_currentDevelopOriginalImage = result.image;
    m_currentDevelopAdjustedImage = QImage();
    m_currentDevelopAdjustedValid = false;
    m_developFitMode = true;
    m_currentDevelopPreviewImage = QImage();
    m_currentDevelopPreviewScale = 1.0;
    m_previewRenderEnabled = false;
    m_fullRenderTimer.stop();

    loadAdjustmentsForAsset(result.assetId);

    const bool identityAdjustments = adjustmentsAreIdentity(m_currentAdjustments);
    if (identityAdjustments) {
        m_currentDevelopAdjustedImage = result.image;
        m_currentDevelopAdjustedValid = true;
        applyDevelopImage(result.image, true, false, 1.0);
        // Update thumbnail with original image (no adjustments)
        schedulePreviewRegeneration(result.assetId, result.image);
    } else {
        requestAdjustmentRender(true);
    }

    populateDevelopMetadata(result.image, result.filePath, result.metadata);

    if (m_jobManager && !m_activeDevelopJobId.isNull()) {
        m_jobManager->completeJob(m_activeDevelopJobId, tr("Ready for Develop"));
        m_activeDevelopJobId = {};
    }

    if (ui->developZoomCombo) {
        QSignalBlocker blocker(ui->developZoomCombo);
        ui->developZoomCombo->setCurrentText(tr("Fit"));
    }

    showStatusMessage(tr("Loaded %1").arg(QFileInfo(result.filePath).fileName()), 2000);
    m_pendingDevelopFilePath.clear();
}

void MainWindow::handleSelectionChanged(const QList<qint64> &selection)
{
    if (selection.isEmpty()) {
        showStatusMessage(tr("No items selected"), 1500);
        return;
    }

    if (selection.size() == 1) {
        showStatusMessage(tr("1 item selected"), 1500);
    } else {
        showStatusMessage(tr("%1 items selected").arg(selection.size()), 1500);
    }
}


MainWindow::~MainWindow()
{
    clearLibrary();
    delete ui;
}

void MainWindow::resizeEvent(QResizeEvent *event)
{
    QMainWindow::resizeEvent(event);

    if (!m_developFitMode) {
        return;
    }
    if (!ui->developImageView || !m_developPixmapItem) {
        return;
    }
    if (m_developPixmapItem->pixmap().isNull()) {
        return;
    }

    fitDevelopViewToImage();
}

// Menubar
void MainWindow::on_actionExit_triggered()
{
    qDebug() << "Exit clicked!";
    QApplication::quit();
}

void MainWindow::on_actionNew_Library_triggered(){
    qDebug() << "New Library clicked!";

    if (!m_libraryManager) {
        QMessageBox::warning(this,
                             tr("Library unavailable"),
                             tr("The library manager is not initialized."));
        return;
    }

    const QString picturesDir = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    if (picturesDir.isEmpty()) {
        QMessageBox::warning(this,
                             tr("Unable to create library"),
                             tr("Could not determine the Pictures directory."));
        return;
    }

    QDir picturesPath(picturesDir);
    if (!picturesPath.exists() && !picturesPath.mkpath(QStringLiteral("."))) {
        QMessageBox::warning(this,
                             tr("Unable to create library"),
                             tr("Could not access or create the Pictures directory."));
        return;
    }

    const QString baseName = tr("Photoroom Library");
    QString targetPath = picturesPath.filePath(baseName);

    int suffix = 1;
    while (QDir(targetPath).exists()) {
        targetPath = picturesPath.filePath(QStringLiteral("%1_%2")
                                           .arg(baseName)
                                           .arg(suffix++));
    }

    QString errorMessage;
    if (!m_libraryManager->createLibrary(targetPath, &errorMessage)) {
        if (errorMessage.isEmpty()) {
            errorMessage = tr("Failed to create a new library at %1.").arg(QDir::toNativeSeparators(targetPath));
        }
        QMessageBox::warning(this,
                             tr("Unable to create library"),
                             errorMessage);
        return;
    }

    showStatusMessage(tr("Created library at %1").arg(QDir::toNativeSeparators(targetPath)), 4000);

    if (ui->stackedWidget && ui->libraryPage) {
        ui->stackedWidget->setCurrentWidget(ui->libraryPage);
    }
}
void MainWindow::on_actionOpen_Library_triggered()
{
    qDebug() << "Open Library clicked!";

    QString folder = QFileDialog::getExistingDirectory(this, "Open Image Library");
    if (!folder.isEmpty()) {
        if (!m_libraryManager) {
            QMessageBox::warning(this,
                                 tr("Library unavailable"),
                                 tr("The library manager is not initialized."));
            return;
        }

        QString errorMessage;
        if (!m_libraryManager->openLibrary(folder, &errorMessage)) {
            if (errorMessage.isEmpty()) {
                errorMessage = tr("Failed to open the selected library.");
            }
            QMessageBox::warning(this,
                                 tr("Unable to open library"),
                                 errorMessage);
            return;
        }

        if (ui->stackedWidget && ui->libraryPage) {
            ui->stackedWidget->setCurrentWidget(ui->libraryPage);
        }
    }
}

void MainWindow::on_actionClear_recents_triggered(){
    qDebug() << "Clear recents clicked!";
}

void MainWindow::on_actionUndo_triggered(){
    qDebug() << "Undo clicked!";
}
void MainWindow::on_actionRedo_triggered(){
    qDebug() << "Redo clicked!";
}

void MainWindow::on_actionCut_triggered(){
    qDebug() << "Cut clicked!";
}
void MainWindow::on_actionCopy_triggered(){
    if (m_currentDevelopAssetId < 0) {
        showStatusMessage(tr("No image selected to copy adjustments from"), 2000);
        return;
    }

    m_copiedAdjustments = m_currentAdjustments;
    m_hasCopiedAdjustments = true;

    const LibraryAsset *asset = assetById(m_currentDevelopAssetId);
    QString fileName = asset ? asset->fileName : tr("image");
    showStatusMessage(tr("Copied adjustments from %1").arg(fileName), 3000);
}
void MainWindow::on_actionPaste_triggered(){
    if (!m_hasCopiedAdjustments) {
        showStatusMessage(tr("No adjustments copied. Copy adjustments from an image first."), 3000);
        return;
    }

    QList<qint64> selectedIds;
    
    // Check which page is currently active
    bool isDevelopPage = false;
    if (ui->stackedWidget) {
        isDevelopPage = (ui->stackedWidget->currentWidget() == ui->developPage);
    }

    // If on Develop page and have a current image, paste to that image only
    if (isDevelopPage && m_currentDevelopAssetId >= 0) {
        selectedIds.append(m_currentDevelopAssetId);
        qDebug() << "Pasting to current Develop image:" << m_currentDevelopAssetId;
    } else {
        // Otherwise, paste to all selected images in library
        if (m_libraryGridView) {
            selectedIds = m_libraryGridView->selectedAssetIds();
        }
        
        if (selectedIds.isEmpty()) {
            showStatusMessage(tr("No images selected. Select images in the library or open an image in Develop."), 3000);
            return;
        }
        qDebug() << "Pasting to" << selectedIds.size() << "selected library image(s)";
    }

    int successCount = 0;
    int failCount = 0;
    QList<qint64> successfulIds;
    bool pastedToCurrentImage = false;

    qDebug() << "Pasting adjustments to" << selectedIds.size() << "image(s)";

    for (qint64 assetId : selectedIds) {
        QString error;
        if (m_libraryManager && m_libraryManager->saveDevelopAdjustments(assetId, m_copiedAdjustments, &error)) {
            successCount++;
            successfulIds.append(assetId);
            qDebug() << "Successfully pasted adjustments to asset" << assetId;
            if (assetId == m_currentDevelopAssetId) {
                pastedToCurrentImage = true;
            }
        } else {
            failCount++;
            qWarning() << "Failed to paste adjustments to asset" << assetId << ":" << error;
        }
    }

    qDebug() << "Paste complete: successCount=" << successCount << "failCount=" << failCount;

    // If pasting to current image, directly set adjustments and re-render
    if (pastedToCurrentImage && m_currentDevelopAssetId >= 0) {
        // Directly set the adjustments instead of reloading from database
        m_currentAdjustments = m_copiedAdjustments;
        syncAdjustmentControls(m_currentAdjustments);
        m_savingAdjustmentsPending = false;
        // Clear any pending adjustment persist timer since we just saved
        m_adjustmentPersistTimer.stop();
        
        // Stop timer
        m_fullRenderTimer.stop();
        
        // Clear cached adjusted image to force fresh render
        m_currentDevelopAdjustedImage = QImage();
        m_currentDevelopAdjustedValid = false;
        m_currentDevelopPreviewImage = QImage();
        m_currentDevelopPreviewScale = 1.0;
        m_previewRenderEnabled = false;
        
        // Force immediate render with the already-loaded original image
        // This applies adjustments without reloading from disk
        // Use QTimer to ensure any pending operations complete before starting render
        if (!m_currentDevelopOriginalImage.isNull()) {
            qDebug() << "Pasting: Scheduling render for current image, assetId:" << m_currentDevelopAssetId;
            // Use a small delay to ensure cancellation completes and state is clean
            QTimer::singleShot(10, this, [this]() {
                qDebug() << "Pasting: Starting render after delay";
                requestAdjustmentRender(true, false);
            });
        } else {
            qWarning() << "Pasting: Cannot render - original image is null";
        }
    }

    // Trigger preview regeneration for all successfully pasted images
    // Process sequentially to avoid loading all images at once
    if (successCount > 0 && m_adjustmentEngine && !successfulIds.isEmpty()) {
        // Create a single job to track all preview regenerations
        if (m_jobManager) {
            m_pastePreviewJobId = m_jobManager->startJob(JobCategory::PreviewGeneration,
                                                         tr("Updating previews"),
                                                         tr("Regenerating %1 preview(s)").arg(successfulIds.size()));
            m_pastePreviewTotal = successfulIds.size();
            m_pastePreviewCompleted = 0;
            m_jobManager->updateProgress(m_pastePreviewJobId, 0, m_pastePreviewTotal);
        }
        
        // Store the list and process one at a time
        m_pendingPreviewRegenerations = successfulIds;
        
        // Start processing the first image
        QTimer::singleShot(0, this, &MainWindow::processNextPreviewRegeneration);
    }

    // Refresh library view to show updated previews
    if (successCount > 0 && m_libraryGridView) {
        m_libraryGridView->viewport()->update();
    }

    if (successCount > 0 && failCount == 0) {
        if (successCount == 1) {
            showStatusMessage(tr("Pasted adjustments to 1 image"), 3000);
        } else {
            showStatusMessage(tr("Pasted adjustments to %1 images").arg(successCount), 3000);
        }
    } else if (successCount > 0 && failCount > 0) {
        showStatusMessage(tr("Pasted to %1 image(s), %2 failed").arg(successCount).arg(failCount), 4000);
    } else {
        showStatusMessage(tr("Failed to paste adjustments"), 3000);
    }
}

void MainWindow::on_actionSelect_All_triggered(){
    qDebug() << "Select All clicked!";
}
void MainWindow::on_actionSelect_None_triggered(){
    qDebug() << "Select None clicked!";
}
void MainWindow::on_actionInverse_Selection_triggered(){
    qDebug() << "Inverse Selection clicked!";
}

void MainWindow::on_actionPreferences_triggered()
{
    qDebug() << "Preferences menu clicked!";

    PreferencesDialog dialog(this);
    dialog.exec();  // or dialog.show() for non-modal
}

void MainWindow::on_actionExport_triggered()
{
    if (!m_libraryManager || !m_libraryManager->hasOpenLibrary()) {
        QMessageBox::information(this,
                                 tr("No open library"),
                                 tr("Open a library before exporting files."));
        return;
    }

    persistCurrentAdjustments();

    QVector<ExportItem> candidateItems;
    candidateItems.reserve(32);
    QSet<QString> seenPaths;

    if (m_libraryGridView) {
        const QList<qint64> selection = m_libraryGridView->selectedAssetIds();
        for (qint64 assetId : selection) {
            const LibraryAsset *asset = assetById(assetId);
            if (!asset) {
                continue;
            }
            const QString originalPath = assetOriginalPath(*asset);
            if (originalPath.isEmpty() || seenPaths.contains(originalPath)) {
                continue;
            }

            ExportItem item;
            item.assetId = assetId;
            item.sourcePath = originalPath;
            item.adjustments = m_libraryManager ? m_libraryManager->loadDevelopAdjustments(assetId)
                                                : defaultDevelopAdjustments();
            item.identity = adjustmentsAreIdentity(item.adjustments);
            candidateItems.append(item);
            seenPaths.insert(originalPath);
        }
    }

    if (candidateItems.isEmpty() && m_currentDevelopAssetId >= 0) {
        const LibraryAsset *asset = assetById(m_currentDevelopAssetId);
        if (asset) {
            const QString originalPath = assetOriginalPath(*asset);
            if (!originalPath.isEmpty() && !seenPaths.contains(originalPath)) {
                ExportItem item;
                item.assetId = m_currentDevelopAssetId;
                item.sourcePath = originalPath;
                item.adjustments = m_libraryManager ? m_libraryManager->loadDevelopAdjustments(m_currentDevelopAssetId)
                                                    : defaultDevelopAdjustments();
                item.identity = adjustmentsAreIdentity(item.adjustments);
                candidateItems.append(item);
                seenPaths.insert(originalPath);
            }
        }
    }

    if (candidateItems.isEmpty()) {
        QMessageBox::information(this,
                                 tr("No images selected"),
                                 tr("Select one or more images in the library to export."));
        return;
    }

    if (m_exportInProgress) {
        QMessageBox::information(this,
                                 tr("Export already running"),
                                 tr("Please wait for the current export to finish before starting a new one."));
        return;
    }

    QStringList candidatePaths;
    candidatePaths.reserve(candidateItems.size());
    for (const ExportItem &item : std::as_const(candidateItems)) {
        candidatePaths.append(item.sourcePath);
    }

    ExportDialog dialog(this);
    dialog.setImageList(candidatePaths, true);

    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    const QStringList selectedPathList = dialog.selectedImages();
    if (selectedPathList.isEmpty()) {
        showStatusMessage(tr("No images selected for export."), 3000);
        return;
    }

    QSet<QString> selectedPathSet;
    selectedPathSet.reserve(selectedPathList.size());
    for (const QString &path : selectedPathList) {
        selectedPathSet.insert(path);
    }

    QVector<ExportItem> selectedItems;
    selectedItems.reserve(selectedPathSet.size());
    for (const ExportItem &item : std::as_const(candidateItems)) {
        if (selectedPathSet.contains(item.sourcePath)) {
            selectedItems.append(item);
        }
    }

    if (selectedItems.isEmpty()) {
        showStatusMessage(tr("No images selected for export."), 3000);
        return;
    }

    QString initialDir = m_lastExportDirectory;
    if (initialDir.isEmpty()) {
        const QString pictures = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
        if (!pictures.isEmpty()) {
            initialDir = pictures;
        } else {
            initialDir = QDir::homePath();
        }
    }

    const QString selectedDirectory = QFileDialog::getExistingDirectory(this,
                                                                        tr("Select Export Folder"),
                                                                        initialDir);
    if (selectedDirectory.isEmpty()) {
        return;
    }

    QString destinationDir = selectedDirectory;
    if (dialog.createSubfolder()) {
        QDir baseDir(destinationDir);
        const QString subfolderName = QStringLiteral("Export_%1")
                                          .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss")));
        if (!baseDir.mkpath(subfolderName)) {
            QMessageBox::warning(this,
                                 tr("Unable to create folder"),
                                 tr("Could not create export subfolder \"%1\".").arg(subfolderName));
            return;
        }
        destinationDir = baseDir.filePath(subfolderName);
    }

    QDir destination(destinationDir);
    if (!destination.exists()) {
        if (!destination.mkpath(QStringLiteral("."))) {
            QMessageBox::warning(this,
                                 tr("Unable to prepare folder"),
                                 tr("Could not create export folder \"%1\".")
                                     .arg(QDir::toNativeSeparators(destinationDir)));
            return;
        }
    }

    m_lastExportDirectory = selectedDirectory;

    const QString format = dialog.exportFormat();
    const QString extension = exportExtensionForFormat(format);
    const bool qualityEnabled = dialog.isQualityEnabled();
    const int quality = qualityEnabled ? dialog.quality() : 100;
    const QString namingMode = dialog.namingMode();
    const QString customPattern = dialog.customPattern();
    const int sequenceStart = dialog.sequenceStart();
    const int sequencePadding = qMax(1, dialog.sequencePadding());
    const QString customSuffix = dialog.customSuffix();

    const int totalCount = selectedItems.size();

    m_exportInProgress = true;

    QUuid jobId;
    if (m_jobManager) {
        const QString detail = tr("%1 file(s) to %2")
                                   .arg(totalCount)
                                   .arg(format.toUpper());
        jobId = m_jobManager->startJob(JobCategory::Export,
                                       tr("Exporting photos"),
                                       detail);
        if (totalCount > 0) {
            m_jobManager->updateProgress(jobId, 0, totalCount);
        } else {
            m_jobManager->setIndeterminate(jobId, true);
        }
    }
    m_activeExportJobId = jobId;

    auto report = QSharedPointer<ExportTaskReport>::create();
    report->destinationDir = destinationDir;

    QPointer<JobManager> jobManagerPtr(m_jobManager);

    auto future = QtConcurrent::run([items = selectedItems,
                                     report,
                                     destinationDir,
                                     format,
                                     extension,
                                     quality,
                                     qualityEnabled,
                                     namingMode,
                                     customPattern,
                                     sequenceStart,
                                     sequencePadding,
                                     customSuffix,
                                     jobId,
                                     jobManagerPtr]() {
        QDir destDir(destinationDir);
        destDir.mkpath(QStringLiteral("."));
        QSet<QString> usedBaseNames;

        std::unique_ptr<DevelopAdjustmentEngine> engine;

        for (int index = 0; index < items.size(); ++index) {
            const ExportItem &exportItem = items.at(index);
            const QString &sourcePath = exportItem.sourcePath;
            const QFileInfo sourceInfo(sourcePath);

            QString baseName = generateExportBaseName(sourceInfo,
                                                      index,
                                                      namingMode,
                                                      customPattern,
                                                      sequenceStart,
                                                      sequencePadding,
                                                      customSuffix);
            baseName = sanitizeFileName(baseName);
            const QString fileName = ensureUniqueFileName(baseName, extension, usedBaseNames, destDir);
            const QString outputPath = destDir.absoluteFilePath(fileName);

            QString loadError;
            QImage image = ImageLoader::loadImageWithRawSupport(sourcePath, &loadError);
            if (image.isNull()) {
                if (loadError.isEmpty()) {
                    loadError = QObject::tr("Failed to load \"%1\".").arg(QDir::toNativeSeparators(sourcePath));
                }
                report->success = false;
                report->errorMessage = loadError;
                break;
            }

            if (!exportItem.identity) {
                if (!engine) {
                    engine = std::make_unique<DevelopAdjustmentEngine>();
                }

                DevelopAdjustmentRequest request;
                request.requestId = index + 1;
                request.image = image;
                request.adjustments = exportItem.adjustments;
                request.isPreview = false;
                request.displayScale = 1.0;

                QFuture<DevelopAdjustmentRenderResult> renderFuture = engine->renderAsync(std::move(request));
                renderFuture.waitForFinished();
                const DevelopAdjustmentRenderResult result = renderFuture.result();
                if (result.cancelled || result.image.isNull()) {
                    report->success = false;
                    report->errorMessage = QObject::tr("Failed to apply adjustments for \"%1\".")
                                               .arg(QDir::toNativeSeparators(sourcePath));
                    break;
                }
                image = result.image;
            }

            if (format == QStringLiteral("jpeg") && image.format() == QImage::Format_Indexed8) {
                image = image.convertToFormat(QImage::Format_RGB888);
            }

            QImageWriter writer(outputPath, format.toUtf8());
            if (qualityEnabled) {
                writer.setQuality(quality);
            } else {
                writer.setQuality(100);
            }

            if (!writer.write(image)) {
                QString err = writer.errorString();
                if (err.isEmpty()) {
                    err = QObject::tr("Unknown error");
                }
                report->success = false;
                report->errorMessage = QObject::tr("Failed to export \"%1\": %2")
                                           .arg(QDir::toNativeSeparators(outputPath), err);
                break;
            }

            report->exportedFiles.append(outputPath);

            if (jobManagerPtr && !jobId.isNull()) {
                const int completed = index + 1;
                const int total = items.size();
                const QString detail = QObject::tr("%1 of %2 files").arg(completed).arg(total);
                QMetaObject::invokeMethod(jobManagerPtr, [jobManagerPtr, jobId, completed, total, detail]() {
                    if (!jobManagerPtr) {
                        return;
                    }
                    jobManagerPtr->updateDetail(jobId, detail);
                    jobManagerPtr->updateProgress(jobId, completed, total);
                }, Qt::QueuedConnection);
            }
        }

        if (!jobManagerPtr || jobId.isNull()) {
            return;
        }

        if (report->success) {
            QMetaObject::invokeMethod(jobManagerPtr, [jobManagerPtr, jobId, count = report->exportedFiles.size()]() {
                if (!jobManagerPtr) {
                    return;
                }
                jobManagerPtr->completeJob(jobId, QObject::tr("%1 file(s)").arg(count));
            }, Qt::QueuedConnection);
        } else {
            QMetaObject::invokeMethod(jobManagerPtr, [jobManagerPtr, jobId, message = report->errorMessage]() {
                if (!jobManagerPtr) {
                    return;
                }
                jobManagerPtr->failJob(jobId, message);
            }, Qt::QueuedConnection);
        }
    });

    auto *watcher = new QFutureWatcher<void>(this);
    connect(watcher, &QFutureWatcher<void>::finished, this, [this, watcher, report]() {
        watcher->deleteLater();
        m_exportInProgress = false;
        m_activeExportJobId = {};

        if (report->success) {
            showStatusMessage(tr("Exported %1 file(s) to %2")
                                  .arg(report->exportedFiles.size())
                                  .arg(QDir::toNativeSeparators(report->destinationDir)),
                              5000);
        } else {
            if (!report->errorMessage.isEmpty()) {
                QMessageBox::warning(this,
                                     tr("Export failed"),
                                     report->errorMessage);
            }
            showStatusMessage(tr("Export failed"), 4000);
        }
    });
    watcher->setFuture(future);
    showStatusMessage(tr("Export started…"), 2000);
}

void MainWindow::on_actionImport_triggered()
{
    if (!m_libraryManager || !m_libraryManager->hasOpenLibrary()) {
        QMessageBox::information(this,
                                 tr("No open library"),
                                 tr("Open a library before importing files."));
        return;
    }

    const QString startDir = m_libraryManager->libraryPath();
    const QString filter = tr("Images (*.png *.jpg *.jpeg *.bmp *.gif *.tif *.tiff *.webp *.heic *.heif *.raw *.cr2 *.nef *.arw *.dng);;All Files (*.*)");
    QStringList files = QFileDialog::getOpenFileNames(this,
                                                      tr("Import Files"),
                                                      startDir,
                                                      filter);
    if (files.isEmpty()) {
        return;
    }

    if (m_jobManager) {
        if (m_importJobActive && !m_activeImportJobId.isNull()) {
            m_jobManager->cancelJob(m_activeImportJobId, tr("Import superseded"));
        }
        const QString detail = tr("%1 items").arg(files.size());
        m_activeImportJobId = m_jobManager->startJob(JobCategory::Import,
                                                     tr("Importing photos"),
                                                     detail);
        if (files.size() > 0) {
            m_jobManager->updateProgress(m_activeImportJobId, 0, files.size());
        } else {
            m_jobManager->setIndeterminate(m_activeImportJobId, true);
        }
        m_importJobActive = true;
        m_activeImportTotal = files.size();
    }

    m_libraryManager->importFiles(files);
}

namespace {
QStringList findPhotoFilesRecursively(const QString &folderPath)
{
    QStringList result;
    QDir dir(folderPath);
    if (!dir.exists()) {
        return result;
    }

    // Get supported extensions from ImageLoader
    QStringList nameFilters = ImageLoader::supportedNameFilters();
    
    // Recursively find all files matching the filters
    QDirIterator it(folderPath, nameFilters, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        result.append(it.next());
    }

    return result;
}
}

void MainWindow::handleFolderDropped(const QString &folderPath)
{
    if (!m_libraryManager || !m_libraryManager->hasOpenLibrary()) {
        QMessageBox::information(this,
                                 tr("No open library"),
                                 tr("Open a library before importing files."));
        return;
    }

    // Recursively find all photo files in the folder
    QStringList files = findPhotoFilesRecursively(folderPath);
    
    if (files.isEmpty()) {
        QMessageBox::information(this,
                                tr("No photos found"),
                                tr("No supported photo files were found in the selected folder."));
        return;
    }

    // Show preview dialog
    ImportPreviewDialog dialog(files, this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    QStringList filesToImport = dialog.selectedFiles();
    if (filesToImport.isEmpty()) {
        return;
    }

    // Start import job
    if (m_jobManager) {
        if (m_importJobActive && !m_activeImportJobId.isNull()) {
            m_jobManager->cancelJob(m_activeImportJobId, tr("Import superseded"));
        }
        const QString detail = tr("%1 items").arg(filesToImport.size());
        m_activeImportJobId = m_jobManager->startJob(JobCategory::Import,
                                                     tr("Importing photos"),
                                                     detail);
        if (filesToImport.size() > 0) {
            m_jobManager->updateProgress(m_activeImportJobId, 0, filesToImport.size());
        } else {
            m_jobManager->setIndeterminate(m_activeImportJobId, true);
        }
        m_importJobActive = true;
        m_activeImportTotal = filesToImport.size();
    }

    m_libraryManager->importFiles(filesToImport);
}
