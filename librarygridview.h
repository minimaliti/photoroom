#ifndef LIBRARYGRIDVIEW_H
#define LIBRARYGRIDVIEW_H

#include <QAbstractScrollArea>
#include <QFutureWatcher>
#include <QHash>
#include <QList>
#include <QPixmap>
#include <QSize>
#include <QSet>
#include <QString>
#include <QVector>

struct LibraryGridItem
{
    qint64 assetId = -1;
    QString photoNumber;
    QString fileName;
    QString previewPath;
    QString originalPath;
};

class LibraryGridView : public QAbstractScrollArea
{
    Q_OBJECT
public:
    explicit LibraryGridView(QWidget *parent = nullptr);
    ~LibraryGridView() override;

    void setItems(const QVector<LibraryGridItem> &items);
    void clear();
    void updateItemPreview(qint64 assetId, const QString &previewPath);
    QList<qint64> selectedAssetIds() const;

signals:
    void selectionChanged(const QList<qint64> &selectedAssetIds);
    void assetActivated(qint64 assetId, const QString &originalPath);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;

private:
    struct Item
    {
        qint64 assetId = -1;
        QString photoNumber;
        QString fileName;
        QString previewPath;
        QString originalPath;
        QPixmap pixmap;
        bool pixmapLoaded = false;
    };

    QVector<Item> m_items;
    QHash<qint64, int> m_indexLookup;
    QSet<int> m_selectedIndices;
    int m_lastSelectedIndex = -1;

    QSize m_itemSize = QSize(200, 150);
    int m_spacing = 12;
    int m_columns = 1;
    int m_horizontalOffset = 0;
    int m_minItemWidth = 200;
    double m_itemAspectRatio = 4.0 / 3.0; // width / height

    void updateLayoutMetrics();
    QRect itemRect(int index, int verticalOffset) const;
    int indexAt(const QPoint &pos) const;
    void ensurePixmapLoaded(int index);
    void schedulePixmapLoad(int index);
    void cancelPendingLoad(int index);
    void cancelPendingLoads();
    void prefetchAround(int index);
    QSize targetPreviewSize() const;
    void setSelectionRange(int start, int end);
    void emitSelectionChanged();

    QHash<int, QFutureWatcher<QPixmap>*> m_pendingLoads;
};

#endif // LIBRARYGRIDVIEW_H


