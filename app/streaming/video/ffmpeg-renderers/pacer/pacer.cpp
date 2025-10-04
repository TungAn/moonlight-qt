#include "pacer.h"
#include "streaming/streamutils.h"

#ifdef Q_OS_WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <VersionHelpers.h>
#include "dxvsyncsource.h"
#endif

#ifdef HAS_WAYLAND
#include "waylandvsyncsource.h"
#endif

#include <SDL_syswm.h>
#include <SDL_timer.h>

// Limit the number of queued frames to prevent excessive memory consumption
// if the V-Sync source or renderer is blocked for a while. It's important
// that the sum of all queued frames between both pacing and rendering queues
// must not exceed the number buffer pool size to avoid running the decoder
// out of available decoding surfaces.
#define MAX_QUEUED_FRAMES 4

// We may be woken up slightly late so don't go all the way
// up to the next V-sync since we may accidentally step into
// the next V-sync period. It also takes some amount of time
// to do the render itself, so we can't render right before
// V-sync happens.
#define TIMER_SLACK_MS 3

// Cap how long we will wait after a V-sync before picking a frame. Keeping this
// small favours latency over perfect freshness because Metal renderers already
// block on display sync internally.
#define MAX_PACING_WAIT_MS 2

static constexpr Uint64 WARMUP_MIN_DURATION_NS = 50000000ULL;   // 500 ms
static constexpr int WARMUP_FRAMES = 120;
static constexpr Uint64 WARMUP_SAFETY_MARGIN_NS = 1000000ULL;    // 2 ms
static constexpr Uint64 STABLE_SAFETY_MARGIN_NS = 250000ULL;     // 0.5 ms
static constexpr Uint64 MAX_FRAME_DEVIATION_NS = 1000000ULL;     // 2 ms
static constexpr Uint32 MANUAL_STALE_FRAME_THRESHOLD_MS = 120;    // Drop only if held > 120 ms
static constexpr Uint32 MANUAL_MAX_OVERSHOOT_FRAMES = 3;          // Allow up to one frame of headroom
static constexpr Uint64 MANUAL_COARSE_SLEEP_THRESHOLD_NS = 3000000ULL;  // 3 ms
static constexpr Uint64 MANUAL_FINE_SLEEP_THRESHOLD_NS = 1000000ULL;    // 1 ms
static constexpr Uint64 MANUAL_OVERSLEEP_LOG_NS = 1500000ULL;          // 1.5 ms

Pacer::Pacer(IFFmpegRenderer* renderer, PVIDEO_STATS videoStats) :
    m_RenderThread(nullptr),
    m_VsyncThread(nullptr),
    m_ManualPacingThread(nullptr),
    m_Stopping(false),
    m_VsyncSource(nullptr),
    m_VsyncRenderer(renderer),
    m_MaxVideoFps(0),
    m_DisplayFps(0),
    m_VideoStats(videoStats),
    m_RendererAttributes(0),
    m_ManualPacingActive(false),
    m_TargetFrameIntervalNs(0),
    m_FixedSafetyMarginNs(STABLE_SAFETY_MARGIN_NS),
    m_ManualStableTimingBaseNs(0),
    m_ManualStreamStartTimeNs(0),
    m_ManualLastFrameTimeNs(0),
    m_ManualLastPresentationTimeNs(0),
    m_ManualWarmupStartTimeNs(0),
    m_ManualMinFrameIntervalNs(0),
    m_ManualMaxFrameDeviationNs(MAX_FRAME_DEVIATION_NS),
    m_ManualFrameSequenceNumber(0),
    m_ManualFrameCount(0),
    m_ManualWarmupFrameCount(0),
    m_ManualIntervalIndex(0),
    m_ManualIsWarmingUp(true),
    m_ManualBurstProtectionActive(false),
    m_WaitingForRFI(false)
{
    m_ManualRecentFrameIntervals.fill(0);
}

Pacer::~Pacer()
{
    m_Stopping = true;

    // Stop the V-sync thread
    if (m_VsyncThread != nullptr) {
        m_PacingQueueNotEmpty.wakeAll();
        m_VsyncSignalled.wakeAll();
        SDL_WaitThread(m_VsyncThread, nullptr);
    }

    // Stop the manual pacing thread
    if (m_ManualPacingThread != nullptr) {
        m_PacingQueueNotEmpty.wakeAll();
        SDL_WaitThread(m_ManualPacingThread, nullptr);
    }

    // Stop V-sync callbacks
    delete m_VsyncSource;
    m_VsyncSource = nullptr;

    // Stop the render thread
    if (m_RenderThread != nullptr) {
        m_RenderQueueNotEmpty.wakeAll();
        SDL_WaitThread(m_RenderThread, nullptr);
    }
    else {
        // Notify the renderer that it is being destroyed soon
        // NB: This must happen on the same thread that calls renderFrame().
        m_VsyncRenderer->cleanupRenderContext();
    }

    // Delete any remaining unconsumed frames
    while (!m_RenderQueue.isEmpty()) {
        AVFrame* frame = m_RenderQueue.dequeue();
        av_frame_free(&frame);
    }
    while (!m_PacingQueue.isEmpty()) {
        AVFrame* frame = m_PacingQueue.dequeue();
        av_frame_free(&frame);
    }
}

