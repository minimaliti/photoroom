#include "imageprocessor.h"
#include <QImage>
#include <QColor>
#include <QtConcurrent/QtConcurrent>

ImageProcessor::ImageProcessor(QObject *parent) : QObject(parent)
{

}

void ImageProcessor::clearCache()
{
    m_cache.clear();
}

void ImageProcessor::setThreadCount(int count)
{
    m_threadCount = count;
}

QPixmap ImageProcessor::applyAdjustments(const QPixmap &pixmap, const ImageAdjustments &adjustments)
{
    if (pixmap.isNull()) {
        return QPixmap();
    }

    QImage image = pixmap.toImage();
    QImage resultImage(image.size(), image.format());

    int stripHeight = image.height() / m_threadCount;
    QList<QFuture<QImage>> futures;

    for (int i = 0; i < m_threadCount; ++i) {
        int yStart = i * stripHeight;
        int yEnd = (i == m_threadCount - 1) ? image.height() : (i + 1) * stripHeight;
        QImage strip = image.copy(0, yStart, image.width(), yEnd - yStart);

        futures.append(QtConcurrent::run([this, strip, adjustments]() {
            QImage processedStrip = strip;

            applyExposure(processedStrip, adjustments.exposure);
            applyContrast(processedStrip, adjustments.contrast);
            applyBrightness(processedStrip, adjustments.brightness);
            applyBlacks(processedStrip, adjustments.blacks);
            applyHighlights(processedStrip, adjustments.highlights);
            applyShadows(processedStrip, adjustments.shadows);
            applyHighlightRolloff(processedStrip, adjustments.highlightRolloff);
            applyClarity(processedStrip, adjustments.clarity);
            applyVibrance(processedStrip, adjustments.vibrance);

            return processedStrip;
        }));
    }

    for (int i = 0; i < futures.size(); ++i) {
        int yStart = i * stripHeight;
        QImage processedStrip = futures[i].result();
        for (int y = 0; y < processedStrip.height(); ++y) {
            memcpy(resultImage.scanLine(yStart + y), processedStrip.constScanLine(y), processedStrip.bytesPerLine());
        }
    }

    return QPixmap::fromImage(resultImage);
}

void ImageProcessor::applyBrightness(QImage &image, int brightness)
{
    if (image.isNull()) {
        return;
    }

    for (int y = 0; y < image.height(); ++y) {
        QRgb *line = (QRgb *)image.scanLine(y);
        for (int x = 0; x < image.width(); ++x) {
            int r = qRed(line[x]) + brightness;
            int g = qGreen(line[x]) + brightness;
            int b = qBlue(line[x]) + brightness;
            line[x] = qRgb(qBound(0, r, 255), qBound(0, g, 255), qBound(0, b, 255));
        }
    }
}

void ImageProcessor::applyExposure(QImage &image, int exposure)
{
    if (image.isNull()) {
        return;
    }

    for (int y = 0; y < image.height(); ++y) {
        QRgb *line = (QRgb *)image.scanLine(y);
        for (int x = 0; x < image.width(); ++x) {
            int r = qRed(line[x]) + exposure;
            int g = qGreen(line[x]) + exposure;
            int b = qBlue(line[x]) + exposure;
            line[x] = qRgb(qBound(0, r, 255), qBound(0, g, 255), qBound(0, b, 255));
        }
    }
}

void ImageProcessor::applyContrast(QImage &image, int contrast)
{
    if (image.isNull()) {
        return;
    }

    float factor = (259.0 * (contrast + 255.0)) / (255.0 * (259.0 - contrast));

    for (int y = 0; y < image.height(); ++y) {
        QRgb *line = (QRgb *)image.scanLine(y);
        for (int x = 0; x < image.width(); ++x) {
            int r = qBound(0, (int)(factor * (qRed(line[x]) - 128) + 128), 255);
            int g = qBound(0, (int)(factor * (qGreen(line[x]) - 128) + 128), 255);
            int b = qBound(0, (int)(factor * (qBlue(line[x]) - 128) + 128), 255);
            line[x] = qRgb(r, g, b);
        }
    }
}

void ImageProcessor::applyBlacks(QImage &image, int blacks)
{
    if (image.isNull()) {
        return;
    }

    for (int y = 0; y < image.height(); ++y) {
        QRgb *line = (QRgb *)image.scanLine(y);
        for (int x = 0; x < image.width(); ++x) {
            int r = qRed(line[x]) + blacks;
            int g = qGreen(line[x]) + blacks;
            int b = qBlue(line[x]) + blacks;
            line[x] = qRgb(qBound(0, r, 255), qBound(0, g, 255), qBound(0, b, 255));
        }
    }
}

void ImageProcessor::applyHighlights(QImage &image, int highlights)
{
    if (image.isNull() || highlights == 0) {
        return;
    }

    // Normalize the slider value (-100 to 0 for reduction) to a factor for calculation
    float factor = highlights / 255.0f;

    for (int y = 0; y < image.height(); ++y) {
        QRgb *line = (QRgb *)image.scanLine(y);
        for (int x = 0; x < image.width(); ++x) {
            int r = qRed(line[x]);
            int g = qGreen(line[x]);
            int b = qBlue(line[x]);

            // Calculate luminance using a standard formula for perceived brightness
            float luminance = 0.2126f * r + 0.7152f * g + 0.0722f * b;

            // Create a smooth mask based on luminance.
            // It's 0 for pixels darker than mid-grey (128) and ramps up to 1 for the brightest pixels.
            float mask = qBound(0.0f, (luminance - 128.0f) / 128.0f, 1.0f);

            // The adjustment is strongest where the mask is 1 (brightest areas)
            float adjustment = 1.0f + factor * mask;

            // Apply the proportional adjustment
            r = qBound(0, (int)(r * adjustment), 255);
            g = qBound(0, (int)(g * adjustment), 255);
            b = qBound(0, (int)(b * adjustment), 255);

            line[x] = qRgb(r, g, b);
        }
    }
}

