#ifndef DRAGGABLELINEEDIT_H
#define DRAGGABLELINEEDIT_H

#include <QLineEdit>
#include <QMouseEvent>

class DraggableLineEdit : public QLineEdit
{
    Q_OBJECT
public:
    explicit DraggableLineEdit(QWidget *parent = nullptr);

signals:
    void valueChanged(int value);

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

private:
    QPoint m_lastMousePos;
    bool m_dragging;
    int m_initialValue;
};

#endif // DRAGGABLELINEEDIT_H
