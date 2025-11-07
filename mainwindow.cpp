#include "mainwindow.h"
#include "./ui_mainwindow.h"
#include "preferencesdialog.h"
#include "librarygridview.h"

#include <QAction>
#include <QDebug>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QLabel>
#include <QMessageBox>
#include <QPixmap>
#include <QSizePolicy>
#include <QStandardPaths>
#include <QToolBar>
#include <QVBoxLayout>
#include <QWidget>

#include "imageloader.h"

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);

    m_libraryManager = new LibraryManager(this);
    bindLibrarySignals();

    m_libraryGridView = new LibraryGridView(this);
    connect(m_libraryGridView, &LibraryGridView::assetActivated,
            this, &MainWindow::openAssetInDevelop);
    connect(m_libraryGridView, &LibraryGridView::selectionChanged,
            this, &MainWindow::handleSelectionChanged);

    if (ui->libraryGridLayout) {
        ui->libraryGridLayout->addWidget(m_libraryGridView);
    }

    // --- NEW: Configure the develop page image label ---
    if (ui->developImageLabel) {
        ui->developImageLabel->setAlignment(Qt::AlignCenter);
        ui->developImageLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    }

    // Connect toolbar actions to switch pages
    connect(ui->actionShowLibrary, &QAction::triggered, [this]() {
        if(ui->stackedWidget) ui->stackedWidget->setCurrentIndex(0);
    });
    connect(ui->actionShowDevelop, &QAction::triggered, [this]() {
        if(ui->stackedWidget) ui->stackedWidget->setCurrentIndex(1);
    });

    if (ui->actionImport) {
        ui->actionImport->setEnabled(false);
    }

}

void MainWindow::bindLibrarySignals()
{
    if (!m_libraryManager) {
        return;
    }

    connect(m_libraryManager, &LibraryManager::libraryOpened, this, [this](const QString &path) {
        currentLibraryPath = path;
        if (ui->actionImport) {
            ui->actionImport->setEnabled(true);
        }
        showStatusMessage(tr("Opened library: %1").arg(QDir(path).dirName()), 4000);
        refreshLibraryView();
    });

    connect(m_libraryManager, &LibraryManager::libraryClosed, this, [this]() {
        currentLibraryPath.clear();
        if (ui->actionImport) {
            ui->actionImport->setEnabled(false);
        }
        clearLibrary();
    });

    connect(m_libraryManager, &LibraryManager::assetsChanged, this, &MainWindow::refreshLibraryView);
    connect(m_libraryManager, &LibraryManager::assetPreviewUpdated, this, &MainWindow::updateThumbnailPreview);

    connect(m_libraryManager, &LibraryManager::importProgress, this, [this](int imported, int total) {
        showStatusMessage(tr("Importing items %1/%2").arg(imported).arg(total), 0);
    });

    connect(m_libraryManager, &LibraryManager::importCompleted, this, [this]() {
        showStatusMessage(tr("Import completed"), 2000);
    });

    connect(m_libraryManager, &LibraryManager::errorOccurred, this, [this](const QString &message) {
        QMessageBox::warning(this, tr("Library error"), message);
    });
}

void MainWindow::refreshLibraryView()
{
    if (!m_libraryGridView) {
        return;
    }

    if (!m_libraryManager || !m_libraryManager->hasOpenLibrary()) {
        m_assets.clear();
        m_libraryGridView->clear();
        return;
    }

    m_assets = m_libraryManager->assets();

    QVector<LibraryGridItem> items;
    items.reserve(m_assets.size());
    for (const LibraryAsset &asset : m_assets) {
        LibraryGridItem item;
        item.assetId = asset.id;
        item.photoNumber = asset.photoNumber;
        item.fileName = asset.fileName;
        item.previewPath = assetPreviewPath(asset);
        item.originalPath = assetOriginalPath(asset);
        items.append(item);
    }

    m_libraryGridView->setItems(items);
}

