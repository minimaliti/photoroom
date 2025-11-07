#include "imageloader.h"

#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QSet>
#include <QLocale>
#include <QObject>
#include <QRegularExpression>
#include <QStringList>
#include <cmath>
#include <cstring>

#include <libraw/libraw.h>

namespace {

QString formatIso(double iso)
{
    if (iso <= 0.0) {
        return {};
    }
    return QObject::tr("ISO %1").arg(QLocale().toString(std::round(iso)));
}

QString formatExposureTime(double seconds)
{
    if (seconds <= 0.0) {
        return {};
    }

    const QLocale locale;
    if (seconds >= 1.0) {
        const int precision = seconds >= 10.0 ? 0 : 1;
        return QObject::tr("%1 s").arg(locale.toString(seconds, 'f', precision));
    }

    double denominator = std::round(1.0 / seconds);
    if (denominator < 1.0) {
        denominator = 1.0;
    }

    const double approximated = 1.0 / denominator;
    if (std::abs(approximated - seconds) > 0.005) {
        return QObject::tr("%1 s").arg(locale.toString(seconds, 'f', 3));
    }

    return QObject::tr("1/%1 s").arg(locale.toString(denominator, 'f', 0));
}

QString formatAperture(double aperture)
{
    if (aperture <= 0.0) {
        return {};
    }
    const int precision = aperture < 10.0 ? 1 : 0;
    return QObject::tr("f/%1").arg(QLocale().toString(aperture, 'f', precision));
}

QString formatFocalLength(double focalLengthMm)
{
    if (focalLengthMm <= 0.0) {
        return {};
    }
    const int precision = focalLengthMm < 10.0 ? 1 : 0;
    return QObject::tr("%1 mm").arg(QLocale().toString(focalLengthMm, 'f', precision));
}

QString formatFocusDistance(double meters)
{
    if (meters <= 0.0) {
        return {};
    }

    if (meters > 1e6) {
        return QObject::tr("∞");
    }

    const QLocale locale;
    if (meters >= 1.0) {
        const int precision = meters >= 10.0 ? 0 : 1;
        return QObject::tr("%1 m").arg(locale.toString(meters, 'f', precision));
    }

    return QObject::tr("%1 cm").arg(locale.toString(meters * 100.0, 'f', 0));
}

double parseRationalString(const QString &value, bool *ok = nullptr)
{
    if (ok) {
        *ok = false;
    }
    if (value.isEmpty()) {
        return 0.0;
    }

    const QString normalized = value.trimmed();
    if (normalized.contains('/')) {
        const QStringList parts = normalized.split('/');
        if (parts.size() == 2) {
            bool okNum = false;
            bool okDen = false;
            const double numerator = parts.at(0).toDouble(&okNum);
            const double denominator = parts.at(1).toDouble(&okDen);
            if (okNum && okDen && denominator != 0.0) {
                if (ok) {
                    *ok = true;
                }
                return numerator / denominator;
            }
        }
    } else {
        bool okValue = false;
        const double numeric = normalized.toDouble(&okValue);
        if (okValue) {
            if (ok) {
                *ok = true;
            }
            return numeric;
        }
    }

    return 0.0;
}

QString describeFlash(int flashValue, bool *fired = nullptr)
{
    const bool flashFired = (flashValue & 0x1) != 0;
    if (fired) {
        *fired = flashFired;
    }
    return flashFired ? QObject::tr("Flash fired") : QObject::tr("Flash off");
}

QString formatFocalRange(double minFocalMm, double maxFocalMm)
{
    const QLocale locale;
    if (minFocalMm <= 0.0 && maxFocalMm <= 0.0) {
        return {};
    }
    if (minFocalMm <= 0.0) {
        return QObject::tr("%1 mm").arg(locale.toString(maxFocalMm, 'f', maxFocalMm < 10.0 ? 1 : 0));
    }
    if (maxFocalMm <= 0.0) {
        return QObject::tr("%1 mm").arg(locale.toString(minFocalMm, 'f', minFocalMm < 10.0 ? 1 : 0));
    }
    if (std::abs(minFocalMm - maxFocalMm) < 0.1) {
        const double average = (minFocalMm + maxFocalMm) / 2.0;
        return QObject::tr("%1 mm").arg(locale.toString(average, 'f', average < 10.0 ? 1 : 0));
    }

    const int precisionMin = minFocalMm < 10.0 ? 1 : 0;
    const int precisionMax = maxFocalMm < 10.0 ? 1 : 0;
    return QObject::tr("%1-%2 mm")
        .arg(locale.toString(minFocalMm, 'f', precisionMin),
             locale.toString(maxFocalMm, 'f', precisionMax));
}

} // namespace

