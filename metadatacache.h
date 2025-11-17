#ifndef METADATACACHE_H
#define METADATACACHE_H

#include <QObject>
#include <QSqlDatabase>
#include <QString>
#include <QStringList>
#include <QDateTime>
#include <QVector>
#include <QRecursiveMutex>

struct AssetMetadata
{
    qint64 assetId = -1;
    int iso = 0;
    QString cameraMake;
    QString cameraModel;
    QDateTime captureDate;
    QStringList tags;
};

struct FilterOptions
{
    enum SortOrder {
        SortByDateDesc,
        SortByDateAsc,
        SortByIsoDesc,
        SortByIsoAsc,
        SortByCameraMake,
        SortByFileName
    };

    SortOrder sortOrder = SortByDateDesc;
    int isoMin = 0;
    int isoMax = 0; // 0 means no max
    QString cameraMake;
    QStringList tags; // Empty means no tag filter
};

class MetadataCache : public QObject
{
    Q_OBJECT
public:
    explicit MetadataCache(QObject *parent = nullptr);
    ~MetadataCache() override;

    bool openCache(const QString &libraryPath, QString *errorMessage = nullptr);
    void closeCache();

    bool hasOpenCache() const;
    QString cachePath() const;

    bool storeMetadata(qint64 assetId, const AssetMetadata &metadata, QString *errorMessage = nullptr);
    bool updateMetadata(qint64 assetId, const AssetMetadata &metadata, QString *errorMessage = nullptr);
    AssetMetadata loadMetadata(qint64 assetId) const;
    bool deleteMetadata(qint64 assetId, QString *errorMessage = nullptr);

    QVector<qint64> filterAssets(const FilterOptions &options) const;

    QStringList getAllCameraMakes() const;
    QStringList getAllTags() const;
    int getMinIso() const;
    int getMaxIso() const;

    bool addTag(qint64 assetId, const QString &tag, QString *errorMessage = nullptr);
    bool removeTag(qint64 assetId, const QString &tag, QString *errorMessage = nullptr);
    bool setTags(qint64 assetId, const QStringList &tags, QString *errorMessage = nullptr);

signals:
    void metadataUpdated(qint64 assetId);

private:
    bool initializeSchema(QString *errorMessage);
    QString makeCachePath(const QString &libraryPath) const;

    QString m_cachePath;
    QString m_connectionName;
    QSqlDatabase m_database;
    mutable QRecursiveMutex m_mutex;
};

#endif // METADATACACHE_H

