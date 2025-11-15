#include "librarymanager.h"

#include "imageloader.h"
#include "previewgenerator.h"
#include "jobmanager.h"
#include "metadatacache.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <QtConcurrent>
#include <QFutureWatcher>

namespace {
constexpr auto kDatabaseFileName = "library.db";
constexpr auto kOriginalsDirName = "originals";
constexpr auto kPreviewsDirName = "previews";
constexpr int kPreviewHeight = 512;
constexpr int kAssetsPerBucket = 128;

QString bucketName(int bucketIndex)
{
    return QString::number(qMax(1, bucketIndex));
}

int bucketIndexForPhotoNumber(const QString &photoNumber)
{
    bool ok = false;
    int numeric = photoNumber.trimmed().toInt(&ok);
    if (!ok || numeric <= 0) {
        return 1;
    }
    return ((numeric - 1) / kAssetsPerBucket) + 1;
}
}

LibraryManager::LibraryManager(QObject *parent)
    : QObject(parent)
    , m_previewGenerator(new PreviewGenerator(this))
    , m_metadataCache(new MetadataCache(this))
{
    connect(m_previewGenerator, &PreviewGenerator::previewReady, this, [this](const PreviewResult &result) {
        if (!hasOpenLibrary()) {
            return;
        }

        const QUuid jobId = m_previewJobIds.take(result.assetId);
        if (!result.success) {
            // Update batch progress even on failure
            m_previewGenerationCompleted++;
            updateBatchPreviewProgress();
            if (m_jobManager && !jobId.isNull() && m_batchPreviewJobId.isNull()) {
                // Only fail individual job if not using batch job
                m_jobManager->failJob(jobId, result.errorMessage);
            }
            emit errorOccurred(result.errorMessage);
            return;
        }

        // Update batch progress
        m_previewGenerationCompleted++;
        updateBatchPreviewProgress();
        
        // Only complete individual job if not using batch job
        if (m_jobManager && !jobId.isNull() && m_batchPreviewJobId.isNull()) {
            m_jobManager->completeJob(jobId, tr("Preview generated"));
        }

        QSqlQuery update(m_database);
        update.prepare(QStringLiteral("UPDATE assets SET preview_path = ?, width = ?, height = ? WHERE id = ?"));
        const QString relativePreview = QDir(m_libraryPath).relativeFilePath(result.previewPath);
        update.addBindValue(relativePreview);
        update.addBindValue(result.imageSize.width());
        update.addBindValue(result.imageSize.height());
        update.addBindValue(result.assetId);

        if (!update.exec()) {
            emit errorOccurred(QStringLiteral("Failed to update preview metadata: %1").arg(update.lastError().text()));
            return;
        }

        emit assetPreviewUpdated(result.assetId, result.previewPath);
        emit assetsChanged();
    });
}

LibraryManager::~LibraryManager()
{
    closeLibrary();
}

bool LibraryManager::hasOpenLibrary() const
{
    return m_database.isValid() && m_database.isOpen();
}

QString LibraryManager::libraryPath() const
{
    return m_libraryPath;
}

bool LibraryManager::createLibrary(const QString &directoryPath, QString *errorMessage)
{
    closeLibrary();

    QString dbPath = ensureLibraryDirectories(directoryPath, errorMessage);
    if (dbPath.isEmpty()) {
        return false;
    }

    if (QFile::exists(dbPath)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("A library already exists at %1").arg(directoryPath);
        }
        return false;
    }

    QSqlDatabase db;
    m_connectionName = QUuid::createUuid().toString(QUuid::Id128);
    db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);
    db.setDatabaseName(dbPath);

    if (!db.open()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Failed to open library database: %1").arg(db.lastError().text());
        }
        QSqlDatabase::removeDatabase(m_connectionName);
        m_connectionName.clear();
        return false;
    }

    m_libraryPath = directoryPath;
    m_database = db;

    if (!initializeDatabaseSchema(errorMessage)) {
        closeLibrary();
        return false;
    }

    if (!ensureDevelopAdjustmentsTable(errorMessage)) {
        closeLibrary();
        return false;
    }

    ensurePhotoNumberSupport();

    // Open metadata cache
    if (m_metadataCache) {
        QString cacheError;
        if (!m_metadataCache->openCache(directoryPath, &cacheError)) {
            qWarning() << "Failed to open metadata cache:" << cacheError;
            // Don't fail library creation if cache fails
        }
    }

    emit libraryOpened(m_libraryPath);
    emit assetsChanged();
    return true;
}

