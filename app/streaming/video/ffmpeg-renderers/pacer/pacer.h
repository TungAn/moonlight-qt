#pragma once

#include "../../decoder.h"
#include "../renderer.h"

#include <QQueue>
#include <QMutex>
#include <QWaitCondition>
#include <array>

#include <SDL_stdinc.h>

class IVsyncSource {
public:
    virtual ~IVsyncSource() {}
    virtual bool initialize(SDL_Window* window, int displayFps) = 0;

    // Asynchronous sources produce callbacks on their own, while synchronous
    // sources require calls to waitForVsync().
    virtual bool isAsync() = 0;

    virtual void waitForVsync() {
        // Synchronous sources must implement waitForVsync()!
        SDL_assert(false);
    }
};



class Pacer
{
public:
    Pacer(IFFmpegRenderer* renderer, PVIDEO_STATS videoStats);

    ~Pacer();

    void submitFrame(AVFrame* frame);

    bool initialize(SDL_Window* window, int maxVideoFps, bool enablePacing, bool allowVsyncSource);

    void signalVsync();

    void renderOnMainThread();

    // Add this method to signal RFI state
    void setWaitingForRFI(bool waiting);
    bool isWaitingForRFI() const { return m_WaitingForRFI; }

private:
    static int vsyncThread(void* context);

    static int renderThread(void* context);

    static int manualPacingThread(void* context);

    void handleVsync(int timeUntilNextVsyncMillis);

    void enqueueFrameForRenderingAndUnlock(AVFrame* frame);

    void renderFrame(AVFrame* frame);

    void dropFrameForEnqueue(QQueue<AVFrame*>& queue);

    void startManualPacingThreadIfNeeded();
    void initializeManualPacingState();
    Uint64 getTimeNs() const;
    Uint64 computeManualReleaseTimeNs(Uint64 nowNs);
    void recordManualFrameInterval(Uint64 nowNs);
    void sleepUntilNs(Uint64 targetNs);
    bool isManualPacingEnabled() const;
    void resetManualTimingState(Uint64 nowNs);

    QQueue<AVFrame*> m_RenderQueue;
    QQueue<AVFrame*> m_PacingQueue;
    QQueue<int> m_PacingQueueHistory;
    QQueue<int> m_RenderQueueHistory;
    QMutex m_FrameQueueLock;
    QWaitCondition m_RenderQueueNotEmpty;
    QWaitCondition m_PacingQueueNotEmpty;
    QWaitCondition m_VsyncSignalled;
    SDL_Thread* m_RenderThread;
    SDL_Thread* m_VsyncThread;
    SDL_Thread* m_ManualPacingThread;
    bool m_Stopping;

    IVsyncSource* m_VsyncSource;
    IFFmpegRenderer* m_VsyncRenderer;
    int m_MaxVideoFps;
    int m_DisplayFps;
    PVIDEO_STATS m_VideoStats;
    int m_RendererAttributes;

    bool m_ManualPacingActive;
    Uint64 m_TargetFrameIntervalNs;
    Uint64 m_FixedSafetyMarginNs;
    Uint64 m_ManualStableTimingBaseNs;
    Uint64 m_ManualStreamStartTimeNs;
    Uint64 m_ManualLastFrameTimeNs;
    Uint64 m_ManualLastPresentationTimeNs;
    Uint64 m_ManualWarmupStartTimeNs;
    Uint64 m_ManualMinFrameIntervalNs;
    Uint64 m_ManualMaxFrameDeviationNs;
    int m_ManualFrameSequenceNumber;
    int m_ManualFrameCount;
    int m_ManualWarmupFrameCount;
    int m_ManualIntervalIndex;
    bool m_ManualIsWarmingUp;
    bool m_ManualBurstProtectionActive;
    std::array<Uint64, 8> m_ManualRecentFrameIntervals;

    bool m_WaitingForRFI;
}; 
