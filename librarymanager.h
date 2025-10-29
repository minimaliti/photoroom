#ifndef LIBRARYMANAGER_H
#define LIBRARYMANAGER_H

#include <QObject>
#include <QString>
#include <QDir>

class LibraryManager : public QObject
{
    Q_OBJECT
public:
    explicit LibraryManager(QObject *parent = nullptr);

    QString getDefaultLibraryPath() const;
    bool createLibrary(const QString &libraryPath);
    bool openLibrary(const QString &libraryPath);
    QString currentLibraryImportPath() const;
    QString currentLibraryCachePath() const;
    QString currentLibraryThumbnailPath() const;
    QString currentLibraryPrerenderPath() const;
    QString currentLibraryRootPath() const { return m_currentLibraryPath; }
    QString getCacheFileName(const QString &originalFilePath) const;
    QString currentLibraryThumbnailCachePath() const;

signals:
    void libraryOpened(const QString &libraryPath);
    void error(const QString &message);

private:
    QString m_currentLibraryPath;
    QHash<QString, QString> m_thumbnailCacheMap;

    bool isValidLibrary(const QString &libraryPath) const;
    void loadThumbnailCacheMap();
    void saveThumbnailCacheMap();

public:
    QString getCacheFileNameFromInfo(const QString &originalFilePath) const;
    void addCacheEntryToInfo(const QString &originalFilePath, const QString &cacheFileName);
};

#endif // LIBRARYMANAGER_H