void Pacer::renderOnMainThread()
{
    // Ignore this call for renderers that work on a dedicated render thread
    if (m_RenderThread != nullptr) {
        return;
    }

    m_FrameQueueLock.lock();

    if (!m_RenderQueue.isEmpty()) {
        AVFrame* frame = m_RenderQueue.dequeue();
        m_FrameQueueLock.unlock();

        renderFrame(frame);
    }
    else {
        m_FrameQueueLock.unlock();
    }
}

int Pacer::vsyncThread(void *context)
{
    Pacer* me = reinterpret_cast<Pacer*>(context);

#if SDL_VERSION_ATLEAST(2, 0, 9)
    SDL_SetThreadPriority(SDL_THREAD_PRIORITY_TIME_CRITICAL);
#else
    SDL_SetThreadPriority(SDL_THREAD_PRIORITY_HIGH);
#endif

    bool async = me->m_VsyncSource->isAsync();
    while (!me->m_Stopping) {
        if (async) {
            // Wait for the VSync source to invoke signalVsync() or 100ms to elapse
            me->m_FrameQueueLock.lock();
            me->m_VsyncSignalled.wait(&me->m_FrameQueueLock, 100);
            me->m_FrameQueueLock.unlock();
        }
        else {
            // Let the VSync source wait in the context of our thread
            me->m_VsyncSource->waitForVsync();
        }

        if (me->m_Stopping) {
            break;
        }

        me->handleVsync(1000 / me->m_DisplayFps);
    }

    return 0;
}

int Pacer::renderThread(void* context)
{
    Pacer* me = reinterpret_cast<Pacer*>(context);

    if (SDL_SetThreadPriority(SDL_THREAD_PRIORITY_HIGH) < 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Unable to set render thread to high priority: %s",
                    SDL_GetError());
    }

    while (!me->m_Stopping) {
        // Wait for the renderer to be ready for the next frame
        me->m_VsyncRenderer->waitToRender();

        // Acquire the frame queue lock to protect the queue and
        // the not empty condition
        me->m_FrameQueueLock.lock();

        // Wait for a frame to be ready to render
        while (!me->m_Stopping && me->m_RenderQueue.isEmpty()) {
            me->m_RenderQueueNotEmpty.wait(&me->m_FrameQueueLock);
        }

        if (me->m_Stopping) {
            // Exit this thread
            me->m_FrameQueueLock.unlock();
            break;
        }

        AVFrame* frame = me->m_RenderQueue.dequeue();
        me->m_FrameQueueLock.unlock();

        me->renderFrame(frame);
    }

    // Notify the renderer that it is being destroyed soon
    // NB: This must happen on the same thread that calls renderFrame().
    me->m_VsyncRenderer->cleanupRenderContext();

    return 0;
}

void Pacer::enqueueFrameForRenderingAndUnlock(AVFrame *frame)
{
    dropFrameForEnqueue(m_RenderQueue);
    m_RenderQueue.enqueue(frame);

    m_FrameQueueLock.unlock();

    if (m_RenderThread != nullptr) {
        m_RenderQueueNotEmpty.wakeOne();
    }
    else {
        SDL_Event event;

        // For main thread rendering, we'll push an event to trigger a callback
        event.type = SDL_USEREVENT;
        event.user.code = SDL_CODE_FRAME_READY;
        SDL_PushEvent(&event);
    }
}

