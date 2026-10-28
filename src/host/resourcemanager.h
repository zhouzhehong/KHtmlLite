// KHtmlLite host-side resource manager.
//
// Owns the host process' RAM/GPU accounting and the *soft* dynamic policy that
// the browser shell applies to per-tab renderer processes.  It deliberately
// has no Qt dependency: the implementation is Win32 + DXGI only, so it can be
// unit/smoke-tested without linking the browser.
//
// Division of responsibility (important):
//   * RAM budget   -- soft, sampled from GlobalMemoryStatusEx and the caller's
//                     per-renderer usage list.  Drives trim/suspend advice and
//                     the *value* passed to the Job Object ceiling.
//   * Job Object   -- the only true hard limit: the browser sets
//                     JOB_OBJECT_LIMIT_PROCESS_MEMORY to the hardCeilingBytes
//                     returned here.  Hitting it makes allocations fail; this
//                     module never enforces anything by itself.
//   * GPU budget   -- DXGI IDXGIAdapter3::QueryVideoMemoryInfo.  A completely
//                     separate accounting domain.  GPU bytes are never added
//                     to, or converted into, the CPU/RAM budget/cap.
//
// Threading: one ResourceManager belongs to one thread (normally the browser
// UI thread).  It is not internally synchronised.  Callbacks run synchronously
// inside sample() on that thread; do not re-enter the manager or destroy it
// from a callback -- post to the event loop instead.
//
// C++17 compatible (the host CMake target currently uses C++17), and also
// clean under C++20.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace khtml {
namespace resource {

// Coarse system pressure.  Ordered; Warning > Normal, Critical > Warning.
enum class PressureLevel : std::uint8_t {
    Normal = 0,
    Warning = 1,
    Critical = 2,
};

// Recommended lifecycle action for a background renderer.
enum class BackgroundAction : std::uint8_t {
    Keep = 0,
    Trim = 1,     // purge caches / shrink tile or image caches, keep process
    Suspend = 2,  // tombstone the tab (kill the renderer, keep snapshot/URL)
};

// Availability of one GPU query (segment, adapter, or the whole sample).
enum class GpuStatus : std::uint8_t {
    Unsupported = 0,  // no DXGI adapter3 (Windows < 8.1) / no adapter
    Available = 1,    // QueryVideoMemoryInfo succeeded
    QueryError = 2,   // API present but the query failed (see hresult)
};

const char *toString(PressureLevel level) noexcept;
const char *toString(BackgroundAction action) noexcept;
const char *toString(GpuStatus status) noexcept;

// ---------------------------------------------------------------------------
// Caller-provided renderer usage
// ---------------------------------------------------------------------------

// The browser builds this from its own tab/renderer bookkeeping.  The manager
// never enumerates processes: it aggregates exactly what the caller supplies.
struct RendererUsage {
    // Stable, unique, non-zero id used to correlate Job Object ceiling updates
    // (e.g. reinterpret_cast<std::uint64_t>(TabPage*) or a monotonic tab id).
    std::uint64_t id = 0;
    // Optional diagnostic pid; 0 when unknown.  Never required for accounting.
    std::uint32_t processId = 0;
    // Renderer process working set, from GetProcessMemoryInfo.  Zero for a
    // suspended/tombstoned tab.
    std::uint64_t workingSetBytes = 0;
    // Optional commit (PagefileUsage), 0 = unknown.
    std::uint64_t commitBytes = 0;
    // Exactly one tab should be foreground in a normal browser.
    bool foreground = false;
    // True when the browser has already tombstoned this tab; suspended entries
    // are excluded from aggregation and from Job Object ceiling updates.
    bool suspended = false;
    // How long since this renderer was last foregrounded, in milliseconds.
    // 0 = unknown / not applicable.  Used only by lifecycle advice.
    std::uint64_t idleMs = 0;
};

// ---------------------------------------------------------------------------
// RAM sample
// ---------------------------------------------------------------------------

struct RamSample {
    bool valid = false;             // GlobalMemoryStatusEx succeeded
    std::uint32_t win32Error = 0;   // GetLastError() when !valid
    std::uint64_t totalPhysBytes = 0;
    std::uint64_t availPhysBytes = 0;
    std::uint64_t usedPhysBytes = 0;  // total - available (derived)
    std::uint64_t totalPageFileBytes = 0;
    std::uint64_t availPageFileBytes = 0;
    std::uint32_t memoryLoadPercent = 0;  // dwMemoryLoad, 0..100 system-wide