void MainWindow::updateThumbnailPreview(qint64 assetId, const QString &previewPath)
{
    if (!m_libraryGridView) {
        return;
    }

    if (!m_assets.isEmpty() && m_libraryManager) {
        const QString libraryPath = m_libraryManager->libraryPath();
        for (LibraryAsset &asset : m_assets) {
            if (asset.id == assetId) {
                if (!libraryPath.isEmpty() && QFileInfo(previewPath).isAbsolute()) {
                    QDir root(libraryPath);
                    asset.previewRelativePath = root.relativeFilePath(previewPath);
                } else {
                    asset.previewRelativePath.clear();
                }
                break;
            }
        }
    }

    m_libraryGridView->updateItemPreview(assetId, previewPath);
}

QString MainWindow::assetPreviewPath(const LibraryAsset &asset) const
{
    if (!m_libraryManager || asset.previewRelativePath.isEmpty()) {
        return {};
    }
    return m_libraryManager->resolvePath(asset.previewRelativePath);
}

QString MainWindow::assetOriginalPath(const LibraryAsset &asset) const
{
    if (!m_libraryManager || asset.originalRelativePath.isEmpty()) {
        return {};
    }
    return m_libraryManager->resolvePath(asset.originalRelativePath);
}

void MainWindow::showStatusMessage(const QString &message, int timeoutMs)
{
    if (statusBar()) {
        statusBar()->showMessage(message, timeoutMs);
    }
}

void MainWindow::clearLibrary()
{
    m_assets.clear();
    if (m_libraryGridView) {
        m_libraryGridView->clear();
    }
}

void MainWindow::openAssetInDevelop(qint64 assetId, const QString &filePath)
{
    Q_UNUSED(assetId);

    if (filePath.isEmpty()) {
        QMessageBox::warning(this,
                             tr("Unable to open image"),
                             tr("The selected asset does not have an original file path."));
        return;
    }

    if (!ui->developImageLabel) {
        qWarning() << "developImageLabel is null! Check mainwindow.ui.";
        return;
    }

    QString loadError;
    QImage image = ImageLoader::loadImageWithRawSupport(filePath, &loadError);
    if (image.isNull()) {
        QMessageBox::warning(this,
                             tr("Unable to open image"),
                             tr("Could not open %1.\n%2").arg(QFileInfo(filePath).fileName(), loadError));
        return;
    }

    QPixmap pix = QPixmap::fromImage(image);
    if (pix.isNull()) {
        QMessageBox::warning(this,
                             tr("Unable to display image"),
                             tr("Failed to convert %1 to a pixmap for display.").arg(QFileInfo(filePath).fileName()));
        return;
    }

    ui->developImageLabel->setPixmap(pix);
    ui->stackedWidget->setCurrentWidget(ui->developPage);
}

void MainWindow::handleSelectionChanged(const QList<qint64> &selection)
{
    if (selection.isEmpty()) {
        showStatusMessage(tr("No items selected"), 1500);
        return;
    }

    if (selection.size() == 1) {
        showStatusMessage(tr("1 item selected"), 1500);
    } else {
        showStatusMessage(tr("%1 items selected").arg(selection.size()), 1500);
    }
}


MainWindow::~MainWindow()
{
    clearLibrary();
    delete ui;
}

// Menubar
void MainWindow::on_actionExit_triggered()
{
    qDebug() << "Exit clicked!";
    QApplication::quit();
}