// Called in an arbitrary thread by the IVsyncSource on V-sync
// or an event synchronized with V-sync
void Pacer::handleVsync(int timeUntilNextVsyncMillis)
{
    // Make sure initialize() has been called
    SDL_assert(m_MaxVideoFps != 0);

    m_FrameQueueLock.lock();

    const int frameIntervalMs = SDL_max(timeUntilNextVsyncMillis, TIMER_SLACK_MS);
    const int pacingWindowMs = SDL_max(frameIntervalMs - TIMER_SLACK_MS, 0);
    const int waitWindowMs = SDL_min(pacingWindowMs, MAX_PACING_WAIT_MS);
    const bool allowLateLatch = (m_MaxVideoFps >= m_DisplayFps) && (waitWindowMs > 0);

    if (allowLateLatch) {
        const Uint32 deadline = SDL_GetTicks() + (Uint32)waitWindowMs;

        while (!m_Stopping) {
            if (m_PacingQueue.isEmpty()) {
                Uint32 now = SDL_GetTicks();

                if (SDL_TICKS_PASSED(now, deadline)) {
                    m_FrameQueueLock.unlock();
                    return;
                }

                int remainingMs = static_cast<int>(deadline - now);
                if (remainingMs <= 0 || !m_PacingQueueNotEmpty.wait(&m_FrameQueueLock, remainingMs)) {
                    m_FrameQueueLock.unlock();
                    return;
                }

                if (m_Stopping) {
                    m_FrameQueueLock.unlock();
                    return;
                }

                continue;
            }

            Uint32 now = SDL_GetTicks();
            if (SDL_TICKS_PASSED(now, deadline)) {
                break;
            }

            int remainingMs = static_cast<int>(deadline - now);
            if (remainingMs <= 0 || !m_PacingQueueNotEmpty.wait(&m_FrameQueueLock, remainingMs)) {
                break;
            }

            if (m_Stopping) {
                m_FrameQueueLock.unlock();
                return;
            }

            // A fresher frame arrived; loop so we can discard stale entries below.
        }

        // Keep a rolling 500 ms window of pacing queue history for diagnostics.
        if (m_PacingQueueHistory.count() == m_DisplayFps / 2) {
            m_PacingQueueHistory.dequeue();
        }
        m_PacingQueueHistory.enqueue(m_PacingQueue.count());

        // Prefer the freshest frame by dropping anything older that queued up while we waited.
        while (m_PacingQueue.count() > 1) {
            AVFrame* staleFrame = m_PacingQueue.dequeue();
            Uint32 now = SDL_GetTicks();
            Uint32 enqueueTicks = static_cast<Uint32>(staleFrame->pkt_dts);
            Uint32 ageMs = now - enqueueTicks;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Pacer: dropped stale frame age=%u ms (remaining=%lld)",
                        ageMs,
                        static_cast<long long>(m_PacingQueue.count()));
            m_FrameQueueLock.unlock();
            m_VideoStats->pacerDroppedFrames++;
            av_frame_free(&staleFrame);
            m_FrameQueueLock.lock();
        }

        if (m_PacingQueue.isEmpty()) {
            m_FrameQueueLock.unlock();
            return;
        }

        AVFrame* frame = m_PacingQueue.dequeue();
        Uint32 now = SDL_GetTicks();
        Uint32 enqueueTicks = static_cast<Uint32>(frame->pkt_dts);
        Uint32 ageMs = now - enqueueTicks;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Pacer: releasing frame age=%u ms (queue=%lld)",
                    ageMs,
                    static_cast<long long>(m_PacingQueue.count()));

        enqueueFrameForRenderingAndUnlock(frame);
        return;
    }

    // Below this point we don't expect the decoder to outrun the display, so fall back
    // to the original immediate pacing heuristics.
    int frameDropTarget = 1;

    while (m_PacingQueue.count() > frameDropTarget) {
        AVFrame* frame = m_PacingQueue.dequeue();

        Uint32 now = SDL_GetTicks();
        Uint32 enqueueTicks = static_cast<Uint32>(frame->pkt_dts);
        Uint32 ageMs = now - enqueueTicks;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Pacer: dropped frame (fallback) age=%u ms (remaining=%lld)",
                    ageMs,
                    static_cast<long long>(m_PacingQueue.count()));

        m_FrameQueueLock.unlock();
        m_VideoStats->pacerDroppedFrames++;
        av_frame_free(&frame);
        m_FrameQueueLock.lock();
    }

    if (m_PacingQueue.isEmpty()) {
        if (waitWindowMs <= 0 || !m_PacingQueueNotEmpty.wait(&m_FrameQueueLock, waitWindowMs)) {
            m_FrameQueueLock.unlock();
            return;
        }

        if (m_Stopping || m_PacingQueue.isEmpty()) {
            m_FrameQueueLock.unlock();
            return;
        }
    }

    AVFrame* frame = m_PacingQueue.dequeue();
    Uint32 now = SDL_GetTicks();
    Uint32 enqueueTicks = static_cast<Uint32>(frame->pkt_dts);
    Uint32 ageMs = now - enqueueTicks;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Pacer: releasing frame (fallback) age=%u ms (queue=%lld)",
                ageMs,
                static_cast<long long>(m_PacingQueue.count()));

    enqueueFrameForRenderingAndUnlock(frame);
}

