# ResourceManager (host RAM + DXGI GPU budgets)

`src/host/resourcemanager.h` / `src/host/resourcemanager.cpp`

A Qt-free (Win32 + DXGI) module that gives the browser host one place to sample
system RAM, aggregate per-tab renderer usage, query DXGI video-memory budgets,
and derive the dynamic *soft* policy that drives tombstoning and Job Object
ceilings.

It is deliberately **not** a scheduler and it does not own renderer processes.
The browser host owns the `ResourceManager`, supplies its renderer usage list,
and applies the returned advice/ceilings.

---

## 1. Ownership and call sequence

```cpp
// KHtmlLiteWindow (browser host, UI thread) -- value member, no Qt inside.
khtml::resource::ResourceManager m_resourceMgr;
```

Per watchdog tick (`checkMemoryBudget()` in the current shell):

```cpp
// 1. Describe every tab.  id must be stable for the tab's lifetime.
std::vector<khtml::resource::RendererUsage> usage;
for (TabPage *pg : m_pages) {
    khtml::resource::RendererUsage u;
    u.id = reinterpret_cast<std::uint64_t>(pg);   // nonzero, stable
    u.processId = 0;                               // diagnostics only
    u.foreground = (pg == currentPage());
    u.suspended = pg->suspended;
    u.idleMs = pg->lastActive > 0
             ? std::uint64_t(now - pg->lastActive) : 0;
    if (!pg->suspended && pg->render) {
        u.workingSetBytes =
            std::uint64_t(qMax<qint64>(0, pg->render->memoryKB())) * 1024;
    }
    usage.push_back(u);
}

// 2. Replace the usage list, then sample RAM + GPU.
m_resourceMgr.updateRendererUsage(std::move(usage));
const auto snap = m_resourceMgr.sample();

// 3. Retime the watchdog (5000 / 2000 / 1000 ms by default).
m_memTimer->start(int(snap.recommendedSampleIntervalMs));

// 4. Apply per-renderer policy.
for (TabPage *pg : m_pages) {
    if (pg->suspended || !pg->render) continue;
    const auto u = /* same description as above */;
    const auto pol = m_resourceMgr.policyFor(u);   // includes RAM pressure
    if (pol.hardCeilingBytes)
        pg->render->setMemoryCeilingKB(qint64(pol.hardCeilingBytes / 1024));

    if (pg == currentPage()) continue;              // never auto-suspend fg
    if (m_resourceMgr.adviseBackground(u)
            == khtml::resource::BackgroundAction::Suspend) {
        suspendTab(pg);
    } else if (u.workingSetBytes > pol.softBudgetBytes) {
        // soft overrun: trim caches / tombstone according to pressure
    }
}

// 5. React to pressure levels (optional).
if (snap.ram.pressure != khtml::resource::PressureLevel::Normal)
    purgeCache();
```

`policyFor(true)` / `policyFor(false)` are also available when the caller only
knows foreground-ness:

```cpp
const auto fg = m_resourceMgr.policyFor(true);
const auto bg = m_resourceMgr.policyFor(false);
```

### Optional callback form for Job Object ceilings

Instead of writing the ceiling in the loop every tick, the host can install the
deduplicating callback once. It fires only when a live renderer's hard ceiling
changes, and re-fires after a suspend/resume because the fresh Job Object starts
with the provisional limit:

```cpp
m_resourceMgr.setJobCeilingCallback([this](std::uint64_t id, std::uint64_t bytes) {
    auto *pg = reinterpret_cast<TabPage *>(id);
    if (pg && pg->render)
        pg->render->setMemoryCeilingKB(qint64(bytes / 1024));
});
```

The callback receives **commit bytes** (`JOB_OBJECT_LIMIT_PROCESS_MEMORY`).
It is not a working-set threshold and it is not a kill signal.

### GPU pressure event

```cpp
m_resourceMgr.setGpuPressureCallback([this](const khtml::resource::GpuPressureEvent &e) {
    fprintf(stderr, "[res] GPU pressure %s -> %s adapter=%d budget=%llu usage=%llu\n",
            khtml::resource::toString(e.previous),
            khtml::resource::toString(e.current),
            e.adapterIndex,
            (unsigned long long)e.budgetBytes,
            (unsigned long long)e.usageBytes);
    // Diagnostics / telemetry / compositor cache policy only.
    // NEVER use GPU bytes to change a CPU budget or Job Object ceiling.
});
```

