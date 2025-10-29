#ifndef IMPORTPREVIEWDIALOG_H
#define IMPORTPREVIEWDIALOG_H

#include <QDialog>
#include <QStringList>
#include <QPixmap>
#include <QFutureWatcher>
#include "librarymanager.h"

namespace Ui {
class ImportPreviewDialog;
}

class ImportPreviewDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ImportPreviewDialog(const QStringList &imagePaths, QWidget *parent = nullptr, LibraryManager *libraryManager = nullptr);
    ~ImportPreviewDialog();

    enum ImportMode {
        CopyMode,
        MoveMode,
        CancelMode
    };

    ImportMode importMode() const { return m_importMode; }

private slots:
    void on_copyButton_clicked();
    void on_moveButton_clicked();
    void on_cancelButton_clicked();
    void onThumbnailLoaded(const QString &filePath, const QPixmap &pixmap);

private:
    Ui::ImportPreviewDialog *ui;
    QStringList m_imagePaths;
    ImportMode m_importMode;
    QFutureWatcher<void> m_thumbnailLoadingWatcher;
    LibraryManager *m_libraryManager;

    void loadThumbnails();
};

#endif // IMPORTPREVIEWDIALOG_H
