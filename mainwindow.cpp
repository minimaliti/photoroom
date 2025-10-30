#include "mainwindow.h"
#include "./ui_mainwindow.h"
#include "preferencesdialog.h"
#include <QVBoxLayout>
#include <QLabel>
#include <QWidget>
#include <QAction>
#include <QDebug>
#include <QToolBar>
#include <QDir>
#include <QPixmap>
#include <QLabel>
#include <QScrollArea>
#include <QGridLayout>
#include <QFileDialog>
#include <QTimer>
#include <QMessageBox>
#include <QInputDialog>
#include <QPushButton>
#include "imagelabel.h"
#include <QMouseEvent>
#include <QtConcurrent/QtConcurrent>
#include <libraw/libraw.h>
#include "imageprocessor.h"
#include "importpreviewdialog.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QFile>

// Helper to get sidecar path
QString getSidecarPath(const QString &imagePath) {
    return imagePath + ".json";
}

void MainWindow::saveSidecarFile(const QString &imagePath, const ImageAdjustments &adjustments) {
    if (imagePath.isEmpty()) {
        qWarning() << "Cannot save sidecar: imagePath is empty.";
        return;
    }

    QJsonObject json;
    json["brightness"] = adjustments.brightness;
    json["exposure"] = adjustments.exposure;
    json["contrast"] = adjustments.contrast;
    json["blacks"] = adjustments.blacks;
    json["highlights"] = adjustments.highlights;
    json["shadows"] = adjustments.shadows;
    json["highlightRolloff"] = adjustments.highlightRolloff;
    json["clarity"] = adjustments.clarity;
    json["vibrance"] = adjustments.vibrance;

    QJsonDocument doc(json);
    QString sidecarFilePath = getSidecarPath(imagePath);
    QFile file(sidecarFilePath);
    if (file.open(QIODevice::WriteOnly)) {
        file.write(doc.toJson());
        file.close();
        qDebug() << "Sidecar saved:" << sidecarFilePath;
    } else {
        qWarning() << "Failed to save sidecar file:" << sidecarFilePath << file.errorString();
    }
}

ImageAdjustments MainWindow::loadSidecarFile(const QString &imagePath) {
    ImageAdjustments adjustments;
    if (imagePath.isEmpty()) {
        qWarning() << "Cannot load sidecar: imagePath is empty.";
        return adjustments;
    }

    QString sidecarFilePath = getSidecarPath(imagePath);
    QFile file(sidecarFilePath);
    if (!file.exists()) {
        qDebug() << "No sidecar found for:" << imagePath;
        return adjustments; // Return default adjustments if no sidecar
    }

    if (file.open(QIODevice::ReadOnly)) {
        QByteArray jsonData = file.readAll();
        file.close();

        QJsonDocument doc = QJsonDocument::fromJson(jsonData);
        if (doc.isObject()) {
            QJsonObject json = doc.object();
            adjustments.brightness = json["brightness"].toInt(adjustments.brightness);
            adjustments.exposure = json["exposure"].toInt(adjustments.exposure);
            adjustments.contrast = json["contrast"].toInt(adjustments.contrast);
            adjustments.blacks = json["blacks"].toInt(adjustments.blacks);
            adjustments.highlights = json["highlights"].toInt(adjustments.highlights);
            adjustments.shadows = json["shadows"].toInt(adjustments.shadows);
            adjustments.highlightRolloff = json["highlightRolloff"].toInt(adjustments.highlightRolloff);
            adjustments.clarity = json["clarity"].toInt(adjustments.clarity);
            adjustments.vibrance = json["vibrance"].toInt(adjustments.vibrance);
            qDebug() << "Sidecar loaded for:" << imagePath;
        } else {
            qWarning() << "Sidecar file is not a valid JSON object:" << sidecarFilePath;
        }
    } else {
        qWarning() << "Failed to open sidecar file for reading:" << sidecarFilePath << file.errorString();
    }
    return adjustments;
}

void MainWindow::updateAdjustmentSliders() {
    // Convert adjustment values back to slider range (0-100)
    ui->whitesSlider->setValue(m_adjustments.brightness / 2 + 50);
    ui->exposureSlider->setValue(m_adjustments.exposure / 2 + 50);
    ui->contrastSlider->setValue(m_adjustments.contrast / 2 + 50);
    ui->blacksSlider->setValue(m_adjustments.blacks / 2 + 50);
    ui->highlightsSlider->setValue(m_adjustments.highlights / 2 + 50);
    ui->shadowsSlider->setValue(m_adjustments.shadows / 2 + 50);
    ui->highlightRolloffSlider->setValue(m_adjustments.highlightRolloff / 2 + 50);
    ui->claritySlider->setValue(m_adjustments.clarity / 2 + 50);
    ui->vibranceSlider->setValue(m_adjustments.vibrance / 2 + 50);
}

void MainWindow::setAdjustmentSlidersEnabled(bool enabled)
{
    ui->whitesSlider->setEnabled(enabled);
    ui->exposureSlider->setEnabled(enabled);
    ui->contrastSlider->setEnabled(enabled);
    ui->blacksSlider->setEnabled(enabled);
    ui->highlightsSlider->setEnabled(enabled);
    ui->shadowsSlider->setEnabled(enabled);
    ui->highlightRolloffSlider->setEnabled(enabled);
    ui->claritySlider->setEnabled(enabled);
    ui->vibranceSlider->setEnabled(enabled);
}