void MainWindow::on_actionNew_Library_triggered(){
    qDebug() << "New Library clicked!";

    if (!m_libraryManager) {
        QMessageBox::warning(this,
                             tr("Library unavailable"),
                             tr("The library manager is not initialized."));
        return;
    }

    const QString picturesDir = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    if (picturesDir.isEmpty()) {
        QMessageBox::warning(this,
                             tr("Unable to create library"),
                             tr("Could not determine the Pictures directory."));
        return;
    }

    QDir picturesPath(picturesDir);
    if (!picturesPath.exists() && !picturesPath.mkpath(QStringLiteral("."))) {
        QMessageBox::warning(this,
                             tr("Unable to create library"),
                             tr("Could not access or create the Pictures directory."));
        return;
    }

    const QString baseName = tr("Photoroom Library");
    QString targetPath = picturesPath.filePath(baseName);

    int suffix = 1;
    while (QDir(targetPath).exists()) {
        targetPath = picturesPath.filePath(QStringLiteral("%1_%2")
                                           .arg(baseName)
                                           .arg(suffix++));
    }

    QString errorMessage;
    if (!m_libraryManager->createLibrary(targetPath, &errorMessage)) {
        if (errorMessage.isEmpty()) {
            errorMessage = tr("Failed to create a new library at %1.").arg(QDir::toNativeSeparators(targetPath));
        }
        QMessageBox::warning(this,
                             tr("Unable to create library"),
                             errorMessage);
        return;
    }

    showStatusMessage(tr("Created library at %1").arg(QDir::toNativeSeparators(targetPath)), 4000);

    if (ui->stackedWidget && ui->libraryPage) {
        ui->stackedWidget->setCurrentWidget(ui->libraryPage);
    }
}
void MainWindow::on_actionOpen_Library_triggered()
{
    qDebug() << "Open Library clicked!";

    QString folder = QFileDialog::getExistingDirectory(this, "Open Image Library");
    if (!folder.isEmpty()) {
        if (!m_libraryManager) {
            QMessageBox::warning(this,
                                 tr("Library unavailable"),
                                 tr("The library manager is not initialized."));
            return;
        }

        QString errorMessage;
        if (!m_libraryManager->openLibrary(folder, &errorMessage)) {
            if (errorMessage.isEmpty()) {
                errorMessage = tr("Failed to open the selected library.");
            }
            QMessageBox::warning(this,
                                 tr("Unable to open library"),
                                 errorMessage);
            return;
        }

        if (ui->stackedWidget && ui->libraryPage) {
            ui->stackedWidget->setCurrentWidget(ui->libraryPage);
        }
    }
}

void MainWindow::on_actionClear_recents_triggered(){
    qDebug() << "Clear recents clicked!";
}

void MainWindow::on_actionUndo_triggered(){
    qDebug() << "Undo clicked!";
}
void MainWindow::on_actionRedo_triggered(){
    qDebug() << "Redo clicked!";
}

void MainWindow::on_actionCut_triggered(){
    qDebug() << "Cut clicked!";
}
void MainWindow::on_actionCopy_triggered(){
    qDebug() << "Copy clicked!";
}
void MainWindow::on_actionPaste_triggered(){
    qDebug() << "Paste clicked!";
}

void MainWindow::on_actionSelect_All_triggered(){
    qDebug() << "Select All clicked!";
}
void MainWindow::on_actionSelect_None_triggered(){
    qDebug() << "Select None clicked!";
}
void MainWindow::on_actionInverse_Selection_triggered(){
    qDebug() << "Inverse Selection clicked!";
}

void MainWindow::on_actionPreferences_triggered()
{
    qDebug() << "Preferences menu clicked!";

    PreferencesDialog dialog(this);
    dialog.exec();  // or dialog.show() for non-modal
}

void MainWindow::on_actionImport_triggered()
{
    if (!m_libraryManager || !m_libraryManager->hasOpenLibrary()) {
        QMessageBox::information(this,
                                 tr("No open library"),
                                 tr("Open a library before importing files."));
        return;
    }

    const QString startDir = m_libraryManager->libraryPath();
    const QString filter = tr("Images (*.png *.jpg *.jpeg *.bmp *.gif *.tif *.tiff *.webp *.heic *.heif *.raw *.cr2 *.nef *.arw *.dng);;All Files (*.*)");
    QStringList files = QFileDialog::getOpenFileNames(this,
                                                      tr("Import Files"),
                                                      startDir,
                                                      filter);
    if (files.isEmpty()) {
        return;
    }

    m_libraryManager->importFiles(files);
}
