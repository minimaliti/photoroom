#ifndef JOBSWINDOW_H
#define JOBSWINDOW_H

#include <QWidget>

class JobManager;
class JobsPanel;

class JobsWindow : public QWidget
{
    Q_OBJECT

public:
    explicit JobsWindow(QWidget *parent = nullptr);

    void setJobManager(JobManager *manager);
    void showRelativeTo(QWidget *reference, int margin = 12);

signals:
    void visibilityChanged(bool visible);

protected:
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    JobsPanel *m_panel = nullptr;
};

#endif // JOBSWINDOW_H

