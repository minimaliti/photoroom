#include "histogramwidget.h"

#include <QLinearGradient>
#include <QPainter>
#include <QPaintEvent>
#include <QPainterPath>
#include <QPen>
#include <QEasingCurve>
#include <algorithm>

namespace {
constexpr int kBins = 256;
}

HistogramWidget::HistogramWidget(QWidget *parent)
    : QWidget(parent)
{
    setAutoFillBackground(false);
    setMinimumHeight(160);
    setStatusMessage(tr("Histogram will appear when an image is loaded."));

    m_animation = new QVariantAnimation(this);
    m_animation->setDuration(250);
    m_animation->setEasingCurve(QEasingCurve::OutCubic);
    connect(m_animation, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
        updateInterpolatedDisplay(value.toReal());
        update();
    });
    connect(m_animation, &QVariantAnimation::finished, this, [this]() {
        updateInterpolatedDisplay(1.0);
        update();
    });
}

void HistogramWidget::setHistogramData(const HistogramData &data)
{
    const bool incomingValid = data.isValid();

    if (!incomingValid) {
        m_histogram = HistogramData{};
        m_hasData = false;
        if (m_animation) {
            m_animation->stop();
        }
        m_statusMessage = tr("Histogram unavailable.");
        m_showStatusMessage = true;
        m_displayRed.clear();
        m_displayGreen.clear();
        m_displayBlue.clear();
        m_displayLuminance.clear();
        m_displayMaxValue = 0.0;
        update();
        return;
    }

    if (!m_animation) {
        m_animation = new QVariantAnimation(this);
    }

    if (!m_hasData || m_displayRed.isEmpty()) {
        m_histogram = data;
        initializeDisplayData(data);
        m_hasData = true;
        m_statusMessage.clear();
        m_showStatusMessage = false;
        update();
        return;
    }

    if (m_animation->state() == QVariantAnimation::Running) {
        m_animation->stop();
    }

    m_histogram = data;
    const bool started = startAnimationTowards(data);
    m_hasData = true;
    m_statusMessage.clear();
    m_showStatusMessage = false;

    if (started) {
        m_animation->start();
    } else {
        update();
    }
}

void HistogramWidget::clear()
{
    m_histogram = HistogramData{};
    m_hasData = false;
    m_statusMessage = tr("Histogram will appear when an image is loaded.");
    m_showStatusMessage = true;
    if (m_animation) {
        m_animation->stop();
    }
    m_displayRed.clear();
    m_displayGreen.clear();
    m_displayBlue.clear();
    m_displayLuminance.clear();
    m_displayMaxValue = 0.0;
    update();
}

void HistogramWidget::setStatusMessage(const QString &message)
{
    m_statusMessage = message;
    const bool hasDisplayData = m_hasData &&
                                m_displayRed.size() == kBins &&
                                m_displayGreen.size() == kBins &&
                                m_displayBlue.size() == kBins &&
                                m_displayLuminance.size() == kBins &&
                                m_displayMaxValue > 0.0;

    m_showStatusMessage = !hasDisplayData;
    if (m_animation) {
        m_animation->stop();
    }
    update();
}