bool Pacer::initialize(SDL_Window* window, int maxVideoFps, bool enablePacing, bool allowVsyncSource)
{
    m_MaxVideoFps = maxVideoFps;
    m_DisplayFps = StreamUtils::getDisplayRefreshRate(window);
    m_RendererAttributes = m_VsyncRenderer->getRendererAttributes();

    if (enablePacing) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Frame pacing: target %d Hz with %d FPS stream",
                    m_DisplayFps, m_MaxVideoFps);

        SDL_SysWMinfo info;
        SDL_VERSION(&info.version);
        if (!SDL_GetWindowWMInfo(window, &info)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "SDL_GetWindowWMInfo() failed: %s",
                         SDL_GetError());
            return false;
        }

        switch (info.subsystem) {
    #ifdef Q_OS_WIN32
        case SDL_SYSWM_WINDOWS:
            // Don't use D3DKMTWaitForVerticalBlankEvent() on Windows 7, because
            // it blocks during other concurrent DX operations (like actually rendering).
            if (IsWindows8OrGreater()) {
                m_VsyncSource = new DxVsyncSource(this);
            }
            break;
    #endif

    #if defined(SDL_VIDEO_DRIVER_WAYLAND) && defined(HAS_WAYLAND)
        case SDL_SYSWM_WAYLAND:
            m_VsyncSource = new WaylandVsyncSource(this);
            break;
    #endif

        default:
            // Platforms without a VsyncSource will just render frames
            // immediately like they used to.
            break;
        }

        SDL_assert(m_VsyncSource != nullptr || !(m_RendererAttributes & RENDERER_ATTRIBUTE_FORCE_PACING));

        if (m_VsyncSource != nullptr && !m_VsyncSource->initialize(window, m_DisplayFps)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Vsync source failed to initialize. Frame pacing will not be available!");
            delete m_VsyncSource;
            m_VsyncSource = nullptr;
        }
    }
    else {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Frame pacing disabled: target %d Hz with %d FPS stream",
                    m_DisplayFps, m_MaxVideoFps);
    }

    if (m_VsyncSource != nullptr) {
        m_VsyncThread = SDL_CreateThread(Pacer::vsyncThread, "PacerVsync", this);
    }
    else if (enablePacing && allowVsyncSource) {
        initializeManualPacingState();
    }

    if (m_VsyncRenderer->isRenderThreadSupported()) {
        m_RenderThread = SDL_CreateThread(Pacer::renderThread, "PacerRender", this);
    }

    return true;
}

void Pacer::signalVsync()
{
    m_VsyncSignalled.wakeOne();
}

void Pacer::renderFrame(AVFrame* frame)
{
    // Count time spent in Pacer's queues
    Uint32 beforeRender = SDL_GetTicks();
    m_VideoStats->totalPacerTime += beforeRender - frame->pkt_dts;

    // Render it
    m_VsyncRenderer->renderFrame(frame);
    Uint32 afterRender = SDL_GetTicks();

    m_VideoStats->totalRenderTime += afterRender - beforeRender;
    m_VideoStats->renderedFrames++;
    av_frame_free(&frame);

    // Drop frames if we have too many queued up for a while
    m_FrameQueueLock.lock();

    int frameDropTarget;

    if (m_RendererAttributes & RENDERER_ATTRIBUTE_NO_BUFFERING) {
        // Renderers that don't buffer any frames but don't support waitToRender() need us to buffer
        // an extra frame to ensure they don't starve while waiting to present.
        frameDropTarget = 1;
    }
    else {
        frameDropTarget = 0;
        for (int queueHistoryEntry : m_RenderQueueHistory) {
            if (queueHistoryEntry == 0) {
                // Be lenient as long as the queue length
                // resolves before the end of frame history
                frameDropTarget = 2;
                break;
            }
        }

        // Keep a rolling 500 ms window of render queue history
        if (m_RenderQueueHistory.count() == m_MaxVideoFps / 2) {
            m_RenderQueueHistory.dequeue();
        }

        m_RenderQueueHistory.enqueue(m_RenderQueue.count());
    }

    // Catch up if we're several frames ahead
    while (m_RenderQueue.count() > frameDropTarget) {
        AVFrame* frame = m_RenderQueue.dequeue();

        // Drop the lock while we call av_frame_free()
        m_FrameQueueLock.unlock();
        m_VideoStats->pacerDroppedFrames++;
        av_frame_free(&frame);
        m_FrameQueueLock.lock();
    }

    m_FrameQueueLock.unlock();
}

void Pacer::dropFrameForEnqueue(QQueue<AVFrame*>& queue)
{
    SDL_assert(queue.size() <= MAX_QUEUED_FRAMES);
    if (queue.size() == MAX_QUEUED_FRAMES) {
        AVFrame* frame = queue.dequeue();
        av_frame_free(&frame);
    }
}