void MainWindow::saveAdjustedThumbnailToCache(const QString &filePath, const QPixmap &adjustedPixmap)
{
    if (adjustedPixmap.isNull() || filePath.isEmpty() || !m_libraryManager) {
        qWarning() << "Cannot save adjusted thumbnail: invalid pixmap, file path, or library manager.";
        return;
    }

    const int THUMBNAIL_CACHE_HEIGHT = 360; // Same height as used in loadPixmapFromFile

    QPixmap scaledPixmap = adjustedPixmap.scaled(adjustedPixmap.width() * THUMBNAIL_CACHE_HEIGHT / adjustedPixmap.height(),
                                                 THUMBNAIL_CACHE_HEIGHT,
                                                 Qt::KeepAspectRatio,
                                                 Qt::SmoothTransformation);

    QString newCacheFileName = m_libraryManager->getCacheFileName(filePath);
    QString cacheDir = m_libraryManager->currentLibraryThumbnailCachePath();
    QString cacheFilePath = QDir(cacheDir).filePath(newCacheFileName);
    QDir().mkpath(cacheDir); // Ensure cache directory exists
    scaledPixmap.save(cacheFilePath, "JPG", 80); // Save as JPG with 80% quality
    m_libraryManager->addCacheEntryToInfo(filePath, newCacheFileName); // Record in info.prinfo
    qDebug() << "Saved adjusted thumbnail to cache and info.prinfo:" << cacheFilePath;

    // Update the thumbnail in the image strip if it's currently displayed
    for (ImageLabel* stripThumb : this->m_imageStripThumbnails) {
        if (stripThumb->property("filePath").toString() == filePath) {
            stripThumb->setPixmap(scaledPixmap.scaled(80, 80, Qt::KeepAspectRatio, Qt::SmoothTransformation));
            break;
        }
    }

    // Update the thumbnail in the main grid if it's currently displayed
    for (ImageLabel* thumbLabel : this->thumbnailWidgets) {
        if (thumbLabel->property("filePath").toString() == filePath) {
            thumbLabel->setPixmap(scaledPixmap);
            break;
        }
    }
}

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);

    m_imageProcessor = new ImageProcessor(this);
    m_libraryManager = new LibraryManager(this);

    connect(m_libraryManager, &LibraryManager::error, this, &MainWindow::onLibraryError);

    // Initialize default library on startup
    QString defaultLibraryPath = m_libraryManager->getDefaultLibraryPath();
    if (!QDir(defaultLibraryPath).exists()) {
        qDebug() << "Default library does not exist. Creating at:" << defaultLibraryPath;
        if (m_libraryManager->createLibrary(defaultLibraryPath)) {
            qDebug() << "Default library created.";
        } else {
            qWarning() << "Failed to create default library.";
        }
    }

    if (m_libraryManager->openLibrary(defaultLibraryPath)) {
        populateLibrary(m_libraryManager->currentLibraryImportPath());
    } else {
        qWarning() << "Failed to open default library.";
    }

    resizeTimer = new QTimer(this);
    resizeTimer->setSingleShot(true);
    connect(resizeTimer, &QTimer::timeout, this, &MainWindow::onResizeTimerTimeout);


    // --- Configure the develop page image label ---
    if (ui->developImageLabel) {
        ui->developImageLabel->setAlignment(Qt::AlignCenter);
        // This size policy is crucial for the label to resize freely
        ui->developImageLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    }

    // Connect toolbar actions to switch pages
    connect(ui->actionShowLibrary, &QAction::triggered, [this]() {
        if(ui->stackedWidget) ui->stackedWidget->setCurrentIndex(0);
    });
    connect(ui->actionShowDevelop, &QAction::triggered, [this]() {
        if(ui->stackedWidget) ui->stackedWidget->setCurrentIndex(1);
        updateImageStrip();
    });

    // Connect the future watcher to update the grid when all thumbnails are loaded
    connect(&m_imageLoadingWatcher, &QFutureWatcher<void>::finished, this, [this]() {
        qDebug() << "Thumbnail loading finished.";
        this->updateImageGrid();
    });

    connect(this, &MainWindow::thumbnailLoaded, this, &MainWindow::onThumbnailReady);
    connect(this, &MainWindow::fullImageLoaded, this, &MainWindow::onFullImageReady);

    // Connect the sliders to the applyAdjustments slot
    connect(ui->whitesSlider, &QSlider::valueChanged, this, &MainWindow::applyAdjustments);
    connect(ui->exposureSlider, &QSlider::valueChanged, this, &MainWindow::applyAdjustments);
    connect(ui->contrastSlider, &QSlider::valueChanged, this, &MainWindow::applyAdjustments);
    connect(ui->blacksSlider, &QSlider::valueChanged, this, &MainWindow::applyAdjustments);
    connect(ui->highlightsSlider, &QSlider::valueChanged, this, &MainWindow::applyAdjustments);
    connect(ui->shadowsSlider, &QSlider::valueChanged, this, &MainWindow::applyAdjustments);
    connect(ui->highlightRolloffSlider, &QSlider::valueChanged, this, &MainWindow::applyAdjustments);
    connect(ui->claritySlider, &QSlider::valueChanged, this, &MainWindow::applyAdjustments);
    connect(ui->vibranceSlider, &QSlider::valueChanged, this, &MainWindow::applyAdjustments);
    connect(ui->threadCountSlider, &QSlider::valueChanged, this, &MainWindow::onThreadCountSliderChanged);

    ui->whitesSlider->setValue(50);
    ui->exposureSlider->setValue(50);
    ui->contrastSlider->setValue(50);
    ui->blacksSlider->setValue(50);
    ui->highlightsSlider->setValue(50);
    ui->shadowsSlider->setValue(50);
    ui->highlightRolloffSlider->setValue(50);
    ui->claritySlider->setValue(50);
    ui->vibranceSlider->setValue(50);
    ui->threadCountSlider->setValue(4);

    m_adjustmentTimer = new QTimer(this);
    m_adjustmentTimer->setSingleShot(true);
    m_adjustmentTimer->setInterval(200); // 200ms delay
    connect(m_adjustmentTimer, &QTimer::timeout, this, &MainWindow::delayedApplyAdjustments);
}

