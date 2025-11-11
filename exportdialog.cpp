#include "exportdialog.h"

#include <QAbstractItemView>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QVBoxLayout>

ExportDialog::ExportDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Export Images"));
    setModal(true);
    setMinimumSize(540, 520);

    setupUi();
    connectSignals();
    updateQualityControls();
    updateNamingControls();
}

void ExportDialog::setImageList(const QStringList &paths, bool preselectAll)
{
    m_imageListWidget->clear();
    m_imageListWidget->setUpdatesEnabled(false);

    for (const QString &path : paths) {
        const QFileInfo info(path);
        auto *item = new QListWidgetItem(info.fileName().isEmpty() ? path : info.fileName());
        item->setData(Qt::UserRole, path);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(preselectAll ? Qt::Checked : Qt::Unchecked);
        m_imageListWidget->addItem(item);
    }

    m_imageListWidget->setUpdatesEnabled(true);
}

QStringList ExportDialog::selectedImages() const
{
    QStringList selected;
    selected.reserve(m_imageListWidget->count());

    for (int i = 0; i < m_imageListWidget->count(); ++i) {
        const QListWidgetItem *item = m_imageListWidget->item(i);
        if (item->checkState() == Qt::Checked) {
            selected.append(item->data(Qt::UserRole).toString());
        }
    }

    return selected;
}

QString ExportDialog::exportFormat() const
{
    return m_formatCombo->currentData().toString();
}

int ExportDialog::quality() const
{
    return m_qualitySpin->value();
}

bool ExportDialog::isQualityEnabled() const
{
    return m_qualitySlider->isEnabled();
}

QString ExportDialog::namingMode() const
{
    return m_namingModeCombo->currentData().toString();
}

QString ExportDialog::customPattern() const
{
    return m_customPatternEdit->text();
}

int ExportDialog::sequenceStart() const
{
    return m_sequenceStartSpin->value();
}

int ExportDialog::sequencePadding() const
{
    return m_sequencePaddingCombo->currentData().toInt();
}

QString ExportDialog::customSuffix() const
{
    return m_customSuffixEdit->text();
}

bool ExportDialog::createSubfolder() const
{
    return m_createSubfolderCheck->isChecked();
}

void ExportDialog::selectAll()
{
    setAllItemsChecked(Qt::Checked);
}

void ExportDialog::clearSelection()
{
    setAllItemsChecked(Qt::Unchecked);
}

void ExportDialog::updateQualityControls()
{
    const QString format = exportFormat();
    const bool lossy = (format == QStringLiteral("jpeg") || format == QStringLiteral("webp"));

    m_qualitySlider->setEnabled(lossy);
    m_qualitySpin->setEnabled(lossy);
    m_qualityLabel->setEnabled(lossy);

    if (!lossy) {
        m_qualitySlider->setValue(100);
        m_qualitySpin->setValue(100);
    }
}

void ExportDialog::updateNamingControls()
{
    const QString mode = namingMode();
    const bool useCustomPattern = (mode == QStringLiteral("custom-pattern"));
    const bool appendSuffix = (mode == QStringLiteral("original-with-suffix"));

    m_customPatternEdit->setVisible(useCustomPattern);
    m_customPatternEdit->setEnabled(useCustomPattern);

    m_sequenceStartSpin->setVisible(useCustomPattern);
    m_sequenceStartSpin->setEnabled(useCustomPattern);

    m_sequencePaddingCombo->setVisible(useCustomPattern);
    m_sequencePaddingCombo->setEnabled(useCustomPattern);

    m_customSuffixEdit->setVisible(useCustomPattern || appendSuffix);
    m_customSuffixEdit->setEnabled(useCustomPattern || appendSuffix);
}