bool LibraryManager::openLibrary(const QString &directoryPath, QString *errorMessage)
{
    closeLibrary();

    QDir dir(directoryPath);
    if (!dir.exists()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Library directory does not exist: %1").arg(directoryPath);
        }
        return false;
    }

    const QString dbPath = dir.filePath(QString::fromLatin1(kDatabaseFileName));
    if (!QFile::exists(dbPath)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("No library database found at %1").arg(dbPath);
        }
        return false;
    }

    QString ensureError;
    if (ensureLibraryDirectories(directoryPath, &ensureError).isEmpty()) {
        if (errorMessage) {
            *errorMessage = ensureError;
        }
        return false;
    }

    QSqlDatabase db;
    m_connectionName = QUuid::createUuid().toString(QUuid::Id128);
    db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);
    db.setDatabaseName(dbPath);

    if (!db.open()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Failed to open library database: %1").arg(db.lastError().text());
        }
        QSqlDatabase::removeDatabase(m_connectionName);
        m_connectionName.clear();
        return false;
    }

    m_libraryPath = directoryPath;
    m_database = db;

    QString adjustmentsError;
    if (!ensureDevelopAdjustmentsTable(&adjustmentsError)) {
        if (errorMessage) {
            *errorMessage = adjustmentsError;
        }
        closeLibrary();
        return false;
    }

    ensurePhotoNumberSupport();

    // Open metadata cache
    if (m_metadataCache) {
        QString cacheError;
        if (!m_metadataCache->openCache(directoryPath, &cacheError)) {
            qWarning() << "Failed to open metadata cache:" << cacheError;
            // Don't fail library opening if cache fails
        }
    }

    emit libraryOpened(m_libraryPath);
    emit assetsChanged();
    return true;
}

void LibraryManager::closeLibrary()
{
    if (m_jobManager) {
        for (const QUuid &jobId : std::as_const(m_previewJobIds)) {
            if (!jobId.isNull()) {
                m_jobManager->cancelJob(jobId, tr("Library closed"));
            }
        }
        for (const QUuid &jobId : std::as_const(m_metadataJobIds)) {
            if (!jobId.isNull()) {
                m_jobManager->cancelJob(jobId, tr("Library closed"));
            }
        }
    }
    m_previewJobIds.clear();
    m_metadataJobIds.clear();
    
    // Cancel batch preview job
    if (m_jobManager && !m_batchPreviewJobId.isNull()) {
        m_jobManager->cancelJob(m_batchPreviewJobId, tr("Library closed"));
        m_batchPreviewJobId = QUuid();
    }
    m_previewGenerationTotal = 0;
    m_previewGenerationCompleted = 0;
    
    // Cancel batch metadata job
    if (m_jobManager && !m_batchMetadataJobId.isNull()) {
        m_jobManager->cancelJob(m_batchMetadataJobId, tr("Library closed"));
        m_batchMetadataJobId = QUuid();
    }
    m_metadataExtractionTotal = 0;
    m_metadataExtractionCompleted = 0;

    // Cancel pending metadata extractions
    for (QFutureWatcher<AssetMetadata> *watcher : m_metadataExtractionWatchers) {
        if (watcher) {
            watcher->cancel();
            watcher->waitForFinished();
            watcher->deleteLater();
        }
    }
    m_metadataExtractionWatchers.clear();

    if (m_metadataCache) {
        m_metadataCache->closeCache();
    }

    if (!m_connectionName.isEmpty()) {
        if (m_database.isOpen()) {
            m_database.close();
        }
        QSqlDatabase::removeDatabase(m_connectionName);
        m_connectionName.clear();
    }
    m_database = QSqlDatabase();
    m_libraryPath.clear();
    emit libraryClosed();
}

void LibraryManager::setJobManager(JobManager *jobManager)
{
    m_jobManager = jobManager;
}

QVector<LibraryAsset> LibraryManager::assets() const
{
    FilterOptions defaultOptions;
    defaultOptions.sortOrder = FilterOptions::SortByDateDesc;
    return assets(defaultOptions);
}

