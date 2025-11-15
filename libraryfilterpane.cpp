#include "libraryfilterpane.h"

#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QLabel>
#include <QRegularExpression>
#include <QSet>

LibraryFilterPane::LibraryFilterPane(QWidget *parent)
    : QWidget(parent)
{
    setupUI();
}

void LibraryFilterPane::setupUI()
{
    auto *mainLayout = new QHBoxLayout(this);
    mainLayout->setContentsMargins(8, 8, 8, 8);
    mainLayout->setSpacing(8);

    // Sort order
    auto *sortLabel = new QLabel(tr("Sort:"), this);
    mainLayout->addWidget(sortLabel);

    m_sortCombo = new QComboBox(this);
    m_sortCombo->addItem(tr("Date (Newest First)"), FilterOptions::SortByDateDesc);
    m_sortCombo->addItem(tr("Date (Oldest First)"), FilterOptions::SortByDateAsc);
    m_sortCombo->addItem(tr("ISO (High to Low)"), FilterOptions::SortByIsoDesc);
    m_sortCombo->addItem(tr("ISO (Low to High)"), FilterOptions::SortByIsoAsc);
    m_sortCombo->addItem(tr("Camera Make"), FilterOptions::SortByCameraMake);
    m_sortCombo->addItem(tr("File Name"), FilterOptions::SortByFileName);
    m_sortCombo->setCurrentIndex(0); // Default to date desc
    // Don't emit filterChanged on initial setup - wait for user interaction
    connect(m_sortCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &LibraryFilterPane::onSortOrderChanged);
    mainLayout->addWidget(m_sortCombo);

    mainLayout->addSpacing(16);

    // ISO range
    auto *isoLabel = new QLabel(tr("ISO:"), this);
    mainLayout->addWidget(isoLabel);

    m_isoMinSpin = new QSpinBox(this);
    m_isoMinSpin->setMinimum(0);
    m_isoMinSpin->setMaximum(1000000);
    m_isoMinSpin->setSpecialValueText(tr("Min"));
    m_isoMinSpin->setValue(0);
    connect(m_isoMinSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, &LibraryFilterPane::onIsoMinChanged);
    mainLayout->addWidget(m_isoMinSpin);

    auto *isoToLabel = new QLabel(tr("to"), this);
    mainLayout->addWidget(isoToLabel);

    m_isoMaxSpin = new QSpinBox(this);
    m_isoMaxSpin->setMinimum(0);
    m_isoMaxSpin->setMaximum(1000000);
    m_isoMaxSpin->setSpecialValueText(tr("Max"));
    m_isoMaxSpin->setValue(0);
    connect(m_isoMaxSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, &LibraryFilterPane::onIsoMaxChanged);
    mainLayout->addWidget(m_isoMaxSpin);

    mainLayout->addSpacing(16);

    // Camera make
    auto *cameraLabel = new QLabel(tr("Camera:"), this);
    mainLayout->addWidget(cameraLabel);

    m_cameraMakeCombo = new QComboBox(this);
    m_cameraMakeCombo->setEditable(false);
    m_cameraMakeCombo->addItem(tr("All"), QString());
    connect(m_cameraMakeCombo, QOverload<const QString &>::of(&QComboBox::currentTextChanged), this, &LibraryFilterPane::onCameraMakeChanged);
    mainLayout->addWidget(m_cameraMakeCombo);

    mainLayout->addSpacing(16);

    // Tags
    auto *tagLabel = new QLabel(tr("Tags:"), this);
    mainLayout->addWidget(tagLabel);

    m_tagFilterEdit = new QLineEdit(this);
    m_tagFilterEdit->setPlaceholderText(tr("Comma-separated tags"));
    m_tagFilterEdit->setToolTip(tr("Enter tags separated by commas"));
    connect(m_tagFilterEdit, &QLineEdit::textChanged, this, &LibraryFilterPane::onTagFilterChanged);
    mainLayout->addWidget(m_tagFilterEdit);

    mainLayout->addSpacing(16);

    // Clear button
    m_clearButton = new QPushButton(tr("Clear Filters"), this);
    connect(m_clearButton, &QPushButton::clicked, this, &LibraryFilterPane::onClearFilters);
    mainLayout->addWidget(m_clearButton);

    mainLayout->addStretch();
}

