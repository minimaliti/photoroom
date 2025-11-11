#ifndef HISTOGRAMWIDGET_H
#define HISTOGRAMWIDGET_H

#include <QWidget>
#include <QVariantAnimation>

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
    void initializeDisplayData(const HistogramData &data);
    bool startAnimationTowards(const HistogramData &data);
    void updateInterpolatedDisplay(qreal progress);
    QVector<qreal> toRealVector(const QVector<int> &values) const;

    HistogramData m_histogram;
    bool m_hasData = false;
    QString m_statusMessage;
    QVariantAnimation *m_animation = nullptr;

    QVector<qreal> m_displayRed;
    QVector<qreal> m_displayGreen;
    QVector<qreal> m_displayBlue;
    QVector<qreal> m_displayLuminance;

    QVector<qreal> m_startRed;
    QVector<qreal> m_startGreen;
    QVector<qreal> m_startBlue;
    QVector<qreal> m_startLuminance;

    QVector<qreal> m_targetRed;
    QVector<qreal> m_targetGreen;
    QVector<qreal> m_targetBlue;
    QVector<qreal> m_targetLuminance;

    qreal m_displayMaxValue = 0.0;
    qreal m_startMaxValue = 0.0;
    qreal m_targetMaxValue = 0.0;
    bool m_showStatusMessage = false;
};

#endif // HISTOGRAMWIDGET_H

