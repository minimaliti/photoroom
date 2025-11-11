#ifndef EXPORTDIALOG_H
#define EXPORTDIALOG_H

#include <QDialog>
#include <QStringList>

class QListWidget;
class QComboBox;
class QSlider;
class QSpinBox;
class QLineEdit;
class QLabel;
class QPushButton;
class QCheckBox;
class QDialogButtonBox;

class ExportDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ExportDialog(QWidget *parent = nullptr);

    void setImageList(const QStringList &paths, bool preselectAll = true);
    QStringList selectedImages() const;

    QString exportFormat() const;
    int quality() const;
    bool isQualityEnabled() const;

    QString namingMode() const;
    QString customPattern() const;
    int sequenceStart() const;
    int sequencePadding() const;
    QString customSuffix() const;
    bool createSubfolder() const;

private slots:
    void selectAll();
    void clearSelection();
    void updateQualityControls();
    void updateNamingControls();

private:
    void setupUi();
    void connectSignals();
    void setAllItemsChecked(Qt::CheckState state);

    QListWidget *m_imageListWidget = nullptr;
    QPushButton *m_selectAllButton = nullptr;
    QPushButton *m_clearSelectionButton = nullptr;

    QComboBox *m_formatCombo = nullptr;
    QSlider *m_qualitySlider = nullptr;
    QSpinBox *m_qualitySpin = nullptr;
    QLabel *m_qualityLabel = nullptr;

    QComboBox *m_namingModeCombo = nullptr;
    QLineEdit *m_customPatternEdit = nullptr;
    QSpinBox *m_sequenceStartSpin = nullptr;
    QComboBox *m_sequencePaddingCombo = nullptr;
    QLineEdit *m_customSuffixEdit = nullptr;
    QCheckBox *m_createSubfolderCheck = nullptr;

    QDialogButtonBox *m_buttonBox = nullptr;
};

#endif // EXPORTDIALOG_H