void MainWindow::onThreadCountSliderChanged(int value)
{
    m_imageProcessor->setThreadCount(value);
}

void MainWindow::applyAdjustments()
{
    if (m_originalPixmap.isNull()) {
        return;
    }

    m_adjustmentTimer->start();
}

void MainWindow::delayedApplyAdjustments()
{
    m_adjustments.brightness = (ui->whitesSlider->value() - 50) * 2;
    m_adjustments.exposure = (ui->exposureSlider->value() - 50) * 2;
    m_adjustments.contrast = (ui->contrastSlider->value() - 50) * 2;
    m_adjustments.blacks = (ui->blacksSlider->value() - 50) * 2;
    m_adjustments.highlights = (ui->highlightsSlider->value() - 50) * 2;
    m_adjustments.shadows = (ui->shadowsSlider->value() - 50) * 2;
    m_adjustments.highlightRolloff = (ui->highlightRolloffSlider->value() - 50) * 2;
    m_adjustments.clarity = (ui->claritySlider->value() - 50) * 2;
    m_adjustments.vibrance = (ui->vibranceSlider->value() - 50) * 2;

    m_currentPixmap = m_imageProcessor->applyAdjustments(m_originalPixmap, m_adjustments);
    updateDevelopImage();
    saveSidecarFile(m_currentDevelopImagePath, m_adjustments);
    saveAdjustedThumbnailToCache(m_currentDevelopImagePath, m_currentPixmap);
}