QVector<LibraryAsset> LibraryManager::assets(const FilterOptions &filterOptions) const
{
    QVector<LibraryAsset> result;
    if (!hasOpenLibrary()) {
        return result;
    }

    // If metadata cache is available and filters are specified, use it
    if (m_metadataCache && m_metadataCache->hasOpenCache()) {
        // Check if any filters are actually applied (not just default sort)
        bool hasActiveFilters = (filterOptions.isoMin > 0 || filterOptions.isoMax > 0 || 
                                 !filterOptions.cameraMake.isEmpty() || !filterOptions.tags.isEmpty());
        
        QVector<qint64> filteredIds = m_metadataCache->filterAssets(filterOptions);
        
        // Only return empty if filters were actively applied and no results found
        // If no filters are active, fall through to query all assets
        if (hasActiveFilters && filteredIds.isEmpty()) {
            // Filters applied but no results
            return result;
        }
        
        // Build query with filtered IDs or all assets
        QString querySql;
        if (!hasActiveFilters && filteredIds.isEmpty()) {
            // No filters and no metadata yet - query all assets from main database
            querySql = QStringLiteral("SELECT id, photo_number, file_name, original_path, preview_path, format, width, height FROM assets");
        } else if (!filteredIds.isEmpty()) {
            // Use filtered/sorted results from metadata cache
            QStringList idPlaceholders;
            for (int i = 0; i < filteredIds.size(); ++i) {
                idPlaceholders.append(QStringLiteral("?"));
            }
            querySql = QStringLiteral("SELECT id, photo_number, file_name, original_path, preview_path, format, width, height FROM assets WHERE id IN (%1)").arg(idPlaceholders.join(QStringLiteral(", ")));
        } else {
            // Has filters but no results - return empty
            return result;
        }

        // Apply sorting if not handled by metadata cache
        // When metadata cache handles sorting, we preserve the order from filteredIds
        QString orderBy;
        if (filteredIds.isEmpty()) {
            // No metadata cache results, use simple sorting from main database
            switch (filterOptions.sortOrder) {
            case FilterOptions::SortByDateDesc:
                orderBy = QStringLiteral("ORDER BY imported_at DESC");
                break;
            case FilterOptions::SortByDateAsc:
                orderBy = QStringLiteral("ORDER BY imported_at ASC");
                break;
            case FilterOptions::SortByFileName:
                orderBy = QStringLiteral("ORDER BY file_name ASC");
                break;
            default:
                orderBy = QStringLiteral("ORDER BY imported_at DESC");
                break;
            }
            if (!orderBy.isEmpty()) {
                querySql += QStringLiteral(" %1").arg(orderBy);
            }
        } else {
            // Metadata cache already sorted, preserve order by using filteredIds order
            // Don't add ORDER BY - we'll preserve the order manually
        }

        QSqlQuery query(m_database);
        if (!filteredIds.isEmpty()) {
            query.prepare(querySql);
            for (qint64 id : filteredIds) {
                query.addBindValue(id);
            }
        } else {
            query.prepare(querySql);
        }

        if (!query.exec()) {
            qWarning() << "Failed to query filtered assets:" << query.lastError();
            return result;
        }

        // Create a map for quick lookup
        QHash<qint64, LibraryAsset> assetMap;
        while (query.next()) {
            LibraryAsset asset = hydrateAsset(query.record());
            assetMap.insert(asset.id, asset);
        }

        // Preserve order from filteredIds if available
        if (!filteredIds.isEmpty()) {
            for (qint64 id : filteredIds) {
                if (assetMap.contains(id)) {
                    result.append(assetMap.value(id));
                }
            }
        } else {
            // No filters, use query order
            result = assetMap.values().toVector();
        }
    } else {
        // Fallback to simple query without metadata cache
        QString orderBy;
        switch (filterOptions.sortOrder) {
        case FilterOptions::SortByDateDesc:
            orderBy = QStringLiteral("ORDER BY imported_at DESC");
            break;
        case FilterOptions::SortByDateAsc:
            orderBy = QStringLiteral("ORDER BY imported_at ASC");
            break;
        case FilterOptions::SortByFileName:
            orderBy = QStringLiteral("ORDER BY file_name ASC");
            break;
        default:
            orderBy = QStringLiteral("ORDER BY imported_at DESC");
            break;
        }

        QSqlQuery query(m_database);
        const QString sql = QStringLiteral("SELECT id, photo_number, file_name, original_path, preview_path, format, width, height FROM assets %1").arg(orderBy);
        if (!query.exec(sql)) {
            qWarning() << "Failed to query assets:" << query.lastError();
            return result;
        }

        while (query.next()) {
            result.append(hydrateAsset(query.record()));
        }
    }

    return result;
}

MetadataCache *LibraryManager::metadataCache() const
{
    return m_metadataCache;
}

QString LibraryManager::resolvePath(const QString &relativePath) const
{
    return absoluteAssetPath(relativePath);
}

