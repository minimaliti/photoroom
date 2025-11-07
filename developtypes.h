#ifndef DEVELOPTYPES_H
#define DEVELOPTYPES_H

#include <QString>
#include <QVector>

struct HistogramData
{
    QVector<int> red;
    QVector<int> green;
    QVector<int> blue;
    QVector<int> luminance;
    int maxValue = 0;
    int totalSamples = 0;

    bool isValid() const
    {
        return maxValue > 0 &&
               totalSamples > 0 &&
               red.size() == 256 &&
               green.size() == 256 &&
               blue.size() == 256 &&
               luminance.size() == 256;
    }
};

struct DevelopMetadata
{
    QString cameraMake;
    QString cameraModel;
    QString lens;
    QString iso;
    QString shutterSpeed;
    QString aperture;
    QString focalLength;
    QString flash;
    QString focusDistance;

    bool flashFired = false;
};

#endif // DEVELOPTYPES_H