namespace ImageLoader {

const QSet<QString>& rawFileExtensions()
{
    static const QSet<QString> kExtensions = {
        QStringLiteral("arw"), QStringLiteral("cr2"), QStringLiteral("cr3"), QStringLiteral("crw"),
        QStringLiteral("dng"), QStringLiteral("erf"), QStringLiteral("kdc"), QStringLiteral("mrw"),
        QStringLiteral("nef"), QStringLiteral("nrw"), QStringLiteral("orf"), QStringLiteral("pef"),
        QStringLiteral("raf"), QStringLiteral("raw"), QStringLiteral("rw2"), QStringLiteral("rwz"),
        QStringLiteral("sr2"), QStringLiteral("srw"), QStringLiteral("x3f")
    };
    return kExtensions;
}

QStringList supportedNameFilters()
{
    QStringList filters = {
        QStringLiteral("*.png"),
        QStringLiteral("*.jpg"),
        QStringLiteral("*.jpeg"),
        QStringLiteral("*.bmp"),
        QStringLiteral("*.tif"),
        QStringLiteral("*.tiff")
    };

    for (const QString &ext : rawFileExtensions()) {
        filters << "*." + ext;
    }

    return filters;
}

bool isRawFile(const QString &filePath)
{
    const QString suffix = QFileInfo(filePath).suffix().toLower();
    return rawFileExtensions().contains(suffix);
}

QByteArray loadEmbeddedRawPreview(const QString &filePath, QString *errorMessage)
{
    if (!isRawFile(filePath)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("File is not a RAW file.");
        }
        return {};
    }

    LibRaw rawProcessor;
    const QByteArray encodedPath = QFile::encodeName(filePath);

    int ret = rawProcessor.open_file(encodedPath.constData());
    if (ret != LIBRAW_SUCCESS) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("LibRaw open_file failed: %1").arg(QString::fromUtf8(libraw_strerror(ret)));
        }
        return {};
    }

    ret = rawProcessor.unpack_thumb();
    if (ret != LIBRAW_SUCCESS) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("LibRaw unpack_thumb failed: %1").arg(QString::fromUtf8(libraw_strerror(ret)));
        }
        rawProcessor.recycle();
        return {};
    }

    libraw_processed_image_t *thumb = rawProcessor.dcraw_make_mem_thumb(&ret);
    if (!thumb || ret != LIBRAW_SUCCESS) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("LibRaw dcraw_make_mem_thumb failed: %1").arg(QString::fromUtf8(libraw_strerror(ret)));
        }
        rawProcessor.recycle();
        return {};
    }

    QByteArray result(reinterpret_cast<const char*>(thumb->data), static_cast<int>(thumb->data_size));
    LibRaw::dcraw_clear_mem(thumb);
    rawProcessor.recycle();

    if (result.isEmpty() && errorMessage && errorMessage->isEmpty()) {
        *errorMessage = QStringLiteral("Embedded preview extraction returned empty data.");
    }

    return result;
}

