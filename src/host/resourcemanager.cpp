// KHtmlLite host-side resource manager implementation.
//
// Win32 + DXGI only; no Qt.  See resourcemanager.h for the contract and
// docs/resource_manager.md for integration notes, policy formulas and limits.

#include "resourcemanager.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <unordered_map>
#include <utility>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#include <dxgi1_4.h>
#endif

namespace khtml {
namespace resource {

namespace {

constexpr std::uint64_t kMiB = 1024ull * 1024ull;
constexpr std::uint32_t kMaxAdapters = 16;  // enumeration safety cap

double clampDouble(double value, double low, double high)
{
    if (value < low)
        return low;
    if (value > high)
        return high;
    return value;
}

std::uint64_t saturatingAdd(std::uint64_t a, std::uint64_t b)
{
    const std::uint64_t max = std::numeric_limits<std::uint64_t>::max();
    return (a > max - b) ? max : a + b;
}

int pressureRank(PressureLevel level)
{
    switch (level) {
    case PressureLevel::Normal:
        return 0;
    case PressureLevel::Warning:
        return 1;
    case PressureLevel::Critical:
        return 2;
    }
    return 0;
}

// RAM: immediate upgrade, banded downgrade.  Example with 85/93/8:
//   Critical -> Warning at load <= 85; Critical -> Normal at load <= 77;
//   Warning  -> Normal  at load <= 77.
PressureLevel applyRamHysteresis(PressureLevel current, std::uint32_t load,
                                 const RamPolicyConfig &cfg)
{
    const int warning = static_cast<int>(cfg.warningLoadPercent);
    const int critical = static_cast<int>(cfg.criticalLoadPercent);
    const int band = static_cast<int>(cfg.hysteresisBandPercent);
    const int loadValue = static_cast<int>(std::min<std::uint32_t>(load, 100));

    PressureLevel raw = PressureLevel::Normal;
    if (loadValue >= critical)
        raw = PressureLevel::Critical;
    else if (loadValue >= warning)
        raw = PressureLevel::Warning;

    if (pressureRank(raw) >= pressureRank(current))
        return raw;  // upgrades (and same level) are immediate

    if (current == PressureLevel::Critical) {
        if (loadValue <= warning - band)
            return PressureLevel::Normal;
        if (loadValue <= critical - band)
            return PressureLevel::Warning;
        return PressureLevel::Critical;
    }
    if (current == PressureLevel::Warning && loadValue <= warning - band)
        return PressureLevel::Normal;
    return current;
}

PressureLevel classifyGpuRatio(double ratio, const GpuPolicyConfig &cfg)
{
    if (ratio >= cfg.criticalUsageRatio)
        return PressureLevel::Critical;
    if (ratio >= cfg.warningUsageRatio)
        return PressureLevel::Warning;
    return PressureLevel::Normal;
}

PressureLevel applyGpuHysteresis(PressureLevel current, double ratio,
                                 const GpuPolicyConfig &cfg)
{
    const PressureLevel raw = classifyGpuRatio(ratio, cfg);
    if (pressureRank(raw) >= pressureRank(current))
        return raw;

    if (current == PressureLevel::Critical) {
        if (ratio <= cfg.warningUsageRatio - cfg.hysteresisRatio)
            return PressureLevel::Normal;
        if (ratio <= cfg.criticalUsageRatio - cfg.hysteresisRatio)
            return PressureLevel::Warning;
        return PressureLevel::Critical;
    }
    if (current == PressureLevel::Warning
        && ratio <= cfg.warningUsageRatio - cfg.hysteresisRatio) {
        return PressureLevel::Normal;
    }
    return current;
}

std::uint32_t intervalFor(const ResourceSnapshot &snapshot, const RamPolicyConfig &cfg)
{
    const PressureLevel worst = pressureRank(snapshot.ram.pressure)
                                        >= pressureRank(snapshot.gpu.pressure)
                                    ? snapshot.ram.pressure
                                    : snapshot.gpu.pressure;
    switch (worst) {
    case PressureLevel::Critical:
        return cfg.criticalSampleIntervalMs;
    case PressureLevel::Warning:
        return cfg.warningSampleIntervalMs;
    case PressureLevel::Normal:
        break;
    }
    return cfg.normalSampleIntervalMs;
}

RamPolicyConfig sanitizeRamConfig(RamPolicyConfig cfg)
{
    cfg.baselinePerRendererBytes =
        std::max<std::uint64_t>(cfg.baselinePerRendererBytes, 32ull * kMiB);
    cfg.referenceMachineBytes =
        std::max<std::uint64_t>(cfg.referenceMachineBytes, 512ull * kMiB);
    cfg.scaleMin = clampDouble(cfg.scaleMin, 0.10, 16.0);
    cfg.scaleMax = clampDouble(cfg.scaleMax, cfg.scaleMin, 16.0);
    cfg.minPerRendererBytes =
        std::max<std::uint64_t>(cfg.minPerRendererBytes, 32ull * kMiB);
    cfg.maxPerRendererBytes =
        std::max<std::uint64_t>(cfg.maxPerRendererBytes, cfg.minPerRendererBytes);
    cfg.maxPerRendererRamFraction =
        clampDouble(cfg.maxPerRendererRamFraction, 0.01, 1.0);

    cfg.warningBudgetFactor = clampDouble(cfg.warningBudgetFactor, 0.05, 1.0);
    cfg.criticalBudgetFactor = clampDouble(cfg.criticalBudgetFactor, 0.05, 1.0);
    if (cfg.criticalBudgetFactor > cfg.warningBudgetFactor)
        cfg.criticalBudgetFactor = cfg.warningBudgetFactor;
    cfg.foregroundWarningFactor =
        clampDouble(cfg.foregroundWarningFactor, cfg.warningBudgetFactor, 1.0);
    cfg.foregroundCriticalFactor =
        clampDouble(cfg.foregroundCriticalFactor, cfg.criticalBudgetFactor, 1.0);
    if (cfg.foregroundCriticalFactor > cfg.foregroundWarningFactor)
        cfg.foregroundCriticalFactor = cfg.foregroundWarningFactor;

    cfg.warningLoadPercent =
        std::min<std::uint32_t>(std::max<std::uint32_t>(cfg.warningLoadPercent, 1), 98);
    cfg.criticalLoadPercent =
        std::min<std::uint32_t>(std::max<std::uint32_t>(cfg.criticalLoadPercent,
                                                        cfg.warningLoadPercent + 1),
                                99);
    cfg.hysteresisBandPercent =
        std::min<std::uint32_t>(cfg.hysteresisBandPercent, 50);
    cfg.hysteresisBandPercent =
        std::min<std::uint32_t>(cfg.hysteresisBandPercent, cfg.warningLoadPercent);

    cfg.normalSampleIntervalMs =
        std::min<std::uint32_t>(std::max<std::uint32_t>(cfg.normalSampleIntervalMs, 250),
                                60000);
    cfg.warningSampleIntervalMs =
        std::min<std::uint32_t>(std::max<std::uint32_t>(cfg.warningSampleIntervalMs, 250),
                                60000);
    cfg.criticalSampleIntervalMs =
        std::min<std::uint32_t>(std::max<std::uint32_t>(cfg.criticalSampleIntervalMs, 250),
                                60000);
    if (cfg.warningSampleIntervalMs > cfg.normalSampleIntervalMs)
        cfg.warningSampleIntervalMs = cfg.normalSampleIntervalMs;
    if (cfg.criticalSampleIntervalMs > cfg.warningSampleIntervalMs)
        cfg.criticalSampleIntervalMs = cfg.warningSampleIntervalMs;

    cfg.trimIdleMs = std::min<std::uint32_t>(cfg.trimIdleMs, 3600000);
    cfg.suspendIdleMs = std::min<std::uint32_t>(cfg.suspendIdleMs, 3600000);
    if (cfg.suspendIdleMs > 0 && cfg.trimIdleMs > cfg.suspendIdleMs)
        cfg.trimIdleMs = cfg.suspendIdleMs;

    cfg.jobCeilingFactor = clampDouble(cfg.jobCeilingFactor, 1.0, 8.0);
    cfg.minHardCeilingBytes =
        std::max<std::uint64_t>(cfg.minHardCeilingBytes, 64ull * kMiB);
    cfg.maxHardCeilingBytes =
        std::max<std::uint64_t>(cfg.maxHardCeilingBytes, cfg.minHardCeilingBytes);
    return cfg;
}

GpuPolicyConfig sanitizeGpuConfig(GpuPolicyConfig cfg)
{
    cfg.warningUsageRatio = clampDouble(cfg.warningUsageRatio, 0.05, 0.99);
    cfg.criticalUsageRatio = clampDouble(cfg.criticalUsageRatio,
                                         cfg.warningUsageRatio + 0.01, 1.0);
    if (cfg.criticalUsageRatio <= cfg.warningUsageRatio)
        cfg.criticalUsageRatio = std::min(1.0, cfg.warningUsageRatio + 0.01);
    cfg.hysteresisRatio = clampDouble(cfg.hysteresisRatio, 0.0, 0.50);
    cfg.hysteresisRatio = std::min(cfg.hysteresisRatio, cfg.warningUsageRatio);
    return cfg;
}

// The RAM policy.  GPU pressure is intentionally not an input here: GPU
// pressure must never change a CPU/RAM budget or the Job Object ceiling.
RendererPolicy makeRendererPolicy(const RamPolicyConfig &cfg,
                                  const ResourceSnapshot &snapshot, bool foreground,
                                  std::uint32_t sampleIntervalMs)
{
    RendererPolicy policy;
    policy.foreground = foreground;
    policy.pressure = snapshot.ram.pressure;
    policy.sampleIntervalMs = sampleIntervalMs;

    const double totalPhys = static_cast<double>(snapshot.ram.totalPhysBytes);
    double base = static_cast<double>(cfg.baselinePerRendererBytes);
    if (totalPhys > 0.0) {
        const double scale = clampDouble(totalPhys / static_cast<double>(cfg.referenceMachineBytes),
                                         cfg.scaleMin, cfg.scaleMax);
        base *= scale;

        const double fractionCap = totalPhys * cfg.maxPerRendererRamFraction;
        if (fractionCap > 0.0)
            base = std::min(base, fractionCap);
    }
    base = std::max(base, static_cast<double>(cfg.minPerRendererBytes));
    base = std::min(base, static_cast<double>(cfg.maxPerRendererBytes));

    double factor = 1.0;
    if (policy.pressure == PressureLevel::Warning) {
        factor = foreground ? cfg.foregroundWarningFactor : cfg.warningBudgetFactor;
    } else if (policy.pressure == PressureLevel::Critical) {
        factor = foreground ? cfg.foregroundCriticalFactor : cfg.criticalBudgetFactor;
    }

    double soft = base * factor;
    soft = std::max(soft, static_cast<double>(cfg.minPerRendererBytes));
    soft = std::min(soft, static_cast<double>(cfg.maxPerRendererBytes));
    policy.softBudgetBytes = static_cast<std::uint64_t>(std::llround(soft));

    // The Job Object is the only hard limit.  It is deliberately looser than
    // the soft budget (2x by default) so the backstop catches runaway commit,
    // not normal operation; the floor keeps heavy pages from failing ordinary
    // allocations while the system is under pressure.
    double hard = soft * cfg.jobCeilingFactor;
    hard = std::max(hard, static_cast<double>(cfg.minHardCeilingBytes));
    hard = std::min(hard, static_cast<double>(cfg.maxHardCeilingBytes));
    hard = std::max(hard, soft);
    policy.hardCeilingBytes = static_cast<std::uint64_t>(std::llround(hard));

    policy.purgeCache = policy.pressure != PressureLevel::Normal;
    policy.allowSuspend = !foreground;
    if (foreground) {
        policy.backgroundAction = BackgroundAction::Keep;
    } else if (policy.pressure == PressureLevel::Critical) {
        policy.backgroundAction = BackgroundAction::Suspend;
    } else if (policy.pressure == PressureLevel::Warning) {
        policy.backgroundAction = BackgroundAction::Trim;
    } else {
        policy.backgroundAction = BackgroundAction::Keep;
    }
    return policy;
}

BackgroundAction adviseBackgroundAction(const RamPolicyConfig &cfg,
                                        const RendererPolicy &policy,
                                        const RendererUsage &usage)
{
    if (usage.foreground || usage.suspended)
        return BackgroundAction::Keep;

    if (policy.pressure == PressureLevel::Critical)
        return BackgroundAction::Suspend;

    const bool overSoftBudget = policy.softBudgetBytes > 0
                                && usage.workingSetBytes > policy.softBudgetBytes;
    const bool idleTooLong = usage.idleMs > 0 && usage.idleMs >= cfg.suspendIdleMs;

    if (policy.pressure == PressureLevel::Warning) {
        if (idleTooLong || overSoftBudget)
            return BackgroundAction::Suspend;
        return BackgroundAction::Trim;
    }

    // Normal: keep the tab unless it is stale or already over the soft target;
    // these are recommendations, the browser can keep its own idle policy.
    if (idleTooLong)
        return BackgroundAction::Suspend;
    if (overSoftBudget)
        return BackgroundAction::Trim;
    return BackgroundAction::Keep;
}

#if defined(_WIN32)

std::string utf8FromWide(const wchar_t *wide)
{
    if (!wide || wide[0] == L'\0')
        return std::string();
    const int needed = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1)
        return std::string();
    std::string out(static_cast<std::size_t>(needed), '\0');
    const int written = WideCharToMultiByte(CP_UTF8, 0, wide, -1, &out[0], needed,
                                            nullptr, nullptr);
    if (written <= 0)
        return std::string();
    out.resize(static_cast<std::size_t>(written - 1));
    return out;
}

std::uint64_t packLuid(const LUID &luid)
{
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(luid.HighPart)) << 32)
           | static_cast<std::uint64_t>(luid.LowPart);
}

