#include "histogramwidget.h"

#include <QLinearGradient>
#include <QPainter>
#include <QPaintEvent>
#include <QPainterPath>
#include <QPen>

namespace {
constexpr int kBins = 256;
}

HistogramWidget::HistogramWidget(QWidget *parent)
    : QWidget(parent)
{
    setAutoFillBackground(false);
    setMinimumHeight(160);
    setStatusMessage(tr("Histogram will appear when an image is loaded."));
}

void HistogramWidget::setHistogramData(const HistogramData &data)
{
    m_histogram = data;
    m_hasData = data.isValid();
    if (m_hasData) {
        m_statusMessage.clear();
    }
    update();
}

void HistogramWidget::clear()
{
    m_histogram = HistogramData{};
    m_hasData = false;
    m_statusMessage = tr("Histogram will appear when an image is loaded.");
    update();
}

void HistogramWidget::setStatusMessage(const QString &message)
{
    m_statusMessage = message;
    m_hasData = false;
    update();
}

void HistogramWidget::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const QRectF bounds = rect();
    painter.fillRect(bounds, QColor(20, 20, 20));

    if (!m_hasData || !m_histogram.isValid()) {
        painter.setPen(QColor(160, 160, 160));
        painter.drawText(bounds.adjusted(8, 8, -8, -8),
                         Qt::AlignCenter | Qt::TextWordWrap,
                         m_statusMessage.isEmpty() ? tr("Histogram unavailable.") : m_statusMessage);
        return;
    }

    const qreal padding = 6.0;
    const QRectF plotRect = bounds.adjusted(padding, padding, -padding, -padding);

    painter.setPen(QPen(QColor(70, 70, 70), 1.0));
    painter.drawRect(plotRect);

    auto drawChannel = [&](const QVector<int> &values, const QColor &color, qreal opacity) {
        if (values.size() != kBins || m_histogram.maxValue <= 0) {
            return;
        }

        QPainterPath path;
        path.moveTo(plotRect.left(), plotRect.bottom());
        for (int i = 0; i < kBins; ++i) {
            const qreal x = plotRect.left() + (plotRect.width() * static_cast<qreal>(i) / (kBins - 1));
            const qreal ratio = static_cast<qreal>(values.at(i)) / static_cast<qreal>(m_histogram.maxValue);
            const qreal y = plotRect.bottom() - ratio * plotRect.height();
            path.lineTo(x, y);
        }
        path.lineTo(plotRect.right(), plotRect.bottom());
        path.closeSubpath();

        QColor fillColor = color;
        fillColor.setAlphaF(opacity);
        painter.setBrush(fillColor);
        painter.setPen(QPen(color, 1.0));
        painter.drawPath(path);
    };

    drawChannel(m_histogram.red, QColor(255, 90, 70), 0.30);
    drawChannel(m_histogram.green, QColor(70, 200, 120), 0.30);
    drawChannel(m_histogram.blue, QColor(80, 140, 255), 0.30);

    if (m_histogram.luminance.size() == kBins && m_histogram.maxValue > 0) {
        QPainterPath luminancePath;
        luminancePath.moveTo(plotRect.left(), plotRect.bottom());
        for (int i = 0; i < kBins; ++i) {
            const qreal x = plotRect.left() + (plotRect.width() * static_cast<qreal>(i) / (kBins - 1));
            const qreal ratio = static_cast<qreal>(m_histogram.luminance.at(i)) / static_cast<qreal>(m_histogram.maxValue);
            const qreal y = plotRect.bottom() - ratio * plotRect.height();
            luminancePath.lineTo(x, y);
        }

        painter.setPen(QPen(QColor(230, 230, 230), 1.5));
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(luminancePath);
    }
}