The event is adapter-wide (DXGI budgets are per adapter/segment, not per
process). It fires only on a level change, not every sample. A GPU-disappeared
transition (`Critical -> Normal`) is reported with `adapterIndex == -1`.

---

## 2. RAM sampling

`sample()` always fills `RamSample`:

| Field | Source | Meaning |
| --- | --- | --- |
| `valid`, `win32Error` | `GlobalMemoryStatusEx` | sampling result |
| `totalPhysBytes`, `availPhysBytes`, `usedPhysBytes` | `MEMORYSTATUSEX` | machine RAM |
| `memoryLoadPercent` | `dwMemoryLoad` | **system-wide** physical-in-use %, a statistic |
| `browserWorkingSetBytes`, `browserCommitBytes` | `GetProcessMemoryInfo(GetCurrentProcess())` | host footprint (`WorkingSetSize`, `PagefileUsage`) |
| `rendererWorkingSetBytes`, `rendererCommitBytes` | supplied `RendererUsage` list | aggregation; suspended entries excluded |
| `rendererCount`, `foregroundRendererCount` | supplied list | bookkeeping |
| `trackedWorkingSetBytes` | browser + renderers | **our** combined statistic, not system RAM usage |
| `pressure` | derived + hysteresis | Normal / Warning / Critical |

The module does **not** enumerate renderer processes. The host already knows its
tabs and passes working-set values; this keeps the module testable and avoids a
second, racy process table walk.

`workingSetBytes` and the Job Object's commit limit are different metrics.
Working set can be trimmed by the OS at any time; commit (`PagefileUsage`) is
what `JOB_OBJECT_LIMIT_PROCESS_MEMORY` caps. `RendererUsage::commitBytes` is
optional input for diagnostics only.

---

## 3. DXGI GPU sampling

Per sample, after a one-time cached `CreateDXGIFactory1`:

1. `IDXGIFactory1::EnumAdapters1` enumerates up to 16 adapters.
2. `GetDesc` fills name (UTF-8), vendor/device/subsystem/revision, dedicated and
   shared adapter properties.
3. `GetDesc1` fills flags (`software`, `remote`); `GetDesc2` fills the LUID.
4. `QueryInterface(IID_IDXGIAdapter3)` obtains the video-memory API.
5. `IDXGIAdapter3::QueryVideoMemoryInfo(0, LOCAL)` and
   `(0, NON_LOCAL)` are queried **independently** per adapter.
6. The aggregate VRAM numbers use only the selected adapter's **LOCAL**
   segment.

### Status semantics

| `GpuStatus` | Meaning |
| --- | --- |
| `Available` | `QueryVideoMemoryInfo` succeeded for at least one LOCAL segment |
| `Unsupported` | no factory/adapter, or no adapter3 (Windows < 8.1 / DXGI < 1.3) |
| `QueryError` | adapter3 exists but every video-memory query failed; see `hresult` |

Per-segment status works the same way (`localMemory.status`,
`nonLocalMemory.status`). An adapter can be `Unsupported` while another is
`Available`. `firstErrorHresult` is the first QI/query failure and is
diagnostic even when the overall sample is `Available`; `factoryHresult` is set
only when factory creation failed.

### Adapter selection

- explicit `setPreferredGpuAdapter(index)` if that adapter has a queryable LOCAL
  segment;
- otherwise the non-software adapter with the largest dedicated VRAM (tie:
  larger LOCAL budget);
- otherwise the first queryable adapter.

On hybrid laptops the browser compositor may not use the adapter this heuristic
prefers. The caller can match the compositor's LUID (or `KHTML_FORCE_WARP`
diagnostics) and call `setPreferredGpuAdapter(index)`; `GpuAdapterInfo::luid`
is packed exactly like `DXGI_ADAPTER_DESC2::AdapterLuid`.

### What the numbers mean (and do not mean)

- `vramBudgetBytes` = selected adapter's **LOCAL** `Budget`. It is the OS's
  budget for that GPU memory segment.
