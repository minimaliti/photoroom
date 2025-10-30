#include "draggablelineedit.h"

DraggableLineEdit::DraggableLineEdit(QWidget *parent)
    : QLineEdit(parent),
      m_dragging(false),
      m_initialValue(0)
{
    setCursor(Qt::SizeHorCursor); // Change cursor to indicate horizontal dragging
}

void DraggableLineEdit::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        m_lastMousePos = event->pos();
        m_dragging = true;
        m_initialValue = text().toInt();
        event->accept();
    } else {
        QLineEdit::mousePressEvent(event);
    }
}

void DraggableLineEdit::mouseMoveEvent(QMouseEvent *event)
{
    if (m_dragging) {
        int deltaX = event->pos().x() - m_lastMousePos.x();
        int newValue = m_initialValue + deltaX; // Simple 1:1 mapping for now

        setText(QString::number(newValue));
        emit valueChanged(newValue); // Emit signal with the new value

        m_lastMousePos = event->pos();
        event->accept();
    } else {
        QLineEdit::mouseMoveEvent(event);
    }
}

void DraggableLineEdit::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && m_dragging) {
        m_dragging = false;
        event->accept();
    } else {
        QLineEdit::mouseReleaseEvent(event);
    }
}