// This function can now load either a fast thumbnail or a full-quality image
QPixmap MainWindow::loadPixmapFromFile(const QString &filePath, bool isThumbnail, LibraryManager *libraryManager)
{
    if (filePath.isEmpty()) {
        qWarning() << "Attempted to load pixmap from an empty file path.";
        return QPixmap();
    }

    QPixmap pixmap;
    const int THUMBNAIL_CACHE_HEIGHT = 360; // Define the fixed height for cached thumbnails

    // --- THUMBNAIL CACHING LOGIC (using info.prinfo) ---
    // Always check cache first if it's a thumbnail request
    if (isThumbnail && libraryManager) {
        QString cachedFileName = libraryManager->getCacheFileNameFromInfo(filePath);
        if (!cachedFileName.isEmpty()) {
            QString cacheDir = libraryManager->currentLibraryThumbnailCachePath();
            QString cacheFilePath = QDir(cacheDir).filePath(cachedFileName);

            if (QFile::exists(cacheFilePath)) {
                pixmap.load(cacheFilePath);
                if (!pixmap.isNull()) {
                    qDebug() << "Loaded thumbnail from info.prinfo cache: " << cacheFilePath;
                    return pixmap; // Return immediately if cached thumbnail is valid
                } else {
                    qWarning() << "Failed to load cached thumbnail from info.prinfo, re-generating: " << cacheFilePath;
                    // If loading from cache failed, proceed to generate and re-cache
                }
            } else {
                qWarning() << "Cached thumbnail file not found on disk, re-generating: " << cacheFilePath;
                // If cache file is missing, proceed to generate and re-cache
            }
        }
    }

    // If not a thumbnail request, or cache lookup failed, proceed with loading from source
    // First, try loading with Qt's native support for common formats
    pixmap.load(filePath);
    if (!pixmap.isNull()) {
        if (isThumbnail && libraryManager) {
            // Scale pixmap before saving to cache
            QPixmap scaledPixmap = pixmap.scaled(pixmap.width() * THUMBNAIL_CACHE_HEIGHT / pixmap.height(),
                                                 THUMBNAIL_CACHE_HEIGHT,
                                                 Qt::KeepAspectRatio,
                                                 Qt::SmoothTransformation);

            // Save to cache if it was a thumbnail and successfully loaded
            QString newCacheFileName = libraryManager->getCacheFileName(filePath);
            QString cacheDir = libraryManager->currentLibraryThumbnailCachePath();
            QString cacheFilePath = QDir(cacheDir).filePath(newCacheFileName);
            QDir().mkpath(cacheDir); // Ensure cache directory exists
            scaledPixmap.save(cacheFilePath, "JPG", 80); // Save as JPG with 80% quality
            libraryManager->addCacheEntryToInfo(filePath, newCacheFileName); // Record in info.prinfo
            qDebug() << "Saved generated thumbnail (Qt native) to cache and info.prinfo: " << cacheFilePath;
            return scaledPixmap; // Return the scaled pixmap
        }
        return pixmap;
    }

    // If it fails, check for raw file extensions and try LibRaw
    QString lowerPath = filePath.toLower();
    if (lowerPath.endsWith(".cr2") || lowerPath.endsWith(".nef") || lowerPath.endsWith(".arw") || lowerPath.endsWith(".dng")) {
        LibRaw rawProcessor;
        QPixmap rawPixmap; // Use a separate pixmap for raw processing results

        // --- PERFORMANCE OPTIMIZATION FOR THUMBNAILS (Embedded Preview) ---
        if (isThumbnail) {
            if (rawProcessor.open_file(filePath.toStdString().c_str()) == LIBRAW_SUCCESS) {
                if (rawProcessor.unpack_thumb() == LIBRAW_SUCCESS) {
                    libraw_processed_image_t *thumb = rawProcessor.dcraw_make_mem_thumb();
                    if(thumb && thumb->type == LIBRAW_IMAGE_JPEG) {
                        rawPixmap.loadFromData(thumb->data, thumb->data_size);
                        LibRaw::dcraw_clear_mem(thumb);
                        rawProcessor.recycle();

                        if (!rawPixmap.isNull() && libraryManager) {
                            // Scale pixmap before saving to cache
                            QPixmap scaledPixmap = rawPixmap.scaled(rawPixmap.width() * THUMBNAIL_CACHE_HEIGHT / rawPixmap.height(),
                                                                     THUMBNAIL_CACHE_HEIGHT,
                                                                     Qt::KeepAspectRatio,
                                                                     Qt::SmoothTransformation);

                            // Save to cache
                            QString newCacheFileName = libraryManager->getCacheFileName(filePath);
                            QString cacheDir = libraryManager->currentLibraryThumbnailCachePath();
                            QString cacheFilePath = QDir(cacheDir).filePath(newCacheFileName);
                            QDir().mkpath(cacheDir); // Ensure cache directory exists
                            scaledPixmap.save(cacheFilePath, "JPG", 80); // Save as JPG with 80% quality
                            libraryManager->addCacheEntryToInfo(filePath, newCacheFileName); // Record in info.prinfo
                            qDebug() << "Saved generated raw embedded thumbnail to cache and info.prinfo: " << cacheFilePath;
                            return scaledPixmap; // Return embedded thumbnail
                        }
                    }
                    if(thumb) LibRaw::dcraw_clear_mem(thumb);
                }
                rawProcessor.recycle(); // Ensure recycle is called even if unpack_thumb fails
            }
            qWarning() << "LibRaw: Embedded thumbnail extraction failed or not JPEG, falling back to full processing for thumbnail: " << filePath;
        }

        // --- FULL IMAGE LOADING WITH WHITE BALANCE CORRECTION (or fallback for thumbnails) ---
        if (rawProcessor.open_file(filePath.toStdString().c_str()) != LIBRAW_SUCCESS) {
            qWarning() << "LibRaw: Failed to open raw file: " << filePath;
            return QPixmap();
        }

        if (rawProcessor.unpack() != LIBRAW_SUCCESS) {
            qWarning() << "LibRaw: Failed to unpack raw data: " << filePath;
            return QPixmap();
        }

        // --- PERFORMANCE OPTIMIZATIONS FOR FULL IMAGE LOADING ---
        // These settings prioritize speed over quality for previewing.
        // Decode at half-size, which is much faster and often sufficient for screen display.
        rawProcessor.imgdata.params.half_size = 1;
        // Use the white balance settings from the camera. It's faster than auto-calculating.
        rawProcessor.imgdata.params.use_camera_wb = 1;
        // Disable auto white balance calculation, which saves processing time.
        rawProcessor.imgdata.params.use_auto_wb = 0;
        // Set the quality to a lower, "draft" mode for faster processing.
        rawProcessor.imgdata.params.user_qual = 1;

        if (rawProcessor.dcraw_process() != LIBRAW_SUCCESS) {
            qWarning() << "LibRaw: Failed to process raw data: " << filePath;
            return QPixmap();
        }

        libraw_processed_image_t *processed_image = rawProcessor.dcraw_make_mem_image();
        if (!processed_image) {
            qWarning() << "LibRaw: Failed to create in-memory image: " << filePath;
            return QPixmap();
        }

        QImage qImage(processed_image->data,
                      processed_image->width,
                      processed_image->height,
                      QImage::Format_RGB888);

        rawPixmap = QPixmap::fromImage(qImage);

        LibRaw::dcraw_clear_mem(processed_image);
        rawProcessor.recycle();

        if (isThumbnail && !rawPixmap.isNull() && libraryManager) {
            // Scale pixmap before saving to cache
            QPixmap scaledPixmap = rawPixmap.scaled(rawPixmap.width() * THUMBNAIL_CACHE_HEIGHT / rawPixmap.height(),
                                                     THUMBNAIL_CACHE_HEIGHT,
                                                     Qt::KeepAspectRatio,
                                                     Qt::SmoothTransformation);

            // Save to cache if it was a thumbnail and successfully loaded (fallback path)
            QString newCacheFileName = libraryManager->getCacheFileName(filePath);
            QString cacheDir = libraryManager->currentLibraryThumbnailCachePath();
            QString cacheFilePath = QDir(cacheDir).filePath(newCacheFileName);
            QDir().mkpath(cacheDir); // Ensure cache directory exists
            scaledPixmap.save(cacheFilePath, "JPG", 80); // Save as JPG with 80% quality
            libraryManager->addCacheEntryToInfo(filePath, newCacheFileName); // Record in info.prinfo
            qDebug() << "Saved generated raw (fallback) thumbnail to cache and info.prinfo: " << cacheFilePath;
            return scaledPixmap; // Return the scaled pixmap
        }

        return rawPixmap;
    }

    return QPixmap(); // Return null pixmap if not a recognized format and not handled by LibRaw
}

void MainWindow::updateDevelopImage()
{
    if (m_currentPixmap.isNull() || !ui->developImageLabel) {
        return;
    }
    QPixmap scaledPix = m_currentPixmap.scaled(ui->developImageLabel->size(),
                                               Qt::KeepAspectRatio,
                                               Qt::SmoothTransformation);
    ui->developImageLabel->setPixmap(scaledPix);
}