- `vramUsageBytes` = **LOCAL** `CurrentUsage`; `vramAvailableBytes` =
  `budget - usage` clamped at zero.
- `sharedSystemMemoryBytes` is a **DXGI adapter property**, not a budget. It is
  reported for diagnostics only and is never summed into the VRAM budget.
- `nonLocalMemory` is reported separately. It is never added to
  `vramBudgetBytes`.
- On integrated GPUs the LOCAL budget is backed by system RAM. It is still a
  GPU-memory budget in a **separate accounting domain**; it must never be
  added to, converted into, or used to lower a CPU/RAM budget or Job Object
  ceiling.
- `vramUsageRatio` and GPU pressure describe adapter-wide usage by all
  processes, not KHtmlLite's share.

This module does not use PDH. The existing PDH `GPU Engine(*)\Utilization
Percentage` sampler measures utilization (0-100%) and is complementary: PDH is
busy-ness, DXGI is memory budget/usage.

---

## 4. Dynamic RAM policy

All policy math is in `makeRendererPolicy()`. GPU pressure is not an input.

### Formula

```text
scale = clamp(totalPhys / 8 GiB, 0.75, 4.0)       // 4/8/16/32 GiB -> 0.75/1/2/4
base  = 400 MiB * scale
base  = min(base, totalPhys * 0.10)                // soft fraction cap
base  = clamp(base, 192 MiB, 2048 MiB)

factor = 1.0                                      // Normal
       = 0.80 (fg) / 0.60 (bg)                    // Warning
       = 0.60 (fg) / 0.35 (bg)                    // Critical

soft  = clamp(base * factor, 192 MiB, 2048 MiB)
hard  = clamp(max(soft * 2, 512 MiB), 512 MiB, 4096 MiB); hard >= soft
```

The 400 MiB baseline is a **reference point on an 8 GiB machine**, not a
universal hard-coded budget. The `scale` and `0.10 * totalPhys` terms make it
follow the machine; the pressure factors make it follow the moment. The
`minPerRendererBytes` floor can exceed the 10% fraction on very small machines
by design.

Worked examples (`soft / hard`, MiB; `fg` = foreground, `bg` = background):

| RAM | Normal fg | Normal bg | Warning fg | Warning bg | Critical fg | Critical bg |
| --- | --- | --- | --- | --- | --- | --- |
| 4 GiB | 300/600 | 300/600 | 240/512 | 192/512 | 192/512 | 192/512 |
| 8 GiB | 400/800 | 400/800 | 320/640 | 240/512 | 240/512 | 192/512 |
| 16 GiB | 800/1600 | 800/1600 | 640/1280 | 480/960 | 480/960 | 280/560 |
| 32 GiB | 1600/3200 | 1600/3200 | 1280/2560 | 960/1920 | 960/1920 | 560/1120 |

(On 4 GiB, `base` is 300 MiB; the background Warning value 180 is clamped up to
the 192 MiB floor. On 32 GiB, background Critical 560 -> hard 1120, foreground
Critical 960 -> hard 1920.)

The actual shell runs with a 4 GiB machine at ~85-93% load in this environment,
which lands in **Warning**: foreground 240 MiB soft / 512 MiB hard, background
192 MiB / 512 MiB. This is intentionally more conservative than the old fixed
512/768/1024 MiB classes, but it still scales up on larger machines.

### Pressure levels and hysteresis

`dwMemoryLoad` is classified with:

| Level | Raw threshold (default) |
| --- | --- |
| Normal | `< 85%` |
| Warning | `85% .. 92%` |
| Critical | `>= 93%` |

Upgrades are immediate. Downgrades happen only after the load falls below
`threshold - hysteresisBandPercent` (default band 8):

- `Critical -> Warning` at `load <= 85%`;
- `Warning -> Normal` at `load <= 77%`;
- `Critical -> Normal` directly when `load <= 77%`.

That 8% band prevents flapping around a threshold while still releasing within
one or two samples. The band is configurable in `RamPolicyConfig`. The previous
tick's level is kept if `GlobalMemoryStatusEx` fails (transient sampling errors
must not pretend the pressure disappeared); `valid=false` and `win32Error` are
reported.