QImage loadRawImage(const QString &filePath, QString *errorMessage)
{
    LibRaw rawProcessor;
    const QByteArray encodedPath = QFile::encodeName(filePath);

    int ret = rawProcessor.open_file(encodedPath.constData());
    if (ret != LIBRAW_SUCCESS) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("LibRaw open_file failed: %1").arg(QString::fromUtf8(libraw_strerror(ret)));
        }
        return {};
    }

    ret = rawProcessor.unpack();
    if (ret != LIBRAW_SUCCESS) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("LibRaw unpack failed: %1").arg(QString::fromUtf8(libraw_strerror(ret)));
        }
        rawProcessor.recycle();
        return {};
    }

    rawProcessor.imgdata.params.output_bps = 16;
    rawProcessor.imgdata.params.output_color = 1; // sRGB

    ret = rawProcessor.dcraw_process();
    if (ret != LIBRAW_SUCCESS) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("LibRaw dcraw_process failed: %1").arg(QString::fromUtf8(libraw_strerror(ret)));
        }
        rawProcessor.recycle();
        return {};
    }

    libraw_processed_image_t *processed = rawProcessor.dcraw_make_mem_image(&ret);
    if (!processed || ret != LIBRAW_SUCCESS) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("LibRaw dcraw_make_mem_image failed: %1").arg(QString::fromUtf8(libraw_strerror(ret)));
        }
        rawProcessor.recycle();
        return {};
    }

    QImage result;

    if (processed->type == LIBRAW_IMAGE_BITMAP) {
        const int width = processed->width;
        const int height = processed->height;
        const int colors = processed->colors;
        const int bits = processed->bits;
        const size_t stride = static_cast<size_t>(width) * colors;

        if (colors == 3 || colors == 4) {
            const bool hasAlpha = colors == 4;
            result = QImage(width, height, hasAlpha ? QImage::Format_RGBA8888 : QImage::Format_RGB888);

            if (!result.isNull()) {
                if (bits == 8) {
                    const unsigned char *src = processed->data;
                    for (int y = 0; y < height; ++y) {
                        uchar *destLine = result.scanLine(y);
                        const unsigned char *srcLine = src + y * stride;
                        for (int x = 0; x < width; ++x) {
                            const int srcIndex = x * colors;
                            const int destIndex = x * colors;
                            destLine[destIndex + 0] = srcLine[srcIndex + 0];
                            destLine[destIndex + 1] = srcLine[srcIndex + 1];
                            destLine[destIndex + 2] = srcLine[srcIndex + 2];
                            if (hasAlpha) {
                                destLine[destIndex + 3] = srcLine[srcIndex + 3];
                            }
                        }
                    }
                } else if (bits == 16) {
                    const unsigned short *src = reinterpret_cast<const unsigned short*>(processed->data);
                    for (int y = 0; y < height; ++y) {
                        uchar *destLine = result.scanLine(y);
                        const unsigned short *srcLine = src + y * stride;
                        for (int x = 0; x < width; ++x) {
                            const int srcIndex = x * colors;
                            const int destIndex = x * colors;
                            destLine[destIndex + 0] = static_cast<uchar>(srcLine[srcIndex + 0] >> 8);
                            destLine[destIndex + 1] = static_cast<uchar>(srcLine[srcIndex + 1] >> 8);
                            destLine[destIndex + 2] = static_cast<uchar>(srcLine[srcIndex + 2] >> 8);
                            if (hasAlpha) {
                                destLine[destIndex + 3] = static_cast<uchar>(srcLine[srcIndex + 3] >> 8);
                            }
                        }
                    }
                } else {
                    if (errorMessage) {
                        *errorMessage = QStringLiteral("Unsupported RAW bit depth: %1").arg(bits);
                    }
                    result = QImage();
                }
            }
        } else if (colors == 1) {
            result = QImage(width, height, QImage::Format_Grayscale8);
            if (!result.isNull()) {
                if (bits == 8) {
                    const unsigned char *src = processed->data;
                    for (int y = 0; y < height; ++y) {
                        uchar *destLine = result.scanLine(y);
                        memcpy(destLine, src + y * stride, static_cast<size_t>(width));
                    }
                } else if (bits == 16) {
                    const unsigned short *src = reinterpret_cast<const unsigned short*>(processed->data);
                    for (int y = 0; y < height; ++y) {
                        uchar *destLine = result.scanLine(y);
                        const unsigned short *srcLine = src + y * stride;
                        for (int x = 0; x < width; ++x) {
                            destLine[x] = static_cast<uchar>(srcLine[x] >> 8);
                        }
                    }
                } else {
                    if (errorMessage) {
                        *errorMessage = QStringLiteral("Unsupported RAW grayscale bit depth: %1").arg(bits);
                    }
                    result = QImage();
                }
            }
        } else {
            if (errorMessage) {
                *errorMessage = QStringLiteral("Unsupported RAW color channel count: %1").arg(colors);
            }
        }
    } else if (processed->type == LIBRAW_IMAGE_JPEG) {
        QImage temp;
        if (temp.loadFromData(processed->data, processed->data_size, "JPEG")) {
            result = temp.convertToFormat(QImage::Format_RGB888);
        } else if (errorMessage) {
            *errorMessage = QStringLiteral("Failed to decode embedded JPEG preview.");
        }
    } else {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Unsupported LibRaw processed image type: %1").arg(processed->type);
        }
    }

    LibRaw::dcraw_clear_mem(processed);
    rawProcessor.recycle();

    if (result.isNull() && errorMessage && errorMessage->isEmpty()) {
        *errorMessage = QStringLiteral("RAW image conversion returned an empty result.");
    }

    return result;
}

QImage loadImageWithRawSupport(const QString &filePath, QString *errorMessage)
{
    if (isRawFile(filePath)) {
        return loadRawImage(filePath, errorMessage);
    }

    QImageReader reader(filePath);
    reader.setAutoTransform(true);
    QImage image = reader.read();
    if (image.isNull() && errorMessage) {
        *errorMessage = reader.errorString();
    }
    return image;
}

