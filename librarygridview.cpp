#include "librarygridview.h"

#include <QCache>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFuture>
#include <QFutureWatcher>
#include <QImageReader>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>
#include <QMutex>
#include <QMutexLocker>
#include <QtConcurrent>

#include <algorithm>

namespace {
constexpr int kInnerPadding = 8;
constexpr int kPreviewCacheBudgetKb = 256 * 1024; // ~256 MB
constexpr int kPrefetchRadius = 4;

QMutex &previewCacheMutex()
{
    static QMutex mutex;
    return mutex;
}

QCache<QString, QPixmap> &previewCache()
{
    static QCache<QString, QPixmap> cache(kPreviewCacheBudgetKb);
    return cache;
}

QString cacheKeyForPath(const QString &path)
{
    if (path.isEmpty()) {
        return {};
    }
    QFileInfo info(path);
    QString absolute;
    if (info.exists()) {
        absolute = info.canonicalFilePath();
    }
    if (absolute.isEmpty()) {
        absolute = info.absoluteFilePath();
    }
    if (absolute.isEmpty()) {
        absolute = path;
    }
    const QString cleaned = QDir::cleanPath(absolute);
#ifdef Q_OS_WIN
    return cleaned.toLower();
#else
    return cleaned;
#endif
}
}

LibraryGridView::LibraryGridView(QWidget *parent)
    : QAbstractScrollArea(parent)
{
    setMouseTracking(false);
    setFocusPolicy(Qt::StrongFocus);
    viewport()->setAutoFillBackground(false);
}

LibraryGridView::~LibraryGridView()
{
    cancelPendingLoads();
}

void LibraryGridView::setItems(const QVector<LibraryGridItem> &items)
{
    cancelPendingLoads();
    m_items.clear();
    m_indexLookup.clear();
    m_selectedIndices.clear();
    m_lastSelectedIndex = -1;

    m_items.reserve(items.size());
    for (int i = 0; i < items.size(); ++i) {
        Item item;
        item.assetId = items.at(i).assetId;
        item.photoNumber = items.at(i).photoNumber;
        item.fileName = items.at(i).fileName;
        item.previewPath = items.at(i).previewPath;
        item.originalPath = items.at(i).originalPath;
        m_items.append(item);
        m_indexLookup.insert(item.assetId, i);
    }

    updateLayoutMetrics();
    viewport()->update();
    const int preloadCount = qMin(m_items.size(), 12);
    for (int i = 0; i < preloadCount; ++i) {
        ensurePixmapLoaded(i);
    }
    emitSelectionChanged();
}

void LibraryGridView::clear()
{
    cancelPendingLoads();
    if (m_items.isEmpty() && m_selectedIndices.isEmpty()) {
        return;
    }

    m_items.clear();
    m_indexLookup.clear();
    m_selectedIndices.clear();
    m_lastSelectedIndex = -1;

    updateLayoutMetrics();
    viewport()->update();
    emitSelectionChanged();
}

void LibraryGridView::updateItemPreview(qint64 assetId, const QString &previewPath)
{
    const int index = m_indexLookup.value(assetId, -1);
    if (index < 0 || index >= m_items.size()) {
        return;
    }

    Item &item = m_items[index];
    const QString previousPath = item.previewPath;
    
    // Clear cache for both old and new paths to ensure fresh load
    if (!previousPath.isEmpty()) {
        const QString key = cacheKeyForPath(previousPath);
        if (!key.isEmpty()) {
            QMutexLocker locker(&previewCacheMutex());
            previewCache().remove(key);
        }
    }
    
    // Also clear cache for new path to force reload
    if (!previewPath.isEmpty()) {
        const QString key = cacheKeyForPath(previewPath);
        if (!key.isEmpty()) {
            QMutexLocker locker(&previewCacheMutex());
            previewCache().remove(key);
        }
    }
    
    item.previewPath = previewPath;
    item.pixmap = QPixmap();
    item.pixmapLoaded = false;
    cancelPendingLoad(index);

    // Force update the entire viewport to ensure refresh
    viewport()->update();
}

QList<qint64> LibraryGridView::selectedAssetIds() const
{
    QList<qint64> ids;
    ids.reserve(m_selectedIndices.size());
    for (int index : m_selectedIndices) {
        if (index >= 0 && index < m_items.size()) {
            ids.append(m_items.at(index).assetId);
        }
    }
    return ids;
}