    bool processMemoryValid = false;  // GetProcessMemoryInfo on browser process
    std::uint64_t browserWorkingSetBytes = 0;
    std::uint64_t browserCommitBytes = 0;  // PagefileUsage

    // Aggregation of RendererUsage entries (suspended entries excluded).
    std::uint64_t rendererWorkingSetBytes = 0;
    std::uint64_t rendererCommitBytes = 0;  // sum of known commitBytes
    std::uint32_t rendererCount = 0;        // entries supplied, incl. suspended
    std::uint32_t foregroundRendererCount = 0;

    // browser + renderers; a statistic, NOT a system-wide total and NOT a cap.
    std::uint64_t trackedWorkingSetBytes = 0;

    // Hysteresis-filtered system RAM pressure for this sample.
    PressureLevel pressure = PressureLevel::Normal;
};

// ---------------------------------------------------------------------------
// GPU sample
// ---------------------------------------------------------------------------

struct GpuMemorySegmentInfo {
    GpuStatus status = GpuStatus::Unsupported;
    std::int32_t hresult = 0;  // meaningful when status != Available

    // DXGI_QUERY_VIDEO_MEMORY_INFO for the requested segment group.
    std::uint64_t budgetBytes = 0;
    std::uint64_t usageBytes = 0;
    // budget - usage, clamped at 0.  Derived, not a second OS reservation.
    std::uint64_t availableBytes = 0;
};

struct GpuAdapterInfo {
    std::uint32_t index = 0;  // EnumAdapters1 index
    std::uint32_t vendorId = 0;
    std::uint32_t deviceId = 0;
    std::uint32_t subsystemId = 0;
    std::uint32_t revision = 0;
    std::uint32_t flags = 0;  // DXGI_ADAPTER_FLAG_*
    std::string name;         // UTF-8 conversion of DXGI_ADAPTER_DESC::Description

    // LUID (packed for cheap comparison, plus the raw halves).  Matches
    // DXGI_ADAPTER_DESC2::AdapterLuid; 0 when the adapter2 query failed.
    std::uint64_t luid = 0;
    std::uint32_t luidLow = 0;
    std::int32_t luidHigh = 0;

    bool software = false;    // DXGI_ADAPTER_FLAG_SOFTWARE (WARP/basic render)
    bool remote = false;      // DXGI_ADAPTER_FLAG_REMOTE, when reported
    // Heuristic only: small/zero dedicated memory plus a larger shared pool.
    // Do not use this as an API contract.
    bool integrated = false;

    // Adapter properties from DXGI_ADAPTER_DESC.  These are *not* budgets:
    // sharedSystemMemoryBytes in particular must never be treated as VRAM.
    std::uint64_t dedicatedVideoMemoryBytes = 0;
    std::uint64_t sharedSystemMemoryBytes = 0;

    // QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL / NON_LOCAL).
    GpuMemorySegmentInfo localMemory;
    GpuMemorySegmentInfo nonLocalMemory;
};

struct GpuSample {
    // Available when at least one adapter answered QueryVideoMemoryInfo for the
    // LOCAL segment.  QueryError when adapter3 exists but every query failed.
    // Unsupported when there is no adapter/factory/adapter3 support.
    GpuStatus status = GpuStatus::Unsupported;
    std::int32_t factoryHresult = 0;     // CreateDXGIFactory1 failure, else 0
    std::int32_t firstErrorHresult = 0;  // first QI/query failure, else 0
    std::uint32_t adapterCount = 0;
    std::vector<GpuAdapterInfo> adapters;

    // Adapter whose LOCAL segment is used for the aggregate numbers.  -1 when
    // nothing was queryable.  Selection is documented in resource_manager.md:
    // explicit preferred adapter, else non-software with the largest dedicated
    // memory, else the first queryable adapter.
    std::int32_t selectedAdapterIndex = -1;