void MainWindow::showLibraryPage() {
    ui->stackedWidget->setCurrentWidget(ui->libraryPage);
}

void MainWindow::showDevelopPage() {
    ui->stackedWidget->setCurrentWidget(ui->developPage);
}

void MainWindow::onResizeTimerTimeout()
{
    qDebug() << "Resize timer timed out, updating visuals.";
    if (ui->stackedWidget->currentWidget() == ui->libraryPage && !this->imageFiles.isEmpty()) {
        this->updateImageGrid();
    }
    if (ui->stackedWidget->currentWidget() == ui->developPage) {
        this->updateDevelopImage();
        this->updateImageStrip();
    }
}

void MainWindow::onThumbnailDoubleClicked()
{
    ImageLabel* clickedLabel = qobject_cast<ImageLabel*>(sender());
    if (!clickedLabel) return;

    QString filePath = clickedLabel->property("filePath").toString();
    if (filePath.isEmpty()) return;

    // If the requested image is already loaded, just switch to the develop page
    if (!m_currentDevelopImagePath.isEmpty() && m_currentDevelopImagePath == filePath) {
        ui->stackedWidget->setCurrentWidget(ui->developPage);
        return; // Avoid reloading the same image
    }

    if (!ui->developImageLabel) {
        qWarning() << "developImageLabel is null! Check mainwindow.ui.";
        return;
    }

    m_imageProcessor->clearCache();

    // 1. Load and display thumbnail immediately
    QPixmap thumbnailPixmap = loadPixmapFromFile(filePath, true, m_libraryManager);
    if (thumbnailPixmap.isNull()) {
        qWarning() << "Failed to load thumbnail for:" << filePath;
        ui->developImageLabel->setText("Failed to load thumbnail.");
        m_currentDevelopImagePath.clear();
        return;
    }

    m_currentDevelopImagePath = filePath; // Set path immediately for consistency
    m_originalPixmap = thumbnailPixmap; // Temporarily store thumbnail as original
    m_currentPixmap = thumbnailPixmap;
    updateDevelopImage();
    ui->stackedWidget->setCurrentWidget(ui->developPage);

    // 2. Disable adjustment sliders
    setAdjustmentSlidersEnabled(false);

    // 3. Asynchronously load the full quality image
    QtConcurrent::run([this, filePath]() {
        QPixmap fullPixmap = loadPixmapFromFile(filePath, false, m_libraryManager);
        if (!fullPixmap.isNull()) {
            emit fullImageLoaded(filePath, fullPixmap);
        }
    });
}

void MainWindow::onImageStripThumbnailClicked()
{
    ImageLabel* clickedLabel = qobject_cast<ImageLabel*>(sender());
    if (!clickedLabel) return;

    QString filePath = clickedLabel->property("filePath").toString();
    if (filePath.isEmpty()) return;

    // If the requested image is already loaded, just switch to the develop page
    if (!m_currentDevelopImagePath.isEmpty() && m_currentDevelopImagePath == filePath) {
        ui->stackedWidget->setCurrentWidget(ui->developPage);
        return; // Avoid reloading the same image
    }

    if (!ui->developImageLabel) {
        qWarning() << "developImageLabel is null! Check mainwindow.ui.";
        return;
    }

    m_imageProcessor->clearCache();

    // 1. Load and display thumbnail immediately
    QPixmap thumbnailPixmap = loadPixmapFromFile(filePath, true, m_libraryManager);
    if (thumbnailPixmap.isNull()) {
        qWarning() << "Failed to load thumbnail for:" << filePath;
        ui->developImageLabel->setText("Failed to load thumbnail.");
        m_currentDevelopImagePath.clear();
        return;
    }

    m_currentDevelopImagePath = filePath; // Set path immediately for consistency
    m_originalPixmap = thumbnailPixmap; // Temporarily store thumbnail as original
    m_currentPixmap = thumbnailPixmap;
    updateDevelopImage();
    ui->stackedWidget->setCurrentWidget(ui->developPage);

    // 2. Disable adjustment sliders
    setAdjustmentSlidersEnabled(false);

    // 3. Asynchronously load the full quality image
    QtConcurrent::run([this, filePath]() {
        QPixmap fullPixmap = loadPixmapFromFile(filePath, false, m_libraryManager);
        if (!fullPixmap.isNull()) {
            emit fullImageLoaded(filePath, fullPixmap);
        }
    });
}


// This slot receives thumbnails from the background thread
void MainWindow::onThumbnailReady(const QString &filePath, const QPixmap &pixmap)
{
    if (pixmap.isNull()) return;

    // Find the ImageLabel that corresponds to this filePath
    for (ImageLabel* thumbLabel : this->thumbnailWidgets) {
        if (thumbLabel->property("filePath").toString() == filePath) {
            thumbLabel->setPixmap(pixmap);
            thumbLabel->setLoading(false); // Hide loading indicator
            qDebug() << "Updated main grid thumbnail for:" << filePath;
            break;
        }
    }

    // Also update the image strip thumbnail if it exists
    for (ImageLabel* stripThumb : this->m_imageStripThumbnails) {
        if (stripThumb->property("filePath").toString() == filePath) {
            stripThumb->setPixmap(pixmap.scaled(80, 80, Qt::KeepAspectRatio, Qt::SmoothTransformation));
            stripThumb->setLoading(false); // Hide loading indicator
            qDebug() << "Updated image strip thumbnail for:" << filePath;
            break;
        }
    }
}