void LibraryManager::importFiles(const QStringList &filePaths)
{
    if (!hasOpenLibrary() || filePaths.isEmpty()) {
        return;
    }

    const int total = filePaths.size();
    int imported = 0;
    int nextPhotoNumber = currentMaxPhotoNumber();
    
    // Count how many files will need metadata extraction and preview generation
    int metadataExtractionCount = 0;
    int previewGenerationCount = 0;
    
    for (const QString &sourceFile : filePaths) {
        QFileInfo info(sourceFile);
        if (info.exists()) {
            previewGenerationCount++;
            if (m_metadataCache && m_metadataCache->hasOpenCache()) {
                metadataExtractionCount++;
            }
        }
    }
    
    if (previewGenerationCount > 0) {
        startBatchPreviewJob(previewGenerationCount);
    }
    if (metadataExtractionCount > 0) {
        startBatchMetadataJob(metadataExtractionCount);
    }

    if (!m_database.transaction()) {
        emit errorOccurred(QStringLiteral("Failed to begin import transaction: %1").arg(m_database.lastError().text()));
        return;
    }

    for (const QString &sourceFile : filePaths) {
        QString errorMessage;
        QFileInfo info(sourceFile);
        if (!info.exists()) {
            emit errorOccurred(QStringLiteral("Skipped missing file: %1").arg(sourceFile));
            continue;
        }

        const int tentativeNumber = nextPhotoNumber + 1;
        const QString assignedPhotoNumber = QString::number(tentativeNumber);
        const int bucketIndex = bucketIndexForPhotoNumber(assignedPhotoNumber);

        QString storedRelative = storeOriginal(sourceFile, bucketIndex, &errorMessage);
        if (storedRelative.isEmpty()) {
            emit errorOccurred(errorMessage);
            continue;
        }

        QSqlQuery insert(m_database);
        insert.prepare(QStringLiteral(
            "INSERT INTO assets (file_name, original_path, format, imported_at, photo_number) "
            "VALUES (?, ?, ?, ?, ?)"));
        insert.addBindValue(info.fileName());
        insert.addBindValue(storedRelative);
        insert.addBindValue(info.suffix().toLower());
        insert.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
        insert.addBindValue(assignedPhotoNumber);

        if (!insert.exec()) {
            emit errorOccurred(QStringLiteral("Failed to insert asset metadata: %1").arg(insert.lastError().text()));
            continue;
        }

        ++nextPhotoNumber;

        const qint64 assetId = insert.lastInsertId().toLongLong();
        LibraryAsset asset;
        asset.id = assetId;
        asset.photoNumber = assignedPhotoNumber;
        asset.fileName = info.fileName();
        asset.originalRelativePath = storedRelative;
        asset.format = info.suffix().toLower();

        QString reservedPreview = reservePreviewPath(assetId, bucketIndex);
        if (reservedPreview.isEmpty()) {
            emit errorOccurred(QStringLiteral("Failed to reserve preview path for %1").arg(info.fileName()));
            continue;
        }
        asset.previewRelativePath = reservedPreview;

        // Enqueue metadata extraction in background
        if (m_metadataCache && m_metadataCache->hasOpenCache()) {
            enqueueMetadataExtraction(assetId, sourceFile);
        }

        enqueuePreviewGeneration(asset);
        imported++;
        emit importProgress(imported, total);
    }

    if (!m_database.commit()) {
        emit errorOccurred(QStringLiteral("Failed to commit import transaction: %1").arg(m_database.lastError().text()));
    }

    emit assetsChanged();
    emit importCompleted();
    
    // Complete batch jobs if all are done (they may complete asynchronously)
    if (m_previewGenerationTotal > 0 && m_previewGenerationCompleted >= m_previewGenerationTotal) {
        completeBatchPreviewJob();
    }
    if (m_metadataExtractionTotal > 0 && m_metadataExtractionCompleted >= m_metadataExtractionTotal) {
        completeBatchMetadataJob();
    }
}

QString LibraryManager::ensureLibraryDirectories(const QString &directoryPath, QString *errorMessage)
{
    QDir root(directoryPath);
    if (!root.exists()) {
        if (!root.mkpath(QStringLiteral("."))) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("Unable to create library directory at %1").arg(directoryPath);
            }
            return {};
        }
    }

    if (!root.exists(QString::fromLatin1(kOriginalsDirName))) {
        if (!root.mkdir(QString::fromLatin1(kOriginalsDirName))) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("Unable to create originals directory.");
            }
            return {};
        }
    }

    QDir originalsDir(root.filePath(QString::fromLatin1(kOriginalsDirName)));
    if (!originalsDir.exists(bucketName(1))) {
        originalsDir.mkpath(bucketName(1));
    }

    if (!root.exists(QString::fromLatin1(kPreviewsDirName))) {
        if (!root.mkdir(QString::fromLatin1(kPreviewsDirName))) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("Unable to create previews directory.");
            }
            return {};
        }
    }

    QDir previewsDir(root.filePath(QString::fromLatin1(kPreviewsDirName)));
    if (!previewsDir.exists(bucketName(1))) {
        previewsDir.mkpath(bucketName(1));
    }

    return root.filePath(QString::fromLatin1(kDatabaseFileName));
}

bool LibraryManager::initializeDatabaseSchema(QString *errorMessage)
{
    if (!hasOpenLibrary()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("No open library to initialize schema.");
        }
        return false;
    }

    QSqlQuery query(m_database);
    const QString createSql = QStringLiteral(
        "CREATE TABLE IF NOT EXISTS assets ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "file_name TEXT NOT NULL,"
        "photo_number TEXT,"
        "original_path TEXT NOT NULL,"
        "preview_path TEXT,"
        "format TEXT,"
        "width INTEGER DEFAULT 0,"
        "height INTEGER DEFAULT 0,"
        "imported_at TEXT NOT NULL"
        ");");

    if (!query.exec(createSql)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Failed to initialize library schema: %1").arg(query.lastError().text());
        }
        return false;
    }

    if (!ensureDevelopAdjustmentsTable(errorMessage)) {
        return false;
    }

    return true;
}

LibraryAsset LibraryManager::hydrateAsset(const QSqlRecord &record) const
{
    LibraryAsset asset;
    asset.id = record.value(QStringLiteral("id")).toLongLong();
    asset.photoNumber = record.value(QStringLiteral("photo_number")).toString();
    asset.fileName = record.value(QStringLiteral("file_name")).toString();
    asset.originalRelativePath = record.value(QStringLiteral("original_path")).toString();
    asset.previewRelativePath = record.value(QStringLiteral("preview_path")).toString();
    asset.format = record.value(QStringLiteral("format")).toString();
    asset.width = record.value(QStringLiteral("width")).toInt();
    asset.height = record.value(QStringLiteral("height")).toInt();
    return asset;
}

