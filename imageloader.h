#ifndef IMAGELOADER_H
#define IMAGELOADER_H

#include <QImage>
#include <QSet>
#include <QString>
#include <QStringList>

#include "developtypes.h"

namespace ImageLoader {

const QSet<QString>& rawFileExtensions();
QStringList supportedNameFilters();
bool isRawFile(const QString &filePath);
QImage loadRawImage(const QString &filePath, QString *errorMessage = nullptr);
QImage loadImageWithRawSupport(const QString &filePath, QString *errorMessage = nullptr);
QByteArray loadEmbeddedRawPreview(const QString &filePath, QString *errorMessage = nullptr);
bool extractMetadata(const QString &filePath, DevelopMetadata *metadata, QString *errorMessage = nullptr);

} // namespace ImageLoader

#endif // IMAGELOADER_H