void MainWindow::onFullImageReady(const QString &filePath, const QPixmap &pixmap)
{
    if (pixmap.isNull()) {
        qWarning() << "Full image ready but pixmap is null for:" << filePath;
        ui->developImageLabel->setText("Failed to load full image.");
        setAdjustmentSlidersEnabled(true); // Re-enable sliders even on failure
        return;
    }

    m_originalPixmap = pixmap;
    m_currentDevelopImagePath = filePath;

    // Load adjustments from sidecar file
    m_adjustments = loadSidecarFile(m_currentDevelopImagePath);
    updateAdjustmentSliders();

    // Apply adjustments immediately and update the display
    delayedApplyAdjustments();

    setAdjustmentSlidersEnabled(true);
    qDebug() << "Full image loaded and displayed for:" << filePath;
}

void MainWindow::populateLibrary(const QString &folderPath)
{
    clearLibrary();

    if (m_imageLoadingWatcher.isRunning()) {
        m_imageLoadingWatcher.cancel();
        m_imageLoadingWatcher.waitForFinished();
    }

    this->currentLibraryPath = folderPath;
    QDir dir(folderPath);
    QStringList filters;
    filters << "*.png" << "*.jpg" << "*.jpeg" << "*.bmp" << "*.CR2" << "*.NEF" << "*.ARW" << "*.DNG";
    this->imageFiles = dir.entryInfoList(filters, QDir::Files | QDir::NoDotAndDotDot, QDir::Name);

    qDebug() << "Found" << this->imageFiles.size() << "images. Populating placeholders and loading thumbnails asynchronously...";

    QGridLayout *gridLayout = qobject_cast<QGridLayout*>(ui->scrollAreaWidgetContents->layout());
    if (!gridLayout) {
        gridLayout = new QGridLayout(ui->scrollAreaWidgetContents);
        ui->scrollAreaWidgetContents->setLayout(gridLayout);
        gridLayout->setSpacing(4);
    }

    QHBoxLayout *stripLayout = qobject_cast<QHBoxLayout*>(ui->imageStripWidgetContents->layout());
    if (!stripLayout) {
        stripLayout = new QHBoxLayout(ui->imageStripWidgetContents);
        ui->imageStripWidgetContents->setLayout(stripLayout);
    }

    QList<QString> filePathsToLoad; // Collect file paths for concurrent loading
    for (const QFileInfo &fileInfo : this->imageFiles) {
        QString filePath = fileInfo.absoluteFilePath();
        filePathsToLoad.append(filePath);

        // Create ImageLabel for the main grid (with loading indicator)
        ImageLabel *thumb = new ImageLabel(ui->scrollAreaWidgetContents);
        thumb->setProperty("filePath", filePath);
        thumb->setStyleSheet("border: 1px solid #444; background-color: #222;");
        thumb->setMinimumSize(120, 120);
        thumb->setLoading(true); // Show loading indicator
        connect(thumb, &ImageLabel::clicked, this, &MainWindow::onThumbnailClicked);
        connect(thumb, &ImageLabel::doubleClicked, this, &MainWindow::onThumbnailDoubleClicked);
        this->thumbnailWidgets.append(thumb);
        qDebug() << "Created main grid thumbnail for:" << filePath;

        // Create ImageLabel for the image strip (with loading indicator)
        ImageLabel *stripThumb = new ImageLabel(ui->imageStripWidgetContents);
        stripThumb->setProperty("filePath", filePath);
        stripThumb->setFixedSize(80, 80);
        stripThumb->setStyleSheet("border: 1px solid #444; background-color: #222;");
        stripThumb->setLoading(true); // Show loading indicator
        connect(stripThumb, &ImageLabel::clicked, this, &MainWindow::onImageStripThumbnailClicked);
        this->m_imageStripThumbnails.append(stripThumb);
        qDebug() << "Created image strip thumbnail for:" << filePath;
    }

    // Arrange placeholders immediately
    updateImageGrid();
    updateImageStrip();

    // --- STEP 2: Start asynchronous thumbnail loading for all placeholders ---
    QFuture<void> future = QtConcurrent::run([this, filePathsToLoad]() {
        for (const QString &filePath : filePathsToLoad) {
            if (filePath.isEmpty() || !QFile::exists(filePath)) {
                qWarning() << "Skipping invalid or non-existent file:" << filePath;
                continue; // Skip this file
            }
            QPixmap pix = loadPixmapFromFile(filePath, true, m_libraryManager);
            if(!pix.isNull()) {
                // Safely emit signal to update GUI on the main thread
                emit thumbnailLoaded(filePath, pix);
            }
        }
    });

    m_imageLoadingWatcher.setFuture(future);
}

void MainWindow::updateImageGrid()
{
    QGridLayout *layout = qobject_cast<QGridLayout*>(ui->scrollAreaWidgetContents->layout());
    if (!layout) {
        layout = new QGridLayout(ui->scrollAreaWidgetContents);
        ui->scrollAreaWidgetContents->setLayout(layout);
        layout->setSpacing(4);
    }

    // Clear existing items from the layout without deleting the widgets
    QLayoutItem* item;
    while ((item = layout->takeAt(0)) != nullptr) {
        // Do not delete item->widget() here, as widgets are managed by thumbnailWidgets list
        delete item;
    }

    // Reset any existing stretches from previous arrangements.
    for (int i = 0; i < layout->columnCount(); ++i) {
        layout->setColumnStretch(i, 0);
    }
    for (int i = 0; i < layout->rowCount(); ++i) {
        layout->setRowStretch(i, 0);
    }

    int spacing = layout->spacing();
    int thumbnailWidth = 160; // The desired width for your thumbnails

    // Use the viewport's width, which correctly accounts for a potential vertical scrollbar
    int viewportWidth = ui->scrollArea->viewport()->width();

    // Calculate the number of columns that can fit in the current width
    int maxCols = qMax(1, (viewportWidth - spacing) / (thumbnailWidth + spacing));

    int row = 0;
    int col = 0;

    // Rearrange all the thumbnail widgets into the new grid configuration.
    for (ImageLabel* thumb : this->thumbnailWidgets) {
        layout->addWidget(thumb, row, col);
        col++;
        if (col >= maxCols) {
            col = 0;
            row++;
        }
    }

    // --- Control Expansion: Push everything to the top-left ---

    // Add a stretching column to take up all remaining horizontal space
    layout->setColumnStretch(maxCols, 1);
    // Add a stretching row to take up all remaining vertical space
    layout->setRowStretch(row + 1, 1);

    qDebug() << "Rearranged grid with" << maxCols << "columns.";
}

