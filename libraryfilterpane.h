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
    void clearFilters();

signals:
    void filterChanged(const FilterOptions &options);

private slots:
    void onSortOrderChanged(int index);
    void onIsoMinChanged(const QString &text);
    void onIsoMaxChanged(const QString &text);
    void onCameraMakeChanged(const QString &text);
    void onTagFilterChanged();
    void onClearFilters();

private:
    void setupUI();
    void emitFilterChanged();

    QComboBox *m_sortCombo = nullptr;
    QComboBox *m_isoMinCombo = nullptr;
    QComboBox *m_isoMaxCombo = nullptr;
    QComboBox *m_cameraMakeCombo = nullptr;
    QLineEdit *m_tagFilterEdit = nullptr;
    QPushButton *m_clearButton = nullptr;

    FilterOptions m_currentOptions;
};

#endif // LIBRARYFILTERPANE_H