void HistogramWidget::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const QRectF bounds = rect();
    painter.fillRect(bounds, QColor(20, 20, 20));

    if (m_showStatusMessage) {
        painter.setPen(QColor(160, 160, 160));
        painter.drawText(bounds.adjusted(8, 8, -8, -8),
                         Qt::AlignCenter | Qt::TextWordWrap,
                         m_statusMessage.isEmpty() ? tr("Histogram unavailable.") : m_statusMessage);
        return;
    }

    if (!m_hasData || m_displayRed.size() != kBins || m_displayGreen.size() != kBins ||
        m_displayBlue.size() != kBins || m_displayLuminance.size() != kBins ||
        m_displayMaxValue <= 0.0) {
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

    const qreal maxValue = std::max<qreal>(1.0, m_displayMaxValue);

    auto drawChannel = [&](const QVector<qreal> &values, const QColor &color, qreal opacity) {
        if (values.size() != kBins) {
            return;
        }

        QPainterPath path;
        path.moveTo(plotRect.left(), plotRect.bottom());
        for (int i = 0; i < kBins; ++i) {
            const qreal x = plotRect.left() + (plotRect.width() * static_cast<qreal>(i) / (kBins - 1));
            const qreal ratio = values.at(i) / maxValue;
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

    drawChannel(m_displayRed, QColor(255, 90, 70), 0.30);
    drawChannel(m_displayGreen, QColor(70, 200, 120), 0.30);
    drawChannel(m_displayBlue, QColor(80, 140, 255), 0.30);

    if (m_displayLuminance.size() == kBins) {
        QPainterPath luminancePath;
        luminancePath.moveTo(plotRect.left(), plotRect.bottom());
        for (int i = 0; i < kBins; ++i) {
            const qreal x = plotRect.left() + (plotRect.width() * static_cast<qreal>(i) / (kBins - 1));
            const qreal ratio = m_displayLuminance.at(i) / maxValue;
            const qreal y = plotRect.bottom() - ratio * plotRect.height();
            luminancePath.lineTo(x, y);
        }

        painter.setPen(QPen(QColor(230, 230, 230), 1.5));
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(luminancePath);
    }
}

void HistogramWidget::initializeDisplayData(const HistogramData &data)
{
    m_displayRed = toRealVector(data.red);
    m_displayGreen = toRealVector(data.green);
    m_displayBlue = toRealVector(data.blue);
    m_displayLuminance = toRealVector(data.luminance);
    m_displayMaxValue = std::max<qreal>(1.0, static_cast<qreal>(data.maxValue));

    m_startRed = m_displayRed;
    m_startGreen = m_displayGreen;
    m_startBlue = m_displayBlue;
    m_startLuminance = m_displayLuminance;
    m_targetRed = m_displayRed;
    m_targetGreen = m_displayGreen;
    m_targetBlue = m_displayBlue;
    m_targetLuminance = m_displayLuminance;
    m_startMaxValue = m_displayMaxValue;
    m_targetMaxValue = m_displayMaxValue;
}

bool HistogramWidget::startAnimationTowards(const HistogramData &data)
{
    if (!m_animation) {
        return false;
    }

    m_startRed = m_displayRed;
    m_startGreen = m_displayGreen;
    m_startBlue = m_displayBlue;
    m_startLuminance = m_displayLuminance;
    m_startMaxValue = m_displayMaxValue;

    m_targetRed = toRealVector(data.red);
    m_targetGreen = toRealVector(data.green);
    m_targetBlue = toRealVector(data.blue);
    m_targetLuminance = toRealVector(data.luminance);
    m_targetMaxValue = std::max<qreal>(1.0, static_cast<qreal>(data.maxValue));

    // Ensure vectors are aligned to avoid mismatched sizes.
    if (m_startRed.size() != kBins || m_targetRed.size() != kBins) {
        initializeDisplayData(data);
        return false;
    }

    m_animation->setStartValue(0.0);
    m_animation->setEndValue(1.0);
    return true;
}

void HistogramWidget::updateInterpolatedDisplay(qreal progress)
{
    progress = std::clamp(progress, 0.0, 1.0);

    if (m_targetRed.size() != kBins || m_startRed.size() != kBins) {
        return;
    }

    auto lerpVectors = [progress](const QVector<qreal> &start, const QVector<qreal> &target, QVector<qreal> &out) {
        if (start.size() != target.size()) {
            return;
        }
        if (out.size() != start.size()) {
            out.resize(start.size());
        }
        for (int i = 0; i < start.size(); ++i) {
            out[i] = start[i] + (target[i] - start[i]) * progress;
        }
    };

    lerpVectors(m_startRed, m_targetRed, m_displayRed);
    lerpVectors(m_startGreen, m_targetGreen, m_displayGreen);
    lerpVectors(m_startBlue, m_targetBlue, m_displayBlue);
    lerpVectors(m_startLuminance, m_targetLuminance, m_displayLuminance);

    m_displayMaxValue = m_startMaxValue + (m_targetMaxValue - m_startMaxValue) * progress;
    m_displayMaxValue = std::max<qreal>(1.0, m_displayMaxValue);
}

QVector<qreal> HistogramWidget::toRealVector(const QVector<int> &values) const
{
    QVector<qreal> result;
    result.reserve(values.size());
    for (int value : values) {
        result.append(static_cast<qreal>(value));
    }
    return result;
}

