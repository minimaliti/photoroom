#ifndef LIBRARYMANAGER_H
#define LIBRARYMANAGER_H

#include <QObject>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QString>
#include <QVector>
#include <QHash>
#include <QUuid>
#include <QFutureWatcher>
#include <QFuture>

#include "developtypes.h"
#include "metadatacache.h"

struct LibraryAsset
{
    qint64 id = -1;
    QString photoNumber;
    QString fileName;
    QString originalRelativePath;
    QString previewRelativePath;
    QString format;
    int width = 0;
    int height = 0;
};

class PreviewGenerator;
class JobManager;

class LibraryManager : public QObject
{
    Q_OBJECT
public:
    explicit LibraryManager(QObject *parent = nullptr);
    ~LibraryManager() override;

    bool hasOpenLibrary() const;
    QString libraryPath() const;

    bool createLibrary(const QString &directoryPath, QString *errorMessage = nullptr);
    bool openLibrary(const QString &directoryPath, QString *errorMessage = nullptr);
    void closeLibrary();

    void setJobManager(JobManager *jobManager);

    QVector<LibraryAsset> assets() const;
    QVector<LibraryAsset> assets(const FilterOptions &filterOptions) const;
    QString resolvePath(const QString &relativePath) const;
    
    MetadataCache *metadataCache() const;

    void importFiles(const QStringList &filePaths);

    DevelopAdjustments loadDevelopAdjustments(qint64 assetId) const;
    bool saveDevelopAdjustments(qint64 assetId,
                                const DevelopAdjustments &adjustments,
                                QString *errorMessage = nullptr);

signals:
    void libraryOpened(const QString &path);
    void libraryClosed();
    void assetsChanged();
    void assetPreviewUpdated(qint64 assetId, const QString &previewPath);
    void importProgress(int imported, int total);
    void importCompleted();
    void errorOccurred(const QString &message);

private:
    bool ensurePhotoNumberColumn(QString *errorMessage = nullptr);
    void ensurePhotoNumbersAssigned();
    int currentMaxPhotoNumber() const;
    void ensurePhotoNumberSupport();
    bool ensureDevelopAdjustmentsTable(QString *errorMessage = nullptr) const;

    QString ensureLibraryDirectories(const QString &directoryPath, QString *errorMessage);
    bool initializeDatabaseSchema(QString *errorMessage);
    LibraryAsset hydrateAsset(const QSqlRecord &record) const;
    QString originalsDirectory() const;
    QString previewsDirectory() const;
    QString absoluteAssetPath(const QString &relativePath) const;
    QString storeOriginal(const QString &sourceFile, int bucketIndex, QString *errorMessage) const;
    QString reservePreviewPath(qint64 assetId, int bucketIndex) const;
    QString makeOriginalRelativePath(int bucketIndex, const QString &fileName) const;
    QString makePreviewRelativePath(int bucketIndex, const QString &fileName) const;
    bool ensureBucketExists(const QString &baseDir, int bucketIndex) const;
    void enqueuePreviewGeneration(const LibraryAsset &asset);
    void ensureAssetStorageConsistency();

    QString m_libraryPath;
    QString m_connectionName;
    QSqlDatabase m_database;
    PreviewGenerator *m_previewGenerator = nullptr;
    JobManager *m_jobManager = nullptr;
    QHash<qint64, QUuid> m_previewJobIds;
    QUuid m_batchPreviewJobId;
    int m_previewGenerationTotal = 0;
    int m_previewGenerationCompleted = 0;
    QHash<qint64, QUuid> m_metadataJobIds;
    QUuid m_batchMetadataJobId;
    int m_metadataExtractionTotal = 0;
    int m_metadataExtractionCompleted = 0;
    MetadataCache *m_metadataCache = nullptr;
    QHash<qint64, QFutureWatcher<AssetMetadata>*> m_metadataExtractionWatchers;
    void enqueueMetadataExtraction(qint64 assetId, const QString &sourceFile);
    void handleMetadataExtractionComplete(qint64 assetId, const QUuid &jobId);
    void startBatchMetadataJob(int total);
    void updateBatchMetadataProgress();
    void completeBatchMetadataJob();
    void startBatchPreviewJob(int total);
    void updateBatchPreviewProgress();
    void completeBatchPreviewJob();
};

#endif // LIBRARYMANAGER_H