FilterOptions LibraryFilterPane::currentFilterOptions() const
{
    return m_currentOptions;
}

void LibraryFilterPane::setAvailableCameraMakes(const QStringList &makes)
{
    if (!m_cameraMakeCombo) {
        return;
    }

    const QString current = m_cameraMakeCombo->currentData().toString();
    m_cameraMakeCombo->clear();
    m_cameraMakeCombo->addItem(tr("All"), QString());

    for (const QString &make : makes) {
        m_cameraMakeCombo->addItem(make, make);
    }

    // Restore selection if still available
    int index = m_cameraMakeCombo->findData(current);
    if (index >= 0) {
        m_cameraMakeCombo->setCurrentIndex(index);
    }
}

void LibraryFilterPane::setAvailableTags(const QStringList &tags)
{
    Q_UNUSED(tags);
    // Could be used for autocomplete or tag selection widget in the future
}

void LibraryFilterPane::setIsoRange(int min, int max)
{
    if (m_isoMinSpin) {
        m_isoMinSpin->setMinimum(min);
        m_isoMinSpin->setMaximum(max);
    }
    if (m_isoMaxSpin) {
        m_isoMaxSpin->setMinimum(min);
        m_isoMaxSpin->setMaximum(max);
    }
}

void LibraryFilterPane::onSortOrderChanged(int index)
{
    if (index < 0 || !m_sortCombo) {
        return;
    }

    const QVariant data = m_sortCombo->itemData(index);
    if (data.isValid()) {
        m_currentOptions.sortOrder = static_cast<FilterOptions::SortOrder>(data.toInt());
        emitFilterChanged();
    }
}

void LibraryFilterPane::onIsoMinChanged(int value)
{
    m_currentOptions.isoMin = value;
    if (m_isoMaxSpin && value > 0 && m_isoMaxSpin->value() > 0 && value > m_isoMaxSpin->value()) {
        m_isoMaxSpin->setValue(value);
    }
    emitFilterChanged();
}

void LibraryFilterPane::onIsoMaxChanged(int value)
{
    m_currentOptions.isoMax = value;
    if (m_isoMinSpin && value > 0 && m_isoMinSpin->value() > 0 && value < m_isoMinSpin->value()) {
        m_isoMinSpin->setValue(value);
    }
    emitFilterChanged();
}

void LibraryFilterPane::onCameraMakeChanged(const QString &text)
{
    if (!m_cameraMakeCombo) {
        return;
    }

    const QVariant data = m_cameraMakeCombo->currentData();
    m_currentOptions.cameraMake = data.toString();
    emitFilterChanged();
}

void LibraryFilterPane::onTagFilterChanged()
{
    if (!m_tagFilterEdit) {
        return;
    }

    const QString text = m_tagFilterEdit->text().trimmed();
    if (text.isEmpty()) {
        m_currentOptions.tags.clear();
    } else {
        // Split by comma and clean up
        QStringList tags = text.split(QRegularExpression(QStringLiteral("[,;]")), Qt::SkipEmptyParts);
        for (QString &tag : tags) {
            tag = tag.trimmed();
        }
        tags.removeAll(QString());
        m_currentOptions.tags = tags;
    }
    emitFilterChanged();
}

void LibraryFilterPane::onClearFilters()
{
    if (m_sortCombo) {
        m_sortCombo->setCurrentIndex(0); // Date desc
    }
    if (m_isoMinSpin) {
        m_isoMinSpin->setValue(0);
    }
    if (m_isoMaxSpin) {
        m_isoMaxSpin->setValue(0);
    }
    if (m_cameraMakeCombo) {
        m_cameraMakeCombo->setCurrentIndex(0); // All
    }
    if (m_tagFilterEdit) {
        m_tagFilterEdit->clear();
    }

    m_currentOptions = FilterOptions();
    emitFilterChanged();
}

void LibraryFilterPane::emitFilterChanged()
{
    emit filterChanged(m_currentOptions);
}

