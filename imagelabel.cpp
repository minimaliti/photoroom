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

void ImageLabel::setImageId(int id)
{
    if (m_imageId != id) {
        m_imageId = id;
        update();
    }
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
    QLabel::paintEvent(event); // Let the base class handle default painting first

    QPainter painter(this);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);

    // Fill the background with a dark color
    painter.fillRect(rect(), QColor(50, 50, 50));

    // First, get a pointer to the pixmap. This is the most compatible way.
    const QPixmap pix = this->pixmap();

    // If there is no pixmap, or the pixmap is empty, and we are loading, display 'Loading...'
    if ((!pix || pix.isNull()) && m_isLoading) {
        painter.setPen(Qt::white);
        painter.drawText(rect(), Qt::AlignCenter, "Loading...");
    } else if (!pix || pix.isNull()) {
        // If no pixmap and not loading, just draw the background
    } else {
        // Now we have a valid pixmap, let's work with the actual object.
        const QPixmap& p = pix;

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
    }

    // Draw image_id in the top-left corner with a semi-transparent background
    if (m_imageId != -1) {
        QString idText = QString::number(m_imageId);
        QFont font("Arial", 10, QFont::Bold);
        painter.setFont(font);

        // Define padding from the top-left corner of the label
        int textPadding = 5; // Adjust this value as needed

        // Calculate text size
        QFontMetrics fm(font);
        QRect textBounds = fm.boundingRect(idText);

        // Define the rectangle where the text will be drawn, offset by textPadding
        QRect drawRect(textPadding, textPadding, textBounds.width() + 2, textBounds.height());

        // Create a slightly larger rectangle for the background, relative to drawRect
        QRect backgroundRect = drawRect.adjusted(-(textPadding + 2), -textPadding, textPadding, textPadding);

        // Draw semi-transparent background
        painter.fillRect(backgroundRect, QColor(0, 0, 0, 150)); // Black with 150 alpha

        painter.setPen(Qt::white);
        painter.drawText(drawRect, idText);
    }

    // If the widget is selected, draw a highlight border
    if (m_isSelected) {
        painter.setPen(QPen(QColor(0, 120, 215), 4)); // A nice blue color, similar to Lightroom selection
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
