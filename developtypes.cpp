#include "developtypes.h"

#include <QJsonDocument>
#include <QJsonValue>

namespace {

constexpr double clampValue(double value, double minValue, double maxValue)
{
    if (value < minValue) {
        return minValue;
    }
    if (value > maxValue) {
        return maxValue;
    }
    return value;
}

} // namespace

DevelopAdjustments defaultDevelopAdjustments()
{
    return DevelopAdjustments{};
}

static QJsonObject serializeBasicFields(const DevelopAdjustments &adjustments)
{
    QJsonObject json;
    json.insert(QStringLiteral("exposure"), adjustments.exposure);
    json.insert(QStringLiteral("contrast"), adjustments.contrast);
    json.insert(QStringLiteral("highlights"), adjustments.highlights);
    json.insert(QStringLiteral("shadows"), adjustments.shadows);
    json.insert(QStringLiteral("whites"), adjustments.whites);
    json.insert(QStringLiteral("blacks"), adjustments.blacks);
    json.insert(QStringLiteral("clarity"), adjustments.clarity);
    json.insert(QStringLiteral("vibrance"), adjustments.vibrance);
    json.insert(QStringLiteral("saturation"), adjustments.saturation);

    json.insert(QStringLiteral("toneCurveHighlights"), adjustments.toneCurveHighlights);
    json.insert(QStringLiteral("toneCurveLights"), adjustments.toneCurveLights);
    json.insert(QStringLiteral("toneCurveDarks"), adjustments.toneCurveDarks);
    json.insert(QStringLiteral("toneCurveShadows"), adjustments.toneCurveShadows);

    json.insert(QStringLiteral("hueShift"), adjustments.hueShift);
    json.insert(QStringLiteral("saturationShift"), adjustments.saturationShift);
    json.insert(QStringLiteral("luminanceShift"), adjustments.luminanceShift);

    json.insert(QStringLiteral("sharpening"), adjustments.sharpening);
    json.insert(QStringLiteral("noiseReduction"), adjustments.noiseReduction);

    json.insert(QStringLiteral("vignette"), adjustments.vignette);
    json.insert(QStringLiteral("grain"), adjustments.grain);
    return json;
}

QJsonObject adjustmentsToJson(const DevelopAdjustments &adjustments)
{
    return serializeBasicFields(adjustments);
}

static double jsonDouble(const QJsonObject &json, const QString &key, double fallback)
{
    const QJsonValue value = json.value(key);
    if (!value.isUndefined() && value.isDouble()) {
        return value.toDouble();
    }
    return fallback;
}

DevelopAdjustments adjustmentsFromJson(const QJsonObject &json)
{
    DevelopAdjustments adjustments = defaultDevelopAdjustments();
    adjustments.exposure = jsonDouble(json, QStringLiteral("exposure"), adjustments.exposure);
    adjustments.contrast = jsonDouble(json, QStringLiteral("contrast"), adjustments.contrast);
    adjustments.highlights = jsonDouble(json, QStringLiteral("highlights"), adjustments.highlights);
    adjustments.shadows = jsonDouble(json, QStringLiteral("shadows"), adjustments.shadows);
    adjustments.whites = jsonDouble(json, QStringLiteral("whites"), adjustments.whites);
    adjustments.blacks = jsonDouble(json, QStringLiteral("blacks"), adjustments.blacks);
    adjustments.clarity = jsonDouble(json, QStringLiteral("clarity"), adjustments.clarity);
    adjustments.vibrance = jsonDouble(json, QStringLiteral("vibrance"), adjustments.vibrance);
    adjustments.saturation = jsonDouble(json, QStringLiteral("saturation"), adjustments.saturation);

    adjustments.toneCurveHighlights = jsonDouble(json, QStringLiteral("toneCurveHighlights"), adjustments.toneCurveHighlights);
    adjustments.toneCurveLights = jsonDouble(json, QStringLiteral("toneCurveLights"), adjustments.toneCurveLights);
    adjustments.toneCurveDarks = jsonDouble(json, QStringLiteral("toneCurveDarks"), adjustments.toneCurveDarks);
    adjustments.toneCurveShadows = jsonDouble(json, QStringLiteral("toneCurveShadows"), adjustments.toneCurveShadows);

    adjustments.hueShift = jsonDouble(json, QStringLiteral("hueShift"), adjustments.hueShift);
    adjustments.saturationShift = jsonDouble(json, QStringLiteral("saturationShift"), adjustments.saturationShift);
    adjustments.luminanceShift = jsonDouble(json, QStringLiteral("luminanceShift"), adjustments.luminanceShift);

    adjustments.sharpening = clampValue(jsonDouble(json, QStringLiteral("sharpening"), adjustments.sharpening), 0.0, 150.0);
    adjustments.noiseReduction = clampValue(jsonDouble(json, QStringLiteral("noiseReduction"), adjustments.noiseReduction), 0.0, 100.0);

    adjustments.vignette = jsonDouble(json, QStringLiteral("vignette"), adjustments.vignette);
    adjustments.grain = jsonDouble(json, QStringLiteral("grain"), adjustments.grain);
    return adjustments;
}

QByteArray serializeAdjustments(const DevelopAdjustments &adjustments)
{
    const QJsonObject json = adjustmentsToJson(adjustments);
    QJsonDocument doc(json);
    return doc.toJson(QJsonDocument::Compact);
}

DevelopAdjustments deserializeAdjustments(const QByteArray &data)
{
    if (data.isEmpty()) {
        return defaultDevelopAdjustments();
    }

    QJsonParseError error;
    const QJsonDocument doc = QJsonDocument::fromJson(data, &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) {
        return defaultDevelopAdjustments();
    }
    return adjustmentsFromJson(doc.object());
}