QString LibraryManager::originalsDirectory() const
{
    if (m_libraryPath.isEmpty()) {
        return {};
    }
    return QDir(m_libraryPath).filePath(QString::fromLatin1(kOriginalsDirName));
}

QString LibraryManager::previewsDirectory() const
{
    if (m_libraryPath.isEmpty()) {
        return {};
    }
    return QDir(m_libraryPath).filePath(QString::fromLatin1(kPreviewsDirName));
}

QString LibraryManager::absoluteAssetPath(const QString &relativePath) const
{
    if (relativePath.isEmpty() || m_libraryPath.isEmpty()) {
        return {};
    }

    if (QDir::isAbsolutePath(relativePath)) {
        return relativePath;
    }
    return QDir(m_libraryPath).filePath(relativePath);
}

QString LibraryManager::storeOriginal(const QString &sourceFile, int bucketIndex, QString *errorMessage) const
{
    const QString originalsRoot = originalsDirectory();
    if (originalsRoot.isEmpty()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Library originals directory is unavailable.");
        }
        return {};
    }

    if (!ensureBucketExists(originalsRoot, bucketIndex)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Unable to prepare originals bucket %1.").arg(bucketName(bucketIndex));
        }
        return {};
    }

    QFileInfo info(sourceFile);
    const QString extension = info.completeSuffix().toLower();
    QString baseName = QUuid::createUuid().toString(QUuid::Id128);
    if (!extension.isEmpty()) {
        baseName.append('.').append(extension);
    }

    const QString relativePath = makeOriginalRelativePath(bucketIndex, baseName);

    const QString destinationPath = absoluteAssetPath(relativePath);

    if (QFile::exists(destinationPath)) {
        QFile::remove(destinationPath);
    }

    if (!QFile::copy(sourceFile, destinationPath)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Failed to copy %1 to library storage.").arg(sourceFile);
        }
        return {};
    }

    return relativePath;
}

QString LibraryManager::reservePreviewPath(qint64 assetId, int bucketIndex) const
{
    const QString previewsRoot = previewsDirectory();
    if (previewsRoot.isEmpty()) {
        return {};
    }

    if (!ensureBucketExists(previewsRoot, bucketIndex)) {
        return {};
    }

    const QString filename = QStringLiteral("%1.jpg").arg(assetId);
    return makePreviewRelativePath(bucketIndex, filename);
}

void LibraryManager::enqueueMetadataExtraction(qint64 assetId, const QString &sourceFile)
{
    if (m_metadataExtractionWatchers.contains(assetId)) {
        // Already extracting metadata for this asset
        return;
    }

    // Use batch job ID if available, otherwise create individual job (for backwards compatibility)
    QUuid jobId = m_batchMetadataJobId.isNull() ? QUuid() : m_batchMetadataJobId;

    auto *watcher = new QFutureWatcher<AssetMetadata>(this);
    m_metadataExtractionWatchers.insert(assetId, watcher);

    connect(watcher, &QFutureWatcher<AssetMetadata>::finished, this, [this, assetId, watcher, jobId]() {
        handleMetadataExtractionComplete(assetId, jobId);
        watcher->deleteLater();
        m_metadataExtractionWatchers.remove(assetId);
    });

    // Extract metadata in background thread
    QFuture<AssetMetadata> future = QtConcurrent::run([assetId, sourceFile]() -> AssetMetadata {
        AssetMetadata meta;
        meta.assetId = assetId;

        DevelopMetadata developMeta;
        if (!ImageLoader::extractMetadata(sourceFile, &developMeta, nullptr)) {
            return meta;
        }

        // Parse ISO from string (e.g., "ISO 100" -> 100)
        QString isoStr = developMeta.iso.trimmed();
        if (isoStr.startsWith(QStringLiteral("ISO"), Qt::CaseInsensitive)) {
            isoStr = isoStr.mid(3).trimmed();
        }
        bool ok = false;
        int isoValue = isoStr.toInt(&ok);
        meta.iso = ok ? isoValue : 0;

        meta.cameraMake = developMeta.cameraMake.trimmed();
        meta.cameraModel = developMeta.cameraModel.trimmed();
        meta.captureDate = developMeta.captureDateTime;

        return meta;
    });

    watcher->setFuture(future);
}

