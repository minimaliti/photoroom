#include "metadatacache.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutexLocker>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QThread>
#include <QUuid>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QVariant>

namespace {
constexpr auto kCacheFileName = "metadata_cache.db";
}

MetadataCache::MetadataCache(QObject *parent)
    : QObject(parent)
{
}

MetadataCache::~MetadataCache()
{
    closeCache();
}

bool MetadataCache::openCache(const QString &libraryPath, QString *errorMessage)
{
    closeCache();

    if (libraryPath.isEmpty()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Library path is empty");
        }
        return false;
    }

    const QString cachePath = makeCachePath(libraryPath);
    QFileInfo info(cachePath);
    QDir dir = info.dir();
    if (!dir.exists()) {
        if (!dir.mkpath(QStringLiteral("."))) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("Failed to create cache directory: %1").arg(dir.absolutePath());
            }
            return false;
        }
    }

    m_connectionName = QUuid::createUuid().toString(QUuid::Id128);
    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);
    db.setDatabaseName(cachePath);

    if (!db.open()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Failed to open metadata cache: %1").arg(db.lastError().text());
        }
        QSqlDatabase::removeDatabase(m_connectionName);
        m_connectionName.clear();
        return false;
    }

    m_cachePath = libraryPath;
    m_database = db;

    if (!initializeSchema(errorMessage)) {
        closeCache();
        return false;
    }

    return true;
}

void MetadataCache::closeCache()
{
    QMutexLocker locker(&m_mutex);
    if (!m_connectionName.isEmpty()) {
        if (m_database.isOpen()) {
            m_database.close();
        }
        // Wait a bit to ensure all queries finish
        QThread::msleep(10);
        QSqlDatabase::removeDatabase(m_connectionName);
        m_connectionName.clear();
    }
    m_database = QSqlDatabase();
    m_cachePath.clear();
}

bool MetadataCache::hasOpenCache() const
{
    return m_database.isValid() && m_database.isOpen();
}

QString MetadataCache::cachePath() const
{
    return m_cachePath;
}

QString MetadataCache::makeCachePath(const QString &libraryPath) const
{
    return QDir(libraryPath).filePath(QString::fromLatin1(kCacheFileName));
}

bool MetadataCache::initializeSchema(QString *errorMessage)
{
    if (!hasOpenCache()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("No open cache to initialize schema");
        }
        return false;
    }

    QSqlQuery query(m_database);

    // Main metadata table
    const QString createMetadataTable = QStringLiteral(
        "CREATE TABLE IF NOT EXISTS asset_metadata ("
        "asset_id INTEGER PRIMARY KEY,"
        "iso INTEGER DEFAULT 0,"
        "camera_make TEXT,"
        "camera_model TEXT,"
        "capture_date TEXT,"
        "tags TEXT DEFAULT '[]'"
        ");");

    if (!query.exec(createMetadataTable)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Failed to create asset_metadata table: %1").arg(query.lastError().text());
        }
        return false;
    }

    // Create indexes for faster queries
    const QStringList indexes = {
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_metadata_iso ON asset_metadata(iso)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_metadata_camera_make ON asset_metadata(camera_make)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_metadata_capture_date ON asset_metadata(capture_date)"),
    };

    for (const QString &indexSql : indexes) {
        if (!query.exec(indexSql)) {
            qWarning() << "Failed to create index:" << query.lastError();
        }
    }

    return true;
}

bool MetadataCache::storeMetadata(qint64 assetId, const AssetMetadata &metadata, QString *errorMessage)
{
    QMutexLocker locker(&m_mutex);
    if (!hasOpenCache() || assetId <= 0) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Cannot store metadata without an open cache or valid asset ID");
        }
        return false;
    }

    QJsonArray tagsArray;
    for (const QString &tag : metadata.tags) {
        tagsArray.append(tag);
    }
    const QString tagsJson = QJsonDocument(tagsArray).toJson(QJsonDocument::Compact);

    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "INSERT INTO asset_metadata (asset_id, iso, camera_make, camera_model, capture_date, tags) "
        "VALUES (?, ?, ?, ?, ?, ?)"));
    query.addBindValue(assetId);
    query.addBindValue(metadata.iso);
    query.addBindValue(metadata.cameraMake);
    query.addBindValue(metadata.cameraModel);
    query.addBindValue(metadata.captureDate.isValid() ? metadata.captureDate.toString(Qt::ISODate) : QString());
    query.addBindValue(tagsJson);

    if (!query.exec()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Failed to store metadata: %1").arg(query.lastError().text());
        }
        return false;
    }

    emit metadataUpdated(assetId);
    return true;
}

