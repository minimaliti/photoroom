#include "previewgenerator.h"

#include "imageloader.h"

#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QImage>

PreviewGenerator::PreviewGenerator(QObject *parent)
    : QObject(parent)
{
}

PreviewGenerator::~PreviewGenerator()
{
    for (QFutureWatcher<PreviewResult>* watcher : std::as_const(m_watchers)) {
        watcher->cancel();
        watcher->waitForFinished();
        watcher->deleteLater();
    }
    m_watchers.clear();
}

void PreviewGenerator::enqueueJob(const PreviewJob &job)
{
    auto *watcher = new QFutureWatcher<PreviewResult>(this);
    connect(watcher,
            &QFutureWatcher<PreviewResult>::finished,
            this,
            [this, watcher]() { handleFinished(watcher); });

    QFuture<PreviewResult> future = QtConcurrent::run([this, job]() {
        return processJob(job);
    });

    watcher->setFuture(future);
    m_watchers.append(watcher);
}

PreviewResult PreviewGenerator::processJob(const PreviewJob &job) const
{
    PreviewResult result;
    result.assetId = job.assetId;
    result.previewPath = job.previewPath;

    QFileInfo sourceInfo(job.sourcePath);
    if (!sourceInfo.exists()) {
        result.errorMessage = QStringLiteral("Source file %1 does not exist.").arg(job.sourcePath);
        return result;
    }

    QString errorMessage;
    QImage image;

    if (ImageLoader::isRawFile(job.sourcePath)) {
        QByteArray embeddedPreview = ImageLoader::loadEmbeddedRawPreview(job.sourcePath, &errorMessage);
        if (!embeddedPreview.isEmpty()) {
            QImage embedded;
            if (embedded.loadFromData(embeddedPreview, "JPEG")) {
                image = embedded;
            } else {
                errorMessage = QStringLiteral("Failed to decode embedded preview for %1.").arg(job.sourcePath);
            }
        }
    }

    if (image.isNull()) {
        image = ImageLoader::loadImageWithRawSupport(job.sourcePath, &errorMessage);
    }

    if (image.isNull()) {
        result.errorMessage = errorMessage.isEmpty()
            ? QStringLiteral("Unable to load image %1 for preview generation.").arg(job.sourcePath)
            : errorMessage;
        return result;
    }

    const int targetSize = qMax(16, job.maxHeight);
    if (image.width() > targetSize || image.height() > targetSize) {
        image = image.scaled(targetSize, targetSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }

    QDir dir = QFileInfo(job.previewPath).dir();
    if (!dir.exists()) {
        if (!dir.mkpath(QStringLiteral("."))) {
            result.errorMessage = QStringLiteral("Unable to create preview directory %1.").arg(dir.absolutePath());
            return result;
        }
    }

    if (!image.save(job.previewPath, "JPG", 90)) {
        result.errorMessage = QStringLiteral("Failed to save preview to %1").arg(job.previewPath);
        return result;
    }

    result.success = true;
    result.imageSize = image.size();
    return result;
}

void PreviewGenerator::handleFinished(QFutureWatcher<PreviewResult> *watcher)
{
    if (!watcher) {
        return;
    }

    PreviewResult result = watcher->result();
    emit previewReady(result);

    m_watchers.removeOne(watcher);
    watcher->deleteLater();
}
