#ifndef LIBRARYMANAGER_H
#define LIBRARYMANAGER_H

#include <QObject>
#include <QString>
#include <QDir>
#include <QList>
#include <QDateTime>
#include <QJsonObject>
#include <QHash>

struct PhotoInfo {
    int photo_id = 0;
    QString file_path; // Path relative to library root
    QDateTime date_taken;
    QString cache_file_path;

    void write(QJsonObject &json) const;
    void read(const QJsonObject &json);
};

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

    void addImportedFile(const QString &originalFilePath, const QString &libraryFilePath);
    QString getCacheFileNameFromInfo(const QString &libraryFilePath) const;
    void addCacheEntryToInfo(const QString &libraryFilePath, const QString &cacheFileName);

signals:
    void libraryOpened(const QString &libraryPath);
    void error(const QString &message);

private:
    QString m_currentLibraryPath;
    QList<PhotoInfo> m_photos;
    QHash<QString, int> m_photoPathMap; // key: relative file path, value: index in m_photos
    int m_lastPhotoId = 0;

    bool isValidLibrary(const QString &libraryPath) const;
    void loadInfo();
    void saveInfo();
};

#endif // LIBRARYMANAGER_H
