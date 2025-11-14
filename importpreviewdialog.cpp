#include "importpreviewdialog.h"

#include "imageloader.h"

#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFileInfo>
#include <QDir>
#include <QMessageBox>
#include <QApplication>
#include <QPainter>
#include <QScrollBar>
#include <QFrame>
#include <QResizeEvent>
#include <QtConcurrent>

namespace {
constexpr int kThumbnailSize = 120;
constexpr int kThumbnailSpacing = 8;
constexpr int kMinThumbnailColumns = 5;
}

ImportPreviewDialog::ImportPreviewDialog(const QStringList &filePaths, QWidget *parent)
    : QDialog(parent)
    , m_filePaths(filePaths)
{
    setWindowTitle(tr("Import Preview - %1 files").arg(filePaths.size()));
    
    // Make dialog fill most of the parent window
    if (parent) {
        QSize parentSize = parent->size();
        QSize dialogSize = parentSize * 0.85; // 85% of parent size
        setMinimumSize(600, 400);
        resize(dialogSize);
    } else {
        setMinimumSize(800, 600);
        resize(1000, 700);
    }

    m_previewItems.reserve(filePaths.size());
    for (const QString &path : filePaths) {
        PreviewItem item;
        item.filePath = path;
        item.fileName = QFileInfo(path).fileName();
        m_previewItems.append(item);
    }

    setupUI();
    loadThumbnails();
}

ImportPreviewDialog::~ImportPreviewDialog()
{
    for (QFutureWatcher<QPixmap> *watcher : m_thumbnailWatchers) {
        if (watcher) {
            watcher->cancel();
            watcher->waitForFinished();
            watcher->deleteLater();
        }
    }
    m_thumbnailWatchers.clear();
}

QStringList ImportPreviewDialog::selectedFiles() const
{
    return m_selectedFiles;
}

void ImportPreviewDialog::setupUI()
{
    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(8, 8, 8, 8);
    mainLayout->setSpacing(6);

    QLabel *infoLabel = new QLabel(tr("%1 files").arg(m_filePaths.size()), this);
    QFont font = infoLabel->font();
    font.setPointSize(font.pointSize() - 1);
    infoLabel->setFont(font);
    mainLayout->addWidget(infoLabel);

    m_scrollArea = new QScrollArea(this);
    m_scrollArea->setWidgetResizable(true);
    m_scrollArea->setBackgroundRole(QPalette::Dark);
    m_scrollArea->setFrameShape(QFrame::NoFrame);

    m_contentWidget = new QWidget();
    m_gridLayout = new QGridLayout(m_contentWidget);
    m_gridLayout->setSpacing(kThumbnailSpacing);
    m_gridLayout->setContentsMargins(kThumbnailSpacing, kThumbnailSpacing, kThumbnailSpacing, kThumbnailSpacing);

    recalculateColumns();

    m_thumbnailLabels.reserve(m_previewItems.size());
    for (int i = 0; i < m_previewItems.size(); ++i) {
        QLabel *label = new QLabel(m_contentWidget);
        label->setFixedSize(kThumbnailSize, kThumbnailSize);
        label->setAlignment(Qt::AlignCenter);
        label->setStyleSheet(QStringLiteral(
            "QLabel {"
            "  background-color: #2a2a2a;"
            "  border: 1px solid #3a3a3a;"
            "  border-radius: 6px;"
            "}"
        ));
        label->setText(tr("Loading..."));

        const int row = i / m_currentColumns;
        const int col = i % m_currentColumns;
        m_gridLayout->addWidget(label, row, col);
        m_thumbnailLabels.append(label);
    }

    m_contentWidget->setLayout(m_gridLayout);
    m_scrollArea->setWidget(m_contentWidget);
    mainLayout->addWidget(m_scrollArea, 1); // Give scroll area stretch factor

    QHBoxLayout *buttonLayout = new QHBoxLayout();
    buttonLayout->setContentsMargins(0, 4, 0, 0);
    buttonLayout->setSpacing(8);
    buttonLayout->addStretch();

    QPushButton *cancelButton = new QPushButton(tr("Cancel"), this);
    cancelButton->setMinimumWidth(80);
    connect(cancelButton, &QPushButton::clicked, this, &ImportPreviewDialog::onCancelClicked);
    buttonLayout->addWidget(cancelButton);

    QPushButton *importButton = new QPushButton(tr("Import %1").arg(m_filePaths.size()), this);
    importButton->setMinimumWidth(100);
    importButton->setDefault(true);
    connect(importButton, &QPushButton::clicked, this, &ImportPreviewDialog::onImportClicked);
    buttonLayout->addWidget(importButton);

    mainLayout->addLayout(buttonLayout);
}

void ImportPreviewDialog::loadThumbnails()
{
    for (int i = 0; i < m_previewItems.size(); ++i) {
        PreviewItem &item = m_previewItems[i];
        if (item.thumbnailLoaded || item.thumbnailFailed) {
            continue;
        }

        auto *watcher = new QFutureWatcher<QPixmap>(this);
        m_thumbnailWatchers.append(watcher);

        const QString filePath = item.filePath;
        const int index = i;

        connect(watcher, &QFutureWatcher<QPixmap>::finished, this, [this, index, watcher]() {
            if (!watcher->isCanceled()) {
                const QPixmap pixmap = watcher->result();
                onThumbnailLoaded(index);
                updateThumbnail(index, pixmap);
            }
            watcher->deleteLater();
            m_thumbnailWatchers.removeOne(watcher);
        });

        QFuture<QPixmap> future = QtConcurrent::run([this, filePath]() -> QPixmap {
            return loadThumbnailForFile(filePath);
        });

        watcher->setFuture(future);
    }
}