void Pacer::submitFrame(AVFrame* frame)
{
    SDL_assert(m_MaxVideoFps != 0);

    static Uint32 lastSubmitLog = 0;
    Uint32 now = SDL_GetTicks();
    if (now - lastSubmitLog > 1000) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Pacer: submitFrame called - queue size=%d",
                    m_PacingQueue.count());
        lastSubmitLog = now;
    }

    // CRITICAL: If waiting for RFI, drop ALL incoming frames immediately
    if (m_WaitingForRFI) {
        SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
                     "Pacer: Dropping frame - waiting for RFI");
        m_VideoStats->pacerDroppedFrames++;
        av_frame_free(&frame);
        return;
    }

    m_FrameQueueLock.lock();
    const bool preferLatestFrame = m_MaxVideoFps >= m_DisplayFps;

    if (m_VsyncSource != nullptr) {
        
        if (preferLatestFrame) {
            // Aggressively keep only the newest frame
            while (m_PacingQueue.count() > 0) {
                AVFrame* staleFrame = m_PacingQueue.dequeue();

                Uint32 now = SDL_GetTicks();
                Uint32 enqueueTicks = static_cast<Uint32>(staleFrame->pkt_dts);
                Uint32 ageMs = now - enqueueTicks;
                SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
                            "Pacer: submit dropping queued frame age=%u ms", ageMs);

                m_FrameQueueLock.unlock();
                m_VideoStats->pacerDroppedFrames++;
                av_frame_free(&staleFrame);
                m_FrameQueueLock.lock();
            }
        }
        else {
            dropFrameForEnqueue(m_PacingQueue);
        }

        m_PacingQueue.enqueue(frame);
        m_FrameQueueLock.unlock();
        m_PacingQueueNotEmpty.wakeOne();
    }
    else {
        if (isManualPacingEnabled()) {
            startManualPacingThreadIfNeeded();

            if (preferLatestFrame) {
                while (m_PacingQueue.count() > 0) {
                    AVFrame* staleFrame = m_PacingQueue.dequeue();

                    Uint32 now = SDL_GetTicks();
                    Uint32 enqueueTicks = static_cast<Uint32>(staleFrame->pkt_dts);
                    Uint32 ageMs = now - enqueueTicks;
                    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
                                "Pacer: submit dropping queued frame age=%u ms", ageMs);

                    m_FrameQueueLock.unlock();
                    m_VideoStats->pacerDroppedFrames++;
                    av_frame_free(&staleFrame);
                    m_FrameQueueLock.lock();
                }
            }
            else {
                dropFrameForEnqueue(m_PacingQueue);
            }

            m_PacingQueue.enqueue(frame);
            m_FrameQueueLock.unlock();
            m_PacingQueueNotEmpty.wakeOne();
        }
        else {
            if (preferLatestFrame) {
                while (m_RenderQueue.count() > 0) {
                    AVFrame* staleFrame = m_RenderQueue.dequeue();

                    Uint32 now = SDL_GetTicks();
                    Uint32 enqueueTicks = static_cast<Uint32>(staleFrame->pkt_dts);
                    Uint32 ageMs = now - enqueueTicks;
                    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
                                "Pacer: submit dropping render frame age=%u ms", ageMs);

                    m_FrameQueueLock.unlock();
                    m_VideoStats->pacerDroppedFrames++;
                    av_frame_free(&staleFrame);
                    m_FrameQueueLock.lock();
                }
            }
            else {
                dropFrameForEnqueue(m_RenderQueue);
            }

            enqueueFrameForRenderingAndUnlock(frame);
        }
    }
}

bool Pacer::isManualPacingEnabled() const
{
    return m_ManualPacingActive;
}

void Pacer::startManualPacingThreadIfNeeded()
{
    if (!m_ManualPacingActive) {
        return;
    }

    if (m_ManualPacingThread == nullptr) {
        m_ManualPacingThread = SDL_CreateThread(Pacer::manualPacingThread, "PacerManual", this);
    }
}

void Pacer::initializeManualPacingState()
{
    Uint64 fps = static_cast<Uint64>(m_DisplayFps != 0 ? m_DisplayFps : m_MaxVideoFps);
    if (fps == 0) {
        fps = 60;
    }

    m_TargetFrameIntervalNs = 1000000000ULL / fps;
    m_ManualMinFrameIntervalNs = m_TargetFrameIntervalNs > m_ManualMaxFrameDeviationNs ?
            m_TargetFrameIntervalNs - m_ManualMaxFrameDeviationNs : m_TargetFrameIntervalNs / 2;

    m_FixedSafetyMarginNs = WARMUP_SAFETY_MARGIN_NS;
    m_ManualStableTimingBaseNs = 0;
    m_ManualStreamStartTimeNs = 0;
    m_ManualLastFrameTimeNs = 0;
    m_ManualLastPresentationTimeNs = 0;
    m_ManualWarmupStartTimeNs = 0;
    m_ManualFrameSequenceNumber = 0;
    m_ManualFrameCount = 0;
    m_ManualWarmupFrameCount = 0;
    m_ManualIntervalIndex = 0;
    m_ManualIsWarmingUp = true;
    m_ManualBurstProtectionActive = false;
    m_ManualRecentFrameIntervals.fill(m_TargetFrameIntervalNs);
    m_ManualPacingActive = true;
}