void LibraryGridView::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);

    QPainter painter(viewport());
    painter.fillRect(viewport()->rect(), palette().window());

    const int viewportHeight = viewport()->height();
    const int viewportWidth = viewport()->width();
    const int yOffset = verticalScrollBar()->value();

    if (m_items.isEmpty()) {
        painter.setPen(palette().color(QPalette::Midlight));
        painter.drawText(viewport()->rect(), Qt::AlignCenter, tr("No items"));
        return;
    }

    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

    const int rowHeight = m_itemSize.height() + m_spacing;
    const int firstRow = qMax(0, yOffset / rowHeight);

    int index = firstRow * m_columns;
    int y = firstRow * rowHeight - yOffset;

    while (index < m_items.size() && y < viewportHeight) {
        int x = m_horizontalOffset;
        for (int column = 0; column < m_columns && index < m_items.size(); ++column, ++index) {
            QRect cellRect(x, y, m_itemSize.width(), m_itemSize.height());
            if (cellRect.right() < 0) {
                x += m_itemSize.width() + m_spacing;
                continue;
            }
            if (cellRect.left() > viewportWidth) {
                break;
            }

            Item &item = m_items[index];
            painter.save();

            QRect frameRect = cellRect.adjusted(0, 0, -1, -1);
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(30, 30, 30));
            painter.drawRoundedRect(frameRect, 8, 8);

            ensurePixmapLoaded(index);
            prefetchAround(index);

            QRect imageRect = frameRect.adjusted(kInnerPadding,
                                                kInnerPadding,
                                                -kInnerPadding,
                                                -kInnerPadding);

            if (!item.pixmap.isNull()) {
                QSize scaledSize = item.pixmap.size().scaled(imageRect.size(), Qt::KeepAspectRatio);
                QRect targetRect(QPoint(0, 0), scaledSize);
                targetRect.moveCenter(imageRect.center());
                painter.drawPixmap(targetRect, item.pixmap);
            } else {
                painter.setPen(QColor(150, 150, 150));
                painter.drawText(imageRect, Qt::AlignCenter | Qt::TextWordWrap, tr("Preview pending"));
            }

            QString overlayText;
            if (!item.photoNumber.trimmed().isEmpty()) {
                overlayText = item.photoNumber.trimmed();
            } else if (!item.fileName.isEmpty()) {
                overlayText = item.fileName;
            } else {
                overlayText = tr("No ID");
            }

            QFontMetrics fm(painter.font());
            const int overlayPadding = 6;
            const int overlayHeight = fm.height() + overlayPadding;
            int overlayWidth = fm.horizontalAdvance(overlayText) + overlayPadding * 2;
            const int maxOverlayWidth = imageRect.width() - overlayPadding * 2;

            if (overlayHeight > 0 && maxOverlayWidth > overlayPadding) {
                overlayWidth = qMin(overlayWidth, maxOverlayWidth);
                overlayWidth = qMax(overlayWidth, overlayPadding * 2);

                QRect overlayRect(imageRect.left() + overlayPadding,
                                  imageRect.bottom() - overlayHeight - overlayPadding,
                                  overlayWidth,
                                  overlayHeight);

                painter.setPen(Qt::NoPen);
                painter.setBrush(QColor(0, 0, 0, 160));
                painter.drawRoundedRect(overlayRect, 6, 6);

                painter.setPen(Qt::white);
                painter.drawText(overlayRect.adjusted(overlayPadding / 2, 0, -overlayPadding / 2, 0),
                                 Qt::AlignVCenter | Qt::AlignLeft,
                                 overlayText);
            }

            if (m_selectedIndices.contains(index)) {
                painter.setPen(QPen(QColor(0, 122, 204), 3));
                painter.setBrush(Qt::NoBrush);
                painter.drawRoundedRect(frameRect.adjusted(1, 1, -1, -1), 8, 8);
            }

            painter.restore();

            x += m_itemSize.width() + m_spacing;
        }

        y += rowHeight;
    }
}

void LibraryGridView::resizeEvent(QResizeEvent *event)
{
    QAbstractScrollArea::resizeEvent(event);
    updateLayoutMetrics();
    viewport()->update();
}

void LibraryGridView::mousePressEvent(QMouseEvent *event)
{
    const int index = indexAt(event->pos());
    const Qt::KeyboardModifiers modifiers = event->modifiers();

    if (index < 0) {
        if (!(modifiers & Qt::ControlModifier) && !(modifiers & Qt::ShiftModifier)) {
            if (!m_selectedIndices.isEmpty()) {
                m_selectedIndices.clear();
                m_lastSelectedIndex = -1;
                emitSelectionChanged();
                viewport()->update();
            }
        }
        QAbstractScrollArea::mousePressEvent(event);
        return;
    }

    if ((modifiers & Qt::ShiftModifier) && m_lastSelectedIndex >= 0) {
        setSelectionRange(m_lastSelectedIndex, index);
    } else if (modifiers & Qt::ControlModifier) {
        if (m_selectedIndices.contains(index)) {
            m_selectedIndices.remove(index);
        } else {
            m_selectedIndices.insert(index);
            m_lastSelectedIndex = index;
        }
    } else {
        m_selectedIndices.clear();
        m_selectedIndices.insert(index);
        m_lastSelectedIndex = index;
    }

    emitSelectionChanged();
    viewport()->update();
    QAbstractScrollArea::mousePressEvent(event);
}