    // LOCAL segment of the selected adapter only.  NEVER includes
    // nonLocalMemory or the adapter's sharedSystemMemory property.
    std::uint64_t vramBudgetBytes = 0;
    std::uint64_t vramUsageBytes = 0;
    std::uint64_t vramAvailableBytes = 0;
    double vramUsageRatio = 0.0;  // usage / budget; 0 when budget is unknown

    // Hysteresis-filtered GPU pressure for this sample.  Normal when no
    // queryable adapter exists -- absence is reported, never invented.
    PressureLevel pressure = PressureLevel::Normal;
};

struct ResourceSnapshot {
    RamSample ram;
    GpuSample gpu;
    // Recommendation for the next sample() call, derived from the worse of
    // RAM/GPU pressure (5000 / 2000 / 1000 ms by default).
    std::uint32_t recommendedSampleIntervalMs = 5000;
};

// ---------------------------------------------------------------------------
// Policy configuration
// ---------------------------------------------------------------------------

// RAM policy.  The 400 MiB baseline is a *reference point* on an 8 GiB
// machine, not a fixed budget: it is scaled by total physical RAM, clamped by
// both an absolute range and a fraction of RAM, then reduced by pressure.
// See resource_manager.md for the formula and worked examples.
struct RamPolicyConfig {
    std::uint64_t baselinePerRendererBytes = 400ull * 1024 * 1024;
    std::uint64_t referenceMachineBytes = 8ull * 1024 * 1024 * 1024;
    double scaleMin = 0.75;  // 4 GiB machine -> 300 MiB baseline
    double scaleMax = 4.0;   // 32 GiB machine -> 1600 MiB baseline
    std::uint64_t minPerRendererBytes = 192ull * 1024 * 1024;
    std::uint64_t maxPerRendererBytes = 2048ull * 1024 * 1024;
    double maxPerRendererRamFraction = 0.10;  // never one tab > 10% of RAM

    // Multipliers applied to the scaled baseline.
    double warningBudgetFactor = 0.60;
    double criticalBudgetFactor = 0.35;
    double foregroundWarningFactor = 0.80;
    double foregroundCriticalFactor = 0.60;

    // System physical-memory load thresholds (dwMemoryLoad).  Defaults match
    // the host's existing watchdog bands; warning is deliberately high.
    std::uint32_t warningLoadPercent = 85;
    std::uint32_t criticalLoadPercent = 93;
    // Downgrade only after load drops this far below the threshold (8-10%).
    // Upgrades are immediate.  E.g. 93 -> Warning at <= 85, Warning -> Normal
    // at <= 77 with the defaults.
    std::uint32_t hysteresisBandPercent = 8;

    // Sampling recommendation; the browser should retime its watchdog to this.
    std::uint32_t normalSampleIntervalMs = 5000;
    std::uint32_t warningSampleIntervalMs = 2000;
    std::uint32_t criticalSampleIntervalMs = 1000;

    // Background lifecycle advice.
    std::uint32_t trimIdleMs = 120000;     // 2 min background -> trim
    std::uint32_t suspendIdleMs = 300000;  // 5 min background -> suspend

    // Job Object commit ceiling = soft budget * jobCeilingFactor, with a floor
    // so the hard backstop never becomes tighter than the renderer can survive.
    double jobCeilingFactor = 2.0;
    std::uint64_t minHardCeilingBytes = 512ull * 1024 * 1024;
    std::uint64_t maxHardCeilingBytes = 4096ull * 1024 * 1024;
};

struct GpuPolicyConfig {
    double warningUsageRatio = 0.80;   // usage / LOCAL budget
    double criticalUsageRatio = 0.92;
    double hysteresisRatio = 0.05;     // downgrade band, upgrades immediate
};

struct RendererPolicy {
    bool foreground = false;
    PressureLevel pressure = PressureLevel::Normal;

    // Soft per-renderer working-set target.  Advisory: the browser uses it for
    // trim/suspend decisions, not as an OS-enforced limit.
    std::uint64_t softBudgetBytes = 0;

    // Value the browser should pass to its Job Object (commit cap, bytes).
    // This is the only true hard limit.  0 = leave the ceiling unchanged.
    std::uint64_t hardCeilingBytes = 0;

