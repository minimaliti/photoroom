#ifndef PREVIEWGENERATOR_H
#define PREVIEWGENERATOR_H

#include <QObject>
#include <QFutureWatcher>
#include <QImage>
#include <QMutex>
#include <QQueue>
#include <QString>
#include <QtConcurrent>

struct PreviewJob
{
    qint64 assetId = -1;
    QString sourcePath;
    QString previewPath;
    int maxHeight = 200;
};

struct PreviewResult
{
    qint64 assetId = -1;
    QString previewPath;
    QSize imageSize;
    bool success = false;
    QString errorMessage;
};

class PreviewGenerator : public QObject
{
    Q_OBJECT
public:
    explicit PreviewGenerator(QObject *parent = nullptr);
    ~PreviewGenerator() override;

    void enqueueJob(const PreviewJob &job);

signals:
    void previewReady(const PreviewResult &result);

private:
    PreviewResult processJob(const PreviewJob &job) const;
    void handleFinished(QFutureWatcher<PreviewResult> *watcher);

    QList<QFutureWatcher<PreviewResult>*> m_watchers;
};

#endif // PREVIEWGENERATOR_H
