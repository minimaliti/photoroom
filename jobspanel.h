#ifndef JOBSPANEL_H
#define JOBSPANEL_H

#include <QHash>
#include <QWidget>

#include "jobmanager.h"

class QLabel;
class QProgressBar;
class QScrollArea;
class QVBoxLayout;

class JobRowWidget : public QWidget
{
    Q_OBJECT
public:
    explicit JobRowWidget(QWidget *parent = nullptr);

    void updateFromJob(const JobInfo &info);
    QUuid jobId() const;

private:
    void applyStateStyling(const JobInfo &info);

    QUuid m_jobId;
    QLabel *m_titleLabel = nullptr;
    QProgressBar *m_progressBar = nullptr;
};

class JobsPanel : public QWidget
{
    Q_OBJECT
public:
    explicit JobsPanel(QWidget *parent = nullptr);

    void setJobManager(JobManager *manager);
    JobManager *jobManager() const;

signals:
    void requestClose();

private:
    void rebuildFromManager();
    void handleJobAdded(const JobInfo &info);
    void handleJobUpdated(const JobInfo &info);
    void handleJobRemoved(const QUuid &id);
    void ensureEmptyStateVisible();

    JobManager *m_jobManager = nullptr;
    QScrollArea *m_scrollArea = nullptr;
    QWidget *m_listContainer = nullptr;
    QVBoxLayout *m_listLayout = nullptr;
    QLabel *m_emptyStateLabel = nullptr;
    QHash<QUuid, JobRowWidget*> m_rows;
};

#endif // JOBSPANEL_H

