#include "libraryfilterpane.h"

#include <QFontMetrics>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QLabel>
#include <QRegularExpression>
#include <QSet>
#include <qabstractitemview.h>

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

    m_isoMinCombo = new QComboBox(this);
    m_isoMaxCombo = new QComboBox(this);

    const QStringList isoValues = {"Any", "50", "100", "200", "400", "800", "1600", "3200", "6400", "12800", "25600"};

    m_isoMinCombo->addItems(isoValues);
    m_isoMinCombo->setItemData(0, 0); // "Any"
    m_isoMaxCombo->addItems(isoValues);
    m_isoMaxCombo->setItemData(0, 0); // "Any"

    connect(m_isoMinCombo, &QComboBox::currentTextChanged, this, &LibraryFilterPane::onIsoMinChanged);
    mainLayout->addWidget(m_isoMinCombo);

    auto *isoToLabel = new QLabel(tr("to"), this);
    mainLayout->addWidget(isoToLabel);

    connect(m_isoMaxCombo, &QComboBox::currentTextChanged, this, &LibraryFilterPane::onIsoMaxChanged);
    mainLayout->addWidget(m_isoMaxCombo);

    mainLayout->addSpacing(16);

    // Camera make
    auto *cameraLabel = new QLabel(tr("Camera:"), this);
    mainLayout->addWidget(cameraLabel);

    m_cameraMakeCombo = new QComboBox(this);
    m_cameraMakeCombo->setEditable(false);
    m_cameraMakeCombo->setMinimumWidth(150);
    m_cameraMakeCombo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
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

    // Find the maximum width needed for all items
    int maxWidth = 150; // Minimum width
    QFontMetrics fm(m_cameraMakeCombo->font());
    
    for (const QString &make : makes) {
        m_cameraMakeCombo->addItem(make, make);
        int textWidth = fm.horizontalAdvance(make);
        if (textWidth > maxWidth) {
            maxWidth = textWidth;
        }
    }
    
    // Set the view's minimum width to prevent text cutoff
    if (m_cameraMakeCombo->view()) {
        m_cameraMakeCombo->view()->setMinimumWidth(maxWidth + 50); // Add padding for scrollbar
    }
    
    // Set combo box width to accommodate content
    m_cameraMakeCombo->setMinimumWidth(qMin(maxWidth + 50, 400)); // Cap at 400px

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

void LibraryFilterPane::onIsoMinChanged(const QString &text)
{
    bool ok;
    int value = text.toInt(&ok);
    if (!ok) { // "Any"
        value = 0;
    }

    m_currentOptions.isoMin = value;

    if (m_isoMaxCombo && value > 0) {
        int maxVal = m_isoMaxCombo->currentText().toInt();
        if (maxVal > 0 && value > maxVal) {
            m_isoMaxCombo->setCurrentText(text);
        }
    }
    emitFilterChanged();
}

void LibraryFilterPane::onIsoMaxChanged(const QString &text)
{
    bool ok;
    int value = text.toInt(&ok);
    if (!ok) { // "Any"
        value = 0;
    }

    m_currentOptions.isoMax = value;

    if (m_isoMinCombo && value > 0) {
        int minVal = m_isoMinCombo->currentText().toInt();
        if (minVal > 0 && value < minVal) {
            m_isoMinCombo->setCurrentText(text);
        }
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

void LibraryFilterPane::clearFilters()
{
    onClearFilters();
}

void LibraryFilterPane::onClearFilters()
{
    if (m_sortCombo) {
        m_sortCombo->setCurrentIndex(0); // Date desc
    }
    if (m_isoMinCombo) {
        m_isoMinCombo->setCurrentIndex(0);
    }
    if (m_isoMaxCombo) {
        m_isoMaxCombo->setCurrentIndex(0);
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