void LibraryManager::handleMetadataExtractionComplete(qint64 assetId, const QUuid &jobId)
{
    auto *watcher = m_metadataExtractionWatchers.value(assetId, nullptr);
    if (!watcher || !watcher->isFinished()) {
        m_metadataExtractionCompleted++;
        updateBatchMetadataProgress();
        return;
    }

    if (!m_metadataCache || !m_metadataCache->hasOpenCache()) {
        m_metadataExtractionCompleted++;
        updateBatchMetadataProgress();
        return;
    }

    AssetMetadata meta = watcher->result();
    if (meta.assetId != assetId) {
        m_metadataExtractionCompleted++;
        updateBatchMetadataProgress();
        return; // Invalid result
    }

    QString metaError;
    if (!m_metadataCache->updateMetadata(assetId, meta, &metaError)) {
        qWarning() << "Failed to store metadata for asset" << assetId << ":" << metaError;
        m_metadataExtractionCompleted++;
        updateBatchMetadataProgress();
    } else {
        m_metadataExtractionCompleted++;
        updateBatchMetadataProgress();
        // Emit signal to update filter pane options if needed
        emit assetsChanged();
    }
}

void LibraryManager::startBatchMetadataJob(int total)
{
    if (!m_jobManager || total <= 0) {
        return;
    }
    
    m_metadataExtractionTotal = total;
    m_metadataExtractionCompleted = 0;
    m_batchMetadataJobId = m_jobManager->startJob(JobCategory::MetadataExtraction, 
                                                   tr("Extracting metadata"), 
                                                   tr("0 of %1 extracted").arg(total));
    m_jobManager->setIndeterminate(m_batchMetadataJobId, false);
    m_jobManager->updateProgress(m_batchMetadataJobId, 0, total);
}

void LibraryManager::updateBatchMetadataProgress()
{
    if (!m_jobManager || m_batchMetadataJobId.isNull() || m_metadataExtractionTotal <= 0) {
        return;
    }
    
    m_jobManager->updateProgress(m_batchMetadataJobId, m_metadataExtractionCompleted, m_metadataExtractionTotal);
    m_jobManager->updateDetail(m_batchMetadataJobId, tr("%1 of %2 extracted").arg(m_metadataExtractionCompleted).arg(m_metadataExtractionTotal));
    
    if (m_metadataExtractionCompleted >= m_metadataExtractionTotal) {
        completeBatchMetadataJob();
    }
}

void LibraryManager::completeBatchMetadataJob()
{
    if (!m_jobManager || m_batchMetadataJobId.isNull()) {
        return;
    }
    
    m_jobManager->completeJob(m_batchMetadataJobId, tr("All metadata extracted"));
    m_batchMetadataJobId = QUuid();
    m_metadataExtractionTotal = 0;
    m_metadataExtractionCompleted = 0;
}

void LibraryManager::startBatchPreviewJob(int total)
{
    if (!m_jobManager || total <= 0) {
        return;
    }
    
    m_previewGenerationTotal = total;
    m_previewGenerationCompleted = 0;
    m_batchPreviewJobId = m_jobManager->startJob(JobCategory::PreviewGeneration, 
                                                  tr("Generating previews"), 
                                                  tr("0 of %1 generated").arg(total));
    m_jobManager->setIndeterminate(m_batchPreviewJobId, false);
    m_jobManager->updateProgress(m_batchPreviewJobId, 0, total);
}

void LibraryManager::updateBatchPreviewProgress()
{
    if (!m_jobManager || m_batchPreviewJobId.isNull() || m_previewGenerationTotal <= 0) {
        return;
    }
    
    m_jobManager->updateProgress(m_batchPreviewJobId, m_previewGenerationCompleted, m_previewGenerationTotal);
    m_jobManager->updateDetail(m_batchPreviewJobId, tr("%1 of %2 generated").arg(m_previewGenerationCompleted).arg(m_previewGenerationTotal));
    
    if (m_previewGenerationCompleted >= m_previewGenerationTotal) {
        completeBatchPreviewJob();
    }
}

void LibraryManager::completeBatchPreviewJob()
{
    if (!m_jobManager || m_batchPreviewJobId.isNull()) {
        return;
    }
    
    m_jobManager->completeJob(m_batchPreviewJobId, tr("All previews generated"));
    m_batchPreviewJobId = QUuid();
    m_previewGenerationTotal = 0;
    m_previewGenerationCompleted = 0;
}

void LibraryManager::enqueuePreviewGeneration(const LibraryAsset &asset)
{
    PreviewJob job;
    job.assetId = asset.id;
    job.sourcePath = absoluteAssetPath(asset.originalRelativePath);
    job.previewPath = absoluteAssetPath(asset.previewRelativePath);
    job.maxHeight = kPreviewHeight;

    // Use batch job ID if available, otherwise create individual job (for backwards compatibility)
    QUuid jobId = m_batchPreviewJobId.isNull() ? QUuid() : m_batchPreviewJobId;
    
    if (m_jobManager && m_batchPreviewJobId.isNull()) {
        // Only create individual job if not using batch job
        const QString title = tr("Generating preview");
        const QString detail = QFileInfo(job.sourcePath).fileName();
        jobId = m_jobManager->startJob(JobCategory::PreviewGeneration, title, detail);
        m_jobManager->setIndeterminate(jobId, true);
        m_previewJobIds.insert(asset.id, jobId);
    } else if (!m_batchPreviewJobId.isNull()) {
        // Store batch job ID for this asset
        m_previewJobIds.insert(asset.id, m_batchPreviewJobId);
    }

    m_previewGenerator->enqueueJob(job);
}

