#include "imagelabel.h"
#include <QMouseEvent>

ImageLabel::ImageLabel(QWidget *parent) : QLabel(parent)
{
    // --- FIX: Change the size policy from Ignored to Expanding ---
    // This policy ensures the widget correctly interacts with the layout manager,
    // providing size hints and allowing it to be resized properly within the grid.
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setAlignment(Qt::AlignCenter); // Center the pixmap within the label.
}

void ImageLabel::setLoading(bool loading)
{
    if (m_isLoading != loading) {
        m_isLoading = loading;
        update(); // Trigger a repaint
    }
}

void ImageLabel::setSelected(bool selected)
{
    if (m_isSelected != selected) {
        m_isSelected = selected;
        update(); // Trigger a repaint to show/hide selection highlight
    }
}

bool ImageLabel::isSelected() const
{
    return m_isSelected;
}

void ImageLabel::paintEvent(QPaintEvent *event)
{
    // First, get a pointer to the pixmap. This is the most compatible way.
    const QPixmap pix = this->pixmap();

    // If there is no pixmap, or the pixmap is empty, and we are loading, display 'Loading...'
    if ((!pix || pix.isNull()) && m_isLoading) {
        QPainter painter(this);
        painter.setPen(Qt::white);
        painter.drawText(rect(), Qt::AlignCenter, "Loading...");
        return;
    }

    // If there is no pixmap, or the pixmap is empty, let the base class handle it.
    if (!pix || pix.isNull()) {
        QLabel::paintEvent(event);
        return;
    }

    // Now we have a valid pixmap, let's work with the actual object.
    const QPixmap& p = pix;

    QPainter painter(this);
    painter.setRenderHint(QPainter::SmoothPixmapTransform); // Makes the scaled image look better

    // Get the widget and pixmap dimensions
    QSize widgetSize = this->size();
    QSize pixmapSize = p.size();

    // Scale the pixmap size to fit within the widget while preserving the aspect ratio
    QSize scaledSize = pixmapSize;
    scaledSize.scale(widgetSize, Qt::KeepAspectRatio);

    // Calculate the top-left point to center the pixmap within the widget
    int x = (widgetSize.width() - scaledSize.width()) / 2;
    int y = (widgetSize.height() - scaledSize.height()) / 2;

    // Draw the pixmap in the calculated rectangle
    painter.drawPixmap(QRect(x, y, scaledSize.width(), scaledSize.height()), p);

    // If the widget is selected, draw a highlight border
    if (m_isSelected) {
        painter.setPen(QPen(QColor(0, 120, 215), 4)); // A nice blue color
        painter.drawRect(this->rect().adjusted(2, 2, -2, -2));
    }
}

void ImageLabel::mousePressEvent(QMouseEvent *event)
{
    emit clicked(event);
    QLabel::mousePressEvent(event);
}

void ImageLabel::mouseDoubleClickEvent(QMouseEvent *event)
{
    emit doubleClicked();
    QLabel::mouseDoubleClickEvent(event);
}
