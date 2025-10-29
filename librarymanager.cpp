#include "librarymanager.h"
#include <QStandardPaths>
#include <QDebug>
#include <QFile>

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
        // If it exists, we might want to just try to open it or validate it.
        // For now, let's consider it an error if we're trying to *create* it.
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