bool LibraryManager::ensurePhotoNumberColumn(QString *errorMessage)
{
    if (!hasOpenLibrary()) {
        return false;
    }

    QSqlQuery pragma(m_database);
    if (!pragma.exec(QStringLiteral("PRAGMA table_info(assets)"))) {
        const QString message = QStringLiteral("Failed to inspect assets table: %1").arg(pragma.lastError().text());
        if (errorMessage) {
            *errorMessage = message;
        }
        return false;
    }

    bool hasColumn = false;
    while (pragma.next()) {
        if (pragma.value(1).toString() == QStringLiteral("photo_number")) {
            hasColumn = true;
            break;
        }
    }

    if (hasColumn) {
        return true;
    }

    QSqlQuery alter(m_database);
    if (!alter.exec(QStringLiteral("ALTER TABLE assets ADD COLUMN photo_number TEXT"))) {
        const QString message = QStringLiteral("Failed to add photo_number column: %1").arg(alter.lastError().text());
        if (errorMessage) {
            *errorMessage = message;
        }
        return false;
    }

    return true;
}

int LibraryManager::currentMaxPhotoNumber() const
{
    if (!hasOpenLibrary()) {
        return 0;
    }

    QSqlQuery query(m_database);
    if (!query.exec(QStringLiteral("SELECT MAX(CAST(photo_number AS INTEGER)) FROM assets"))) {
        qWarning() << "Failed to query max photo number:" << query.lastError();
        return 0;
    }

    if (query.next()) {
        return query.value(0).toInt();
    }

    return 0;
}

void LibraryManager::ensurePhotoNumbersAssigned()
{
    if (!hasOpenLibrary()) {
        return;
    }

    QSqlQuery query(m_database);
    if (!query.exec(QStringLiteral("SELECT id FROM assets WHERE photo_number IS NULL OR TRIM(photo_number) = '' ORDER BY imported_at ASC, id ASC"))) {
        qWarning() << "Failed to find assets missing photo numbers:" << query.lastError();
        return;
    }

    int nextNumber = currentMaxPhotoNumber();

    while (query.next()) {
        const qint64 assetId = query.value(0).toLongLong();
        ++nextNumber;

        QSqlQuery update(m_database);
        update.prepare(QStringLiteral("UPDATE assets SET photo_number = ? WHERE id = ?"));
        update.addBindValue(QString::number(nextNumber));
        update.addBindValue(assetId);

        if (!update.exec()) {
            qWarning() << "Failed to assign photo number:" << update.lastError();
            break;
        }
    }
}

void LibraryManager::ensurePhotoNumberSupport()
{
    if (!hasOpenLibrary()) {
        return;
    }

    QString errorMessage;
    if (!ensurePhotoNumberColumn(&errorMessage)) {
        if (!errorMessage.isEmpty()) {
            emit errorOccurred(errorMessage);
        }
        return;
    }

    ensurePhotoNumbersAssigned();
    ensureAssetStorageConsistency();
}

bool LibraryManager::ensureDevelopAdjustmentsTable(QString *errorMessage) const
{
    if (!hasOpenLibrary()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("No open library for adjustments schema.");
        }
        return false;
    }

    QSqlQuery query(m_database);
    const QString createSql = QStringLiteral(
        "CREATE TABLE IF NOT EXISTS develop_adjustments ("
        "asset_id INTEGER PRIMARY KEY,"
        "payload TEXT NOT NULL,"
        "updated_at TEXT NOT NULL,"
        "FOREIGN KEY(asset_id) REFERENCES assets(id) ON DELETE CASCADE"
        ");");

    if (!query.exec(createSql)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Failed to ensure develop adjustments table: %1").arg(query.lastError().text());
        }
        return false;
    }

    QSqlQuery indexQuery(m_database);
    if (!indexQuery.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS idx_develop_adjustments_updated_at ON develop_adjustments(updated_at DESC)"))) {
        // Index creation failure should not be fatal but log it.
        qWarning() << "Failed to create develop adjustments index:" << indexQuery.lastError();
    }

    return true;
}

DevelopAdjustments LibraryManager::loadDevelopAdjustments(qint64 assetId) const
{
    if (!hasOpenLibrary() || assetId <= 0) {
        return defaultDevelopAdjustments();
    }

    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("SELECT payload FROM develop_adjustments WHERE asset_id = ?"));
    query.addBindValue(assetId);

    if (!query.exec()) {
        qWarning() << "Failed to load develop adjustments for asset" << assetId << query.lastError();
        return defaultDevelopAdjustments();
    }

    if (!query.next()) {
        return defaultDevelopAdjustments();
    }

    const QByteArray payload = query.value(0).toByteArray();
    return deserializeAdjustments(payload);
}