void ExportDialog::setupUi()
{
    auto *mainLayout = new QVBoxLayout(this);

    auto *selectionGroup = new QGroupBox(tr("Images to Export"), this);
    auto *selectionLayout = new QVBoxLayout(selectionGroup);

    m_imageListWidget = new QListWidget(selectionGroup);
    m_imageListWidget->setSelectionMode(QAbstractItemView::NoSelection);
    m_imageListWidget->setAlternatingRowColors(true);
    m_imageListWidget->setMinimumHeight(160);
    selectionLayout->addWidget(m_imageListWidget);

    auto *selectionActionsLayout = new QHBoxLayout();
    m_selectAllButton = new QPushButton(tr("Select All"), selectionGroup);
    m_clearSelectionButton = new QPushButton(tr("Clear"), selectionGroup);
    selectionActionsLayout->addWidget(m_selectAllButton);
    selectionActionsLayout->addWidget(m_clearSelectionButton);
    selectionActionsLayout->addStretch();
    selectionLayout->addLayout(selectionActionsLayout);

    mainLayout->addWidget(selectionGroup);

    auto *formatGroup = new QGroupBox(tr("Format"), this);
    auto *formatLayout = new QGridLayout(formatGroup);

    m_formatCombo = new QComboBox(formatGroup);
    m_formatCombo->addItem(tr("JPEG"), QStringLiteral("jpeg"));
    m_formatCombo->addItem(tr("PNG"), QStringLiteral("png"));
    m_formatCombo->addItem(tr("TIFF"), QStringLiteral("tiff"));
    m_formatCombo->addItem(tr("WEBP"), QStringLiteral("webp"));

    formatLayout->addWidget(new QLabel(tr("File Format")), 0, 0);
    formatLayout->addWidget(m_formatCombo, 0, 1);

    m_qualityLabel = new QLabel(tr("Quality"), formatGroup);
    m_qualitySlider = new QSlider(Qt::Horizontal, formatGroup);
    m_qualitySlider->setRange(1, 100);
    m_qualitySlider->setValue(90);

    m_qualitySpin = new QSpinBox(formatGroup);
    m_qualitySpin->setRange(1, 100);
    m_qualitySpin->setValue(90);

    formatLayout->addWidget(m_qualityLabel, 1, 0);
    formatLayout->addWidget(m_qualitySlider, 1, 1);
    formatLayout->addWidget(m_qualitySpin, 1, 2);
    formatLayout->setColumnStretch(1, 1);

    mainLayout->addWidget(formatGroup);

    auto *namingGroup = new QGroupBox(tr("Naming"), this);
    auto *namingLayout = new QGridLayout(namingGroup);

    m_namingModeCombo = new QComboBox(namingGroup);
    m_namingModeCombo->addItem(tr("Use original filenames"), QStringLiteral("original"));
    m_namingModeCombo->addItem(tr("Original filenames with suffix"), QStringLiteral("original-with-suffix"));
    m_namingModeCombo->addItem(tr("Custom pattern with sequence"), QStringLiteral("custom-pattern"));

    namingLayout->addWidget(new QLabel(tr("Mode")), 0, 0);
    namingLayout->addWidget(m_namingModeCombo, 0, 1, 1, 3);

    m_customPatternEdit = new QLineEdit(namingGroup);
    m_customPatternEdit->setPlaceholderText(tr("Pattern, e.g. Export_{index}"));

    namingLayout->addWidget(new QLabel(tr("Pattern")), 1, 0);
    namingLayout->addWidget(m_customPatternEdit, 1, 1, 1, 3);

    m_sequenceStartSpin = new QSpinBox(namingGroup);
    m_sequenceStartSpin->setMinimum(0);
    m_sequenceStartSpin->setMaximum(99999);
    m_sequenceStartSpin->setValue(1);

    namingLayout->addWidget(new QLabel(tr("Start index")), 2, 0);
    namingLayout->addWidget(m_sequenceStartSpin, 2, 1);

    m_sequencePaddingCombo = new QComboBox(namingGroup);
    m_sequencePaddingCombo->addItem(tr("1 digit"), 1);
    m_sequencePaddingCombo->addItem(tr("2 digits"), 2);
    m_sequencePaddingCombo->addItem(tr("3 digits"), 3);
    m_sequencePaddingCombo->addItem(tr("4 digits"), 4);

    namingLayout->addWidget(new QLabel(tr("Number padding")), 2, 2);
    namingLayout->addWidget(m_sequencePaddingCombo, 2, 3);

    m_customSuffixEdit = new QLineEdit(namingGroup);
    m_customSuffixEdit->setPlaceholderText(tr("Suffix, e.g. _edit"));

    namingLayout->addWidget(new QLabel(tr("Suffix")), 3, 0);
    namingLayout->addWidget(m_customSuffixEdit, 3, 1, 1, 3);

    mainLayout->addWidget(namingGroup);

    auto *destinationGroup = new QGroupBox(tr("Destination"), this);
    auto *destinationLayout = new QVBoxLayout(destinationGroup);

    m_createSubfolderCheck = new QCheckBox(tr("Place exports in a subfolder named after the job"), destinationGroup);
    destinationLayout->addWidget(m_createSubfolderCheck);

    mainLayout->addWidget(destinationGroup);

    m_buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, Qt::Horizontal, this);
    mainLayout->addWidget(m_buttonBox);
}

void ExportDialog::connectSignals()
{
    connect(m_selectAllButton, &QPushButton::clicked, this, &ExportDialog::selectAll);
    connect(m_clearSelectionButton, &QPushButton::clicked, this, &ExportDialog::clearSelection);

    connect(m_formatCombo, &QComboBox::currentIndexChanged, this, &ExportDialog::updateQualityControls);
    connect(m_qualitySlider, &QSlider::valueChanged, m_qualitySpin, &QSpinBox::setValue);
    connect(m_qualitySpin, QOverload<int>::of(&QSpinBox::valueChanged), m_qualitySlider, &QSlider::setValue);

    connect(m_namingModeCombo, &QComboBox::currentIndexChanged, this, &ExportDialog::updateNamingControls);

    connect(m_buttonBox, &QDialogButtonBox::accepted, this, &ExportDialog::accept);
    connect(m_buttonBox, &QDialogButtonBox::rejected, this, &ExportDialog::reject);
}

void ExportDialog::setAllItemsChecked(Qt::CheckState state)
{
    for (int i = 0; i < m_imageListWidget->count(); ++i) {
        QListWidgetItem *item = m_imageListWidget->item(i);
        item->setCheckState(state);
    }
}