void LibraryGridView::mouseDoubleClickEvent(QMouseEvent *event)
{
    const int index = indexAt(event->pos());
    if (index >= 0 && index < m_items.size()) {
        const Item &item = m_items.at(index);
        emit assetActivated(item.assetId, item.originalPath);
    }
    QAbstractScrollArea::mouseDoubleClickEvent(event);
}

void LibraryGridView::updateLayoutMetrics()
{
    const int viewportWidth = viewport()->width();
    const int viewportHeight = viewport()->height();

    if (viewportWidth <= 0) {
        m_columns = 1;
        m_horizontalOffset = 0;
        return;
    }

    int columns = qMax(1, viewportWidth / (m_minItemWidth + m_spacing));
    columns = qMax(1, columns);

    while (columns > 1) {
        const int totalSpacing = (columns - 1) * m_spacing;
        const int tentativeWidth = (viewportWidth - totalSpacing) / columns;
        if (tentativeWidth >= m_minItemWidth) {
            break;
        }
        --columns;
    }

    m_columns = qMax(1, columns);

    const int totalSpacing = (m_columns - 1) * m_spacing;
    int itemWidth = m_columns > 0 ? (viewportWidth - totalSpacing) / m_columns : viewportWidth;
    itemWidth = qMax(100, itemWidth);
    int itemHeight = qMax(100, static_cast<int>(itemWidth / m_itemAspectRatio));

    m_itemSize = QSize(itemWidth, itemHeight);

    const int usedWidth = m_columns * m_itemSize.width() + totalSpacing;
    m_horizontalOffset = qMax(0, (viewportWidth - usedWidth) / 2);

    const int totalRows = m_columns > 0 ? (m_items.size() + m_columns - 1) / m_columns : 0;
    const int contentHeight = totalRows > 0
        ? totalRows * m_itemSize.height() + qMax(0, (totalRows - 1) * m_spacing)
        : viewportHeight;

    verticalScrollBar()->setPageStep(viewportHeight);
    verticalScrollBar()->setSingleStep(m_itemSize.height() + m_spacing);
    verticalScrollBar()->setRange(0, qMax(0, contentHeight - viewportHeight));
    horizontalScrollBar()->setRange(0, 0);
}

QRect LibraryGridView::itemRect(int index, int verticalOffset) const
{
    if (index < 0 || index >= m_items.size() || m_columns <= 0) {
        return {};
    }

    const int row = index / m_columns;
    const int column = index % m_columns;

    const int x = m_horizontalOffset + column * (m_itemSize.width() + m_spacing);
    const int y = row * (m_itemSize.height() + m_spacing) - verticalOffset;

    return {x, y, m_itemSize.width(), m_itemSize.height()};
}

int LibraryGridView::indexAt(const QPoint &pos) const
{
    if (m_items.isEmpty() || m_columns <= 0) {
        return -1;
    }

    const int columnWidth = m_itemSize.width() + m_spacing;
    const int rowHeight = m_itemSize.height() + m_spacing;

    if (columnWidth <= 0 || rowHeight <= 0) {
        return -1;
    }

    const int adjustedX = pos.x() - m_horizontalOffset;
    if (adjustedX < 0) {
        return -1;
    }

    const int column = adjustedX / columnWidth;
    if (column < 0 || column >= m_columns) {
        return -1;
    }

    if (adjustedX % columnWidth > m_itemSize.width()) {
        return -1;
    }

    const int adjustedY = pos.y() + verticalScrollBar()->value();
    if (adjustedY < 0) {
        return -1;
    }

    const int row = adjustedY / rowHeight;
    if (row < 0) {
        return -1;
    }

    const int index = row * m_columns + column;
    if (index < 0 || index >= m_items.size()) {
        return -1;
    }

    if (!itemRect(index, verticalScrollBar()->value()).contains(pos)) {
        return -1;
    }

    return index;
}