bool MetadataCache::updateMetadata(qint64 assetId, const AssetMetadata &metadata, QString *errorMessage)
{
    QMutexLocker locker(&m_mutex);
    if (!hasOpenCache() || assetId <= 0) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Cannot update metadata without an open cache or valid asset ID");
        }
        return false;
    }

    QJsonArray tagsArray;
    for (const QString &tag : metadata.tags) {
        tagsArray.append(tag);
    }
    const QString tagsJson = QJsonDocument(tagsArray).toJson(QJsonDocument::Compact);

    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "INSERT INTO asset_metadata (asset_id, iso, camera_make, camera_model, capture_date, tags) "
        "VALUES (?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(asset_id) DO UPDATE SET "
        "iso = excluded.iso, "
        "camera_make = excluded.camera_make, "
        "camera_model = excluded.camera_model, "
        "capture_date = excluded.capture_date, "
        "tags = excluded.tags"));
    query.addBindValue(assetId);
    query.addBindValue(metadata.iso);
    query.addBindValue(metadata.cameraMake);
    query.addBindValue(metadata.cameraModel);
    query.addBindValue(metadata.captureDate.isValid() ? metadata.captureDate.toString(Qt::ISODate) : QString());
    query.addBindValue(tagsJson);

    if (!query.exec()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Failed to update metadata: %1").arg(query.lastError().text());
        }
        return false;
    }

    emit metadataUpdated(assetId);
    return true;
}

AssetMetadata MetadataCache::loadMetadata(qint64 assetId) const
{
    AssetMetadata metadata;
    metadata.assetId = assetId;

    QMutexLocker locker(&m_mutex);
    if (!hasOpenCache() || assetId <= 0) {
        return metadata;
    }

    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("SELECT iso, camera_make, camera_model, capture_date, tags FROM asset_metadata WHERE asset_id = ?"));
    query.addBindValue(assetId);

    if (!query.exec() || !query.next()) {
        return metadata;
    }

    metadata.iso = query.value(0).toInt();
    metadata.cameraMake = query.value(1).toString();
    metadata.cameraModel = query.value(2).toString();
    const QString dateStr = query.value(3).toString();
    if (!dateStr.isEmpty()) {
        metadata.captureDate = QDateTime::fromString(dateStr, Qt::ISODate);
    }
    const QString tagsJson = query.value(4).toString();
    if (!tagsJson.isEmpty()) {
        QJsonParseError error;
        QJsonDocument doc = QJsonDocument::fromJson(tagsJson.toUtf8(), &error);
        if (error.error == QJsonParseError::NoError && doc.isArray()) {
            QJsonArray array = doc.array();
            for (const QJsonValue &value : array) {
                if (value.isString()) {
                    metadata.tags.append(value.toString());
                }
            }
        }
    }

    return metadata;
}

bool MetadataCache::deleteMetadata(qint64 assetId, QString *errorMessage)
{
    QMutexLocker locker(&m_mutex);
    if (!hasOpenCache() || assetId <= 0) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Cannot delete metadata without an open cache or valid asset ID");
        }
        return false;
    }

    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("DELETE FROM asset_metadata WHERE asset_id = ?"));
    query.addBindValue(assetId);

    if (!query.exec()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Failed to delete metadata: %1").arg(query.lastError().text());
        }
        return false;
    }

    return true;
}