void MainWindow::updateImageStrip()
{
    QHBoxLayout *layout = qobject_cast<QHBoxLayout*>(ui->imageStripWidgetContents->layout());
    if (!layout) {
        layout = new QHBoxLayout(ui->imageStripWidgetContents);
        ui->imageStripWidgetContents->setLayout(layout);
    } else {
        QLayoutItem* item;
        while ((item = layout->takeAt(0)) != nullptr) {
            // Do not delete item->widget() here, as widgets are managed by m_imageStripThumbnails list
            delete item;
        }
    }

    for (ImageLabel* thumb : this->m_imageStripThumbnails) {
        layout->addWidget(thumb);
    }
    layout->addStretch(); // Push images to the left
}

void MainWindow::resizeEvent(QResizeEvent *event)
{
    QMainWindow::resizeEvent(event);
    if (resizeTimer) {
        resizeTimer->start(60); // Use a small delay
    }
}

void MainWindow::clearLibrary()
{
    this->imageFiles.clear();

    qDeleteAll(this->thumbnailWidgets);
    this->thumbnailWidgets.clear();
    qDeleteAll(this->m_imageStripThumbnails);
    this->m_imageStripThumbnails.clear();
    m_lastSelected = nullptr;
    m_currentPixmap = QPixmap();
    m_currentDevelopImagePath.clear();

    QGridLayout *layout = qobject_cast<QGridLayout*>(ui->scrollAreaWidgetContents->layout());
    if (layout) {
        QLayoutItem* item;
        while ((item = layout->takeAt(0)) != nullptr) {
            delete item->widget(); // Also delete the widget
            delete item;
        }
    }

    QHBoxLayout *stripLayout = qobject_cast<QHBoxLayout*>(ui->imageStripWidgetContents->layout());
    if (stripLayout) {
        QLayoutItem* item;
        while ((item = stripLayout->takeAt(0)) != nullptr) {
            delete item->widget(); // Also delete the widget
            delete item;
        }
    }
}

void MainWindow::onThumbnailClicked(QMouseEvent *event)
{
    ImageLabel* clickedLabel = qobject_cast<ImageLabel*>(sender());
    if (!clickedLabel) return;

    Qt::KeyboardModifiers modifiers = event->modifiers();

    if (modifiers & Qt::ShiftModifier && m_lastSelected) {
        int startIndex = thumbnailWidgets.indexOf(m_lastSelected);
        int endIndex = thumbnailWidgets.indexOf(clickedLabel);

        if (startIndex > endIndex) qSwap(startIndex, endIndex);

        for (ImageLabel* label : thumbnailWidgets) label->setSelected(false);
        for (int i = startIndex; i <= endIndex; ++i) {
            thumbnailWidgets.at(i)->setSelected(true);
        }
    } else if (modifiers & Qt::ControlModifier) {
        clickedLabel->setSelected(!clickedLabel->isSelected());
        m_lastSelected = clickedLabel;
    } else {
        for (ImageLabel* label : thumbnailWidgets) {
            label->setSelected(label == clickedLabel);
        }
        m_lastSelected = clickedLabel;
    }
}

MainWindow::~MainWindow()
{
    clearLibrary();
    delete ui;
}

// Menubar
void MainWindow::on_actionExit_triggered()
{
    QApplication::quit();
}