bool isIntegratedHeuristic(const GpuAdapterInfo &info)
{
    if (info.software)
        return false;
    // Discrete adapters have large dedicated VRAM and a shared pool of roughly
    // half of system RAM.  Integrated adapters report a small dedicated slice
    // (often 128 MiB) and/or no dedicated memory at all.  This is only a hint
    // for display/diagnostics; budgets always come from QueryVideoMemoryInfo.
    if (info.dedicatedVideoMemoryBytes == 0)
        return true;
    return info.dedicatedVideoMemoryBytes <= 512ull * kMiB
           && info.sharedSystemMemoryBytes > info.dedicatedVideoMemoryBytes;
}

#endif  // _WIN32

}  // namespace

const char *toString(PressureLevel level) noexcept
{
    switch (level) {
    case PressureLevel::Normal:
        return "normal";
    case PressureLevel::Warning:
        return "warning";
    case PressureLevel::Critical:
        return "critical";
    }
    return "unknown";
}

const char *toString(BackgroundAction action) noexcept
{
    switch (action) {
    case BackgroundAction::Keep:
        return "keep";
    case BackgroundAction::Trim:
        return "trim";
    case BackgroundAction::Suspend:
        return "suspend";
    }
    return "unknown";
}

const char *toString(GpuStatus status) noexcept
{
    switch (status) {
    case GpuStatus::Unsupported:
        return "unsupported";
    case GpuStatus::Available:
        return "available";
    case GpuStatus::QueryError:
        return "query-error";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// Implementation state
// ---------------------------------------------------------------------------

struct ResourceManager::Impl {
    std::vector<RendererUsage> usage;
    RamPolicyConfig ramConfig;
    GpuPolicyConfig gpuConfig;
    ResourceSnapshot latest;
    PressureLevel ramPressure = PressureLevel::Normal;
    PressureLevel gpuPressure = PressureLevel::Normal;
    std::int32_t preferredAdapter = -1;
    JobCeilingCallback jobCallback;
    GpuPressureCallback gpuCallback;
    RamPressureCallback ramCallback;
    std::unordered_map<std::uint64_t, std::uint64_t> appliedHardCeilings;

#if defined(_WIN32)
    IDXGIFactory1 *factory = nullptr;
#endif

    RamSample sampleRam() const;
    GpuSample sampleGpu();
    void releaseFactory();
};

RamSample ResourceManager::Impl::sampleRam() const
{
    RamSample sample;

    for (const RendererUsage &entry : usage) {
        ++sample.rendererCount;
        if (entry.foreground)
            ++sample.foregroundRendererCount;
        if (entry.suspended)
            continue;  // tombstoned: no process, no working set
        sample.rendererWorkingSetBytes =
            saturatingAdd(sample.rendererWorkingSetBytes, entry.workingSetBytes);
        sample.rendererCommitBytes =
            saturatingAdd(sample.rendererCommitBytes, entry.commitBytes);
    }

#if defined(_WIN32)
    MEMORYSTATUSEX mem = {};
    mem.dwLength = sizeof(mem);
    if (GlobalMemoryStatusEx(&mem)) {
        sample.valid = true;
        sample.totalPhysBytes = mem.ullTotalPhys;
        sample.availPhysBytes = mem.ullAvailPhys;
        sample.usedPhysBytes =
            mem.ullTotalPhys > mem.ullAvailPhys ? mem.ullTotalPhys - mem.ullAvailPhys : 0;
        sample.totalPageFileBytes = mem.ullTotalPageFile;
        sample.availPageFileBytes = mem.ullAvailPageFile;
        sample.memoryLoadPercent = std::min<std::uint32_t>(mem.dwMemoryLoad, 100);
    } else {
        sample.win32Error = static_cast<std::uint32_t>(GetLastError());
    }

    PROCESS_MEMORY_COUNTERS counters = {};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) {
        sample.processMemoryValid = true;
        sample.browserWorkingSetBytes = static_cast<std::uint64_t>(counters.WorkingSetSize);
        sample.browserCommitBytes = static_cast<std::uint64_t>(counters.PagefileUsage);
    }
#else
    // Non-Windows builds are for syntax/policy tests only; there is no system
    // RAM sampler here.  valid stays false and callers fall back to the
    // documented baseline instead of inventing numbers.
    sample.win32Error = 0;
#endif

    sample.trackedWorkingSetBytes =
        saturatingAdd(sample.browserWorkingSetBytes, sample.rendererWorkingSetBytes);
    return sample;
}

void ResourceManager::Impl::releaseFactory()
{
#if defined(_WIN32)
    if (factory) {
        factory->Release();
        factory = nullptr;
    }
#endif
}

GpuSample ResourceManager::Impl::sampleGpu()
{
    GpuSample sample;

#if !defined(_WIN32)
    return sample;  // unsupported, no fake budgets
#else
    if (!factory) {
        IDXGIFactory1 *created = nullptr;
        const HRESULT hr = CreateDXGIFactory1(IID_IDXGIFactory1,
                                              reinterpret_cast<void **>(&created));
        if (FAILED(hr) || !created) {
            sample.factoryHresult = static_cast<std::int32_t>(hr);
            return sample;
        }
        factory = created;
    }

    bool sawQuery = false;
    bool anyLocalAvailable = false;

    for (std::uint32_t index = 0; index < kMaxAdapters; ++index) {
        IDXGIAdapter1 *adapter = nullptr;
        HRESULT hr = factory->EnumAdapters1(static_cast<UINT>(index), &adapter);
        if (hr == DXGI_ERROR_NOT_FOUND)
            break;
        if (FAILED(hr) || !adapter) {
            if (sample.firstErrorHresult == 0)
                sample.firstErrorHresult = static_cast<std::int32_t>(hr);
            break;
        }

        GpuAdapterInfo info;
        info.index = index;

        DXGI_ADAPTER_DESC desc = {};
        if (SUCCEEDED(adapter->GetDesc(&desc))) {
            info.vendorId = desc.VendorId;
            info.deviceId = desc.DeviceId;
            info.subsystemId = desc.SubSysId;
            info.revision = desc.Revision;
            info.dedicatedVideoMemoryBytes =
                static_cast<std::uint64_t>(desc.DedicatedVideoMemory);
            info.sharedSystemMemoryBytes =
                static_cast<std::uint64_t>(desc.SharedSystemMemory);
            info.name = utf8FromWide(desc.Description);
        }

        DXGI_ADAPTER_DESC1 desc1 = {};
        if (SUCCEEDED(adapter->GetDesc1(&desc1))) {
            info.flags = static_cast<std::uint32_t>(desc1.Flags);
            info.software = (desc1.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
            info.remote = (desc1.Flags & DXGI_ADAPTER_FLAG_REMOTE) != 0;
        }

        IDXGIAdapter2 *adapter2 = nullptr;
        if (SUCCEEDED(adapter->QueryInterface(IID_IDXGIAdapter2,
                                              reinterpret_cast<void **>(&adapter2)))
            && adapter2) {
            DXGI_ADAPTER_DESC2 desc2 = {};
            if (SUCCEEDED(adapter2->GetDesc2(&desc2))) {
                info.luidLow = static_cast<std::uint32_t>(desc2.AdapterLuid.LowPart);
                info.luidHigh = static_cast<std::int32_t>(desc2.AdapterLuid.HighPart);
                info.luid = packLuid(desc2.AdapterLuid);
            }
            adapter2->Release();
        }

        info.integrated = isIntegratedHeuristic(info);

        IDXGIAdapter3 *adapter3 = nullptr;
        hr = adapter->QueryInterface(IID_IDXGIAdapter3,
                                     reinterpret_cast<void **>(&adapter3));
        if (FAILED(hr) || !adapter3) {
            // Windows < 8.1 / DXGI < 1.3: no IDXGIAdapter3, hence no
            // video-memory budget API.  This is an honest unsupported result,
            // not a zero budget.
            info.localMemory.status = GpuStatus::Unsupported;
            info.localMemory.hresult = static_cast<std::int32_t>(hr);
            info.nonLocalMemory.status = GpuStatus::Unsupported;
            info.nonLocalMemory.hresult = static_cast<std::int32_t>(hr);
            if (sample.firstErrorHresult == 0)
                sample.firstErrorHresult = static_cast<std::int32_t>(hr);
        } else {
            auto querySegment = [&](DXGI_MEMORY_SEGMENT_GROUP group,
                                    GpuMemorySegmentInfo &segment) {
                DXGI_QUERY_VIDEO_MEMORY_INFO query = {};
                const HRESULT queryHr = adapter3->QueryVideoMemoryInfo(0, group, &query);
                sawQuery = true;
                if (SUCCEEDED(queryHr)) {
                    segment.status = GpuStatus::Available;
                    segment.hresult = 0;
                    segment.budgetBytes = static_cast<std::uint64_t>(query.Budget);
                    segment.usageBytes = static_cast<std::uint64_t>(query.CurrentUsage);
                    segment.availableBytes = segment.budgetBytes > segment.usageBytes
                                                 ? segment.budgetBytes - segment.usageBytes
                                                 : 0;
                } else {
                    segment.status = GpuStatus::QueryError;
                    segment.hresult = static_cast<std::int32_t>(queryHr);
                    if (sample.firstErrorHresult == 0)
                        sample.firstErrorHresult = static_cast<std::int32_t>(queryHr);
                }
            };

            // Both groups are queried and reported independently.  Only LOCAL
            // is used for the aggregate VRAM figures; NON_LOCAL is diagnostic
            // and may legitimately be zero on integrated adapters.
            querySegment(DXGI_MEMORY_SEGMENT_GROUP_LOCAL, info.localMemory);
            querySegment(DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, info.nonLocalMemory);
            if (info.localMemory.status == GpuStatus::Available)
                anyLocalAvailable = true;

            adapter3->Release();
        }

        sample.adapters.push_back(std::move(info));
        adapter->Release();
    }

    sample.adapterCount = static_cast<std::uint32_t>(sample.adapters.size());

    if (anyLocalAvailable) {
        sample.status = GpuStatus::Available;
    } else if (sawQuery) {
        sample.status = GpuStatus::QueryError;
    } else {
        // No adapters, factory created no adapters, or no adapter3 anywhere.
        sample.status = GpuStatus::Unsupported;
    }

    // --- Selection: explicit preference, then the discrete-looking adapter
    // with the most dedicated memory, then any queryable adapter.  The
    // aggregate deliberately uses only the selected adapter's LOCAL segment.
    std::int32_t selected = -1;
    if (preferredAdapter >= 0
        && static_cast<std::size_t>(preferredAdapter) < sample.adapters.size()
        && sample.adapters[static_cast<std::size_t>(preferredAdapter)].localMemory.status
               == GpuStatus::Available) {
        selected = preferredAdapter;
    } else {
        for (std::size_t i = 0; i < sample.adapters.size(); ++i) {
            const GpuAdapterInfo &candidate = sample.adapters[i];
            if (candidate.localMemory.status != GpuStatus::Available || candidate.software)
                continue;
            if (selected < 0) {
                selected = static_cast<std::int32_t>(i);
                continue;
            }
            const GpuAdapterInfo &best = sample.adapters[static_cast<std::size_t>(selected)];
            if (candidate.dedicatedVideoMemoryBytes > best.dedicatedVideoMemoryBytes
                || (candidate.dedicatedVideoMemoryBytes == best.dedicatedVideoMemoryBytes
                    && candidate.localMemory.budgetBytes > best.localMemory.budgetBytes)) {
                selected = static_cast<std::int32_t>(i);
            }
        }
        if (selected < 0) {
            for (std::size_t i = 0; i < sample.adapters.size(); ++i) {
                if (sample.adapters[i].localMemory.status == GpuStatus::Available) {
                    selected = static_cast<std::int32_t>(i);
                    break;
                }
            }
        }
    }

    sample.selectedAdapterIndex = selected;
    if (selected >= 0) {
        const GpuAdapterInfo &info = sample.adapters[static_cast<std::size_t>(selected)];
        sample.vramBudgetBytes = info.localMemory.budgetBytes;
        sample.vramUsageBytes = info.localMemory.usageBytes;
        sample.vramAvailableBytes = info.localMemory.availableBytes;
        if (sample.vramBudgetBytes > 0) {
            sample.vramUsageRatio = static_cast<double>(sample.vramUsageBytes)
                                    / static_cast<double>(sample.vramBudgetBytes);
        }
    }

    return sample;
#endif  // _WIN32
}

// ---------------------------------------------------------------------------
// ResourceManager
// ---------------------------------------------------------------------------

ResourceManager::ResourceManager() : m_impl(std::make_unique<Impl>()) {}

ResourceManager::~ResourceManager()
{
    if (m_impl)
        m_impl->releaseFactory();
}

void ResourceManager::updateRendererUsage(std::vector<RendererUsage> usage)
{
    m_impl->usage = std::move(usage);
}

const std::vector<RendererUsage> &ResourceManager::rendererUsage() const noexcept
{
    return m_impl->usage;
}

ResourceSnapshot ResourceManager::sample()
{
    ResourceSnapshot snapshot;

    snapshot.ram = m_impl->sampleRam();
    const PressureLevel previousRam = m_impl->ramPressure;
    if (snapshot.ram.valid) {
        m_impl->ramPressure = applyRamHysteresis(previousRam,
                                                 snapshot.ram.memoryLoadPercent,
                                                 m_impl->ramConfig);
    }
    snapshot.ram.pressure = m_impl->ramPressure;

    snapshot.gpu = m_impl->sampleGpu();
    const PressureLevel previousGpu = m_impl->gpuPressure;
    double ratio = 0.0;
    if (snapshot.gpu.status == GpuStatus::Available
        && snapshot.gpu.vramBudgetBytes > 0) {
        ratio = snapshot.gpu.vramUsageRatio;
    }
    m_impl->gpuPressure = applyGpuHysteresis(previousGpu, ratio, m_impl->gpuConfig);
    snapshot.gpu.pressure = m_impl->gpuPressure;

    snapshot.recommendedSampleIntervalMs = intervalFor(snapshot, m_impl->ramConfig);
    m_impl->latest = snapshot;

    // Plan Job Object ceiling updates before invoking any callback.  Suspended
    // entries lose their memo so a resumed tab gets its ceiling re-applied to
    // the fresh job object; ids that disappeared are forgotten.
    const JobCeilingCallback jobCallback = m_impl->jobCallback;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> ceilingUpdates;
    if (jobCallback) {
        for (const RendererUsage &entry : m_impl->usage) {
            if (entry.suspended) {
                m_impl->appliedHardCeilings.erase(entry.id);
                continue;
            }
            const RendererPolicy policy = policyFor(entry.foreground);
            if (policy.hardCeilingBytes == 0)
                continue;
            auto it = m_impl->appliedHardCeilings.find(entry.id);
            if (it == m_impl->appliedHardCeilings.end()
                || it->second != policy.hardCeilingBytes) {
                ceilingUpdates.emplace_back(entry.id, policy.hardCeilingBytes);
            }
        }
        for (auto it = m_impl->appliedHardCeilings.begin();
             it != m_impl->appliedHardCeilings.end();) {
            bool stillPresent = false;
            for (const RendererUsage &entry : m_impl->usage) {
                if (entry.id == it->first) {
                    stillPresent = true;
                    break;
                }
            }
            if (stillPresent)
                ++it;
            else
                it = m_impl->appliedHardCeilings.erase(it);
        }
        for (const auto &update : ceilingUpdates)
            m_impl->appliedHardCeilings[update.first] = update.second;
    } else {
        m_impl->appliedHardCeilings.clear();
    }

    if (previousRam != m_impl->ramPressure && m_impl->ramCallback)
        m_impl->ramCallback(previousRam, m_impl->ramPressure, m_impl->latest.ram);

    if (previousGpu != m_impl->gpuPressure && m_impl->gpuCallback) {
        GpuPressureEvent event;
        event.previous = previousGpu;
        event.current = m_impl->gpuPressure;
        event.adapterIndex = m_impl->latest.gpu.selectedAdapterIndex;
        event.budgetBytes = m_impl->latest.gpu.vramBudgetBytes;
        event.usageBytes = m_impl->latest.gpu.vramUsageBytes;
        event.usageRatio = m_impl->latest.gpu.vramUsageRatio;
        event.hresult = m_impl->latest.gpu.status == GpuStatus::Available
                            ? 0
                            : (m_impl->latest.gpu.firstErrorHresult != 0
                                   ? m_impl->latest.gpu.firstErrorHresult
                                   : m_impl->latest.gpu.factoryHresult);
        m_impl->gpuCallback(event);
    }

    for (const auto &update : ceilingUpdates) {
        if (m_impl->jobCallback)
            m_impl->jobCallback(update.first, update.second);
    }

    return m_impl->latest;
}

const ResourceSnapshot &ResourceManager::latest() const noexcept
{
    return m_impl->latest;
}

RendererPolicy ResourceManager::policyFor(bool foreground) const
{
    return makeRendererPolicy(m_impl->ramConfig, m_impl->latest, foreground,
                              recommendedSampleIntervalMs());
}

RendererPolicy ResourceManager::policyFor(const RendererUsage &usage) const
{
    RendererPolicy policy = policyFor(usage.foreground);
    if (!usage.foreground)
        policy.backgroundAction = adviseBackground(usage);
    return policy;
}

BackgroundAction ResourceManager::adviseBackground(const RendererUsage &usage) const
{
    const RendererPolicy policy = policyFor(false);
    return adviseBackgroundAction(m_impl->ramConfig, policy, usage);
}

bool ResourceManager::shouldSuspend(const RendererUsage &usage) const
{
    return adviseBackground(usage) == BackgroundAction::Suspend;
}

std::uint32_t ResourceManager::recommendedSampleIntervalMs() const noexcept
{
    return intervalFor(m_impl->latest, m_impl->ramConfig);
}

void ResourceManager::setJobCeilingCallback(JobCeilingCallback callback)
{
    m_impl->jobCallback = std::move(callback);
    if (!m_impl->jobCallback)
        m_impl->appliedHardCeilings.clear();
}

void ResourceManager::setGpuPressureCallback(GpuPressureCallback callback)
{
    m_impl->gpuCallback = std::move(callback);
}

void ResourceManager::setRamPressureCallback(RamPressureCallback callback)
{
    m_impl->ramCallback = std::move(callback);
}

void ResourceManager::setRamPolicyConfig(const RamPolicyConfig &config)
{
    m_impl->ramConfig = sanitizeRamConfig(config);
}

const RamPolicyConfig &ResourceManager::ramPolicyConfig() const noexcept
{
    return m_impl->ramConfig;
}

void ResourceManager::setGpuPolicyConfig(const GpuPolicyConfig &config)
{
    m_impl->gpuConfig = sanitizeGpuConfig(config);
}

const GpuPolicyConfig &ResourceManager::gpuPolicyConfig() const noexcept
{
    return m_impl->gpuConfig;
}

void ResourceManager::setPreferredGpuAdapter(std::int32_t adapterIndex)
{
    m_impl->preferredAdapter = adapterIndex >= 0 ? adapterIndex : -1;
}

std::int32_t ResourceManager::preferredGpuAdapter() const noexcept
{
    return m_impl->preferredAdapter;
}

void ResourceManager::reset()
{
    m_impl->usage.clear();
    m_impl->latest = ResourceSnapshot();
    m_impl->ramPressure = PressureLevel::Normal;
    m_impl->gpuPressure = PressureLevel::Normal;
    m_impl->appliedHardCeilings.clear();
    m_impl->releaseFactory();
}

}  // namespace resource
}  // namespace khtml