    bool purgeCache = false;
    bool allowSuspend = true;  // false for foreground renderers
    BackgroundAction backgroundAction = BackgroundAction::Keep;
    std::uint32_t sampleIntervalMs = 5000;
};

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------

struct GpuPressureEvent {
    PressureLevel previous = PressureLevel::Normal;
    PressureLevel current = PressureLevel::Normal;
    std::int32_t adapterIndex = -1;
    std::uint64_t budgetBytes = 0;
    std::uint64_t usageBytes = 0;
    double usageRatio = 0.0;
    std::int32_t hresult = 0;  // 0 on a successful query
};

// Bytes are commit bytes, to mirror JOB_OBJECT_LIMIT_PROCESS_MEMORY.
using JobCeilingCallback =
    std::function<void(std::uint64_t rendererId, std::uint64_t commitCeilingBytes)>;
using GpuPressureCallback = std::function<void(const GpuPressureEvent &event)>;
using RamPressureCallback =
    std::function<void(PressureLevel previous, PressureLevel current, const RamSample &ram)>;

// ---------------------------------------------------------------------------
// ResourceManager
// ---------------------------------------------------------------------------

class ResourceManager {
public:
    ResourceManager();
    ~ResourceManager();

    ResourceManager(const ResourceManager &) = delete;
    ResourceManager &operator=(const ResourceManager &) = delete;
    ResourceManager(ResourceManager &&) = delete;
    ResourceManager &operator=(ResourceManager &&) = delete;

    // Replace the caller's renderer usage list.  Call before sample() each
    // watchdog tick.  Entries marked suspended are excluded from aggregation.
    void updateRendererUsage(std::vector<RendererUsage> usage);
    const std::vector<RendererUsage> &rendererUsage() const noexcept;

    // Sample system RAM, the browser process, aggregate the supplied renderer
    // usage, enumerate DXGI adapters and query LOCAL/NON_LOCAL video memory.
    // Applies hysteresis, fires pressure callbacks on level changes, and calls
    // the Job ceiling callback when a renderer's hard ceiling changes.
    // Returns a copy of the latest snapshot.
    ResourceSnapshot sample();
    const ResourceSnapshot &latest() const noexcept;

    // Dynamic policy for one renderer class.  Uses the latest sample; call
    // sample() at least once before relying on RAM-scaled budgets (otherwise a
    // conservative 400 MiB reference baseline is used).
    RendererPolicy policyFor(bool foreground) const;
    RendererPolicy policyFor(const RendererUsage &usage) const;

    // Per-tab background lifecycle recommendation, incorporating pressure,
    // idle time and whether the tab already exceeds its soft budget.
    BackgroundAction adviseBackground(const RendererUsage &usage) const;
    bool shouldSuspend(const RendererUsage &usage) const;

    // Combined RAM/GPU sampling cadence recommendation.
    std::uint32_t recommendedSampleIntervalMs() const noexcept;

    // The browser maps rendererId -> TabPage*/RenderTab and calls
    // setMemoryCeilingKB(bytes / 1024) there.  Fired only when the value
    // changes, and never for suspended/removed entries.
    void setJobCeilingCallback(JobCeilingCallback callback);

    // Diagnostics/telemetry.  Never use GPU bytes to change a CPU/RAM cap.
    void setGpuPressureCallback(GpuPressureCallback callback);
    // Optional early-warning hook for cache purges/eviction.
    void setRamPressureCallback(RamPressureCallback callback);

    // Setters sanitise out-of-range values (min <= max, warning < critical,
    // sensible bands/factors) rather than rejecting silently.
    void setRamPolicyConfig(const RamPolicyConfig &config);
    const RamPolicyConfig &ramPolicyConfig() const noexcept;
    void setGpuPolicyConfig(const GpuPolicyConfig &config);
    const GpuPolicyConfig &gpuPolicyConfig() const noexcept;

    // Adapter used for aggregate VRAM numbers; -1 = automatic heuristic.
    // Useful when the compositor already knows which LUID it renders with.
    void setPreferredGpuAdapter(std::int32_t adapterIndex);
    std::int32_t preferredGpuAdapter() const noexcept;

    // Clear usage/samples/pressure state and ceiling memo.  Keeps configuration
    // and callbacks; releases the cached DXGI factory.
    void reset();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace resource
}  // namespace khtml
