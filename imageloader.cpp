#include "imageloader.h"

#include <QColor>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QLocale>
#include <QObject>
#include <QPainter>
#include <QRegularExpression>
#include <QSet>
#include <QLinearGradient>
#include <QStringList>
#include <cmath>
#include <cstring>
#include <QElapsedTimer>
#include <QDebug>
#include <libraw/libraw.h>

namespace {

// --- Utility Lambdas for Reuse ---
auto localeStr = [](double val, int prec = 0) { return QLocale().toString(val, 'f', prec); };

QString formatIso(double iso) {
    return (iso > 0.0) ? QObject::tr("ISO %1").arg(localeStr(std::round(iso))) : QString();
}

QString formatExposureTime(double seconds)
{
    if (seconds <= 0.0)
        return QString();
    QLocale locale;
    if (seconds >= 1.0)
        return QObject::tr("%1 s").arg(locale.toString(seconds, 'f', (seconds >= 10.0 ? 0 : 1)));
    double denominator = 1.0 / seconds;
    denominator = denominator < 1.0 ? 1.0 : std::round(denominator);
    double approx = 1.0 / denominator;
    if (std::abs(approx - seconds) > 0.005)
        return QObject::tr("%1 s").arg(locale.toString(seconds, 'f', 3));
    return QObject::tr("1/%1 s").arg(locale.toString(denominator, 'f', 0));
}

QString formatAperture(double aperture) {
    if (aperture <= 0.0) return {};
    return QObject::tr("f/%1").arg(localeStr(aperture, aperture < 10.0 ? 1 : 0));
}

QString formatFocalLength(double focalLengthMm) {
    if (focalLengthMm <= 0.0) return {};
    return QObject::tr("%1 mm").arg(localeStr(focalLengthMm, focalLengthMm < 10.0 ? 1 : 0));
}

QString formatFocusDistance(double meters) {
    if (meters <= 0.0) return {};
    if (meters > 1e6) return QObject::tr("∞");
    if (meters >= 1.0)
        return QObject::tr("%1 m").arg(localeStr(meters, meters >= 10.0 ? 0 : 1));
    return QObject::tr("%1 cm").arg(localeStr(meters * 100.0, 0));
}

double parseRationalString(const QString &v, bool *ok = nullptr) {
    if (ok) *ok = false;
    QString value = v.trimmed();
    if (value.isEmpty()) return 0.0;
    if (value.contains('/')) {
        auto parts = value.split('/');
        if (parts.size() == 2) {
            bool ok1 = false, ok2 = false;
            double num = parts[0].toDouble(&ok1), den = parts[1].toDouble(&ok2);
            if (ok1 && ok2 && den != 0.0) { if (ok) *ok = true; return num / den; }
        }
    } else {
        bool okValue = false;
        double numeric = value.toDouble(&okValue);
        if (okValue) { if (ok) *ok = true; return numeric; }
    }
    return 0.0;
}

QString describeFlash(int flashValue, bool *fired = nullptr) {
    bool flashFired = (flashValue & 0x1) != 0;
    if (fired) *fired = flashFired;
    return QObject::tr(flashFired ? "Flash fired" : "Flash off");
}

QString formatFocalRange(double minF, double maxF) {
    const QLocale locale;
    if (minF <= 0.0 && maxF <= 0.0) return {};
    if (minF <= 0.0) return QObject::tr("%1 mm").arg(localeStr(maxF, maxF < 10.0 ? 1 : 0));
    if (maxF <= 0.0) return QObject::tr("%1 mm").arg(localeStr(minF, minF < 10.0 ? 1 : 0));
    if (std::abs(minF - maxF) < 0.1) {
        double average = (minF + maxF) / 2.0;
        return QObject::tr("%1 mm").arg(localeStr(average, average < 10.0 ? 1 : 0));
    }
    int pMin = minF < 10.0 ? 1 : 0, pMax = maxF < 10.0 ? 1 : 0;
    return QObject::tr("%1-%2 mm")
        .arg(localeStr(minF, pMin), localeStr(maxF, pMax));
}

} // namespace

