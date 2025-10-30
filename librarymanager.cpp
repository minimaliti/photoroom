#include "librarymanager.h"
#include <QStandardPaths>
#include <QDebug>
#include <QFile>
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QFileInfo>
#include <libraw/libraw.h>
#include <QImageReader>

void PhotoInfo::write(QJsonObject &json) const {
    json["photo_id"] = photo_id;
    json["file_path"] = file_path;
    json["date_taken"] = date_taken.toString(Qt::ISODate);
    json["cache_file_path"] = cache_file_path;
}

void PhotoInfo::read(const QJsonObject &json) {
    photo_id = json["photo_id"].toInt();
    file_path = json["file_path"].toString();
    date_taken = QDateTime::fromString(json["date_taken"].toString(), Qt::ISODate);
    cache_file_path = json["cache_file_path"].toString();
}

LibraryManager::LibraryManager(QObject *parent) : QObject(parent)
{

}

QString LibraryManager::getDefaultLibraryPath() const
{
    QString picturesLocation = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    return QDir(picturesLocation).filePath("Default.prlibrary");
}

bool LibraryManager::createLibrary(const QString &libraryPath)
{
    QDir libraryDir(libraryPath);

    if (libraryDir.exists()) {
        qWarning() << "Library already exists at:" << libraryPath;
        emit error(tr("Library already exists at %1").arg(libraryPath));
        return false;
    }

    if (!libraryDir.mkpath(".")) {
        emit error(tr("Failed to create library directory: %1").arg(libraryPath));
        return false;
    }

    // Create info.prinfo file
    QFile prinfoFile(libraryDir.filePath("info.prinfo"));
    if (!prinfoFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
        emit error(tr("Failed to create info.prinfo file in %1").arg(libraryPath));
        return false;
    }
    prinfoFile.write("{\n    \"photos\": []\n}\n");
    prinfoFile.close();

    // Create subdirectories
    if (!libraryDir.mkpath("imported") ||
        !libraryDir.mkpath("cache/thumbnail") ||
        !libraryDir.mkpath("cache/prerender"))
    {
        emit error(tr("Failed to create subdirectories in library: %1").arg(libraryPath));
        return false;
    }

    qDebug() << "Library created successfully at:" << libraryPath;
    return true;
}

bool LibraryManager::isValidLibrary(const QString &libraryPath) const
{
    QDir libraryDir(libraryPath);
    return libraryDir.exists() &&
           QFile::exists(libraryDir.filePath("info.prinfo")) &&
           libraryDir.exists("imported") &&
           libraryDir.exists("cache/thumbnail") &&
           libraryDir.exists("cache/prerender");
}

bool LibraryManager::openLibrary(const QString &libraryPath)
{
    if (!isValidLibrary(libraryPath)) {
        emit error(tr("Invalid PhotoRoom library: %1").arg(libraryPath));
        return false;
    }

    m_currentLibraryPath = libraryPath;
    loadInfo();
    qDebug() << "Library opened successfully:" << libraryPath;
    emit libraryOpened(libraryPath);
    return true;
}

QString LibraryManager::currentLibraryImportPath() const
{
    if (m_currentLibraryPath.isEmpty()) return QString();
    return QDir(m_currentLibraryPath).filePath("imported");
}

QString LibraryManager::currentLibraryCachePath() const
{
    if (m_currentLibraryPath.isEmpty()) return QString();
    return QDir(m_currentLibraryPath).filePath("cache");
}

QString LibraryManager::currentLibraryThumbnailPath() const
{
    if (m_currentLibraryPath.isEmpty()) return QString();
    return QDir(m_currentLibraryPath).filePath("cache/thumbnail");
}

QString LibraryManager::currentLibraryPrerenderPath() const
{
    if (m_currentLibraryPath.isEmpty()) return QString();
    return QDir(m_currentLibraryPath).filePath("cache/prerender");
}

QString LibraryManager::getCacheFileName(const QString &originalFilePath) const
{
    if (originalFilePath.isEmpty()) return QString();
    QByteArray hash = QCryptographicHash::hash(originalFilePath.toUtf8(), QCryptographicHash::Md5);
    return hash.toHex() + ".jpg"; // Assuming thumbnails are stored as JPG
}

QString LibraryManager::currentLibraryThumbnailCachePath() const
{
    if (m_currentLibraryPath.isEmpty()) return QString();
    return QDir(m_currentLibraryPath).filePath("cache/thumbnail");
}

