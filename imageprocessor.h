#ifndef IMAGEPROCESSOR_H
#define IMAGEPROCESSOR_H

#include <QObject>
#include <QPixmap>
#include <QHash>
#include <QtConcurrent/QtConcurrent>
#include <QFuture>
#include "imageadjustments.h"

class ImageProcessor : public QObject
{
    Q_OBJECT
public:
    explicit ImageProcessor(QObject *parent = nullptr);

    QPixmap applyAdjustments(const QPixmap &pixmap, const ImageAdjustments &adjustments);
    void clearCache();
    void setThreadCount(int count);

private:
    QHash<QString, QPixmap> m_cache;
    int m_threadCount = 4;

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

#endif // IMAGEPROCESSOR_H
