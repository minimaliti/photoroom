#ifndef DEVELOPTYPES_H
#define DEVELOPTYPES_H

#include <QString>
#include <QVector>
#include <QJsonObject>
#include <QByteArray>
#include <QDateTime>

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
    QDateTime captureDateTime;
};

struct DevelopAdjustments
{
    double exposure = 0.0;
    double contrast = 0.0;
    double highlights = 0.0;
    double shadows = 0.0;
    double whites = 0.0;
    double blacks = 0.0;
    double clarity = 0.0;
    double vibrance = 0.0;
    double saturation = 0.0;

    double toneCurveHighlights = 0.0;
    double toneCurveLights = 0.0;
    double toneCurveDarks = 0.0;
    double toneCurveShadows = 0.0;

    double hueShift = 0.0;
    double saturationShift = 0.0;
    double luminanceShift = 0.0;

    double sharpening = 0.0;
    double noiseReduction = 0.0;

    double vignette = 0.0;
    double grain = 0.0;
};

DevelopAdjustments defaultDevelopAdjustments();
QJsonObject adjustmentsToJson(const DevelopAdjustments &adjustments);
DevelopAdjustments adjustmentsFromJson(const QJsonObject &json);
QByteArray serializeAdjustments(const DevelopAdjustments &adjustments);
DevelopAdjustments deserializeAdjustments(const QByteArray &data);

#endif // DEVELOPTYPES_H