QPixmap ImportPreviewDialog::loadThumbnailForFile(const QString &filePath)
{
    QPixmap result;

    // Try to load embedded preview for RAW files first
    if (ImageLoader::isRawFile(filePath)) {
        QString errorMessage;
        QByteArray embeddedPreview = ImageLoader::loadEmbeddedRawPreview(filePath, &errorMessage);
        if (!embeddedPreview.isEmpty()) {
            QImage image;
            if (image.loadFromData(embeddedPreview, "JPEG")) {
                result = QPixmap::fromImage(image);
            }
        }
    }

    // If no embedded preview or not a RAW file, try loading the image directly
    if (result.isNull()) {
        QString errorMessage;
        QImage image = ImageLoader::loadImageWithRawSupport(filePath, &errorMessage);
        if (!image.isNull()) {
            result = QPixmap::fromImage(image);
        }
    }

    // Scale to thumbnail size if we have an image
    if (!result.isNull()) {
        result = result.scaled(kThumbnailSize, kThumbnailSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }

    return result;
}

void ImportPreviewDialog::onThumbnailLoaded(int index)
{
    if (index < 0 || index >= m_previewItems.size()) {
        return;
    }

    PreviewItem &item = m_previewItems[index];
    item.thumbnailLoaded = true;
}

void ImportPreviewDialog::updateThumbnail(int index, const QPixmap &pixmap)
{
    if (index < 0 || index >= m_thumbnailLabels.size()) {
        return;
    }

    PreviewItem &item = m_previewItems[index];
    item.thumbnail = pixmap;
    item.thumbnailLoaded = true;

    QLabel *label = m_thumbnailLabels[index];
    if (!pixmap.isNull()) {
        // Create a pixmap with border and filename
        QPixmap displayPixmap(kThumbnailSize, kThumbnailSize);
        displayPixmap.fill(QColor(42, 42, 42));

        QPainter painter(&displayPixmap);
        painter.setRenderHint(QPainter::Antialiasing, true);

        // Draw thumbnail centered
        QSize scaledSize = pixmap.size().scaled(kThumbnailSize - 16, kThumbnailSize - 32, Qt::KeepAspectRatio);
        QRect targetRect((kThumbnailSize - scaledSize.width()) / 2,
                         (kThumbnailSize - scaledSize.height() - 18) / 2,
                         scaledSize.width(),
                         scaledSize.height());
        painter.drawPixmap(targetRect, pixmap);

        // Draw filename at bottom
        painter.setPen(Qt::white);
        QFont font = painter.font();
        font.setPointSize(7);
        painter.setFont(font);
        QRect textRect(4, kThumbnailSize - 16, kThumbnailSize - 8, 12);
        QString elidedText = painter.fontMetrics().elidedText(item.fileName, Qt::ElideMiddle, textRect.width());
        painter.drawText(textRect, Qt::AlignCenter | Qt::TextSingleLine, elidedText);

        label->setPixmap(displayPixmap);
    } else {
        item.thumbnailFailed = true;
        label->setText(tr("Failed"));
        label->setStyleSheet(QStringLiteral(
            "QLabel {"
            "  background-color: #2a2a2a;"
            "  border: 1px solid #cc0000;"
            "  border-radius: 6px;"
            "  color: #cc0000;"
            "}"
        ));
    }
}

void ImportPreviewDialog::onImportClicked()
{
    m_selectedFiles = m_filePaths;
    accept();
}

void ImportPreviewDialog::onCancelClicked()
{
    reject();
}

void ImportPreviewDialog::recalculateColumns()
{
    if (!m_scrollArea || !m_gridLayout) {
        m_currentColumns = kMinThumbnailColumns;
        return;
    }
    
    QVBoxLayout *layout = qobject_cast<QVBoxLayout*>(this->layout());
    int margins = layout ? (layout->contentsMargins().left() + layout->contentsMargins().right()) : 16;
    int availableWidth = width() - margins - 2 * kThumbnailSpacing - m_scrollArea->verticalScrollBar()->sizeHint().width();
    m_currentColumns = qMax(kMinThumbnailColumns, availableWidth / (kThumbnailSize + kThumbnailSpacing));
}

void ImportPreviewDialog::resizeEvent(QResizeEvent *event)
{
    QDialog::resizeEvent(event);
    
    if (m_gridLayout && m_thumbnailLabels.size() > 0) {
        int oldColumns = m_currentColumns;
        recalculateColumns();
        
        if (oldColumns != m_currentColumns) {
            // Reorganize grid layout
            for (int i = 0; i < m_thumbnailLabels.size(); ++i) {
                QLabel *label = m_thumbnailLabels[i];
                m_gridLayout->removeWidget(label);
                const int row = i / m_currentColumns;
                const int col = i % m_currentColumns;
                m_gridLayout->addWidget(label, row, col);
            }
        }
    }
}