Uint64 Pacer::getTimeNs() const
{
    Uint64 counter = SDL_GetPerformanceCounter();
    Uint64 frequency = SDL_GetPerformanceFrequency();
    if (frequency == 0) {
        return SDL_GetTicks64() * 1000000ULL;
    }
    return (counter * 1000000000ULL) / frequency;
}

void Pacer::recordManualFrameInterval(Uint64 nowNs)
{
    m_ManualBurstProtectionActive = false;

    if (m_ManualStreamStartTimeNs == 0) {
        m_ManualStreamStartTimeNs = nowNs;
    }

    if (m_ManualIsWarmingUp) {
        if (m_ManualWarmupStartTimeNs == 0) {
            m_ManualWarmupStartTimeNs = nowNs;
        }
        m_ManualWarmupFrameCount++;

        if (m_ManualWarmupFrameCount >= WARMUP_FRAMES &&
                nowNs - m_ManualWarmupStartTimeNs > WARMUP_MIN_DURATION_NS) {
            m_ManualIsWarmingUp = false;
            m_FixedSafetyMarginNs = STABLE_SAFETY_MARGIN_NS;
        }
        else {
            m_FixedSafetyMarginNs = WARMUP_SAFETY_MARGIN_NS;
        }
    }
    else {
        m_FixedSafetyMarginNs = STABLE_SAFETY_MARGIN_NS;
    }

    if (m_ManualLastFrameTimeNs > 0) {
        Uint64 actualInterval = nowNs - m_ManualLastFrameTimeNs;
        m_ManualRecentFrameIntervals[m_ManualIntervalIndex] = actualInterval;
        m_ManualIntervalIndex = (m_ManualIntervalIndex + 1) % m_ManualRecentFrameIntervals.size();
    }

    if (m_ManualWarmupStartTimeNs == 0) {
        m_ManualWarmupStartTimeNs = nowNs;
    }

    m_ManualLastFrameTimeNs = nowNs;
}

Uint64 Pacer::computeManualReleaseTimeNs(Uint64 nowNs)
{
    if (m_TargetFrameIntervalNs == 0) {
        return nowNs;
    }

    Uint64 targetPtsNs;

    if (m_ManualFrameCount == 0 || m_ManualLastPresentationTimeNs == 0) {
        m_ManualFrameSequenceNumber = 1;
        targetPtsNs = nowNs + m_FixedSafetyMarginNs;
    }
    else {
        m_ManualFrameSequenceNumber++;
        targetPtsNs = m_ManualLastPresentationTimeNs + m_TargetFrameIntervalNs;

        Uint64 minimumTarget = nowNs + m_FixedSafetyMarginNs;
        if (targetPtsNs < minimumTarget) {
            targetPtsNs = minimumTarget;
        }
    }

    if (targetPtsNs <= m_ManualLastPresentationTimeNs) {
        targetPtsNs = m_ManualLastPresentationTimeNs + (m_TargetFrameIntervalNs / 2);
    }

    if (m_TargetFrameIntervalNs != 0) {
        Uint64 maximumTarget = nowNs + m_TargetFrameIntervalNs + m_FixedSafetyMarginNs;
        if (targetPtsNs > maximumTarget) {
            targetPtsNs = maximumTarget;
        }
    }

    m_ManualLastPresentationTimeNs = targetPtsNs;
    m_ManualFrameCount++;

    return targetPtsNs;
}

void Pacer::resetManualTimingState(Uint64 nowNs)
{
    m_FixedSafetyMarginNs = WARMUP_SAFETY_MARGIN_NS;
    m_ManualStableTimingBaseNs = 0;
    m_ManualStreamStartTimeNs = nowNs;
    m_ManualLastFrameTimeNs = 0;
    m_ManualLastPresentationTimeNs = 0;
    m_ManualWarmupStartTimeNs = nowNs;
    m_ManualFrameSequenceNumber = 0;
    m_ManualFrameCount = 0;
    m_ManualWarmupFrameCount = 0;
    m_ManualIntervalIndex = 0;
    m_ManualIsWarmingUp = true;
    m_ManualBurstProtectionActive = false;

    const Uint64 seedInterval = (m_TargetFrameIntervalNs != 0) ?
                m_TargetFrameIntervalNs : 0;
    m_ManualRecentFrameIntervals.fill(seedInterval);
}