### Sampling cadence

`recommendedSampleIntervalMs` uses the **worse** of RAM and GPU pressure:

| Worst level | Default interval |
| --- | --- |
| Normal | 5000 ms |
| Warning | 2000 ms |
| Critical | 1000 ms |

The host should retime its watchdog from the snapshot. The current shell does
`m_memTimer->start(int(snap.recommendedSampleIntervalMs))`. A kernel low-memory
notification thread may still call the watchdog early; that is compatible.

### Foreground priority

Foreground renderers:

- never receive `BackgroundAction::Suspend` (`allowSuspend=false`);
- keep higher budget factors under pressure (0.80/0.60 vs 0.60/0.35);
- are skipped by the host's auto-suspend loop.

They can still receive `purgeCache=true` under Warning/Critical, which is a
cheap, non-destructive release step.

### Background lifecycle advice

`adviseBackground(usage)` returns:

| Condition | Action |
| --- | --- |
| foreground or already suspended | `Keep` |
| Critical, or Warning with idle >= 5 min / over soft budget | `Suspend` |
| Warning (otherwise) | `Trim` |
| Normal, idle >= 5 min | `Suspend` |
| Normal, over soft budget | `Trim` |
| Normal otherwise | `Keep` |

`Trim` means purge regenerable caches / shrink tile or image caches but keep
the process. `Suspend` means the host's tombstone path (snapshot, kill renderer,
keep URL/title/scroll). The manager only advises; the host decides and performs
the tombstone.

`policyFor(usage)` combines `policyFor(usage.foreground)` with this advice, so a
single call gives both budget and action.

---

## 5. Hard limit vs soft threshold vs statistic

This distinction is intentional and must survive integration:

| Thing | Enforced by | Consequence |
| --- | --- | --- |
| **Job Object commit ceiling** (`hardCeilingBytes`) | `JOB_OBJECT_LIMIT_PROCESS_MEMORY` on the renderer's Job Object | **true hard limit**: allocations fail at the commit cap; the OS does not negotiate |
| Soft budget (`softBudgetBytes`) | this module + host trim/suspend policy | advisory working-set target; no OS enforcement |
| `trackedWorkingSetBytes` | this module's aggregation | statistic about our processes, not system-wide use |
| `memoryLoadPercent` | `GlobalMemoryStatusEx` | system-wide statistic; other applications dominate it |
| VRAM budget/usage | DXGI LOCAL segment | GPU accounting; never a CPU/RAM cap |
| GPU pressure | derived from LOCAL usage/budget | advisory event; no automatic RAM action |

The hard ceiling is deliberately looser than the soft budget (2x, with a
512 MiB floor): the Job Object is a crash-prevention backstop for runaway
commit, while tombstoning/trimming is the normal pressure response. On a 4 GiB
machine at Critical the soft budget is 192 MiB but the hard ceiling stays
512 MiB so a heavy page is not killed by ordinary allocation spikes.

The browser keeps `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` on the same Job Object:
if the browser dies, the kernel reaps renderers. That is independent of the
memory ceiling.

---

## 6. Build integration

The module is C++17-compatible (also compiles as C++20) and has no Qt include.
For the current host target:

```cmake
# src/host/CMakeLists.txt
add_executable(khtml_browser WIN32
    khtml_browser.cpp
    pageloader.cpp
    resourcemanager.cpp      # new
)
# dxgi: DXGI factory/Adapter3; dxguid: IID_IDXGIFactory1,
# IID_IDXGIAdapter2, IID_IDXGIAdapter3.  pdh already present.
target_link_libraries(khtml_browser PRIVATE ${KHTML_HOST_LIBS} pdh dxgi dxguid)
```

`GetProcessMemoryInfo` resolves through MinGW's `K32GetProcessMemoryInfo`
alias in kernel32; no separate `psapi` link is required on this toolchain.
If another toolchain still resolves the old psapi entry point, add `psapi`.

No CMake edits were made by this module's author; the host integration owns
that change.

### Standalone compile check (no full app build)

From the repository root, with MSYS2 MinGW64 on `PATH`:

