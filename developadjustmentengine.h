#ifndef DEVELOPADJUSTMENTENGINE_H
#define DEVELOPADJUSTMENTENGINE_H

#include <QObject>
#include <QImage>
#include <QFuture>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QMutex>
#include <QThreadStorage>

#include <atomic>
#include <memory>
#include <mutex>

#include "developtypes.h"

struct DevelopAdjustmentRenderResult
{
    int requestId = 0;
    QImage image;
    bool cancelled = false;
    qint64 elapsedMs = 0;
    bool isPreview = false;
    double displayScale = 1.0;
    QString errorMessage;
};

struct DevelopAdjustmentRequest
{
    int requestId = 0;
    QImage image;
    DevelopAdjustments adjustments;
    bool isPreview = false;
    double displayScale = 1.0;
};

class DevelopAdjustmentEngine : public QObject
{
    Q_OBJECT

public:
    struct CancellationToken {
        std::atomic<bool> cancelled{false};
    };

    explicit DevelopAdjustmentEngine(QObject *parent = nullptr);
    ~DevelopAdjustmentEngine() override;

    // Must be called on the main/GUI thread to initialize GPU resources
    void initializeGpuOnMainThread();

    QFuture<DevelopAdjustmentRenderResult> renderAsync(DevelopAdjustmentRequest request);

    void cancelActive();

private:

    QFuture<DevelopAdjustmentRenderResult> startRender(DevelopAdjustmentRequest request,
                                                       const std::shared_ptr<CancellationToken> &token);

    std::shared_ptr<CancellationToken> makeActiveToken();

    mutable std::mutex m_mutex;
    std::shared_ptr<CancellationToken> m_activeToken;
    bool initializeGpu();
    DevelopAdjustmentRenderResult renderWithGpu(const DevelopAdjustmentRequest &request,
                                                const std::shared_ptr<CancellationToken> &token);

    bool m_gpuInitialized = false;
    bool m_gpuAvailable = false;
    std::unique_ptr<QOpenGLContext> m_glContext;
    std::unique_ptr<QOffscreenSurface> m_offscreenSurface;
    mutable QMutex m_glMutex;
    GLuint m_computeProgram = 0;
    
    // Thread-local contexts for background rendering (shared with main context)
    mutable QThreadStorage<QOpenGLContext*> m_threadContexts;
    mutable QThreadStorage<QOffscreenSurface*> m_threadSurfaces;
    QOpenGLContext* getOrCreateThreadContext() const;
};

#endif // DEVELOPADJUSTMENTENGINE_H