void Pacer::sleepUntilNs(Uint64 targetNs)
{
    Uint64 startNs = getTimeNs();
    const Uint64 MAX_SLEEP_NS = 100000000ULL; // 100ms max total sleep time
    
    while (!m_Stopping) {
        Uint64 nowNs = getTimeNs();
        
        // Emergency: abort if we've been sleeping too long
        if (nowNs - startNs > MAX_SLEEP_NS) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Pacer: Emergency sleep abort after %lld ms (target was %lld ms in future)",
                        static_cast<long long>((nowNs - startNs) / 1000000ULL),
                        static_cast<long long>((targetNs - startNs) / 1000000ULL));
            break;
        }
        
        if (nowNs >= targetNs) {
            break;
        }

        Uint64 diffNs = targetNs - nowNs;
        
        // Cap individual sleep iteration to prevent infinite waits
        if (diffNs > 50000000ULL) { // Cap at 50ms per iteration
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Pacer: Capping excessive sleep of %lld ms to 50ms",
                        static_cast<long long>(diffNs / 1000000ULL));
            diffNs = 50000000ULL;
        }
        
        if (diffNs > MANUAL_COARSE_SLEEP_THRESHOLD_NS) {
            Uint64 sleepBudgetNs = diffNs - MANUAL_FINE_SLEEP_THRESHOLD_NS;
            Uint32 delayMs = static_cast<Uint32>(sleepBudgetNs / 1000000ULL);
            SDL_Delay(SDL_max(delayMs, static_cast<Uint32>(1)));
        }
        else if (diffNs > MANUAL_FINE_SLEEP_THRESHOLD_NS) {
            SDL_Delay(1);
        }
        else {
            SDL_Delay(0);
        }
    }

    if (!m_Stopping) {
        Uint64 endNs = getTimeNs();
        if (endNs > targetNs + MANUAL_OVERSLEEP_LOG_NS) {
            Uint64 overNs = endNs - targetNs;
            SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
                         "Pacer: manual pacing overslept %lld ms",
                         static_cast<long long>(overNs / 1000000ULL));
        }
    }
}