```powershell
C:\msys64\mingw64\bin\g++.exe -std=c++17 -O2 -Wall -Wextra -Wpedantic `
  -I src/host -c src/host/resourcemanager.cpp -o resourcemanager.o
```

C++20 is also clean:

```powershell
C:\msys64\mingw64\bin\g++.exe -std=c++20 -O2 -Wall -Wextra -Wpedantic `
  -I src/host -c src/host/resourcemanager.cpp -o resourcemanager20.o
```

### Self-contained smoke test

Write `smoke.cpp` outside the repository (or in a build directory) with:

```cpp
#include "resourcemanager.h"
#include <cstdio>
int main() {
    khtml::resource::ResourceManager rm;
    int ceilings = 0;
    rm.setJobCeilingCallback([&](std::uint64_t, std::uint64_t) { ++ceilings; });
    std::vector<khtml::resource::RendererUsage> u(2);
    u[0].id = 1; u[0].foreground = true; u[0].workingSetBytes = 350ull << 20;
    u[1].id = 2; u[1].workingSetBytes = 500ull << 20; u[1].idleMs = 400000;
    rm.updateRendererUsage(u);
    const auto s = rm.sample();
    std::printf("ram=%s load=%u%% pressure=%s gpu=%s adapters=%u selected=%d "
                "vram=%lluMiB ceilings=%d interval=%u\n",
        s.ram.valid ? "ok" : "fail", s.ram.memoryLoadPercent,
        khtml::resource::toString(s.ram.pressure),
        khtml::resource::toString(s.gpu.status), s.gpu.adapterCount,
        s.gpu.selectedAdapterIndex,
        (unsigned long long)(s.gpu.vramBudgetBytes >> 20), ceilings,
        s.recommendedSampleIntervalMs);
}
```

Build and run:

```powershell
C:\msys64\mingw64\bin\g++.exe -std=c++17 -O2 -Wall -Wextra `
  -I src/host smoke.cpp src/host/resourcemanager.cpp `
  -o smoke.exe -ldxgi -ldxguid
.\smoke.exe
```

Result on the development machine (Windows 10, 4 GiB RAM, hybrid NVIDIA
940MX + Intel HD 620, MinGW GCC 16.2.0):

```text
compile: exit 0 (C++17 and C++20, -Wall -Wextra -Wpedantic)
run:
  RAM valid=yes load=87% total=3976 MiB pressure=warning
  rendererWS=850 MiB tracked=856 MiB (suspended entry excluded)
  policy fg=240 MiB soft / 512 MiB hard, bg=192 MiB / 512 MiB
  GPU status=available adapters=4 selected=0
  adapter 0 "NVIDIA GeForce 940MX"  dedicated=2001 MiB local=1701/0/1701 MiB
  adapter 1 "Intel(R) HD Graphics 620" dedicated=128 MiB local=1789/0/1789 MiB
  adapter 2 "Intel(R) HD Graphics 620" dedicated=128 MiB local=1789/0/1789 MiB
  adapter 3 "Microsoft Basic Render Driver" (software) local=1789/0/1789 MiB
  job ceilings: 2 initial calls; unchanged sample: 0 extra; suspend+resume: 1 re-apply
  interval recommended=2000 ms at Warning
```

The exact budgets/load vary per machine; the structural checks are the point:
suspended entries excluded, ceilings deduplicated and re-applied after resume,
LOCAL-only aggregation, and no fake numbers when a query is unavailable.

---

## 7. Limitations and fallbacks

- **Windows version / driver**: `QueryVideoMemoryInfo` needs `IDXGIAdapter3`
  (Windows 8.1+, DXGI 1.3). On older stacks the status is `Unsupported`, budgets
  are 0, and no pressure event claims otherwise. `QueryError` carries the
  HRESULT. There is no synthetic fallback number.
- **Integrated GPUs**: the LOCAL budget is system-RAM-backed. It is reported as
  a GPU budget in its own domain, never as extra CPU RAM. The `integrated` flag
  is a heuristic for display only.
- **Shared/non-local memory**: `SharedSystemMemory` (adapter property) and
  `NON_LOCAL` segment are reported but never added to `vramBudgetBytes`. They
  are not "VRAM" for budgeting.
