#include "imageloader.h"

#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QSet>
#include <cstring>

#include <libraw/libraw.h>

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

} // namespace ImageLoader