bool extractMetadata(const QString &filePath, DevelopMetadata *metadata, QString *errorMessage)
{
    if (!metadata) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Metadata pointer is null.");
        }
        return false;
    }

    *metadata = DevelopMetadata{};

    if (isRawFile(filePath)) {
        LibRaw rawProcessor;
        const QByteArray encodedPath = QFile::encodeName(filePath);

        int ret = rawProcessor.open_file(encodedPath.constData());
        if (ret != LIBRAW_SUCCESS) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("LibRaw open_file failed: %1").arg(QString::fromUtf8(libraw_strerror(ret)));
            }
            return false;
        }

        ret = rawProcessor.unpack();
        if (ret != LIBRAW_SUCCESS) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("LibRaw unpack failed: %1").arg(QString::fromUtf8(libraw_strerror(ret)));
            }
            rawProcessor.recycle();
            return false;
        }

        metadata->cameraMake = QString::fromLatin1(rawProcessor.imgdata.idata.make).trimmed();
        metadata->cameraModel = QString::fromLatin1(rawProcessor.imgdata.idata.model).trimmed();

        const char *lensName = rawProcessor.imgdata.lens.Lens;
        if (lensName && lensName[0]) {
            metadata->lens = QString::fromUtf8(lensName).trimmed();
        } else {
            const QString lensMake = QString::fromUtf8(rawProcessor.imgdata.lens.LensMake).trimmed();
            if (!lensMake.isEmpty()) {
                metadata->lens = lensMake;
            } else {
                const double minFocal = rawProcessor.imgdata.lens.MinFocal;
                const double maxFocal = rawProcessor.imgdata.lens.MaxFocal;
                const QString range = formatFocalRange(minFocal, maxFocal);
                if (!range.isEmpty()) {
                    metadata->lens = range;
                }
            }
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

    QImageReader reader(filePath);
    reader.setAutoTransform(true);
    const QStringList keys = reader.textKeys();

    auto fetchText = [&](const QString &key) -> QString {
        if (keys.contains(key)) {
            return reader.text(key).trimmed();
        }
        return {};
    };

    metadata->cameraMake = fetchText(QStringLiteral("Exif.Image.Make"));
    metadata->cameraModel = fetchText(QStringLiteral("Exif.Image.Model"));

    const QString lensModel = fetchText(QStringLiteral("Exif.Photo.LensModel"));
    const QString lensSpecification = fetchText(QStringLiteral("Exif.Photo.LensSpecification"));
    if (!lensModel.isEmpty()) {
        metadata->lens = lensModel;
    } else if (!lensSpecification.isEmpty()) {
        const QStringList tokens = lensSpecification.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
        if (tokens.size() >= 2) {
            bool okMin = false;
            bool okMax = false;
            const double minFocal = parseRationalString(tokens.at(0), &okMin);
            const double maxFocal = parseRationalString(tokens.at(1), &okMax);
            const QString formatted = formatFocalRange(okMin ? minFocal : 0.0, okMax ? maxFocal : 0.0);
            if (!formatted.isEmpty()) {
                metadata->lens = formatted;
            }
        }
    }
    if (metadata->lens.isEmpty()) {
        metadata->lens = fetchText(QStringLiteral("Exif.Photo.LensMake"));
    }

    bool okValue = false;
    double numeric = parseRationalString(fetchText(QStringLiteral("Exif.Photo.ISOSpeedRatings")), &okValue);
    if (!okValue) {
        numeric = parseRationalString(fetchText(QStringLiteral("Exif.Photo.ISOSpeed")), &okValue);
    }
    if (okValue) {
        metadata->iso = formatIso(numeric);
    }

    numeric = parseRationalString(fetchText(QStringLiteral("Exif.Photo.ExposureTime")), &okValue);
    if (okValue) {
        metadata->shutterSpeed = formatExposureTime(numeric);
    }

    numeric = parseRationalString(fetchText(QStringLiteral("Exif.Photo.FNumber")), &okValue);
    if (okValue) {
        metadata->aperture = formatAperture(numeric);
    }

    numeric = parseRationalString(fetchText(QStringLiteral("Exif.Photo.FocalLength")), &okValue);
    if (okValue) {
        metadata->focalLength = formatFocalLength(numeric);
    }

    const QString flashText = fetchText(QStringLiteral("Exif.Photo.Flash"));
    if (!flashText.isEmpty()) {
        bool okFlash = false;
        const int flashValue = flashText.toInt(&okFlash);
        if (okFlash) {
            metadata->flash = describeFlash(flashValue, &metadata->flashFired);
        } else {
            metadata->flash = flashText;
        }
    }

    numeric = parseRationalString(fetchText(QStringLiteral("Exif.Photo.SubjectDistance")), &okValue);
    if (okValue) {
        metadata->focusDistance = formatFocusDistance(numeric);
    }

    if (metadata->lens.isEmpty()) {
        metadata->lens = fetchText(QStringLiteral("Exif.Photo.LensMake"));
    }

    return true;
}

} // namespace ImageLoader