QVector<qint64> MetadataCache::filterAssets(const FilterOptions &options) const
{
    QVector<qint64> result;

    QMutexLocker locker(&m_mutex);
    if (!hasOpenCache()) {
        return result;
    }

    QStringList conditions;
    QVariantList bindValues;

    // ISO range filter
    if (options.isoMin > 0 || options.isoMax > 0) {
        if (options.isoMin > 0 && options.isoMax > 0) {
            // Both min and max specified: range filter
            conditions.append(QStringLiteral("CAST(iso AS INTEGER) >= ? AND CAST(iso AS INTEGER) <= ?"));
            bindValues.append(options.isoMin);
            bindValues.append(options.isoMax);
        } else if (options.isoMin > 0) {
            // Only min specified: >= filter
            conditions.append(QStringLiteral("CAST(iso AS INTEGER) >= ?"));
            bindValues.append(options.isoMin);
        } else if (options.isoMax > 0) {
            // Only max specified (min is "Any"): <= filter, but exclude ISO 0 (no data)
            conditions.append(QStringLiteral("CAST(iso AS INTEGER) > 0 AND CAST(iso AS INTEGER) <= ?"));
            bindValues.append(options.isoMax);
        }
    }

    // Camera make/model filter
    // The filter value is in format "Make Model" or just "Make" or "Model"
    if (!options.cameraMake.isEmpty()) {
        // Try to match the combined string against make+model or individual fields
        // Split the filter value to see if it contains both make and model
        QStringList parts = options.cameraMake.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (parts.size() >= 2) {
            // Assume first part is make, rest is model
            QString filterMake = parts.first();
            QString filterModel = parts.mid(1).join(QLatin1Char(' '));
            conditions.append(QStringLiteral("(camera_make = ? AND camera_model = ?) OR (camera_make || ' ' || camera_model = ?)"));
            bindValues.append(filterMake);
            bindValues.append(filterModel);
            bindValues.append(options.cameraMake);
        } else {
            // Single value - match against either make or model
            conditions.append(QStringLiteral("(camera_make = ? OR camera_model = ? OR camera_make || ' ' || camera_model = ?)"));
            bindValues.append(options.cameraMake);
            bindValues.append(options.cameraMake);
            bindValues.append(options.cameraMake);
        }
    }

    // Tags filter
    if (!options.tags.isEmpty()) {
        QStringList tagConditions;
        for (const QString &tag : options.tags) {
            tagConditions.append(QStringLiteral("tags LIKE ?"));
            bindValues.append(QStringLiteral("%\"%1\"%").arg(tag));
        }
        if (!tagConditions.isEmpty()) {
            conditions.append(QStringLiteral("(%1)").arg(tagConditions.join(QStringLiteral(" OR "))));
        }
    }

    QString whereClause;
    if (!conditions.isEmpty()) {
        whereClause = QStringLiteral("WHERE %1").arg(conditions.join(QStringLiteral(" AND ")));
    }

    // Build ORDER BY clause
    QString orderBy;
    switch (options.sortOrder) {
    case FilterOptions::SortByDateDesc:
        // Use capture_date if available, otherwise fall back to asset_id (newer assets have higher IDs)
        orderBy = QStringLiteral("ORDER BY CASE WHEN capture_date IS NULL OR capture_date = '' THEN 0 ELSE 1 END DESC, capture_date DESC, asset_id DESC");
        break;
    case FilterOptions::SortByDateAsc:
        orderBy = QStringLiteral("ORDER BY CASE WHEN capture_date IS NULL OR capture_date = '' THEN 1 ELSE 0 END ASC, capture_date ASC, asset_id ASC");
        break;
    case FilterOptions::SortByIsoDesc:
        orderBy = QStringLiteral("ORDER BY CAST(iso AS INTEGER) DESC, asset_id DESC");
        break;
    case FilterOptions::SortByIsoAsc:
        orderBy = QStringLiteral("ORDER BY CAST(iso AS INTEGER) ASC, asset_id ASC");
        break;
    case FilterOptions::SortByCameraMake:
        orderBy = QStringLiteral("ORDER BY camera_make ASC, camera_model ASC, capture_date DESC");
        break;
    case FilterOptions::SortByFileName:
        // This would need to join with assets table, but for now we'll sort by asset_id
        orderBy = QStringLiteral("ORDER BY asset_id ASC");
        break;
    }

    QSqlQuery query(m_database);
    const QString sql = QStringLiteral("SELECT asset_id FROM asset_metadata %1 %2").arg(whereClause, orderBy);
    query.prepare(sql);

    for (const QVariant &value : bindValues) {
        query.addBindValue(value);
    }

    if (!query.exec()) {
        qWarning() << "Failed to filter assets:" << query.lastError();
        return result;
    }

    while (query.next()) {
        result.append(query.value(0).toLongLong());
    }

    return result;
}