void MainWindow::on_actionNew_Library_triggered()
{
    QString newLibraryName = QInputDialog::getText(this, tr("New Library"), tr("Enter new library name:"), QLineEdit::Normal, tr("My Photo Library"));
    if (newLibraryName.isEmpty()) return;

    QString picturesLocation = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    QString newLibraryPath = QDir(picturesLocation).filePath(newLibraryName + ".prlibrary");

    if (m_libraryManager->createLibrary(newLibraryPath)) {
        if (m_libraryManager->openLibrary(newLibraryPath)) {
            populateLibrary(m_libraryManager->currentLibraryImportPath());
            ui->stackedWidget->setCurrentIndex(0);
        }
    }
}
void MainWindow::on_actionOpen_Library_triggered()
{
    QString folder = QFileDialog::getExistingDirectory(this, tr("Open PhotoRoom Library"),
                                                       QStandardPaths::writableLocation(QStandardPaths::PicturesLocation),
                                                       QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (!folder.isEmpty()) {
        if (m_libraryManager->openLibrary(folder)) {
            this->populateLibrary(m_libraryManager->currentLibraryImportPath());
            ui->stackedWidget->setCurrentIndex(0);
        }
    }
}

void MainWindow::on_actionClear_recents_triggered(){ }
void MainWindow::on_actionUndo_triggered(){ }
void MainWindow::on_actionRedo_triggered(){ }
void MainWindow::on_actionCut_triggered(){ }
void MainWindow::on_actionCopy_triggered(){ }
void MainWindow::on_actionPaste_triggered(){ }
void MainWindow::on_actionSelect_All_triggered(){ }
void MainWindow::on_actionSelect_None_triggered(){ }
void MainWindow::on_actionInverse_Selection_triggered(){ }

void MainWindow::on_actionImport_triggered() {
    qDebug() << "Library > Import triggered.";

    QStringList filesToImport;

    QMessageBox msgBox;
    msgBox.setText(tr("Import Options"));
    msgBox.setInformativeText(tr("Do you want to import files or a folder?"));
    QPushButton *importFilesButton = msgBox.addButton(tr("Import Files"), QMessageBox::ActionRole);
    QPushButton *importFolderButton = msgBox.addButton(tr("Import Folder"), QMessageBox::ActionRole);
    msgBox.addButton(QMessageBox::Cancel);

    msgBox.exec();

    if (static_cast<QPushButton*>(msgBox.clickedButton()) == importFilesButton) {
        filesToImport = QFileDialog::getOpenFileNames(this,
                                                    tr("Import Images"),
                                                    QStandardPaths::writableLocation(QStandardPaths::PicturesLocation),
                                                    tr("Image Files (*.png *.jpg *.jpeg *.bmp *.CR2 *.NEF *.ARW *.DNG)"));
    } else if (static_cast<QPushButton*>(msgBox.clickedButton()) == importFolderButton) {
        QString folderPath = QFileDialog::getExistingDirectory(this, tr("Import Folder"),
                                                               QStandardPaths::writableLocation(QStandardPaths::PicturesLocation));
        if (!folderPath.isEmpty()) {
            filesToImport = findImageFilesInDirectory(folderPath);
        }
    } else {
        qDebug() << "Import cancelled by user.";
        return;
    }

    if (filesToImport.isEmpty()) {
        qDebug() << "No files selected for import.";
        return;
    }

    qDebug() << "Selected" << filesToImport.size() << "files. Opening ImportPreviewDialog.";
    ImportPreviewDialog dialog(filesToImport, this, m_libraryManager);
    if (dialog.exec() == QDialog::Accepted) {
        qDebug() << "ImportPreviewDialog accepted. Processing import.";
        ImportPreviewDialog::ImportMode mode = dialog.importMode();

        if (mode == ImportPreviewDialog::CancelMode) {
            qDebug() << "Import cancelled by user.";
            return;
        }

        QString importPath = m_libraryManager->currentLibraryImportPath();
        if (importPath.isEmpty()) {
            QMessageBox::critical(this, tr("Import Error"), tr("Current library import path is not set."));
            qDebug() << "Import error: Current library import path is not set.";
            return;
            }

            QDir importDir(importPath);
            if (!importDir.exists()) {
                if (!importDir.mkpath(".")) {
                    QMessageBox::critical(this, tr("Import Error"), tr("Failed to create import directory: %1").arg(importPath));
                    qDebug() << "Import error: Failed to create import directory:" << importPath;
                    return;
                }
            }

            for (const QString &filePath : filesToImport) {
                QFileInfo fileInfo(filePath);
                QString destinationPath = importDir.filePath(fileInfo.fileName());

                bool success = false;
                QFile sourceFile(filePath);
                if (mode == ImportPreviewDialog::MoveMode) {
                    success = sourceFile.rename(destinationPath);
                    if (!success) {
                        qWarning() << "Failed to move file" << filePath << "to" << destinationPath << ":" << sourceFile.errorString();
                    }
                } else if (mode == ImportPreviewDialog::CopyMode) {
                    success = sourceFile.copy(destinationPath);
                    if (!success) {
                        qWarning() << "Failed to copy file" << filePath << "to" << destinationPath << ":" << sourceFile.errorString();
                    }
                }

                if (!success) {
                    QMessageBox::warning(this, tr("Import Warning"), tr("Failed to import file: %1").arg(fileInfo.fileName()));
                    qDebug() << "Import warning: Failed to import file:" << fileInfo.fileName();
                }
            }

            populateLibrary(importPath);
            QMessageBox::information(this, tr("Import Complete"), tr("Selected images have been imported."));
            qDebug() << "Image import process completed.";
        } else {
            qDebug() << "ImportPreviewDialog rejected or closed.";
        }
    }

    QStringList MainWindow::findImageFilesInDirectory(const QString &folderPath)
    {
        QStringList imageFiles;
        QDir dir(folderPath);
        QStringList filters;
        filters << "*.png" << "*.jpg" << "*.jpeg" << "*.bmp" << "*.CR2" << "*.NEF" << "*.ARW" << "*.DNG";

        QFileInfoList entries = dir.entryInfoList(filters, QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot);

        for (const QFileInfo &entry : entries) {
            if (entry.isFile()) {
                imageFiles.append(entry.absoluteFilePath());
            } else if (entry.isDir()) {
                imageFiles.append(findImageFilesInDirectory(entry.absoluteFilePath())); // Recursively search subdirectories
            }
        }
        return imageFiles;
    }

void MainWindow::on_actionRefresh_triggered() {
    qDebug() << "Library > Refresh";
}
void MainWindow::on_actionSettings_triggered(){
    qDebug() << "Library > Settings";
}
void MainWindow::on_actionBackup_triggered(){
    qDebug() << "Library > Backup";
}

void MainWindow::onLibraryError(const QString &message)
{
    QMessageBox::critical(this, tr("Library Error"), message);
}

void MainWindow::on_actionPreferences_triggered()
{
    PreferencesDialog dialog(this);
    dialog.exec();
}
