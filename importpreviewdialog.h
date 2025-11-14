#ifndef IMPORTPREVIEWDIALOG_H
#define IMPORTPREVIEWDIALOG_H

#include <QDialog>
#include <QStringList>
#include <QVector>
#include <QFutureWatcher>
#include <QGridLayout>
#include <QScrollArea>
#include <QLabel>
#include <QPixmap>

struct PreviewItem
{
    QString filePath;
    QString fileName;
    QPixmap thumbnail;
    bool thumbnailLoaded = false;
    bool thumbnailFailed = false;
};

class ImportPreviewDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ImportPreviewDialog(const QStringList &filePaths, QWidget *parent = nullptr);
    ~ImportPreviewDialog() override;

    QStringList selectedFiles() const;

private slots:
    void onThumbnailLoaded(int index);
    void onImportClicked();
    void onCancelClicked();

private:
    void setupUI();
    void loadThumbnails();
    void recalculateColumns();
    QPixmap loadThumbnailForFile(const QString &filePath);
    void updateThumbnail(int index, const QPixmap &pixmap);
    
protected:
    void resizeEvent(QResizeEvent *event) override;

    QStringList m_filePaths;
    QVector<PreviewItem> m_previewItems;
    QScrollArea *m_scrollArea = nullptr;
    QWidget *m_contentWidget = nullptr;
    QGridLayout *m_gridLayout = nullptr;
    QVector<QLabel*> m_thumbnailLabels;
    QVector<QFutureWatcher<QPixmap>*> m_thumbnailWatchers;
    QStringList m_selectedFiles;
    int m_currentColumns = 5; // kMinThumbnailColumns
};

#endif // IMPORTPREVIEWDIALOG_H