void LibraryGridView::ensurePixmapLoaded(int index)
{
    if (index < 0 || index >= m_items.size()) {
        return;
    }

    Item &item = m_items[index];
    if (item.pixmapLoaded) {
        return;
    }

    if (item.previewPath.isEmpty()) {
        item.pixmapLoaded = true;
        return;
    }

    const QString key = cacheKeyForPath(item.previewPath);
    if (!key.isEmpty()) {
        QMutexLocker locker(&previewCacheMutex());
        if (QPixmap *cached = previewCache().object(key)) {
            item.pixmap = *cached;
            item.pixmapLoaded = true;
            return;
        }
    }

    if (m_pendingLoads.contains(index)) {
        return;
    }

    schedulePixmapLoad(index);
}

void LibraryGridView::schedulePixmapLoad(int index)
{
    if (index < 0 || index >= m_items.size()) {
        return;
    }

    Item &item = m_items[index];
    if (item.previewPath.isEmpty()) {
        item.pixmapLoaded = true;
        return;
    }

    auto *watcher = new QFutureWatcher<QPixmap>(this);
    m_pendingLoads.insert(index, watcher);

    const QString path = item.previewPath;
    const QString cacheKey = cacheKeyForPath(path);
    const QSize desiredSize = targetPreviewSize();

    connect(watcher, &QFutureWatcher<QPixmap>::finished, this, [this, index, path, cacheKey, watcher]() {
        m_pendingLoads.remove(index);
        const QPixmap pixmap = watcher->result();
        watcher->deleteLater();

        if (index < 0 || index >= m_items.size()) {
            return;
        }

        Item &current = m_items[index];
        if (current.previewPath != path) {
            return;
        }

        current.pixmapLoaded = true;
        current.pixmap = pixmap;

        if (!pixmap.isNull() && !cacheKey.isEmpty()) {
            QMutexLocker locker(&previewCacheMutex());
            const int cost = qMax(1, (pixmap.width() * pixmap.height()) / 16);
            previewCache().insert(cacheKey, new QPixmap(pixmap), cost);
        }

        viewport()->update(itemRect(index, verticalScrollBar()->value()));
        prefetchAround(index);
    });

    QFuture<QPixmap> future = QtConcurrent::run([path, desiredSize]() -> QPixmap {
        if (path.isEmpty() || !QFile::exists(path)) {
            return {};
        }

        QImageReader reader(path);
        reader.setAutoTransform(true);
        QImage image = reader.read();
        if (!image.isNull()) {
            if (desiredSize.isValid()) {
                image = image.scaled(desiredSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            }
            return QPixmap::fromImage(image);
        }

        QPixmap pix(path);
        if (!pix.isNull() && desiredSize.isValid()) {
            return pix.scaled(desiredSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        }
        return pix;
    });

    watcher->setFuture(future);
}

void LibraryGridView::cancelPendingLoad(int index)
{
    auto it = m_pendingLoads.find(index);
    if (it == m_pendingLoads.end()) {
        return;
    }

    QFutureWatcher<QPixmap> *watcher = it.value();
    m_pendingLoads.erase(it);
    if (!watcher) {
        return;
    }
    watcher->cancel();
    watcher->deleteLater();
}

void LibraryGridView::cancelPendingLoads()
{
    const auto watchers = m_pendingLoads;
    for (QFutureWatcher<QPixmap> *watcher : watchers) {
        if (!watcher) {
            continue;
        }
        watcher->cancel();
        watcher->deleteLater();
    }
    m_pendingLoads.clear();
}

void LibraryGridView::prefetchAround(int index)
{
    if (index < 0 || index >= m_items.size()) {
        return;
    }

    for (int offset = -kPrefetchRadius; offset <= kPrefetchRadius; ++offset) {
        if (offset == 0) {
            continue;
        }
        const int neighbor = index + offset;
        if (neighbor < 0 || neighbor >= m_items.size()) {
            continue;
        }
        ensurePixmapLoaded(neighbor);
    }
}

QSize LibraryGridView::targetPreviewSize() const
{
    QSize size = m_itemSize;
    size.rwidth() = qMax(32, size.width() - kInnerPadding * 2);
    size.rheight() = qMax(32, size.height() - kInnerPadding * 2);
    return size;
}

void LibraryGridView::setSelectionRange(int start, int end)
{
    if (start > end) {
        std::swap(start, end);
    }

    m_selectedIndices.clear();
    for (int i = start; i <= end; ++i) {
        if (i >= 0 && i < m_items.size()) {
            m_selectedIndices.insert(i);
        }
    }
    m_lastSelectedIndex = end;
}

void LibraryGridView::emitSelectionChanged()
{
    emit selectionChanged(selectedAssetIds());
}