QStringList MetadataCache::getAllCameraMakes() const
{
    QStringList result;
    QSet<QString> cameraSet;

    QMutexLocker locker(&m_mutex);
    if (!hasOpenCache()) {
        return result;
    }

    QSqlQuery query(m_database);
    if (!query.exec(QStringLiteral("SELECT DISTINCT camera_make, camera_model FROM asset_metadata WHERE (camera_make IS NOT NULL AND camera_make != '') OR (camera_model IS NOT NULL AND camera_model != '') ORDER BY camera_make, camera_model"))) {
        qWarning() << "Failed to get camera makes:" << query.lastError();
        return result;
    }

    while (query.next()) {
        const QString make = query.value(0).toString().trimmed();
        const QString model = query.value(1).toString().trimmed();
        
        QString camera;
        if (!make.isEmpty() && !model.isEmpty()) {
            camera = QStringLiteral("%1 %2").arg(make, model);
        } else if (!make.isEmpty()) {
            camera = make;
        } else if (!model.isEmpty()) {
            camera = model;
        }
        
        if (!camera.isEmpty() && !cameraSet.contains(camera)) {
            cameraSet.insert(camera);
            result.append(camera);
        }
    }

    return result;
}

QStringList MetadataCache::getAllTags() const
{
    QStringList result;
    QSet<QString> tagSet;

    QMutexLocker locker(&m_mutex);
    if (!hasOpenCache()) {
        return result;
    }

    QSqlQuery query(m_database);
    if (!query.exec(QStringLiteral("SELECT tags FROM asset_metadata WHERE tags IS NOT NULL AND tags != '' AND tags != '[]'"))) {
        qWarning() << "Failed to get tags:" << query.lastError();
        return result;
    }

    while (query.next()) {
        const QString tagsJson = query.value(0).toString();
        if (tagsJson.isEmpty()) {
            continue;
        }

        QJsonParseError error;
        QJsonDocument doc = QJsonDocument::fromJson(tagsJson.toUtf8(), &error);
        if (error.error == QJsonParseError::NoError && doc.isArray()) {
            QJsonArray array = doc.array();
            for (const QJsonValue &value : array) {
                if (value.isString()) {
                    tagSet.insert(value.toString());
                }
            }
        }
    }

    result = tagSet.values();
    result.sort();
    return result;
}

int MetadataCache::getMinIso() const
{
    QMutexLocker locker(&m_mutex);
    if (!hasOpenCache()) {
        return 0;
    }

    QSqlQuery query(m_database);
    if (!query.exec(QStringLiteral("SELECT MIN(iso) FROM asset_metadata WHERE iso > 0"))) {
        return 0;
    }

    if (query.next()) {
        return query.value(0).toInt();
    }

    return 0;
}

int MetadataCache::getMaxIso() const
{
    QMutexLocker locker(&m_mutex);
    if (!hasOpenCache()) {
        return 0;
    }

    QSqlQuery query(m_database);
    if (!query.exec(QStringLiteral("SELECT MAX(iso) FROM asset_metadata WHERE iso > 0"))) {
        return 0;
    }

    if (query.next()) {
        return query.value(0).toInt();
    }

    return 0;
}

bool MetadataCache::addTag(qint64 assetId, const QString &tag, QString *errorMessage)
{
    AssetMetadata metadata = loadMetadata(assetId);
    if (metadata.assetId != assetId) {
        metadata.assetId = assetId;
    }

    if (!metadata.tags.contains(tag)) {
        metadata.tags.append(tag);
    }

    return updateMetadata(assetId, metadata, errorMessage);
}

bool MetadataCache::removeTag(qint64 assetId, const QString &tag, QString *errorMessage)
{
    QMutexLocker locker(&m_mutex);
    AssetMetadata metadata = loadMetadata(assetId);
    if (metadata.assetId != assetId) {
        return true; // No metadata exists, nothing to remove
    }

    metadata.tags.removeAll(tag);
    return updateMetadata(assetId, metadata, errorMessage);
}

bool MetadataCache::setTags(qint64 assetId, const QStringList &tags, QString *errorMessage)
{
    QMutexLocker locker(&m_mutex);
    AssetMetadata metadata = loadMetadata(assetId);
    if (metadata.assetId != assetId) {
        metadata.assetId = assetId;
    }

    metadata.tags = tags;
    return updateMetadata(assetId, metadata, errorMessage);
}

