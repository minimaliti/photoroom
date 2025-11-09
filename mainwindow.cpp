#include "mainwindow.h"
#include "./ui_mainwindow.h"
#include "preferencesdialog.h"
#include "librarygridview.h"
#include "histogramwidget.h"

#include <QAction>
#include <QAbstractItemView>
#include <QColor>
#include <QComboBox>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QGraphicsBlurEffect>
#include <QGraphicsView>
#include <QImage>
#include <QImageReader>
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
#include <QTransform>
#include <QColorSpace>
#include <QtConcurrent/QtConcurrentRun>
#include <QtGlobal>
#include <QFuture>
#include <algorithm>
#include <cmath>
#include <utility>
#include <QPixmap>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QStandardPaths>
#include <QToolBar>
#include <QVBoxLayout>
#include <QWidget>

#include "imageloader.h"

namespace {

constexpr int kHistogramBins = 256;
constexpr int kHistogramTargetSampleCount = 750000;

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
    if (image.format() != QImage::Format_RGBA8888 &&
        image.format() != QImage::Format_RGB888 &&
        image.format() != QImage::Format_RGB32) {
        image = image.convertToFormat(QImage::Format_RGB32);
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

    const bool isRgb32 = image.format() == QImage::Format_RGB32;
    const bool isRgb888 = image.format() == QImage::Format_RGB888;

    int sampleCount = 0;
    for (int y = 0; y < height; y += strideStep) {
        const uchar *line = image.constScanLine(y);
        if (!line) {
            continue;
        }

        for (int x = 0; x < width; x += strideStep) {
            int r = 0;
            int g = 0;
            int b = 0;

            if (isRgb32) {
                const QRgb *pixels = reinterpret_cast<const QRgb *>(line);
                const QRgb pixel = pixels[x];
                r = qRed(pixel);
                g = qGreen(pixel);
                b = qBlue(pixel);
            } else if (isRgb888) {
                const uchar *pixel = line + (x * 3);
                r = pixel[0];
                g = pixel[1];
                b = pixel[2];
            } else {
                const QRgb *pixels = reinterpret_cast<const QRgb *>(line);
                const QRgb pixel = pixels[x];
                r = qRed(pixel);
                g = qGreen(pixel);
                b = qBlue(pixel);
            }

            const int luminance = qBound(0, qGray(r, g, b), 255);

            histogram.red[r]++;
            histogram.green[g]++;
            histogram.blue[b]++;
            histogram.luminance[luminance]++;

            ++sampleCount;

            histogram.maxValue = std::max({histogram.maxValue,
                                           histogram.red[r],
                                           histogram.green[g],
                                           histogram.blue[b],
                                           histogram.luminance[luminance]});
        }
    }

    histogram.totalSamples = sampleCount;
    return histogram;
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

    m_libraryManager = new LibraryManager(this);
    bindLibrarySignals();

    m_libraryGridView = new LibraryGridView(this);
    connect(m_libraryGridView, &LibraryGridView::assetActivated,
            this, &MainWindow::openAssetInDevelop);
    connect(m_libraryGridView, &LibraryGridView::selectionChanged,
            this, &MainWindow::handleSelectionChanged);

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

    if (ui->developResetAdjustmentsButton) {
        connect(ui->developResetAdjustmentsButton, &QPushButton::clicked, this, [this]() {
            showStatusMessage(tr("Reset adjustments (not yet implemented)"), 2000);
        });
    }

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
    if (m_developScene || !ui->developImageView) {
        initializeDevelopHistogram();
        return;
    }

    m_developScene = new QGraphicsScene(this);
    m_developScene->setItemIndexMethod(QGraphicsScene::NoIndex);
    ui->developImageView->setScene(m_developScene);
    ui->developImageView->setViewportUpdateMode(QGraphicsView::SmartViewportUpdate);
    ui->developImageView->setRenderHint(QPainter::SmoothPixmapTransform, true);
    ui->developImageView->setDragMode(QGraphicsView::ScrollHandDrag);
    ui->developImageView->setOptimizationFlags(QGraphicsView::DontSavePainterState | QGraphicsView::DontAdjustForAntialiasing);
    ui->developImageView->setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
    ui->developImageView->setResizeAnchor(QGraphicsView::AnchorUnderMouse);
    ui->developImageView->setBackgroundBrush(QColor(30, 30, 30));

    m_developPixmapItem = m_developScene->addPixmap(QPixmap());
    m_developPixmapItem->setVisible(false);

    m_developBlurEffect = new QGraphicsBlurEffect(this);
    m_developBlurEffect->setBlurRadius(18.0);
    m_developBlurEffect->setEnabled(false);
    m_developPixmapItem->setGraphicsEffect(m_developBlurEffect);

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
    m_currentDevelopAssetId = -1;
    m_developZoom = 1.0;
    m_developFitMode = true;

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
    if (m_developBlurEffect) {
        m_developBlurEffect->setEnabled(false);
    }
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
}

void MainWindow::showDevelopPreview(const QPixmap &pixmap)
{
    if (!m_developScene || !m_developPixmapItem || pixmap.isNull()) {
        return;
    }

    m_developPixmapItem->setPixmap(pixmap);
    m_developPixmapItem->setVisible(true);
    m_developScene->setSceneRect(pixmap.rect());

    if (m_developBlurEffect) {
        m_developBlurEffect->setEnabled(true);
    }

    if (ui->developViewerStack && ui->developImageViewPage) {
        ui->developViewerStack->setCurrentWidget(ui->developImageViewPage);
    }

    if (ui->stackedWidget && ui->developPage) {
        ui->stackedWidget->setCurrentWidget(ui->developPage);
    }

    fitDevelopViewToImage();
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

    updateHistogram(result.histogram);
}

void MainWindow::requestHistogramComputation(const QImage &image, int requestId)
{
    if (!m_histogramWatcher) {
        return;
    }

    if (image.isNull()) {
        resetHistogram();
        return;
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

    connect(m_libraryManager, &LibraryManager::assetsChanged, this, &MainWindow::refreshLibraryView);
    connect(m_libraryManager, &LibraryManager::assetPreviewUpdated, this, &MainWindow::updateThumbnailPreview);

    connect(m_libraryManager, &LibraryManager::importProgress, this, [this](int imported, int total) {
        showStatusMessage(tr("Importing items %1/%2").arg(imported).arg(total), 0);
    });

    connect(m_libraryManager, &LibraryManager::importCompleted, this, [this]() {
        showStatusMessage(tr("Import completed"), 2000);
    });

    connect(m_libraryManager, &LibraryManager::errorOccurred, this, [this](const QString &message) {
        QMessageBox::warning(this, tr("Library error"), message);
    });
}

void MainWindow::refreshLibraryView()
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

    m_assets = m_libraryManager->assets();

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
}

void MainWindow::updateDevelopFilmstrip()
{
    if (!ui->developFilmstripList) {
        return;
    }

    QSignalBlocker blocker(ui->developFilmstripList);
    ui->developFilmstripList->clear();

    const QSize iconSize = ui->developFilmstripList->iconSize();

    for (const LibraryAsset &asset : std::as_const(m_assets)) {
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
        const QDateTime captured = info.birthTime().isValid() ? info.birthTime() : info.lastModified();
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

    ensureDevelopViewInitialized();

    if (!m_developScene || !m_developPixmapItem) {
        qWarning() << "Develop view is not initialized.";
        return;
    }

    selectFilmstripItem(assetId);

    const QString displayName = QFileInfo(filePath).fileName();
    showDevelopLoadingState(tr("Loading %1…").arg(displayName));

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
        QMessageBox::warning(this,
                             tr("Unable to open image"),
                             tr("Could not open %1.\n%2")
                                 .arg(QFileInfo(result.filePath).fileName(),
                                      result.errorMessage));
        clearDevelopView();
        return;
    }

    if (!m_developScene || !m_developPixmapItem) {
        return;
    }

    const QPixmap pix = QPixmap::fromImage(result.image);
    if (pix.isNull()) {
        QMessageBox::warning(this,
                             tr("Unable to display image"),
                             tr("Failed to convert %1 to a pixmap for display.")
                                 .arg(QFileInfo(result.filePath).fileName()));
        clearDevelopView();
        return;
    }

    m_developPixmapItem->setPixmap(pix);
    m_developPixmapItem->setVisible(true);
    m_developScene->setSceneRect(pix.rect());

    if (m_developBlurEffect) {
        m_developBlurEffect->setEnabled(false);
    }

    if (ui->developViewerStack && ui->developImageViewPage) {
        ui->developViewerStack->setCurrentWidget(ui->developImageViewPage);
    }

    if (ui->stackedWidget && ui->developPage) {
        ui->stackedWidget->setCurrentWidget(ui->developPage);
    }

    fitDevelopViewToImage();

    populateDevelopMetadata(result.image, result.filePath, result.metadata);
    requestHistogramComputation(result.image, result.requestId);

    m_currentDevelopAssetId = result.assetId;
    selectFilmstripItem(result.assetId);

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
    qDebug() << "Copy clicked!";
}
void MainWindow::on_actionPaste_triggered(){
    qDebug() << "Paste clicked!";
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

    m_libraryManager->importFiles(files);
}
