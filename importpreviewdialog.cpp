#include "importpreviewdialog.h"
#include "ui_importpreviewdialog.h"
#include <QGridLayout>
#include <QLabel>
#include <QScrollArea>
#include <QPushButton>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrent>
#include <QDebug>
#include <QImage>
#include "mainwindow.h" // Include mainwindow.h to access MainWindow::loadPixmapFromFile

ImportPreviewDialog::ImportPreviewDialog(const QStringList &imagePaths, QWidget *parent, LibraryManager *libraryManager) :
    QDialog(parent),
    ui(new Ui::ImportPreviewDialog),
    m_imagePaths(imagePaths),
    m_importMode(CancelMode),
    m_libraryManager(libraryManager)
{
    ui->setupUi(this);
    setWindowTitle(tr("Import Preview"));
    setMinimumSize(800, 600);

    // Connect buttons
    connect(ui->copyButton, &QPushButton::clicked, this, &ImportPreviewDialog::on_copyButton_clicked);
    connect(ui->moveButton, &QPushButton::clicked, this, &ImportPreviewDialog::on_moveButton_clicked);
    connect(ui->cancelButton, &QPushButton::clicked, this, &ImportPreviewDialog::on_cancelButton_clicked);

    // Connect the future watcher to update the grid when all thumbnails are loaded
    connect(&m_thumbnailLoadingWatcher, &QFutureWatcher<void>::finished, this, [this]() {
        qDebug() << "Thumbnail loading finished in ImportPreviewDialog.";
        QGridLayout *gridLayout = qobject_cast<QGridLayout*>(ui->scrollAreaWidgetContents->layout());
        if (!gridLayout) return;

        int row = 0;
        int col = 0;
        int maxCols = 4; // Example: 4 columns

        // Re-add widgets to layout to ensure proper arrangement after all are loaded
        for (QObject *child : ui->scrollAreaWidgetContents->children()) {
            if (QLabel *label = qobject_cast<QLabel*>(child)) {
                gridLayout->addWidget(label, row, col);
                col++;
                if (col >= maxCols) {
                    col = 0;
                    row++;
                }
            }
        }
    });

    loadThumbnails();
}

ImportPreviewDialog::~ImportPreviewDialog()
{
    delete ui;
}

void ImportPreviewDialog::loadThumbnails()
{
    if (m_thumbnailLoadingWatcher.isRunning()) {
        m_thumbnailLoadingWatcher.cancel();
        m_thumbnailLoadingWatcher.waitForFinished();
    }

    QFuture<void> future = QtConcurrent::map(m_imagePaths, [this](const QString &filePath) {
        QPixmap pix = MainWindow::loadPixmapFromFile(filePath, true, m_libraryManager);
        if(!pix.isNull()) {
            QMetaObject::invokeMethod(this, "onThumbnailLoaded", Qt::QueuedConnection,
                                      Q_ARG(QString, filePath),
                                      Q_ARG(QPixmap, pix));
        }
    });
    m_thumbnailLoadingWatcher.setFuture(future);
}

void ImportPreviewDialog::onThumbnailLoaded(const QString &filePath, const QPixmap &pixmap)
{
    if (pixmap.isNull()) return;

    QGridLayout *gridLayout = qobject_cast<QGridLayout*>(ui->scrollAreaWidgetContents->layout());
    if (!gridLayout) return;

    QLabel *thumbLabel = new QLabel(ui->scrollAreaWidgetContents);
    thumbLabel->setPixmap(pixmap.scaled(150, 150, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    thumbLabel->setFixedSize(150, 150);
    thumbLabel->setAlignment(Qt::AlignCenter);
    thumbLabel->setToolTip(filePath);
    thumbLabel->setStyleSheet("border: 1px solid #555; background-color: #333;");

    // Add to layout, but don't arrange yet. Arrangement happens after all are loaded.
    // We add it as a child of the gridLayout's parent to ensure it's part of the dialog's widget tree
    // but not immediately placed in the grid until all are loaded.
    // The actual adding to the gridLayout will happen in the finished signal handler.
    // For now, just set the parent to scrollAreaWidgetContents
    thumbLabel->setParent(ui->scrollAreaWidgetContents);
}

void ImportPreviewDialog::on_copyButton_clicked()
{
    m_importMode = CopyMode;
    accept();
}

void ImportPreviewDialog::on_moveButton_clicked()
{
    m_importMode = MoveMode;
    accept();
}

void ImportPreviewDialog::on_cancelButton_clicked()
{
    m_importMode = CancelMode;
    reject();
}
