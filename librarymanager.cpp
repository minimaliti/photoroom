#include "librarymanager.h"
#include <QStandardPaths>
#include <QDebug>
#include <QFile>
#include <QCryptographicHash>
#include <QTextStream> // For reading/writing text files

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
    if (!prinfoFile.open(QIODevice::WriteOnly)) {
        emit error(tr("Failed to create info.prinfo file in %1").arg(libraryPath));
        return false;
    }
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
    m_thumbnailCacheMap.clear(); // Clear previous map
    loadThumbnailCacheMap(); // Load cache map for the newly opened library
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

// New methods implementation
void LibraryManager::loadThumbnailCacheMap()
{
    QString infoFilePath = QDir(m_currentLibraryPath).filePath("info.prinfo");
    QFile infoFile(infoFilePath);
    if (!infoFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qWarning() << "Failed to open info.prinfo for reading:" << infoFile.errorString();
        return;
    }

    QTextStream in(&infoFile);
    while (!in.atEnd()) {
        QString line = in.readLine();
        QStringList parts = line.split("|"); // Using '|' as a delimiter
        if (parts.size() == 2) {
            m_thumbnailCacheMap.insert(parts[0], parts[1]);
        }
    }
    infoFile.close();
    qDebug() << "Loaded thumbnail cache map from" << infoFilePath << ":" << m_thumbnailCacheMap.size() << "entries.";
}

void LibraryManager::saveThumbnailCacheMap()
{
    QString infoFilePath = QDir(m_currentLibraryPath).filePath("info.prinfo");
    QFile infoFile(infoFilePath);
    // Use Truncate to clear existing content before writing
    if (!infoFile.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        qWarning() << "Failed to open info.prinfo for writing:" << infoFile.errorString();
        return;
    }

    QTextStream out(&infoFile);
    for (auto it = m_thumbnailCacheMap.constBegin(); it != m_thumbnailCacheMap.constEnd(); ++it) {
        out << it.key() << "|" << it.value() << "\n";
    }
    infoFile.close();
    qDebug() << "Saved thumbnail cache map to" << infoFilePath << ":" << m_thumbnailCacheMap.size() << "entries.";
}

QString LibraryManager::getCacheFileNameFromInfo(const QString &originalFilePath) const
{
    return m_thumbnailCacheMap.value(originalFilePath);
}

void LibraryManager::addCacheEntryToInfo(const QString &originalFilePath, const QString &cacheFileName)
{
    m_thumbnailCacheMap.insert(originalFilePath, cacheFileName);
    saveThumbnailCacheMap(); // Save immediately after adding an entry
}