void ImageProcessor::applyShadows(QImage &image, int shadows)
{
    if (image.isNull() || shadows == 0) {
        return;
    }

    // Normalize the slider value (0 to 100 for lifting) to a factor for calculation
    float factor = shadows / 255.0f;

    for (int y = 0; y < image.height(); ++y) {
        QRgb *line = (QRgb *)image.scanLine(y);
        for (int x = 0; x < image.width(); ++x) {
            int r = qRed(line[x]);
            int g = qGreen(line[x]);
            int b = qBlue(line[x]);

            // Calculate luminance using a standard formula for perceived brightness
            float luminance = 0.2126f * r + 0.7152f * g + 0.0722f * b;

            // Create an inverted mask for shadows.
            // It's 1 for the darkest pixels and ramps down to 0 for pixels brighter than mid-grey (128).
            float mask = qBound(0.0f, (128.0f - luminance) / 128.0f, 1.0f);

            // The adjustment is strongest where the mask is 1 (darkest areas)
            float adjustment = factor * mask;

            // Apply the adjustment. We add the adjustment multiplied by the "room" to white (255-val).
            // This pulls the dark tones up towards the mid-tones in a more natural way.
            r = qBound(0, r + (int)(adjustment * (255 - r)), 255);
            g = qBound(0, g + (int)(adjustment * (255 - g)), 255);
            b = qBound(0, b + (int)(adjustment * (255 - b)), 255);

            line[x] = qRgb(r, g, b);
        }
    }
}

void ImageProcessor::applyHighlightRolloff(QImage &image, int rolloff)
{
    if (image.isNull()) {
        return;
    }

    float limit = 255 - rolloff;

    for (int y = 0; y < image.height(); ++y) {
        QRgb *line = (QRgb *)image.scanLine(y);
        for (int x = 0; x < image.width(); ++x) {
            int r = qRed(line[x]);
            int g = qGreen(line[x]);
            int b = qBlue(line[x]);

            if (r > limit) r = limit + (r - limit) * (255 - limit) / (255 - limit + 1);
            if (g > limit) g = limit + (g - limit) * (255 - limit) / (255 - limit + 1);
            if (b > limit) b = limit + (b - limit) * (255 - limit) / (255 - limit + 1);

            line[x] = qRgb(qBound(0, r, 255), qBound(0, g, 255), qBound(0, b, 255));
        }
    }
}

void ImageProcessor::applyClarity(QImage &image, int clarity)
{
    if (image.isNull() || clarity == 0) {
        return;
    }

    // A blurred version of the image is used to determine local contrast.
    // This is a fast but effective blurring method.
    QImage blurred = image.scaled(image.size() / 2, Qt::KeepAspectRatio, Qt::SmoothTransformation)
                         .scaled(image.size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);

    float factor = clarity / 100.0f;

    for (int y = 0; y < image.height(); ++y) {
        QRgb *line = (QRgb *)image.scanLine(y);
        QRgb *blurredLine = (QRgb *)blurred.scanLine(y);
        for (int x = 0; x < image.width(); ++x) {
            QColor originalColor(line[x]);
            QColor blurredColor(blurredLine[x]);

            // Convert to HSL (Hue, Saturation, Lightness) which separates brightness from color
            int h, s, l_original;
            originalColor.getHsl(&h, &s, &l_original);

            int l_blurred = blurredColor.lightness();

            // The "clarity" effect is the difference in lightness between the original and blurred image
            int lightness_diff = l_original - l_blurred;

            // Create a mid-tone mask: the effect is strongest in mid-tones and fades in shadows/highlights
            // This prevents clipping and creates a more pleasing, "punchy" look without being harsh.
            float midtone_mask = 1.0f - std::abs(l_original / 127.5f - 1.0f);

            // Apply the lightness difference, scaled by the clarity factor and the mid-tone mask
            int l_new = l_original + static_cast<int>(lightness_diff * factor * midtone_mask);

            originalColor.setHsl(h, s, qBound(0, l_new, 255));
            line[x] = originalColor.rgb();
        }
    }
}

void ImageProcessor::applyVibrance(QImage &image, int vibrance)
{
    if (image.isNull() || vibrance == 0) {
        return;
    }

    float factor = vibrance / 100.0f; // Normalize slider value to a factor (e.g., -1.0 to 1.0)

    for (int y = 0; y < image.height(); ++y) {
        QRgb *line = (QRgb *)image.scanLine(y);
        for (int x = 0; x < image.width(); ++x) {
            QColor color(line[x]);
            int h, s, v;
            color.getHsv(&h, &s, &v);

            // Don't process greyscale pixels (no hue)
            if (h == -1) continue;

            // 1. Proportional adjustment: The less saturated the color, the stronger the effect.
            // We use pow() to make the falloff more aggressive, strongly favoring unsaturated colors.
            float saturation_weight = qPow(1.0f - (s / 255.0f), 2.0f);

            // 2. Skin tone protection: Reduce the effect on hues corresponding to skin tones (oranges/yellows).
            // Qt hue is 0-359. Skin tones are roughly in the 15-50 range.
            if (h > 15 && h < 50) {
                saturation_weight *= 0.25f; // Reduce effect to 25% for skin tones
            }

            // Calculate the new saturation
            int s_new = s + static_cast<int>(factor * saturation_weight * 255.0f);

            color.setHsv(h, qBound(0, s_new, 255), v);
            line[x] = color.rgb();
        }
    }
}