namespace ImageLoader {

const QSet<QString>& rawFileExtensions() {
    static const QSet<QString> exts = {
        "arw","cr2","cr3","crw","dng","erf","kdc","mrw","nef","nrw","orf","pef",
        "raf","raw","rw2","rwz","sr2","srw","x3f"
    };
    return exts;
}

QStringList supportedNameFilters() {
    QStringList filters = { "*.png", "*.jpg", "*.jpeg", "*.bmp", "*.tif", "*.tiff" };
    for (const QString &ext : rawFileExtensions()) filters << "*." + ext;
    return filters;
}

bool isRawFile(const QString &filePath) {
    return rawFileExtensions().contains(QFileInfo(filePath).suffix().toLower());
}

QByteArray loadEmbeddedRawPreview(const QString &filePath, QString *errorMessage) {
    if (!isRawFile(filePath)) {
        if (errorMessage) *errorMessage = QStringLiteral("File is not a RAW file.");
        return {};
    }
    LibRaw rawProcessor;
    int ret = rawProcessor.open_file(QFile::encodeName(filePath).constData());
    if (ret != LIBRAW_SUCCESS) {
        if (errorMessage) *errorMessage = QStringLiteral("LibRaw open_file failed: %1").arg(QString::fromUtf8(libraw_strerror(ret)));
        return {};
    }
    ret = rawProcessor.unpack_thumb();
    if (ret != LIBRAW_SUCCESS) {
        if (errorMessage) *errorMessage = QStringLiteral("LibRaw unpack_thumb failed: %1").arg(QString::fromUtf8(libraw_strerror(ret)));
        rawProcessor.recycle();
        return {};
    }
    libraw_processed_image_t *thumb = rawProcessor.dcraw_make_mem_thumb(&ret);
    if (!thumb || ret != LIBRAW_SUCCESS) {
        if (errorMessage) *errorMessage = QStringLiteral("LibRaw dcraw_make_mem_thumb failed: %1").arg(QString::fromUtf8(libraw_strerror(ret)));
        rawProcessor.recycle();
        return {};
    }
    QByteArray result(reinterpret_cast<const char*>(thumb->data), static_cast<int>(thumb->data_size));
    LibRaw::dcraw_clear_mem(thumb);
    rawProcessor.recycle();
    if (result.isEmpty() && errorMessage && errorMessage->isEmpty())
        *errorMessage = QStringLiteral("Embedded preview extraction returned empty data.");
    return result;
}

static QImage loadEmbeddedRawPreviewImage(const QString &filePath, QString *errorMessage) {
    QString previewError;
    QByteArray data = loadEmbeddedRawPreview(filePath, &previewError);
    if (!data.isEmpty()) {
        QImage image;
        if (image.loadFromData(data)) return image;
        if (errorMessage && errorMessage->isEmpty()) *errorMessage = QObject::tr("Failed to decode embedded RAW preview.");
    }
    if (errorMessage && !previewError.isEmpty()) {
        if (!errorMessage->isEmpty()) *errorMessage += QLatin1Char('\n');
        *errorMessage += previewError;
    }
    return {};
}

static QImage buildPlaceholderPreview(const QString &reason) {
    const QSize size(480, 480);
    QImage image(size, QImage::Format_RGB32);
    QLinearGradient gradient(0, 0, 0, size.height());
    gradient.setColorAt(0.0, QColor(32, 32, 32));
    gradient.setColorAt(1.0, QColor(12, 12, 12));
    QPainter painter(&image);
    painter.fillRect(image.rect(), gradient);
    painter.setPen(QColor(220, 220, 220));
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.drawRoundedRect(image.rect().adjusted(6, 6, -6, -6), 12, 12);
    painter.setPen(QColor(200, 200, 200));
    painter.drawText(image.rect().adjusted(24, 24, -24, -24), Qt::AlignCenter | Qt::TextWordWrap,
                     QObject::tr("Preview unavailable\n%1").arg(reason));
    return image;
}

static QImage fallbackRawPreview(const QString &filePath, QString *errorMessage, const QString &reason) {
    if (errorMessage) {
        if (!errorMessage->isEmpty()) *errorMessage += QLatin1Char('\n');
        *errorMessage += reason;
    }
    QImage embedded = loadEmbeddedRawPreviewImage(filePath, errorMessage);
    return !embedded.isNull() ? embedded : buildPlaceholderPreview(reason);
}

QImage loadRawImage(const QString &filePath, QString *errorMessage)
{
    QElapsedTimer timer; timer.start();
    LibRaw rawProcessor;
    int ret = rawProcessor.open_file(QFile::encodeName(filePath).constData());
    if (ret != LIBRAW_SUCCESS) {
        const QString reason = QObject::tr("LibRaw open_file failed: %1").arg(QString::fromUtf8(libraw_strerror(ret)));
        qWarning() << reason;
        qDebug() << "loadRawImage execution time:" << timer.elapsed() << "ms";
        return fallbackRawPreview(filePath, errorMessage, reason);
    }
    ret = rawProcessor.unpack();
    if (ret != LIBRAW_SUCCESS) {
        const QString reason = QObject::tr("LibRaw unpack failed: %1").arg(QString::fromUtf8(libraw_strerror(ret)));
        qWarning() << reason;
        rawProcessor.recycle();
        qDebug() << "loadRawImage execution time:" << timer.elapsed() << "ms";
        return fallbackRawPreview(filePath, errorMessage, reason);
    }
    rawProcessor.imgdata.params.output_bps = 16;
    rawProcessor.imgdata.params.output_color = 1; // sRGB
    rawProcessor.imgdata.params.no_auto_bright = 0;
    rawProcessor.imgdata.params.use_camera_wb = 1;
    rawProcessor.imgdata.params.half_size = 1;
    ret = rawProcessor.dcraw_process();
    if (ret != LIBRAW_SUCCESS) {
        const QString reason = QObject::tr("LibRaw dcraw_process failed: %1").arg(QString::fromUtf8(libraw_strerror(ret)));
        qWarning() << reason;
        rawProcessor.recycle();
        qDebug() << "loadRawImage execution time:" << timer.elapsed() << "ms";
        return fallbackRawPreview(filePath, errorMessage, reason);
    }
    libraw_processed_image_t *processed = rawProcessor.dcraw_make_mem_image(&ret);
    if (!processed || ret != LIBRAW_SUCCESS) {
        const QString reason = QObject::tr("LibRaw dcraw_make_mem_image failed: %1").arg(QString::fromUtf8(libraw_strerror(ret)));
        qWarning() << reason;
        rawProcessor.recycle();
        qDebug() << "loadRawImage execution time:" << timer.elapsed() << "ms";
        return fallbackRawPreview(filePath, errorMessage, reason);
    }
    QImage result;
    const int width = processed->width, height = processed->height, colors = processed->colors, bits = processed->bits;
    const size_t stride = static_cast<size_t>(width) * colors;
    if (processed->type == LIBRAW_IMAGE_BITMAP) {
        if (colors == 3 || colors == 4) {
            bool hasAlpha = colors == 4;
            result = QImage(width, height, hasAlpha ? QImage::Format_RGBA8888 : QImage::Format_RGB888);
            if (!result.isNull()) {
                if (bits == 8) {
                    const unsigned char *src = processed->data;
                    for (int y = 0; y < height; ++y) {
                        uchar *dest = result.scanLine(y);
                        const unsigned char *srcLine = src + y * stride;
                        if (hasAlpha)
                            for (int x = 0; x < width * colors; ++x) dest[x] = srcLine[x];
                        else
                            for (int x = 0; x < width; ++x) {
                                int idx = x * 3;
                                dest[idx + 0] = srcLine[idx + 0];
                                dest[idx + 1] = srcLine[idx + 1];
                                dest[idx + 2] = srcLine[idx + 2];
                            }
                    }
                } else if (bits == 16) {
                    const unsigned short *src = reinterpret_cast<const unsigned short*>(processed->data);
                    for (int y = 0; y < height; ++y) {
                        uchar *dest = result.scanLine(y);
                        const unsigned short *srcLine = src + y * stride;
                        for (int x = 0; x < width; ++x)
                            for (int c = 0; c < colors; ++c)
                                dest[x * colors + c] = static_cast<uchar>(srcLine[x * colors + c] >> 8);
                    }
                } else goto unsupported_bitmap;
            }
        } else if (colors == 1) {
            result = QImage(width, height, QImage::Format_Grayscale8);
            if (!result.isNull()) {
                if (bits == 8) {
                    const unsigned char *src = processed->data;
                    for (int y = 0; y < height; ++y)
                        memcpy(result.scanLine(y), src + y * stride, static_cast<size_t>(width));
                } else if (bits == 16) {
                    const unsigned short *src = reinterpret_cast<const unsigned short*>(processed->data);
                    for (int y = 0; y < height; ++y) {
                        uchar *dest = result.scanLine(y);
                        const unsigned short *srcLine = src + y * stride;
                        for (int x = 0; x < width; ++x) dest[x] = static_cast<uchar>(srcLine[x] >> 8);
                    }
                } else goto unsupported_bitmap;
            }
        } else {
unsupported_bitmap:
            {
                QString reason = QObject::tr(colors == 1 ?
                    "Unsupported RAW grayscale bit depth: %1" :
                    colors == 3 || colors == 4 ?
                        "Unsupported RAW bit depth: %1" :
                        "Unsupported RAW color channel count: %1")
                    .arg(bits);
                qWarning() << reason;
                LibRaw::dcraw_clear_mem(processed);
                rawProcessor.recycle();
                qDebug() << "loadRawImage execution time:" << timer.elapsed() << "ms";
                return fallbackRawPreview(filePath, errorMessage, reason);
            }
        }
    } else if (processed->type == LIBRAW_IMAGE_JPEG) {
        QImage temp;
        if (temp.loadFromData(processed->data, processed->data_size, "JPEG")) {
            result = temp.convertToFormat(QImage::Format_RGB888);
        } else {
            QString reason = QObject::tr("Failed to decode embedded JPEG preview.");
            qWarning() << reason;
            LibRaw::dcraw_clear_mem(processed);
            rawProcessor.recycle();
            qDebug() << "loadRawImage execution time:" << timer.elapsed() << "ms";
            return fallbackRawPreview(filePath, errorMessage, reason);
        }
    } else {
        QString reason = QObject::tr("Unsupported LibRaw processed image type: %1").arg(processed->type);
        qWarning() << reason;
        LibRaw::dcraw_clear_mem(processed);
        rawProcessor.recycle();
        qDebug() << "loadRawImage execution time:" << timer.elapsed() << "ms";
        return fallbackRawPreview(filePath, errorMessage, reason);
    }
    unsigned int warnings = rawProcessor.imgdata.process_warnings;
    LibRaw::dcraw_clear_mem(processed);
    rawProcessor.recycle();
    if (warnings != 0) {
        QString reason = QObject::tr("LibRaw reported warnings (%1) reading %2")
                                   .arg(warnings)
                                   .arg(QFileInfo(filePath).fileName());
        qWarning() << reason;
        qDebug() << "loadRawImage execution time:" << timer.elapsed() << "ms";
        return fallbackRawPreview(filePath, errorMessage, reason);
    }
    if (result.isNull()) {
        QString reason = QObject::tr("RAW image conversion returned an empty result.");
        qWarning() << reason;
        qDebug() << "loadRawImage execution time:" << timer.elapsed() << "ms";
        return fallbackRawPreview(filePath, errorMessage, reason);
    }
    qDebug() << "loadRawImage execution time:" << timer.elapsed() << "ms";
    return result;
}

QImage loadImageWithRawSupport(const QString &filePath, QString *errorMessage){
    if (isRawFile(filePath)) {
        return loadRawImage(filePath, errorMessage);
    }
    QImageReader reader(filePath);
    reader.setAutoTransform(true);
    QImage image = reader.read();
    if (image.isNull() && errorMessage) *errorMessage = reader.errorString();
    return image;
}

bool extractMetadata(const QString &filePath, DevelopMetadata *metadata, QString *errorMessage) {
    if (!metadata) {
        if (errorMessage) *errorMessage = QStringLiteral("Metadata pointer is null.");
        return false;
    }
    *metadata = DevelopMetadata{};
    if (isRawFile(filePath)) {
        LibRaw rawProcessor;
        int ret = rawProcessor.open_file(QFile::encodeName(filePath).constData());
        if (ret != LIBRAW_SUCCESS) {
            if (errorMessage) *errorMessage = QStringLiteral("LibRaw open_file failed: %1").arg(QString::fromUtf8(libraw_strerror(ret)));
            return false;
        }
        ret = rawProcessor.unpack();
        if (ret != LIBRAW_SUCCESS) {
            if (errorMessage) *errorMessage = QStringLiteral("LibRaw unpack failed: %1").arg(QString::fromUtf8(libraw_strerror(ret)));
            rawProcessor.recycle();
            return false;
        }
        metadata->cameraMake = QString::fromLatin1(rawProcessor.imgdata.idata.make).trimmed();
        metadata->cameraModel = QString::fromLatin1(rawProcessor.imgdata.idata.model).trimmed();
        const char *lensName = rawProcessor.imgdata.lens.Lens;
        if (lensName && lensName[0])
            metadata->lens = QString::fromUtf8(lensName).trimmed();
        else if (!(metadata->lens = QString::fromUtf8(rawProcessor.imgdata.lens.LensMake).trimmed()).isEmpty()) { }
        else {
            QString range = formatFocalRange(rawProcessor.imgdata.lens.MinFocal, rawProcessor.imgdata.lens.MaxFocal);
            if (!range.isEmpty()) metadata->lens = range;
        }
        metadata->iso = formatIso(rawProcessor.imgdata.other.iso_speed);
        metadata->shutterSpeed = formatExposureTime(rawProcessor.imgdata.other.shutter);
        metadata->aperture = formatAperture(rawProcessor.imgdata.other.aperture);
        metadata->focalLength = formatFocalLength(rawProcessor.imgdata.other.focal_len);
        metadata->flash.clear();
        metadata->flashFired = false;
        metadata->focusDistance.clear();
        rawProcessor.recycle();
        return true;
    }
    QImageReader reader(filePath); reader.setAutoTransform(true);
    const QStringList keys = reader.textKeys();
    auto fetchText = [&](const QString &key) -> QString {
        return keys.contains(key) ? reader.text(key).trimmed() : QString();
    };
    metadata->cameraMake = fetchText("Exif.Image.Make");
    metadata->cameraModel = fetchText("Exif.Image.Model");

    if (!(metadata->lens = fetchText("Exif.Photo.LensModel")).isEmpty()) {}
    else if (!fetchText("Exif.Photo.LensSpecification").isEmpty()) {
        auto tokens = fetchText("Exif.Photo.LensSpecification").split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
        if (tokens.size() >= 2) {
            bool okMin = false, okMax = false;
            double minFocal = parseRationalString(tokens[0], &okMin);
            double maxFocal = parseRationalString(tokens[1], &okMax);
            QString formatted = formatFocalRange(okMin ? minFocal : 0.0, okMax ? maxFocal : 0.0);
            if (!formatted.isEmpty()) metadata->lens = formatted;
        }
    }
    if (metadata->lens.isEmpty()) metadata->lens = fetchText("Exif.Photo.LensMake");

    bool okValue = false;
    double numeric;
    numeric = parseRationalString(fetchText("Exif.Photo.ISOSpeedRatings"), &okValue);
    if (!okValue) numeric = parseRationalString(fetchText("Exif.Photo.ISOSpeed"), &okValue);
    if (okValue) metadata->iso = formatIso(numeric);

    numeric = parseRationalString(fetchText("Exif.Photo.ExposureTime"), &okValue);
    if (okValue) metadata->shutterSpeed = formatExposureTime(numeric);

    numeric = parseRationalString(fetchText("Exif.Photo.FNumber"), &okValue);
    if (okValue) metadata->aperture = formatAperture(numeric);

    numeric = parseRationalString(fetchText("Exif.Photo.FocalLength"), &okValue);
    if (okValue) metadata->focalLength = formatFocalLength(numeric);

    QString flashText = fetchText("Exif.Photo.Flash");
    if (!flashText.isEmpty()) {
        bool okFlash = false;
        int flashValue = flashText.toInt(&okFlash);
        metadata->flash = okFlash ? describeFlash(flashValue, &metadata->flashFired) : flashText;
    }

    numeric = parseRationalString(fetchText("Exif.Photo.SubjectDistance"), &okValue);
    if (okValue) metadata->focusDistance = formatFocusDistance(numeric);

    if (metadata->lens.isEmpty())
        metadata->lens = fetchText("Exif.Photo.LensMake");
    return true;
}
} // namespace ImageLoader
