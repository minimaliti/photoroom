#ifndef IMAGELABEL_H
#define IMAGELABEL_H

#include <QLabel>
#include <QPainter>
#include <QPixmap>
#include <QMouseEvent>

class ImageLabel : public QLabel
{
    Q_OBJECT
public:
    explicit ImageLabel(QWidget *parent = nullptr);
    void setSelected(bool selected);
    bool isSelected() const;

signals:
    void clicked(QMouseEvent* event);
    void doubleClicked();

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;

private:
    bool m_isSelected = false;
};

#endif // IMAGELABEL_H
