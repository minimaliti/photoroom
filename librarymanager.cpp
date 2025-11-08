#include "librarymanager.h"

#include "imageloader.h"
#include "previewgenerator.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>

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
{
    connect(m_previewGenerator, &PreviewGenerator::previewReady, this, [this](const PreviewResult &result) {
        if (!hasOpenLibrary()) {
            return;
        }

        if (!result.success) {
            emit errorOccurred(result.errorMessage);
            return;
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

    ensurePhotoNumberSupport();

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

    ensurePhotoNumberSupport();

    emit libraryOpened(m_libraryPath);
    emit assetsChanged();
    return true;
}

void LibraryManager::closeLibrary()
{
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

QVector<LibraryAsset> LibraryManager::assets() const
{
    QVector<LibraryAsset> result;
    if (!hasOpenLibrary()) {
        return result;
    }

    QSqlQuery query(m_database);
    if (!query.exec(QStringLiteral("SELECT id, photo_number, file_name, original_path, preview_path, format, width, height FROM assets ORDER BY imported_at DESC"))) {
        qWarning() << "Failed to query assets:" << query.lastError();
        return result;
    }

    while (query.next()) {
        result.append(hydrateAsset(query.record()));
    }
    return result;
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

        enqueuePreviewGeneration(asset);
        imported++;
        emit importProgress(imported, total);
    }

    if (!m_database.commit()) {
        emit errorOccurred(QStringLiteral("Failed to commit import transaction: %1").arg(m_database.lastError().text()));
    }

    emit assetsChanged();
    emit importCompleted();
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

void LibraryManager::enqueuePreviewGeneration(const LibraryAsset &asset)
{
    PreviewJob job;
    job.assetId = asset.id;
    job.sourcePath = absoluteAssetPath(asset.originalRelativePath);
    job.previewPath = absoluteAssetPath(asset.previewRelativePath);
    job.maxHeight = kPreviewHeight;

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