int Pacer::manualPacingThread(void* context)
{
    Pacer* me = reinterpret_cast<Pacer*>(context);

    if (SDL_SetThreadPriority(SDL_THREAD_PRIORITY_HIGH) < 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Unable to set manual pacing thread priority: %s",
                    SDL_GetError());
    }

    while (!me->m_Stopping) {
        me->m_FrameQueueLock.lock();

        while (!me->m_Stopping && me->m_PacingQueue.isEmpty()) {
            me->m_PacingQueueNotEmpty.wait(&me->m_FrameQueueLock);
        }

        if (me->m_Stopping) {
            me->m_FrameQueueLock.unlock();
            break;
        }

        while (me->m_PacingQueue.count() > 1) {
            AVFrame* staleFrame = me->m_PacingQueue.dequeue();
            Uint32 nowTicks = SDL_GetTicks();
            Uint32 enqueueTicks = static_cast<Uint32>(staleFrame->pkt_dts);
            Uint32 ageMs = nowTicks - enqueueTicks;
            SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
                         "Pacer: manual pre-drop queued frame age=%u ms (queue=%lld)",
                         ageMs,
                         static_cast<long long>(me->m_PacingQueue.count()));
            me->m_VideoStats->pacerDroppedFrames++;
            av_frame_free(&staleFrame);
        }

        AVFrame* frame = me->m_PacingQueue.dequeue();

        me->m_FrameQueueLock.unlock();

        Uint64 nowNs = me->getTimeNs();
        me->recordManualFrameInterval(nowNs);
        Uint64 releaseNs = me->computeManualReleaseTimeNs(nowNs);

        // DIAGNOSTIC: Log planned sleep duration
        Uint64 sleepDurationNs = releaseNs > nowNs ? releaseNs - nowNs : 0;
        if (sleepDurationNs > 50000000ULL) {  // Warn if > 50ms
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Pacer: Planning to sleep for %lld ms (unusually long)",
                        static_cast<long long>(sleepDurationNs / 1000000ULL));
        }

        // Cap how far into the future we can schedule a release. This prevents
        // runaway timing drift from parking the manual pacing thread for long
        // periods, which in turn starves the decoder queue and causes repeated
        // frame drops and IDR requests.
        Uint64 maxAllowedDeltaNs;
        if (me->m_TargetFrameIntervalNs != 0) {
            Uint32 overshootFrames = MANUAL_MAX_OVERSHOOT_FRAMES;
            if (me->m_ManualBurstProtectionActive && overshootFrames > 1) {
                overshootFrames = SDL_max(overshootFrames / 2, 1U);
            }

            maxAllowedDeltaNs = (me->m_TargetFrameIntervalNs * overshootFrames) +
                    me->m_ManualMaxFrameDeviationNs +
                    me->m_FixedSafetyMarginNs;
        }
        else {
            // Fall back to ~30 ms if we don't know the frame interval.
            maxAllowedDeltaNs = 30000000ULL;
        }

        Uint64 nowNs2 = me->getTimeNs();
        Uint64 maxAllowedTargetNs = nowNs2 + maxAllowedDeltaNs;
        if (releaseNs > maxAllowedTargetNs) {
            Uint64 overshootNs = releaseNs - maxAllowedTargetNs;

            bool severeOvershoot;
            if (me->m_TargetFrameIntervalNs != 0) {
                severeOvershoot = overshootNs > (me->m_TargetFrameIntervalNs + me->m_FixedSafetyMarginNs);
            }
            else {
                severeOvershoot = overshootNs > 20000000ULL; // ~20 ms default budget
            }

            if (severeOvershoot) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "Pacer: manual pacing overshoot=%lld ms; dropping queued frames",
                            static_cast<long long>(overshootNs / 1000000ULL));

                me->resetManualTimingState(nowNs2);
                releaseNs = nowNs2 + me->m_FixedSafetyMarginNs;

                me->m_FrameQueueLock.lock();
                while (!me->m_RenderQueue.isEmpty()) {
                    AVFrame* dropped = me->m_RenderQueue.dequeue();
                    me->m_FrameQueueLock.unlock();
                    me->m_VideoStats->pacerDroppedFrames++;
                    av_frame_free(&dropped);
                    me->m_FrameQueueLock.lock();
                }
                me->m_FrameQueueLock.unlock();

                me->m_FrameQueueLock.lock();
                while (!me->m_PacingQueue.isEmpty()) {
                    AVFrame* dropped = me->m_PacingQueue.dequeue();
                    me->m_FrameQueueLock.unlock();
                    me->m_VideoStats->pacerDroppedFrames++;
                    av_frame_free(&dropped);
                    me->m_FrameQueueLock.lock();
                }
                me->m_FrameQueueLock.unlock();
            }
            else {
                SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
                             "Pacer: manual pacing correction=%lld ms",
                             static_cast<long long>(overshootNs / 1000000ULL));
                releaseNs = maxAllowedTargetNs;
            }

            me->m_ManualBurstProtectionActive = false;
            me->m_ManualLastPresentationTimeNs = releaseNs;
        }

        me->sleepUntilNs(releaseNs);

        if (me->m_Stopping) {
            av_frame_free(&frame);
            break;
        }

        me->m_FrameQueueLock.lock();
        Uint32 nowTicks = SDL_GetTicks();
        Uint32 heldMs = nowTicks - static_cast<Uint32>(frame->pkt_dts);
        frame->pkt_dts = nowTicks;

        if (heldMs > MANUAL_STALE_FRAME_THRESHOLD_MS) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Pacer: manual release dropping stale frame age=%u ms",
                        heldMs);
            me->m_FrameQueueLock.unlock();
            me->m_VideoStats->pacerDroppedFrames++;
            av_frame_free(&frame);
            continue;
        }

        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Pacer: manual release frame age=%u ms",
                    heldMs);
        me->enqueueFrameForRenderingAndUnlock(frame);
    }

    return 0;
}

// Add this method to pacer.cpp:
void Pacer::setWaitingForRFI(bool waiting)
{
    if (waiting != m_WaitingForRFI) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Pacer: RFI state changed to %s", waiting ? "WAITING" : "READY");
        
        m_WaitingForRFI = waiting;
        
        // If we're now waiting for RFI, aggressively clear all queues
        if (waiting) {
            m_FrameQueueLock.lock();
            
            int droppedPacing = 0;
            while (!m_PacingQueue.isEmpty()) {
                AVFrame* frame = m_PacingQueue.dequeue();
                av_frame_free(&frame);
                droppedPacing++;
            }
            
            int droppedRender = 0;
            while (!m_RenderQueue.isEmpty()) {
                AVFrame* frame = m_RenderQueue.dequeue();
                av_frame_free(&frame);
                droppedRender++;
            }
            
            m_FrameQueueLock.unlock();
            
            if (droppedPacing > 0 || droppedRender > 0) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "Pacer: RFI wait - dropped %d pacing + %d render frames",
                            droppedPacing, droppedRender);
            }
        }
    }
}