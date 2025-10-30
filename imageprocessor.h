#ifndef IMAGEPROCESSOR_H
#define IMAGEPROCESSOR_H

#include <QObject>
#include <QPixmap>
#include <QHash>
#include <QtConcurrent/QtConcurrent>
#include <QFuture>
#include <QThreadPool>
#include <QRunnable>
#include "imageadjustments.h"

class ImageProcessor : public QObject
{
    Q_OBJECT
public:
    explicit ImageProcessor(QObject *parent = nullptr);

    void applyAdjustments(const QPixmap &pixmap, const ImageAdjustments &adjustments);
    void clearCache();
    void setThreadCount(int count);

signals:
    void processingFinished(const QPixmap &pixmap);

private:
    QHash<QString, QPixmap> m_cache;
    int m_threadCount = 4;
    QThreadPool m_threadPool;

public:
    void applyBrightness(QImage &image, int brightness);
    void applyExposure(QImage &image, int exposure);
    void applyContrast(QImage &image, int contrast);
    void applyBlacks(QImage &image, int blacks);
    void applyHighlights(QImage &image, int highlights);
    void applyShadows(QImage &image, int shadows);
    void applyHighlightRolloff(QImage &image, int rolloff);
    void applyClarity(QImage &image, int clarity);
    void applyVibrance(QImage &image, int vibrance);
};

class ImageProcessingTask : public QRunnable
{
public:
    ImageProcessingTask(ImageProcessor *processor, QImage strip, ImageAdjustments adjustments, QImage *resultImage, int yOffset, QSharedPointer<QAtomicInt> runningTasks);

    void run() override;

private:
    ImageProcessor *m_processor;
    QImage m_strip;
    ImageAdjustments m_adjustments;
    QImage *m_resultImage;
    int m_yOffset;
    QSharedPointer<QAtomicInt> m_runningTasks;
};

#endif // IMAGEPROCESSOR_H