- **Adapter-wide usage**: DXGI does not attribute GPU memory to a process.
  `vramUsageBytes` includes all system clients. `setPreferredGpuAdapter` lets
  the host match the compositor's LUID; the default heuristic picks the largest
  dedicated adapter.
- **Multi-node adapters**: only node 0 is queried. Linked-display / multi-node
  GPUs are not split per node.
- **Adapter cap**: enumeration stops at 16 adapters for safety.
- **eGPU/hotplug**: the DXGI factory is cached but adapters are re-enumerated
  every sample, so adapter changes are seen; a stale factory that starts
  failing yields `Unsupported`/`QueryError` rather than a stale budget.
- **No process enumeration / no enforcement**: the host supplies renderer
  usage; the module never opens other processes and never kills/tombstones a
  tab. The host applies the advice.
- **Metrics differ**: `WorkingSetSize` (soft, trim advice) is not
  `PagefileUsage`/commit (hard Job Object cap). A renderer can be under the
  working-set soft budget and still hit the commit ceiling, or vice versa.
- **Hysteresis state is in-memory**: restarting the browser resets levels to
  Normal; the first sample may immediately upgrade to the current level and
  fire one event.
- **Unsupported/query-error data is honest absence**: status fields and HRESULTs
  are exposed rather than guessed.
- **Threading/reentrancy**: one instance per UI thread; no locking. Callbacks
  run synchronously in `sample()`; do not re-enter or destroy the manager from
  a callback. Callback lifetime is the caller's responsibility (clear callbacks
  before the captured objects die, or ensure the manager outlives them).
- **Callback dedup**: the hard-ceiling callback is edge-triggered per renderer
  id. A suspended id is removed from the memo so a resume re-applies the
  ceiling; a removed id is forgotten. If the host recreates a Job Object
  without suspending, it should still set an initial ceiling when the renderer
  starts (or call the policy directly).
- **GPU pressure is not a RAM signal**: by design, `policyFor()` uses only
  `snap.ram.pressure`. The host may separately reduce GPU work on
  `GpuPressureEvent`, but must not derive a CPU cap from VRAM bytes.

---

## 8. API quick reference

```cpp
namespace khtml::resource {

enum class PressureLevel { Normal, Warning, Critical };
enum class BackgroundAction { Keep, Trim, Suspend };
enum class GpuStatus { Unsupported, Available, QueryError };

struct RendererUsage { id, processId, workingSetBytes, commitBytes,
                       foreground, suspended, idleMs };
struct RamSample;           // RAM + browser + renderer aggregation + pressure
struct GpuMemorySegmentInfo;// status/hresult/budget/usage/available
struct GpuAdapterInfo;      // name/LUID/dedicated/shared/local/nonLocal
struct GpuSample;           // status/adapters/selected/local aggregate/pressure
struct ResourceSnapshot { RamSample ram; GpuSample gpu;
                          std::uint32_t recommendedSampleIntervalMs; };
struct RamPolicyConfig;     // baseline/scales/factors/thresholds/intervals
struct GpuPolicyConfig;     // warning/critical usage ratio + hysteresis
struct RendererPolicy { softBudgetBytes, hardCeilingBytes, purgeCache,
                        allowSuspend, backgroundAction, sampleIntervalMs };
struct GpuPressureEvent;

class ResourceManager {
    void updateRendererUsage(std::vector<RendererUsage>);
    ResourceSnapshot sample();
    const ResourceSnapshot &latest() const;
    RendererPolicy policyFor(bool foreground) const;
    RendererPolicy policyFor(const RendererUsage &) const;
    BackgroundAction adviseBackground(const RendererUsage &) const;
    bool shouldSuspend(const RendererUsage &) const;
    std::uint32_t recommendedSampleIntervalMs() const;
    void setJobCeilingCallback(JobCeilingCallback);
    void setGpuPressureCallback(GpuPressureCallback);
    void setRamPressureCallback(RamPressureCallback);
    void setRamPolicyConfig(const RamPolicyConfig &);
    void setGpuPolicyConfig(const GpuPolicyConfig &);
    void setPreferredGpuAdapter(std::int32_t);
    void reset();
};

}  // namespace khtml::resource
```
