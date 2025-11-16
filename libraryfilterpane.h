#ifndef LIBRARYFILTERPANE_H
#define LIBRARYFILTERPANE_H

#include "metadatacache.h"

#include <QWidget>
#include <QComboBox>
#include <QSpinBox>
#include <QLineEdit>
#include <QPushButton>

class LibraryFilterPane : public QWidget
{
    Q_OBJECT
public:
    explicit LibraryFilterPane(QWidget *parent = nullptr);

    FilterOptions currentFilterOptions() const;
    void setAvailableCameraMakes(const QStringList &makes);
    void setAvailableTags(const QStringList &tags);
    void setIsoRange(int min, int max);
    void clearFilters();

signals:
    void filterChanged(const FilterOptions &options);

private slots:
    void onSortOrderChanged(int index);
    void onIsoMinChanged(int value);
    void onIsoMaxChanged(int value);
    void onCameraMakeChanged(const QString &text);
    void onTagFilterChanged();
    void onClearFilters();

private:
    void setupUI();
    void emitFilterChanged();

    QComboBox *m_sortCombo = nullptr;
    QSpinBox *m_isoMinSpin = nullptr;
    QSpinBox *m_isoMaxSpin = nullptr;
    QComboBox *m_cameraMakeCombo = nullptr;
    QLineEdit *m_tagFilterEdit = nullptr;
    QPushButton *m_clearButton = nullptr;

    FilterOptions m_currentOptions;
};

#endif // LIBRARYFILTERPANE_H