void LibraryManager::loadInfo()
{
    m_photos.clear();
    m_photoPathMap.clear();
    m_lastPhotoId = 0;
    QString infoFilePath = QDir(m_currentLibraryPath).filePath("info.prinfo");
    QFile infoFile(infoFilePath);
    if (!infoFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qWarning() << "Failed to open info.prinfo for reading:" << infoFile.errorString();
        return;
    }

    QByteArray data = infoFile.readAll();
    infoFile.close();

    if (data.isEmpty()) {
        qDebug() << "info.prinfo is empty, using empty data sets.";
        return;
    }

    QJsonDocument doc = QJsonDocument::fromJson(data);
    if (doc.isNull() || !doc.isObject()) {
        qWarning() << "Failed to parse info.prinfo as JSON object:" << infoFilePath;
        return;
    }

    QJsonObject rootObj = doc.object();

    if (rootObj.contains("photos") && rootObj["photos"].isArray()) {
        QJsonArray photosArray = rootObj["photos"].toArray();
        for (const QJsonValue &value : photosArray) {
            QJsonObject obj = value.toObject();
            PhotoInfo info;
            info.read(obj);
            m_photos.append(info);
            m_photoPathMap.insert(info.file_path, m_photos.size() - 1);
            if (info.photo_id > m_lastPhotoId) {
                m_lastPhotoId = info.photo_id;
            }
        }
    }
    qDebug() << "Loaded info from" << infoFilePath << ":" << m_photos.size() << "photos.";
}

void LibraryManager::saveInfo()
{
    QString infoFilePath = QDir(m_currentLibraryPath).filePath("info.prinfo");
    QFile infoFile(infoFilePath);
    if (!infoFile.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        qWarning() << "Failed to open info.prinfo for writing:" << infoFile.errorString();
        return;
    }

    QJsonArray photosArray;
    for (const auto &info : m_photos) {
        QJsonObject infoObj;
        info.write(infoObj);
        photosArray.append(infoObj);
    }

    QJsonObject rootObj;
    rootObj["photos"] = photosArray;

    QJsonDocument doc(rootObj);
    infoFile.write(doc.toJson(QJsonDocument::Indented));
    infoFile.close();
    qDebug() << "Saved info to" << infoFilePath << ":" << m_photos.size() << "photos.";
}

void LibraryManager::addImportedFile(const QString &originalFilePath, const QString &libraryFilePath)
{
    PhotoInfo info;
    m_lastPhotoId++;
    info.photo_id = m_lastPhotoId;
    info.file_path = QDir(m_currentLibraryPath).relativeFilePath(libraryFilePath);

    QString lowerPath = originalFilePath.toLower();
    bool isRaw = lowerPath.endsWith(".cr2") || lowerPath.endsWith(".nef") || lowerPath.endsWith(".arw") || lowerPath.endsWith(".dng");

    if (isRaw) {
        // Use LibRaw for RAW files
        LibRaw rawProcessor;
        if (rawProcessor.open_file(originalFilePath.toStdString().c_str()) == LIBRAW_SUCCESS) {
            if (rawProcessor.unpack_thumb() == LIBRAW_SUCCESS) {
                if (rawProcessor.imgdata.other.timestamp > 0) {
                    info.date_taken = QDateTime::fromSecsSinceEpoch(rawProcessor.imgdata.other.timestamp);
                }
            }
            rawProcessor.recycle();
        }
    } else {
        // Use QImageReader for other formats (JPG, PNG, etc.)
        QImageReader reader(originalFilePath);
        QString dateString = reader.text("DateTimeOriginal"); // EXIF field
        if (!dateString.isEmpty()) {
            // EXIF date format is "YYYY:MM:DD hh:mm:ss"
            info.date_taken = QDateTime::fromString(dateString, "yyyy:MM:dd hh:mm:ss");
        }
    }

    // Fallback to file creation date
    if (!info.date_taken.isValid()) {
        info.date_taken = QFileInfo(originalFilePath).birthTime();
    }

    // Fallback to now if still invalid
    if (!info.date_taken.isValid()) {
        info.date_taken = QDateTime::currentDateTime();
    }

    m_photos.append(info);
    m_photoPathMap.insert(info.file_path, m_photos.size() - 1);
    saveInfo();
}

QString LibraryManager::getCacheFileNameFromInfo(const QString &libraryFilePath) const
{
    QString relativePath = QDir(m_currentLibraryPath).relativeFilePath(libraryFilePath);
    if (m_photoPathMap.contains(relativePath)) {
        int index = m_photoPathMap.value(relativePath);
        return m_photos[index].cache_file_path;
    }
    return QString();
}

void LibraryManager::addCacheEntryToInfo(const QString &libraryFilePath, const QString &cacheFileName)
{
    QString relativePath = QDir(m_currentLibraryPath).relativeFilePath(libraryFilePath);
    if (m_photoPathMap.contains(relativePath)) {
        int index = m_photoPathMap.value(relativePath);
        m_photos[index].cache_file_path = cacheFileName;
        saveInfo();
    } else {
        qWarning() << "Could not find photo info for" << libraryFilePath << "to add cache entry.";
    }
}
