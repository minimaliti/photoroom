#ifndef HISTOGRAMWIDGET_H
#define HISTOGRAMWIDGET_H

#include <QWidget>

#include "developtypes.h"

class HistogramWidget : public QWidget
{
    Q_OBJECT

public:
    explicit HistogramWidget(QWidget *parent = nullptr);

    void setHistogramData(const HistogramData &data);
    void clear();
    void setStatusMessage(const QString &message);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    HistogramData m_histogram;
    bool m_hasData = false;
    QString m_statusMessage;
};

#endif // HISTOGRAMWIDGET_H