bool LibraryManager::saveDevelopAdjustments(qint64 assetId,
                                            const DevelopAdjustments &adjustments,
                                            QString *errorMessage)
{
    if (!hasOpenLibrary() || assetId <= 0) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Cannot save adjustments without an open library.");
        }
        return false;
    }

    const QByteArray payload = serializeAdjustments(adjustments);
    const QString payloadText = QString::fromUtf8(payload);
    const QString timestamp = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);

    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "INSERT INTO develop_adjustments(asset_id, payload, updated_at) "
        "VALUES(?, ?, ?) "
        "ON CONFLICT(asset_id) DO UPDATE SET "
        "payload = excluded.payload, "
        "updated_at = excluded.updated_at;"));
    query.addBindValue(assetId);
    query.addBindValue(payloadText);
    query.addBindValue(timestamp);

    if (!query.exec()) {
        const QString message = QStringLiteral("Failed to persist develop adjustments: %1").arg(query.lastError().text());
        if (errorMessage) {
            *errorMessage = message;
        } else {
            qWarning() << message;
        }
        return false;
    }

    return true;
}

void LibraryManager::ensureAssetStorageConsistency()
{
    if (!hasOpenLibrary()) {
        return;
    }

    const QString originalsRoot = originalsDirectory();
    const QString previewsRoot = previewsDirectory();

    QSqlQuery query(m_database);
    if (!query.exec(QStringLiteral("SELECT id, photo_number, original_path, preview_path FROM assets"))) {
        qWarning() << "Failed to query assets for storage consistency:" << query.lastError();
        return;
    }

    while (query.next()) {
        const qint64 assetId = query.value(0).toLongLong();
        const QString photoNumber = query.value(1).toString();
        if (photoNumber.trimmed().isEmpty()) {
            continue;
        }

        const int bucketIndex = bucketIndexForPhotoNumber(photoNumber);
        ensureBucketExists(originalsRoot, bucketIndex);
        ensureBucketExists(previewsRoot, bucketIndex);

        const QString originalRel = query.value(2).toString();
        if (!originalRel.isEmpty()) {
            const QString fileName = QFileInfo(originalRel).fileName();
            if (!fileName.isEmpty()) {
                const QString expectedRel = makeOriginalRelativePath(bucketIndex, fileName);
                if (expectedRel != originalRel) {
                    const QString currentPath = absoluteAssetPath(originalRel);
                    const QString targetPath = absoluteAssetPath(expectedRel);
                    const QString targetDir = QFileInfo(targetPath).dir().absolutePath();
                    QDir().mkpath(targetDir);
                    if (QFile::exists(currentPath)) {
                        QFile::remove(targetPath);
                        if (QFile::rename(currentPath, targetPath)) {
                            QSqlQuery update(m_database);
                            update.prepare(QStringLiteral("UPDATE assets SET original_path = ? WHERE id = ?"));
                            update.addBindValue(expectedRel);
                            update.addBindValue(assetId);
                            if (!update.exec()) {
                                qWarning() << "Failed to update original_path for asset" << assetId << update.lastError();
                            }
                        } else {
                            qWarning() << "Failed to move original" << currentPath << "to" << targetPath;
                        }
                    }
                }
            }
        }

        const QString previewRel = query.value(3).toString();
        if (!previewRel.isEmpty()) {
            const QString fileName = QFileInfo(previewRel).fileName();
            if (!fileName.isEmpty()) {
                const QString expectedRel = makePreviewRelativePath(bucketIndex, fileName);
                if (expectedRel != previewRel) {
                    const QString currentPath = absoluteAssetPath(previewRel);
                    const QString targetPath = absoluteAssetPath(expectedRel);
                    const QString targetDir = QFileInfo(targetPath).dir().absolutePath();
                    QDir().mkpath(targetDir);
                    if (QFile::exists(currentPath)) {
                        QFile::remove(targetPath);
                        if (QFile::rename(currentPath, targetPath)) {
                            QSqlQuery update(m_database);
                            update.prepare(QStringLiteral("UPDATE assets SET preview_path = ? WHERE id = ?"));
                            update.addBindValue(expectedRel);
                            update.addBindValue(assetId);
                            if (!update.exec()) {
                                qWarning() << "Failed to update preview_path for asset" << assetId << update.lastError();
                            }
                        } else {
                            qWarning() << "Failed to move preview" << currentPath << "to" << targetPath;
                        }
                    }
                }
            }
        }
    }
}

QString LibraryManager::makeOriginalRelativePath(int bucketIndex, const QString &fileName) const
{
    return QStringLiteral("%1/%2/%3")
        .arg(QString::fromLatin1(kOriginalsDirName), bucketName(bucketIndex), fileName);
}

QString LibraryManager::makePreviewRelativePath(int bucketIndex, const QString &fileName) const
{
    return QStringLiteral("%1/%2/%3")
        .arg(QString::fromLatin1(kPreviewsDirName), bucketName(bucketIndex), fileName);
}

bool LibraryManager::ensureBucketExists(const QString &baseDir, int bucketIndex) const
{
    if (baseDir.isEmpty()) {
        return false;
    }

    QDir dir(baseDir);
    const QString bucket = bucketName(bucketIndex);
    if (dir.exists(bucket)) {
        return true;
    }
    return dir.mkpath(bucket);
}
