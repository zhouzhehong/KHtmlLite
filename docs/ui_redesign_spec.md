# KHtmlLite EXE 外壳 UI 重设计规范 — 经典 Safari × Windows 原生

| 项 | 值 |
|---|---|
| 文档 | `docs/ui_redesign_spec.md`（task-3 交付物） |
| 适用对象 | **仅 `khtml_browser.exe` 外壳（`KHtmlLiteWindow::buildChrome` / `safariStyleSheet` / `chromeGlyph` / `SafariTabBar` / `nativeEvent`）**；不动渲染进程、不动页面/引擎、不动 CMake |
| 设计参照 | **Safari 4/5 for Windows 的轮廓**：标签并入标题条、Windows 标题按钮在右上、下方一条统一工具栏。**不是** Safari 14/15 的 Liquid Glass / 半透明 / 大圆角胶囊视觉 |
| 审计基线 | HEAD `fc46ab7` + 2026-10-28 22:11 工作区中的崩溃恢复改动（这些改动只碰恢复流程，未改 chrome 代码） |
| 状态 | 规范定稿，待 Lead 实现；ui-spec 未修改任何源码 |
| 硬性回归约束 | 页面原点 `(11,87)` 物理（dpr=1.5）、back `(34,66)`、forward `(68,66)`、tab `(170,22)`、地址栏中心 `y=66` |

---

## 0. 结论先行（Lead 可直接照此实现）

1. **推荐网格 30 + 28**（逻辑 px）替代当前的 26 + 32：总和仍是 **58 逻辑 = 87 物理**，页面原点不变；换来 **46×30 的 Windows 原生标题按钮**，且工具栏几何中心正好是 **44 逻辑 = 66 物理**，地址栏中心 `y=66` 从"落在范围内"升级为"精确命中中心"。
2. **外层方角**：删除 `applyRoundedCorners()` 的 `DWMWCP_ROUND(2)`，改为 `DWMWCP_DONOTROUND(1)`，并让 chrome **满幅到窗口边缘**（页面仍保留 7px 左右留白）。Windows 标题按钮出现在**右上角**，删除红黄绿。
3. **Windows 标题按钮**：`最小化 / 最大化(还原) / 关闭`，每个 `46×30` 逻辑、右对齐贴边、直角、单色 10px 线性图标；悬停 `#E5E5E5`、按下 `#D6D6D6`；关闭悬停 `#C42B1C`+白图标。不是 traffic lights。
4. **工具栏层级**：`[◀ ▶ 分段导航 48×24] [地址栏（内嵌 reload，flex）] [搜索 176×24] [⚙ 24×24]`，全部 4px 圆角、24px 高、8px 栅格间距。
5. **边缘缩放**：保留 `Qt::FramelessWindowHint`，但给 `GWL_STYLE` 注入 `WS_THICKFRAME`，并处理 `WM_NCCALCSIZE` 返回 0（最大化时扣掉隐形边框），命中测试交给系统默认边框；**现行"只回 HTLEFT 没有 WS_THICKFRAME"必须废弃**（实测 style `0x960B0000`，无 `WS_THICKFRAME`，这就是拖边框不缩放的根因）。
6. **现有测试坐标一个都不用改**（详见 §15）。新增行为（标题按钮、边缘缩放、方角、焦点环）另写新探针，不改旧探针。

---

## 1. 范围与非目标

### 1.1 范围（只做外壳）

- 窗口框架：方角、1px 内描边、DWM 属性、原生缩放。
- 顶部 58 逻辑 px：30 标签条 + 28 工具栏。
- 控件：标签、关闭、新建、`+`、Windows 标题按钮、后退/前进、地址栏、reload、搜索框、齿轮菜单、焦点环、状态页。
- 交互：拖拽、双击最大化、最小化/最大化/关闭、边缘缩放、快捷键、无障碍。

### 1.2 非目标（明确不做）

- 不改 `khtml_renderer.exe`、KHTML 引擎、页面 CSS、DPI 渲染管线、tile/damage 协议。
- 不做半透明/Mica/backdrop blur、页面延伸到 chrome 下方、大圆角浮动标签、SF Symbols 风格图标。
- 不新增 CMake 依赖（DWM 继续用现有 `LoadLibraryW("dwmapi.dll")` 动态取函数）。
- 不新增下载管理器/设置页**功能**；当前代码里也不存在（见 §2.4）。齿轮菜单只放**已实现**动作。
- 不引入菜单栏（File/Edit/...），不做书签栏（会改变页面原点）。

---

## 2. 现状审计（证据优先）

### 2.1 实机测量（本机 Windows 11，150% 缩放，dpr=1.5）

测量方法：启动 `build/host/khtml_browser.exe` 打开一个 `margin:0` 的红色方块页面，`ClientToScreen` + 桌面 DC 截取客户区（与 `tests/win_shot.py` 相同路径），ctypes 读窗口样式/DPI/NCHITTEST。

| 项 | 实测值 | 说明 |
|---|---|---|
| 客户区 | `1805×927` | 默认窗口（94%/92% 工作区） |
| 窗口矩形 | `1805×927`，客户区原点 == 窗口原点 | 无边框，客户区 = 窗口矩形 |
| DPI | `144` → scale `1.5` | `AA_EnableHighDpiScaling` + `PassThrough` 生效 |
| `GWL_STYLE` | `0x960B0000` | `WS_POPUP` + `WS_SYSMENU/MIN/MAX` + CLIP*；**无 `WS_THICKFRAME`、无 `WS_CAPTION`** |
| `GWL_EXSTYLE` | `0x00000000` | — |
| `DWMWA_WINDOW_CORNER_PREFERENCE` | `2`（`DWMWCP_ROUND`） | 当前外角是**圆角** |
| 页面内容原点 | 红块 bbox 左上 `(11,87)` | 与回归探针 `PAGE_ORIGIN=(11,87)` 一致 |
| 顶部条带 | 标签条 `0..39` 物理、工具栏 `39..87` 物理 | = 26 + 32 逻辑 |
| 地址栏 | `x=124..1679`、`y=45..80` 物理 | 约 `83..1119`、`30..53` 逻辑；圆角 8、高 24 |
| 红绿灯 | 直径约 15 物理、中心 `x≈31/58/85`、`y≈19` | 12 逻辑、`border-radius:6px` |
| 当前 1px 边框 | 渲染成 2 行（如 y=37/38、85/86） | 1 逻辑 px 在 1.5x 下 AA 成 1.5 设备 px，无设备栅格对齐 |
| NCHITTEST | 左 2px→`HTLEFT`、右 2px→`HTRIGHT`、上/下/角均正确返回 | 但**拖拽不缩放**（见下） |

### 2.2 为什么边缘缩放现在不工作（根因）

- style 里没有 `WS_THICKFRAME`，`Qt::FramelessWindowHint` 让窗口是 `WS_POPUP`；`nativeEvent(WM_NCHITTEST)` 虽然返回 `HTLEFT/HTRIGHT/...`，但系统没有 sizing frame 可拖，DefWindowProc 不会进入缩放循环。
- HANDOVER §5aj 实测同一结论：宽度/高度 delta = 0。
- 附带 bug：`nativeEvent` 里 `case WM_EXITSIZEMOVE:` 调完 `endLiveResize()` **缺 `break;`**，会贯穿进 `WM_NCHITTEST` 分支；实现时应顺手修正。
- 边框命中用的是 `const int border = 6;`（**物理** px，1.5x 下只有 4 逻辑 px），偏薄且不随 DPI 缩放。

### 2.3 当前代码结构（审计对象）

- `KHtmlLiteWindow`（`src/host/khtml_browser.cpp`，约 `class KHtmlLiteWindow` 段）：
  - 构造：`setWindowFlag(Qt::FramelessWindowHint, true)`；`QTimer::singleShot(0, applyRoundedCorners)`。
  - `buildChrome()`：`central` + `QVBoxLayout root(7,0,7,4)`；`DragStrip` 26（红绿灯、侧边栏占位、`SafariTabBar`、`+`）；`DragStrip` 32（back/fwd 24×24、地址 `QLineEdit`、reload 24×24、share 24×24）；`QStackedWidget`。
  - `safariStyleSheet()`：WhiteSur-Monterey 数值（`#f5f5f5/#e8e8e8/#b6b6b6/#5683da`、8px 胶囊、`font-size:12px`）。
  - `chromeGlyph()`：36×36 pixmap 画 18 逻辑图标，再缩到 16 逻辑（`QIcon`），1.5x 下实际设备尺寸 24，**从 36 降采样 → 发糊**。
  - `SafariTabBar`：自绘 8px 圆角浮动标签、`tabSizeHint` 高 22、宽 `qBound(112, text+10, 220)`；空白区手动双击 + `startSystemMove`（HANDOVER 已验证可拖）。
  - `DragStrip`：空白按压 `startSystemMove`；双击走 `mouseDoubleClickEvent`（**没有**手动双击检测，系统移动循环会吞掉 dblclick，与 tab bar 的实现不一致）。
  - `WM_NCHITTEST` 边框 6 物理 px；`DWMWA_CORNER_PREFERENCE=ROUND`。
  - `makeCrashWidget()`：`⚠` emoji 48px + 单标签两行文案 + 默认样式 `QPushButton`（100px 宽）。
  - 工作区新增（并发改动）：`makeRecoveryWidget()`（"正在恢复页面…" #444 15px 600 / 副标题 #888 12px，全部 inline `setStyleSheet`）、`RenderTab::ready()/firstFrame()`、`startRecoveryRetry/finishRecovery`、tab 标题"正在恢复…"。

### 2.4 缺陷清单（对应修复位置）

| # | 缺陷 | 证据 | 修复（§） |
|---|---|---|---|
| D1 | 左上角是 macOS 红黄绿，右上角**没有**窗口按钮；关闭只能点红点/Alt+F4 | `mkLight()`、`m_lightClose/Min/Max` | §7.3、§11 |
| D2 | 外层圆角（与"方角"要求相反） | DWM 实测 2 | §6 |
| D3 | 边框拖拽不缩放 | style 无 `WS_THICKFRAME` | §12 |
| D4 | 死控件：侧边栏、分享（`connect(...,[](){})`） | 代码审计 | 删除；见 §2.5 |
| D5 | 地址栏一根 8px 圆角胶囊铺满 ~1038 物理 px，无搜索栏，层级弱 | 实测 + QSS | §8 |
| D6 | 标签是 8px 圆角浮动胶囊；标题条里红绿灯+侧栏+标签+`+` 混排，间距 6/7/8 混用 | `SafariTabBar::paintEvent`、`addSpacing(7)` | §7 |
| D7 | 1px 线在 1.5x 下糊成 2 行、且 chrome 四周有 7px 白边 | 实测 y=37/38、85/86；截图 | §6、§10 |
| D8 | 拖拽/双击实现分裂：tab bar 有手动双击，DragStrip 没有 | 代码审计 | §11.2 |
| D9 | 16px 图标由 36px pixmap 降采样，发糊 | `chromeGlyph()` | §9 |
| D10 | 崩溃页用彩色 emoji + 系统默认按钮；恢复页 inline 样式；墓碑页白纱+12pt | `makeCrashWidget/makeRecoveryWidget/TombstoneWidget` | §13 |
| D11 | 无 loading 态；恢复页无进度语言 | 并发改动 | §13 |
| D12 | 无 accessible name、无 Ctrl+L/Ctrl+T/Alt+←→、无可见焦点环；红绿灯 12px 命中太小 | 代码审计 | §14 |
| D13 | 颜色硬编码、无 token、只有浅色；placeholder `#8A8A8A` 对白 3.45:1 不达 AA | QSS | §10 |
| D14 | 注释/默认尺寸算法仍按"原生边框 16×40"预留，现已 client==window | 构造注释 | 不改尺寸算法（保持 1805×927），仅改注释 |

### 2.5 功能矩阵（"不丢功能"的准确口径）

| 功能 | 现状 | 重设计后 | 结论 |
|---|---|---|---|
| 后退/前进 | `m_back/m_fwd` + `updateNavButtons` | 分段导航，同 slot | 保留 |
| 重新加载 | `m_reload`（崩溃→`retryCrashedTab`；运行→`reload`；挂起→`resumeTab`） | 移入地址栏右端，同 slot | 保留 |
| 地址导航 | `m_addr` + `returnPressed` + `resolveInput` | 同，另加 Ctrl+L | 保留 |
| 搜索 | 地址栏输入非 URL → Bing；`!y ` → Yandex | 独立搜索框（Enter→Bing，同 `resolveInput` 语义）+ 地址栏仍可搜 | 保留并显式化 |
| 标签：新建/关闭/切换/拖动换序 | `+`、`×`/Ctrl+W/右键、QTabBar、`tabMoved` | 同 | 保留 |
| 清除缓存 | Ctrl+Shift+Del QAction | 齿轮菜单项 + 同快捷键 | 保留 |
| F11 全屏 | QAction | 同 | 保留 |
| 最小化/最大化/关闭 | 红绿灯 | 右上 Windows 按钮，同 QWidget slot | 保留 |
| 侧边栏按钮 | **空 lambda（无功能）** | 删除 | 无损失（死控件） |
| 分享按钮 | **空 lambda（无功能）** | 删除 | 无损失（死控件） |
| 下载 | **不存在**：`PageLoader` 的 "download" 是内部资源镜像/抓取，无用户下载管理器、无下载 UI | 不伪造按钮；齿轮菜单预留 `下载 (Ctrl+J)` 槽位，**实现后再出现** | 无可保留项 |
| 设置 | **不存在**：只有 `clearPageCacheNow()` + Ctrl+Shift+Del；无设置对话框 | 齿轮菜单：新标签/重载/关标签/清缓存；`设置… (Ctrl+,)` 槽位预留，**实现后再出现** | 无可保留项 |

> 口径：**当前二进制里真实存在且有响应的功能一个不丢**；两个空 lambda 按钮删除不构成功能损失；下载/设置从未存在，禁止做成灰色或点击无响应的假控件（与"去 clutter"冲突）。若主人坚持要入口，必须先实现最小功能（下载列表数据源 / 设置项读写）再接 UI。

---

## 3. 设计方向：经典 Safari（4/5 Windows），不是 Safari 14/15

### 3.1 要的

- **Tabs-on-top + Windows caption**：标签条即标题条；右上角是 Windows 的最小化/最大化/关闭；左上是标签，不是红绿灯。
- **一条统一工具栏**：后退/前进分段控件 → 地址栏 → reload → 搜索框 → 齿轮。
- **平静的层级**：浅中性灰工具栏 / 略深的标签条 / 白色页面；1px 分隔线；8px 栅格。
- **克制的圆角**：内部控件统一 4px（分段导航、输入框、按钮、菜单、标签顶部）；外窗 0；不使用 8–12px 胶囊。
- **单色线性图标**：1.2–1.6 逻辑 px 描边，`#3C3C3C`（深色 `#E6E6E6`），无彩色 emoji。
- **whitespace**：控件之间 8px；标签文字左右 10/30px；输入框内 8–10px；不做贴边堆叠。

### 3.2 明确不要（防止"Liquid Glass 化"）

- 半透明/Mica/backdrop blur/毛玻璃；页面透到 chrome 下。
- 8px+ 大圆角、浮动标签胶囊、整条超大搜索胶囊。
- macOS 红黄绿灯、SF Symbols、彩色 emoji。
- 居中悬浮的巨型地址胶囊（Safari 15 风）。
- 渐变/高光/阴影堆叠（允许 1px 顶高光 + 1px 分隔线这种克制细节）。

---

## 4. 几何不变量（先立数）

### 4.1 推荐方案：30 标签条 + 28 工具栏 = 58 逻辑

理由：

1. **页面原点不变**：`58 × 1.5 = 87` 物理；`7 × 1.5 = 10.5 → 11` 物理（实测量到的 11）。
2. **地址栏中心精确命中**：工具栏 `30..58`，内容 `32..56`，中心 `44 逻辑 = 66 物理`，正好是既有 `address center y=66` 不变量。
3. **46×30 接近 Windows 10/11 标题按钮的系统比例**（约 46×30–32；这里受 30px 条高约束取 30），比 26px 条高时更 native。
4. **标签仍有 28px 可见高度**（活动）/26px（非活动），经典 Safari 比例。

| 行 | 逻辑范围 | 1.5x 物理 | 内容 |
|---|---|---|---|
| 标签条 | `y 0..30` | `0..45` | 标签、`+`、标题按钮 |
| 工具栏 | `y 30..58` | `45..87` | 导航、地址、reload、搜索、齿轮 |
| 页面 | `y 58..H-4` | `87..(H-6)` | 左右各 7 逻辑留白 |

### 4.2 页面原点保持的关键：root margin 改成满幅 + pageHost 留白

当前 `root->setContentsMargins(7,0,7,4)` 让 **chrome 也** 内缩 7px（截图里窗口四周有一圈白边，D7）。重设计后：

```
central (满幅, 白底)
 └ QVBoxLayout root: margins (0,0,0,4), spacing 0      ← 左右改成 0，chrome 满幅
    ├ strip   (30 高, 满幅)
    ├ toolbar (28 高, 满幅)
    └ pageHost QWidget (stretch)
        └ QHBoxLayout: margins (7,0,7,0), spacing 0
            └ pageStack (QStackedWidget)
```

- **页面原点仍为**：x = `7 逻辑 → 11 物理`，y = `30+28 = 58 → 87 物理`；页面尺寸不变（左右各 7、底部 4 与现在完全一致）。
- **不要把 7 改成 8 或 6 来"对齐栅格"**：会直接破坏 `(11,87)` 与全部截图/点击探针。
- chrome 满幅后，右上标题按钮才能贴到窗口右边缘 `x=W`；左/右/底外沿由 §6 的 1px overlay 描边。

### 4.3 备选方案：26 + 32（用户原始建议，同样保原点）

如果 Lead 坚持原 26+32，所有 x 公式不变，只需按下表改 y：

| 项 | 30+28（推荐） | 26+32（备选） |
|---|---|---|
| 标签条 | `0..30` | `0..26` |
| 工具栏 | `30..58` | `26..58` |
| 工具栏内容 | `32..56`（24 高，中心 44=66 物理） | `30..54`（24 高，中心 42=63 物理；`y=66` 物理=44 逻辑仍落在框内） |
| 标题按钮 | `46×30`，贴 `y0..30` | `46×26`，贴 `y0..26` |
| `+` | `24×24`，`y3..27` | `22×22`，`y2..24` |
| 标签绘制 | 活动 `y2..30`、非活动 `y4..30` | 活动 `y2..26`、非活动 `y3..26` |
| 命中 | tabRect 高 30 | tabRect 高 26 |

两方案都满足 `(170,22)`：22 物理 = 14.67 逻辑，均在标签绘制范围内。

### 4.4 页面几何（不变）

```
逻辑：页面 x = 7 .. W-7；y = 58 .. H-4
物理(1.5x)：x = 11 .. ；y = 87 .. H-6
```

当前实测 `(11,87)` 即由此得到。**任何改动后必须用红块页面重测该点。**

### 4.5 若保留当前 chrome 内缩（root margin 7）：x 整体平移

本文 §7/§8 的 x 表假设 **chrome 满幅**（§4.2，root 左右 margin = 0）。如果实现选择**不改 root margin、继续让 chrome 内缩 7px**，则：

- 所有**左侧** x 坐标 `+7`（标签栏区从 8 变 15，导航 8→15，地址 64→71）；
- 所有**右侧** x 坐标 `−7`（标题按钮组右缘从 `W` 变 `W-7`，`+`、搜索、齿轮同理）；
- 页面原点不变（仍是 7 → 11 物理，因为页面本来就吃 root 的 7）；
- **回归红线仍必须满足**：tab0 起点 15、最小宽 **144** → 右缘 ≥159，关闭热区左缘 `≥ tabRight-27 ≥132`，`(170,22)=113.3` 安全；
- **危险组合（当前工作区一度出现）**：tab0 起点 15 + 最小宽 112 + 关闭热区 `[right-20,right-4]` → 热区约 `[106,122]` 逻辑，**113.3 正好落在关闭热区里**，`tombstone_probe` 的 `(170,22)` 会关掉 tab 而不是恢复。必须把最小宽提到 ≥144（或把 tab 起点拉回 8）。

---

## 5. DPI 映射（本机 150%，逻辑 px 设计）

| 逻辑 | 1.0x | 1.25x | **1.5x（本机）** | 2.0x |
|---|---|---|---|---|
| 1px 描边 | 1 | 1–2 | **1–2**（设备栅格对齐，见 §10.4） | 2 |
| 7（页面左留白） | 7 | 9 | **11**（10.5 圆整） | 14 |
| 8（控件起点） | 8 | 10 | **12** | 16 |
| 24（控件高） | 24 | 30 | **36** | 48 |
| 30（标签条） | 30 | 38 | **45** | 60 |
| 28（工具栏） | 28 | 35 | **42** | 56 |
| 44（工具栏内容中心） | 44 | 55 | **66** | 88 |
| 46（标题按钮宽） | 46 | 58 | **69** | 92 |
| 58（chrome 总高） | 58 | 73 | **87** | 116 |
| 6（缩放热区） | 6 | 8 | **9** | 12 |

规则：

- 所有尺寸/坐标以**逻辑 px** 写代码；Qt（`AA_EnableHighDpiScaling` + `PassThrough`）负责映射。
- 不要用 `<1 逻辑 px` 的线宽（1.5x 下会闪）；要 1 设备 px 的线时，用 `1.0/dpr` 的逻辑宽度并做设备栅格对齐。
- 图标**矢量直绘**（`QPainter`），不用位图缓存；菜单 `QIcon` 例外：`QPixmap(qCeil(s*dpr),...)` + `setDevicePixelRatio(dpr)`。
- 窗口跨屏（100%↔150%）时，在 `changeEvent(QEvent::ScreenChangeInternal)` / `QWindow::screenChanged` 里 `update()`，不要缓存像素尺寸。
- 1.25/1.5/2.0 都要肉眼过一遍；**回归只锁 1.5**（现有探针按 1.5 物理坐标写死）。

---

## 6. 窗口框架：方角、满幅、1px 描边

### 6.1 DWM

```cpp
// 替换 applyRoundedCorners()
void applySquareFrame()
{
    HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
    if (!dwm) return;
    typedef HRESULT (WINAPI *DwmSetAttr)(HWND, DWORD, LPCVOID, DWORD);
    auto fn = reinterpret_cast<DwmSetAttr>(GetProcAddress(dwm, "DwmSetWindowAttribute"));
    if (!fn) return;
    const int corner = 1;                        // DWMWCP_DONOTROUND
    fn((HWND)winId(), 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &corner, sizeof(corner));
    const DWORD noBorder = 0xFFFFFFFE;           // DWMWA_COLOR_NONE (Win11 22000+)
    fn((HWND)winId(), 34 /* DWMWA_BORDER_COLOR */, &noBorder, sizeof(noBorder));
}
```

- 删除构造里的 `QTimer::singleShot(0, this, &KHtmlLiteWindow::applyRoundedCorners);`，改成 `applySquareFrame()`（仍可 `singleShot(0,...)` 让其拿到 HWND）。
- Win10 忽略返回码即可；不要因为 34 失败就回退圆角。
- **不要** 设置 `DWMWA_SYSTEMBACKDROP_TYPE` / `DWMSBT_MAINWINDOW`，不做 Mica。

### 6.2 chrome 满幅 + 1px 内描边

- `root` 左右 margin 改 0（§4.2），strip/toolbar 背景铺满窗口宽度。
- 1px 描边用一个**不参与布局**的 overlay 子控件：

```cpp
class EdgeBorder : public QWidget {
public:
    explicit EdgeBorder(QWidget *p) : QWidget(p) {
        setAttribute(Qt::WA_TransparentForMouseEvents, true);
        setAttribute(Qt::WA_NoSystemBackground, true);
    }
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setPen(QPen(QColor(0xC9,0xC9,0xC9), 1.0));   // dark: #3C3C3C
        p.setBrush(Qt::NoBrush);
        p.drawRect(rect().adjusted(0,0,-1,-1));
    }
};
```

- `EdgeBorder` 只覆盖在非 native 子控件上：页面表面（`PageSurface` 是 `WA_NativeWindow`）从 `x=11 物理` 开始、底部还留 4 逻辑 px，所以 1px 描边**不会**压在 native 子窗口上被盖住。
- 窗口最大化时描边仍画在最外圈（视觉上贴屏幕边），可接受；若不想要，最大化时隐藏。
- **外角 0 圆角**：不做 `p.drawRoundedRect` 的窗口外形；标签/按钮的圆角只作用于内部控件。

### 6.3 激活/非激活状态

- 不做 Windows 标题栏的激活/非激活渐变；chrome 在激活/非激活时保持同色（避免颜色断言复杂化）。可选 P2：非激活时图标降为 `#5F5F5F`。

---

## 7. 标签条（0..30）与 Windows 标题按钮

### 7.1 结构

```
strip (fixed height 30, bg #ECECEC, 满幅)
 └ QHBoxLayout: margins (8,0,0,0), spacing 0
    ├ SafariTabBar (stretch)                         x = 8 .. W-174
    ├ addSpacing(6)
    ├ ChromeButton plus 24×24 (AlignVCenter)         x = W-168 .. W-144
    ├ addSpacing(6)
    ├ CaptionButton min   46×30                      x = W-138 .. W-92
    ├ CaptionButton max   46×30                      x = W-92  .. W-46
    └ CaptionButton close 46×30                      x = W-46  .. W
```

### 7.2 x 坐标（逻辑 px，W = 窗口逻辑宽）

| 元素 | 公式 | W=720 | W=1203 | W=1920 |
|---|---|---|---|---|
| 标签栏区 | `[8, W-174]` | `[8,546]` | `[8,1029]` | `[8,1746]` |
| `+` 新建 | `[W-168, W-144]` | `[552,576]` | `[1035,1059]` | `[1752,1776]` |
| 最小化 | `[W-138, W-92]` | `[582,628]` | `[1065,1111]` | `[1782,1828]` |
| 最大化/还原 | `[W-92, W-46]` | `[628,674]` | `[1111,1157]` | `[1828,1874]` |
| 关闭 | `[W-46, W]` | `[674,720]` | `[1157,1203]` | `[1874,1920]` |

### 7.3 Windows 标题按钮（不是红绿灯）

| 项 | 规范 |
|---|---|
| 尺寸 | `46×30` 逻辑（1.5x = 69×45 物理），右对齐贴窗口边，**直角**，无圆角 |
| 背景 | 常态透明（strip `#ECECEC`）；悬停 `#E5E5E5`；按下 `#D6D6D6` |
| 关闭悬停 | `#C42B1C`，图标 `#FFFFFF`；按下 `#B21B14` |
| 图标色 | `#202020`（深色 `#F2F2F2`）；10×10 逻辑，居中于 (23,15) |
| 最小化 | 10×1 横线，线宽 1.0（不 AA） |
| 最大化 | 10×10 空心方框，线宽 1.0，直角 |
| 还原 | 两个 8×8 方框错位 (2,2)，线宽 1.0 |
| 关闭 | 10×10 的 ×，线宽 1.2 |
| 热区 | 整个 46×30；**排除在边缘缩放热区之外**（见 §12.2 第 4 步 / §12.5） |
| 键盘 | 不进入 Tab 顺序（与原生 Windows 一致）；Alt+F4/Win+Up/Down 走系统；`Alt+Space` 系统菜单可用 |
| 无障碍 | `setAccessibleName(u"最小化"/u"最大化"/u"还原"/u"关闭")`，Role=Button；不设 tooltip（原生标题按钮无 tooltip） |
| 行为 | 最小化→`showMinimized()`；最大化→`showMaximized()`/`showNormal()`；关闭→`close()`（沿用现有 `closeEvent` 杀 renderer 的非阻塞清理） |
| 状态同步 | `changeEvent(QEvent::WindowStateChange)` 里切换 最大化/还原 图标；全屏/最大化不得出现错误图标 |
| 双击 | 双击标题按钮**不**最大化/还原（Windows 行为；双击关闭按钮也不应连续关闭两次） |

> 红黄绿彻底删除：删掉 `mkLight()`、`m_lightClose/Min/Max` 及对应 `connect`。

### 7.4 标签绘制（经典 Safari 轮廓）

命中几何：`QTabBar::tabSizeHint()` 返回 **高 30**、宽 `qBound(144, textWidth+24, 240)`；`tabRect(0)` 从标签栏区 x=8 开始。**绘制必须画在 `tabRect` 内**，保证命中区 ≥ 可见区（现有代码的 `adjusted(2,2,-2,-2)` 思路可保留，但上限要重新算）。

| 状态 | 绘制 | 颜色（浅色 / 深色） |
|---|---|---|
| 活动 | 顶角半径 4，`y2..30`（高 28），左右上 1px 描边，底部**盖住**条底分隔线，与工具栏同色 | 填充 `#F7F7F7` / `#2A2A2A`；描边 `#C9C9C9` / `#3C3C3C`；文字 `#1F1F1F` / `#F2F2F2`，12px Medium(500) |
| 非活动 | 顶角半径 4，`y4..30`（高 26），底部不封面，条底分隔线可见 | 填充 `#DEDEDE` / `#2F2F2F`；无描边（可 1px 顶边 `#D0D0D0`）；文字 `#555555` / `#B8B8B8`，12px Regular |
| 悬停 | 非活动→`#E6E6E6` / `#383838`；文字 `#333333` / `#E6E6E6` | — |
| 按下 | `#D9D9D9` / `#404040` | — |
| 相邻分隔 | 相邻标签共享边，1px `#CFCFCF` / `#444444` 竖线（不额外留缝） | — |
| 文字 | 左内边距 10，右内边距 30（关闭热区），`ElideRight`；整串 tooltip | — |
| 关闭按钮 | 视觉 16×16 圆（半径 8）悬停底 `rgba(0,0,0,.10)`；`×` 7×7、线宽 1.2、`#5F5F5F`；活动标签常显，非活动悬停才显 | 深色：底 `rgba(255,255,255,.12)`，图标 `#D8D8D8` |
| 关闭热区 | **24×24**，中心 `(tabRight-15, 15)` → 范围 `[tabRight-27, tabRight-3] × [3,27]` | — |
| 拖拽换序 | 保留 `setMovable(true)` + `tabMoved`；拖动标签时**不**移动窗口 | — |
| 右键 | 保留现有 QMenu（关闭/新建） | — |
| 溢出 | 标签不缩到 144 以下；`setUsesScrollButtons(true)`，滚动按钮重画成 16px 单色 ‹ ›（20px 宽、悬停底 4px 圆角），不用 Qt 默认箭头 | — |

**回归红线（tab 命中）**：`(170,22)` 物理 = `(113.3,14.7)` 逻辑必须落在 **tab0 的标题区**：

- tab0 起点 = 8 逻辑、可见最小宽 144 → 右边界 ≥152；
- 关闭热区左缘 = `tabRight-27 ≥ 125`；
- 所以 113.3 距关闭热区 ≥11.7 逻辑，安全。
- **禁止**把标签起点挪到 >105 逻辑、或把 tab0 缩到 <137 逻辑宽、或把关闭热区改到右侧 20px 却保留旧宽度。

### 7.5 `+` 新建标签

- `24×24`，中心 `y=15`，`x=[W-168,W-144]`；悬停 `rgba(0,0,0,.06)` 4px 圆角；按下 `.10`。
- 图标：`+` 10×10、线宽 1.4、`#3C3C3C`；tooltip "新标签页 (Ctrl+T)"，accessible name 同名。
- 行为：`addTab(QUrl("https://cn.bing.com"))`（与现状一致）；新增 Ctrl+T。
- 标签溢出时 `+` 固定可见（在布局里位于拉伸的 tab bar 之后）。

---

## 8. 工具栏（30..58）与地址/搜索层级

### 8.1 结构

```
toolbar (fixed height 28, bg #F7F7F7, 满幅)
 └ QHBoxLayout: margins (8,2,8,2), spacing 8
    ├ NavControl 48×24            x = 8 .. 56
    ├ AddressBar (stretch, 24h)   x = 64 .. W-224
    ├ SearchField 176×24          x = W-216 .. W-40
    └ ChromeButton gear 24×24     x = W-32 .. W-8
```

### 8.2 x 坐标表（逻辑 px）

| 元素 | 公式 | W=720 | W=1203（本机默认） | W=1920 |
|---|---|---|---|---|
| 导航分段 | `[8, 56]` | `[8,56]` | `[8,56]` | `[8,56]` |
| 地址栏 | `[64, W-224]` | `[64,496]` | `[64,979]` | `[64,1696]` |
| reload（地址栏内） | `[W-249, W-227]`，`y33..55` | `[471,493]` | `[954,976]` | `[1671,1693]` |
| 搜索 | `[W-216, W-40]` | `[504,680]` | `[987,1163]` | `[1704,1880]` |
| 齿轮 | `[W-32, W-8]` | `[688,712]` | `[1171,1195]` | `[1888,1912]` |

- 全部控件高 **24 逻辑**，垂直居中于 `y32..56`；地址栏中心 = `44 逻辑 = 66 物理`（**既有 `y=66` 不变量精确命中**）。
- 间距统一 8；地址栏吸收剩余宽度（不需要 max-width；若 1920 下觉得过长可加 `max 1100` + 右侧留白，但 v1 不做，保持经典 Safari 的无上限行为）。
- 最小窗口 `720×480` 下地址栏仍 ≥432 逻辑。

### 8.3 导航：后退/前进分段控件

- 外框 `48×24`、圆角 4、1px 边框 `#C9C9C9`（深色 `#3C3C3C`）；中间 1px 分隔线 `#CFCFCF`（上下各内缩 4）。
- 两段各 `24×24`：后退 `[8,32]`，前进 `[32,56]`；图标 16px 盒内的 7×12 chevron，线宽 1.6，圆角端点。
- 状态：
  - 可用：图标 `#3C3C3C`；悬停段底 `rgba(0,0,0,.06)`；按下 `.10`。
  - 禁用（无历史）：图标 `#A8A8A8`（深色 `#6A6A6A`），不显示悬停底，cursor 仍是箭头。
  - 焦点：该段 2px `#2F6FD0` 圆角描边（外侧半圆角保留 4）。
- 键盘：每段可 Tab 聚焦；Enter/Space 触发；tooltip "后退 (Alt+←)" / "前进 (Alt+→)"。
- 行为：`goBack()/goForward()` + `updateNavButtons()`（现状保留）。
- 回归：`(34,66)` 物理 = `(22.7,44)` 落在后退段；`(68,66)` = `(45.3,44)` 落在前进段。

### 8.4 地址栏（经典 Safari：URL 字段 + 内嵌 reload）

- 外框 `flex × 24`、圆角 4、1px 边框 `#C2C2C2`、白底；文字 12px `#1F1F1F`；左内边距 10；右内边距 32（给 reload）。
- placeholder "输入网址或搜索"（`#767676`）。
- **reload 作为地址栏内的子按钮**（经典 Safari 位置）：`22×22` 热区，距右边框 3px、垂直居中（`y33..55`）；图标 12×12 圆弧箭头、线宽 1.4；悬停底 3px 圆角 `rgba(0,0,0,.07)`。
  - 行为严格沿用现有 `m_reload` lambda：崩溃页→`retryCrashedTab`；运行中→`reload()`；挂起→`resumeTab()`。
  - `setFocusPolicy(Qt::NoFocus)`，点击不抢地址栏焦点；tooltip "重新加载 (Ctrl+R)"。
- focus：2px `#2F6FD0` 边框；用 `padding` 补偿避免文字位移（常态 `padding:3px 10px`；聚焦 `padding:2px 9px`，右边距同步 31）。
- Esc：恢复当前页 URL 并把焦点还给页面；Enter：`onAddressEntered()`（现状）。
- 新增 Ctrl+L：聚焦 + 全选。
- 不做 favicon/lock 图标（v1 单色极简；若要 lock，16px 左侧、`#767676`，后续再说）。

### 8.5 搜索框（经典 Safari 的独立搜索）

- `176×24`、圆角 4、1px `#C2C2C2`、白底；左侧 12×12 放大镜 `#6B6B6B`（x=8，垂直居中）；文字左内边距 26；placeholder "搜索"（`#767676`）。
- Enter：`https://cn.bing.com/search?q=<percent-encoded>`；空串忽略；`!y ` 前缀保持 Yandex 语义（复用 `resolveInput` 的搜索分支，避免两套解析）。
- focus 同上（2px 蓝框 + padding 补偿）。
- accessible name "搜索"、tooltip "搜索 (Enter)"。

### 8.6 齿轮（Actions）菜单

- `24×24`、圆角 4、16px 齿轮线性图标（外圆 r5.2 + 8 齿 r5.2..7.2 + 内圆 r2.2，线宽 1.2），悬停底 `.06`。
- 点击弹出 `QMenu`（**只放真实动作**）：
  1. 新标签页 — Ctrl+T
  2. 重新加载 — Ctrl+R
  3. 关闭标签页 — Ctrl+W
  4. ——分隔线——
  5. 清除缓存… — Ctrl+Shift+Del（trigger 现有 `clearPageCacheNow()`）
- 预留但**当前不显示**：`下载 (Ctrl+J)`、`设置… (Ctrl+,)`——等有真实实现再出现（见 §2.5）。
- 菜单样式：白底/`#2A2A2A` 深色、1px `#C9C9C9`、圆角 4、item 高 26、左右 padding 10/24、快捷键右对齐 `#8A8A8A`、悬停 `#EDF2FB`（深色 `#3A3A3A`）、分隔线 1px `#E5E5E5`。
- 菜单出现位置：齿轮下方右对齐，距 4px。

### 8.7 统一 hover/pressed/focus

| 控件 | 常态 | hover | pressed | focus |
|---|---|---|---|---|
| 图标按钮（+、齿轮、reload） | 透明 | `rgba(0,0,0,.06)` 4px | `.10` | 2px `#2F6FD0` 圆角 4 |
| 导航段 | 透明 | `.06` | `.10` | 2px `#2F6FD0`（该段外侧圆角） |
| 输入框 | 白底 1px `#C2C2C2` | 边框 `#AFAFAF` | — | 2px `#2F6FD0` |
| 标签 | §7.4 | §7.4 | §7.4 | 活动标签内 2px `#2F6FD0`（键盘聚焦时） |
| 标题按钮 | 透明 | `#E5E5E5` | `#D6D6D6` | 不适用（不 Tab） |
| 菜单项 | 透明 | `#EDF2FB` | — | 系统菜单键盘高亮 |

深色 token 见 §10。

---

## 9. 图标系统（单色线性）

### 9.1 绘制规则

- 全部 `QPainter` 矢量直绘；**不再** `QPixmap(36,36)` 缩到 16 逻辑（D9）。
- 16 逻辑盒居中；坐标用 `QRectF` 以盒中心对齐，避免奇偶像素抖动。
- 曲线（chevron/reload/gear/spinner）开抗锯齿，线宽 1.2–1.6 逻辑；直线（标题按钮）关抗锯齿、线宽 1.0，落在整数坐标以获得 Windows 式硬边。
- 禁用态：图标色 `#A8A8A8`（深色 `#6A6A6A`）；不要用 opacity 叠到看不清。
- 菜单 `QIcon`：按当前 dpr 生成 `QPixmap`，`setDevicePixelRatio(dpr)`，避免跨屏切换糊。

### 9.2 形状表

| 名称 | 盒 | 形状 | 线宽 |
|---|---|---|---|
| 后退 | 16 | 7×12 chevron ‹ | 1.6，圆端点 |
| 前进 | 16 | 7×12 chevron › | 1.6，圆端点 |
| reload | 16 | 300° 圆弧 r5.2 + 3.5 箭头 | 1.4 |
| 搜索 | 16 | 圆 d7 + 4.5 手柄（45°） | 1.4 |
| `+` | 16 | 10×10 十字 | 1.4 |
| 标签关闭 | 16 | 7×7 × | 1.2 |
| 齿轮 | 16 | r5.2 外圆 + 8 齿 + r2.2 内圆 | 1.2 |
| 标题最小化 | 46×30 | 10×1 横线 | 1.0，不 AA |
| 标题最大化 | 46×30 | 10×10 方框 | 1.0，不 AA |
| 标题还原 | 46×30 | 两个 8×8 方框错位 2 | 1.0，不 AA |
| 标题关闭 | 46×30 | 10×10 × | 1.2，不 AA |
| 崩溃 | 40 | 24×30 页面轮廓 + 折角 + 裂纹折线 | 1.6 |
| 墓碑暂停 | 48 | 两条 6×18 圆角竖条，间距 6 | 填充 |
| spinner | 14 | 270° 圆弧，12fps 旋转 | 2.0 |

**禁止**彩色 emoji（`⚠`）、系统 `QStyle::standardIcon`、彩色点。

---

## 10. 颜色、字体、间距（token）

### 10.1 浅色（默认）

| token | 值 | 用途 | 对比 |
|---|---|---|---|
| `chrome.base` | `#F7F7F7` | 工具栏/活动标签 | — |
| `chrome.strip` | `#ECECEC` | 标签条 | — |
| `page.bg` | `#FFFFFF` | 页面/状态页 | — |
| `tab.inactive` | `#DEDEDE` | 非活动标签 | — |
| `tab.hover` | `#E6E6E6` | 悬停 | — |
| `tab.pressed` | `#D9D9D9` | 按下 | — |
| `border.strong` | `#C9C9C9` | 外框/活动标签/控件边 | — |
| `border.strip` | `#D5D5D5` | 标签条底 | — |
| `border.toolbar` | `#E2E2E2` | 工具栏底 | — |
| `border.control` | `#C2C2C2` | 输入框 | — |
| `divider` | `#CFCFCF` | 分段导航/标签间 | — |
| `text.primary` | `#1F1F1F` | 主文字/活动标签 | 15.39:1 on base |
| `text.secondary` | `#5A5A5A` | 非活动标签/副文案 | 6.44:1 on base |
| `text.placeholder` | `#767676` | placeholder | 4.54:1 on white |
| `text.disabled` | `#A8A8A8` | 禁用图标 | 装饰性豁免 |
| `icon` | `#3C3C3C` | 图标 | 10.30:1 on base |
| `hover` | `rgba(0,0,0,.06)` | 悬停底 | — |
| `pressed` | `rgba(0,0,0,.10)` | 按下底 | — |
| `focus` | `#2F6FD0` | 焦点环 | 4.55:1 on base / 4.88:1 on white |
| `focus.ring` | `rgba(47,111,208,.25)` | 辅助环 | — |
| `close.hover` | `#C42B1C` | 关闭悬停 | 白图标 5.66:1 |
| `close.pressed` | `#B21B14` | 关闭按下 | 白图标 6.86:1 |
| `selection` | `#B3D7FF` | 文本选择 | — |
| `status.icon` | `#5A5A5A` | 状态页图标 | 6.44:1 |
| `status.sub` | `#6E6E6E` | 状态副文案 | 5.10:1 on white |

### 10.2 深色（token 先定，v1 可先只上浅色）

| token | 值 | 备注 |
|---|---|---|
| `chrome.base` | `#2A2A2A` | 文字 `#F2F2F2` 12.82:1 |
| `chrome.strip` | `#232323` | — |
| `tab.inactive` | `#2F2F2F` | 文字 `#B8B8B8` 7.24:1 |
| `tab.hover` / `pressed` | `#383838` / `#404040` | — |
| `border.strong` | `#3C3C3C` | — |
| `border.strip` | `#1B1B1B` | — |
| `border.toolbar` | `#3A3A3A` | — |
| `border.control` | `#4A4A4A` | 输入框底 `#1B1B1B`，placeholder `#9A9A9A` 6.12:1 |
| `text.primary` | `#F2F2F2` | — |
| `text.secondary` | `#B8B8B8` | — |
| `icon` | `#E6E6E6` | 11.50:1 |
| `hover` / `pressed` | `rgba(255,255,255,.08)` / `.14` | — |
| `focus` | `#6AA8FF` | 5.92:1 on base |
| `close.hover` / `pressed` | `#C42B1C` / `#B21B14` | 同浅色 |
| `page.bg` | `#FFFFFF` | **页面仍是白的**（引擎不参与深色），文档中注明限制 |

> 深色实现方式：启动时读一次 `HKCU\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize\AppsUseLightTheme`（或 `RegGetValueW`），`WM_SETTINGCHANGE("ImmersiveColorSet")` 时重建 QSS 并 `update()`。**v1 可只交付浅色**，但所有颜色必须走 token，禁止散落 hex。

### 10.3 字体

| 用途 | 字号（逻辑） | 字重 |
|---|---|---|
| 标签 | 12px | 活动 500（Medium），非活动 400 |
| 地址/搜索 | 12px | 400 |
| 菜单 | 12px | 400 |
| 状态页标题 | 14px | 600 |
| 状态页副文案 | 12px | 400 |
| 崩溃页文案 | 13px | 400（保持原文案，见 §13） |
| 工具栏 tooltip | 系统 QToolTip | — |

- 字体族用 `QApplication::font()`（中文 Windows 下即系统 UI 字体，通常是 Microsoft YaHei UI）；不要硬编码 "Segoe UI"。
- 1.5x 下 12 逻辑 = 18 物理，清晰。

### 10.4 1px 边框与"不糊"

- QSS 的 `1px` 在 1.5x 下由 Qt 画成 1.5 设备 px，会 AA 成 2 行（现状 D7）。可接受，但**自绘**控件要设备栅格对齐：

```cpp
static QRectF snapRect(QRectF r, qreal dpr) {
    return QRectF(QPointF(qRound(r.x()*dpr), qRound(r.y()*dpr)) / dpr,
                  QPointF(qRound((r.x()+r.width())*dpr), qRound((r.y()+r.height())*dpr)) / dpr - QPointF(qRound(r.x()*dpr), qRound(r.y()*dpr)) / dpr);
}
```

- 需要"1 设备像素"的分隔线：`pen.setWidthF(1.0 / dpr)`，并画在 `(qRound(x*dpr)+0.5)/dpr`。
- 输入框/按钮的 1px 边框用 QSS 即可；不要在 QSS 里用 0.5px。

### 10.5 间距

- 8px 栅格：区块间距、工具栏左右边距、控件间隔。
- 4px：控件内部 padding、圆角。
- 6px：标签条内 `+`/标题按钮前的小间距（经典 Safari 节奏）。
- 7px：**仅**页面左右留白（回归硬约束，不参与栅格化）。
- 4px：页面底部留白（硬约束）。

---

## 11. 窗口行为：拖动、双击、最小化/最大化/关闭

### 11.1 区域规则（必须逐条实现）

| 区域 | 左键按下 | 双击 | 说明 |
|---|---|---|---|
| 标签条空白（标签右侧、`+` 左侧） | `startSystemMove()` | 最大化/还原 | 现有 QTabBar 空白分支保留，抽公共函数 |
| 工具栏空白（控件之间/右侧） | `startSystemMove()` | 最大化/还原 | 现有 `DragStrip`，补手动双击检测 |
| 标签本体 | 选中/开始拖拽换序 | **无窗口动作** | 不得触发最大化 |
| 标签关闭 / `+` / 导航 / 地址 / 搜索 / reload / 齿轮 / 标题按钮 | 控件动作 | **无窗口动作** | 标题按钮双击只按一次生效 |
| 页面区 | 页面交互 | 页面交互 | 永不拖窗口 |
| 边缘 6 逻辑 px（标题按钮区除外） | 系统缩放 | — | §12 |
| 全屏 | 不拖、不双击最大化 | — | 避免全屏下误操作 |

### 11.2 拖拽/双击的统一实现（修 D8）

- 公共判定（两种空白区共用）：

```cpp
bool isCaptionDoubleClick(const QPoint &pos)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const bool dbl = m_lastCaptionClick > 0
        && now - m_lastCaptionClick <= QApplication::doubleClickInterval()
        && (pos - m_lastCaptionPos).manhattanLength() <= QApplication::startDragDistance();
    m_lastCaptionClick = dbl ? 0 : now;
    m_lastCaptionPos = pos;
    return dbl;
}
```

- 按下时：先判双击 → 是则 `showMaximized()/showNormal()` 并 `return`；否则 `windowHandle()->startSystemMove()`。
- **为什么手动**：`startSystemMove()` 会进入系统移动循环，吞掉 Qt 的 `mouseDoubleClickEvent`（HANDOVER 已实证）；两条空白区必须同一套逻辑。
- 双击目标只认空白区域；标签/按钮上的双击被控件先吃掉。
- 拖动标签（`QTabBar` movable）时不得同时 `startSystemMove`。
- 禁止用 `WM_NCHITTEST → HTCAPTION` 代替（HANDOVER 实测在该 frameless 窗口不移动；且会抢掉子控件点击）。

### 11.3 最小化/最大化/关闭

- 标题按钮 → 同 §7.3；`showMaximized()` 后标题按钮图标变"还原"，`showNormal()` 复原。
- 最大化/还原/全屏时，`changeEvent` 里更新图标并 `update()`；不要重建控件。
- 关闭：`close()` → 现有 `closeEvent` 非阻塞杀 renderer；标题按钮/Alt+F4 同一路径。
- 双击标题条（空白）→ 与系统"双击标题栏最大化"一致；若主人关掉了系统该行为，仍按此实现（Windows 默认如此），文档记录。
- `setMinimumSize(720, 480)`（逻辑）；`WM_GETMINMAXINFO` 由 Qt 处理；标题按钮在 720 宽时仍完整可见（§7.2）。
- 最大化时页面原点在客户区坐标系仍是 `(11,87)`；`resize_probe` 已覆盖最大化/还原。

### 11.4 全屏

- F11 现有行为保留；chrome 保留（58 逻辑），页面原点仍 87 物理。
- 全屏时禁用拖动/双击/边缘缩放，标题按钮可保留（图标正确）；Esc 退出全屏？现有只 F11，保持不变（可选加 Esc，但会与页面冲突，不做）。

---

## 12. 边缘缩放：推荐方案与取舍

### 12.1 事实

- 现在：`WS_POPUP`、无 `WS_THICKFRAME`，`WM_NCHITTEST` 返回 `HTLEFT` 等没有意义 → 拖边框不缩放（HANDOVER §5aj、本机复测 delta=0）。
- 目标：原生实时缩放、Aero Snap、最大化/还原动画、DWM 阴影；**客户区原点/大小不变**（页面原点 11,87 与 1.5 DPI 不变）。

### 12.2 推荐：R1 — 保留 frameless + 注入 `WS_THICKFRAME` + 吃掉非客户区

1. **保留** `Qt::FramelessWindowHint`（Qt 认为 frame margin = 0，客户区原点稳定）。
2. 首次 `showEvent` 时：

```cpp
LONG_PTR st = GetWindowLongPtrW(hwnd, GWL_STYLE);
SetWindowLongPtrW(hwnd, GWL_STYLE, st | WS_THICKFRAME);
SetWindowPos(hwnd, nullptr, 0,0,0,0,
             SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE|SWP_FRAMECHANGED);
```

3. `nativeEvent` 处理 `WM_NCCALCSIZE`（wParam=TRUE）：

```cpp
case WM_NCCALCSIZE:
    if (msg->wParam) {
        auto *p = reinterpret_cast<NCCALCSIZE_PARAMS *>(msg->lParam);
        if (IsZoomed((HWND)winId())) {           // 最大化：扣掉隐形 sizing border
            const int pad = GetSystemMetrics(SM_CXSIZEFRAME)
                          + GetSystemMetrics(SM_CXPADDEDBORDER);
            p->rgrc[0].left += pad;  p->rgrc[0].top    += pad;
            p->rgrc[0].right -= pad; p->rgrc[0].bottom -= pad;
        }
        *result = 0;
        return true;                              // 客户区 = 窗口矩形
    }
    break;
```

4. **命中测试（保留显式分支，但要按 DPI 修）**：`WS_THICKFRAME` 就位后，**继续由 `WM_NCHITTEST` 显式返回** `HTLEFT/HTRIGHT/HTTOP/HTBOTTOM/HT*`（这样路径确定、不依赖 DefWindowProc 对 `WS_POPUP+WS_THICKFRAME` 的默认行为）。把现在的 `const int border = 6;`（物理 px）改成：
   ```cpp
   const int border = qRound(6.0 * devicePixelRatioF());   // 6 逻辑 px
   ```
   覆盖规则：
   - 标题按钮区域（物理 `x ≥ W*dpr - 138*dpr && y < 30*dpr`）→ `HTCLIENT`（按钮优先，否则点按钮上沿会变成 resize）；
   - 最大化/全屏 → `HTCLIENT`（不要边框缩放）；
   - 其余四边/四角按 6 逻辑 px（角 8 逻辑）返回 `HT*`。
   - 顶部标签条其余区域在顶 6 逻辑 px 内是 resize；标签命中 `y=14.7` 逻辑不受影响。
   - 备选：若实测确认 `WS_POPUP+WS_THICKFRAME` 下 DefWindowProc 默认命中可靠，可删掉手写分支；**默认建议显式**，便于与标题按钮/最大化状态做排除。
5. 保留现有 `WM_ENTERSIZEMOVE/WM_EXITSIZEMOVE` 的 live-resize 合并逻辑（并把缺失的 `break;` 补上）；缩放结束只提交一次最终尺寸。
6. `SetWindowPos(..., SWP_FRAMECHANGED)` 后重新 `applySquareFrame()`（DWM 属性不丢，但保险）。
7. 验收：style 含 `WS_THICKFRAME`；`GetClientRect == GetWindowRect`（拖动前后一致）；从左/右/上/下/角拖动，宽高变化 ≥50；页面原点仍 `(11,87)`；Aero Snap 半屏；最大化后客户区 = 工作区且无 8px 裁切。
8. 已知风险：Qt 可能在后续 `setGeometry`/状态切换时按新 style 重算 frame margin。**第一步先只加 style + NCCALCSIZE 构建验证**；若出现客户区偏移或窗口跳动，改 R2。

### 12.3 备选：R2 — 普通窗口（去掉 FramelessWindowHint）+ `WM_NCCALCSIZE` 吃掉整个非客户区

- 保留 `WS_OVERLAPPEDWINDOW` 的系统能力（缩放/阴影/动画/Snap），`WM_NCCALCSIZE` 返回 0（最大化同上 pad），Qt 的 frame margin 由 `WM_NCCALCSIZE`/`WM_GETMINMAXINFO` 得出。
- 风险：Qt 可能报告非零 frame margin，导致客户区尺寸/原点与 1.5 DPI 下 `(11,87)` 不一致；需要实测。
- 只在 R1 验证失败时使用。

### 12.4 明确拒绝

| 方案 | 为什么拒绝 |
|---|---|
| 只改 `WM_NCHITTEST` 返回 `HT*`（现状） | 无 `WS_THICKFRAME`，系统不缩放（已实测 delta=0） |
| 纯 Qt 手写 `SetWindowPos` 缩放循环 | 无 Snap/动画、缩放抖动、要自己处理 capture/DPI/多屏，风险与收益倒挂 |
| 返回 `HTCAPTION` 让系统拖动 | HANDOVER 实测此窗口不移动；且会抢走子控件点击 |

### 12.5 缩放热区

- 边框 6 逻辑 px（1.5x = 9 物理）：左/右/上/下 + 四角；四角 8 逻辑。
- 底部 6 逻辑会压到页面底部 4px 留白 2px，可接受（原实现是 6 物理 px = 4 逻辑，正好等于留白；现在是 6 逻辑）。
- 标题按钮 46×30 区域整体排除（见 R1 第 4 步）。
- 最小化/最大化状态、全屏：不返回缩放命中。

---

## 13. 状态页视觉语言（崩溃/恢复/挂起/加载）

统一原则：**页面区域内的状态页 = 白底 + 居中窄列 + 单色线性图标 + 13/12px 中性文字 + 4px 圆角按钮**；与外壳同一套 token；**不用 emoji、不用系统默认 QPushButton、不用彩色**。

### 13.1 共享组件（建议 `StatusPage`）

```
StatusPage (QWidget, bg page.bg)
 └ QVBoxLayout, 居中, spacing 0
    ├ 图标/ spinner（40×40；恢复态 14×14 spinner）
    ├ 16px
    ├ 标题 QLabel（14px, 600, text.primary）
    ├ 6px
    ├ 副文案 QLabel（12px, 400, text.secondary/status.sub）
    ├ 20px
    └ 主按钮（仅崩溃态：100×28）
```

- 列宽 `360` 逻辑、居中；文本 `Qt::PlainText`；不换行/不截断（文案短）。
- 状态页容器 `setObjectName("statusPage")`，QSS 背景取 token；不要透明叠在白页上导致闪。
- 页面切换时（恢复→渲染容器）两边都是白底，避免白闪。

### 13.2 四种状态

| 状态 | 触发（现有/并发改动） | 视觉 | 文案（**保持现有字符串**） |
|---|---|---|---|
| 恢复中 | `startRecoveryRetry()`（首次自动恢复、清缓存后再试、手动重试） | 14px spinner + 标题 + 副文案 | 标题"正在恢复页面…"；副文案"正在重新创建渲染进程并重载当前页面"；tab 标题"正在恢复…" |
| 崩溃（终态） | 反复失败后 `makeCrashWidget()` | 40px 页面裂纹图标 + 单标签两行文案 + "重新加载"按钮 | 保持现有整串：`页面渲染进程已崩溃\n点击重新加载`；tab"已崩溃"；窗口标题"已崩溃 - KHtmlLite" |
| 挂起（墓碑） | `TombstoneWidget`（内存/空闲驱逐） | 快照（KeepAspectRatio）+ 白纱 `rgba(255,255,255,.72)` + 48px 暂停图标 + 标题 + 副文案 | 副文案保持"（已挂起 · 点击标签恢复）"；tab 标题带 "[挂起]" 后缀 |
| 加载中（可选 P2） | 仅当 Lead 拿到真实 `loadStarted/firstFrame` 信号时 | 14px spinner + "正在加载…" | **当前没有加载开始信号**，不要伪造；恢复态就是现有的"加载式"占位 |

### 13.3 崩溃页细则

- 图标：40px 页面轮廓 + 裂纹，`status.icon`；**删除 `⚠` emoji**。
- 文案：为兼容 `tests/crash_recovery_probe.py` 可能按整串匹配，**默认保留单个 QLabel 的原文案**（仅改字号 13px、色 `#4A4A4A`）；若 Lead 确认探针只按按钮文本匹配，可拆成两行两层字号（标题/副文案）。
- 按钮：`100×28`，4px 圆角，底 `#EDEDED`、边 `#C6C6C6`、文字 12px `#1F1F1F`；hover `#E3E3E3`；pressed `#D8D8D8`；focus 2px `#2F6FD0`；文案"重新加载"（**不改**）；accessible name"重新加载页面"。
- 布局：图标 40 → 16 → 标签 → 20 → 按钮；整列居中，不贴边。
- 深色：图标 `#C8C8C8`、文字 `#F2F2F2`、按钮 `#333/#4A4A4A`。

### 13.4 恢复页细则

- 在并发改动现有 `makeRecoveryWidget()` 基础上去掉 inline `setStyleSheet`，改用 token；标题/副文案字号保持 14/12、色 `#333/#6E6E6E`。
- 加 14px spinner（`QPropertyAnimation`/`QTimer` 33ms，仅在 visible 时转；`SPI_GETCLIENTAREAANIMATION==FALSE` 时静态 270° 弧）。
- 无按钮（恢复由 `ready()`/`firstFrame()` 自动完成，现有逻辑不变）。
- 过渡：`finishRecovery()` 换入容器时保持白底；不额外加淡入（避免"动效优先于机制"）。

### 13.5 墓碑细则

- 快照缩放保持现有 `KeepAspectRatio + SmoothTransformation`；留白用 `#F2F2F2`。
- 白纱从 `QColor(255,255,255,60)` 改为 `rgba(255,255,255,.72)`（文字可读；深色为 `rgba(24,24,24,.72)`）。
- 图标 48px 暂停条；标题 13px `#3A3A3A`；副文案 12px `#7A7A7A`；去掉 12pt 的 `setPointSize`（与外壳 px 字体体系一致）。
- 点击整个墓碑区域恢复（现有 tab 点击路径）；tab 命中不受影响。

---

## 14. 无障碍、键盘与对比度

### 14.1 键盘

| 快捷键 | 动作 | 现状 |
|---|---|---|
| Ctrl+L | 聚焦并全选地址栏 | **新增** |
| Ctrl+T | 新标签 | **新增**（`+` 已存在） |
| Ctrl+R | 重新加载 | **新增**（reload 按钮已存在） |
| Ctrl+W | 关闭当前标签 | 现有 |
| Ctrl+Shift+Del | 清除缓存 | 现有（移入齿轮菜单同时保留快捷键） |
| Alt+← / Alt+→ | 后退/前进 | **新增** |
| F11 | 全屏 | 现有 |
| Esc | 地址栏恢复当前 URL 并还焦点给页面 | **新增** |
| Tab / Shift+Tab | 导航→地址→reload→搜索→齿轮→+→标签栏→页面 | **新增焦点顺序** |
| Enter/Space | 激活聚焦控件 | Qt 默认 |

- 焦点顺序用 `setTabOrder` 显式固定；标题按钮不进 Tab（原生约定）。
- 页面重新获得焦点的路径不变（`PageSurface` StrongFocus）。

### 14.2 焦点可见性

- 2px `#2F6FD0`（深色 `#6AA8FF`），圆角随控件 4px；输入框用 2px 边框 + padding 补偿。
- 焦点指示必须**同时**改变宽度（1px→2px）与颜色，不能只靠颜色。
- 高对比度模式（`SPI_GETHIGHCONTRAST`）：边框/文字用 `QPalette::WindowText`，焦点用 `QPalette::Highlight`，去掉自定义半透明 hover；布局与命中不变。

### 14.3 对比度（实测计算）

| 组合 | 比值 | 达标 |
|---|---|---|
| `#1F1F1F` / `#F7F7F7` | 15.39:1 | AAA |
| `#555555` / `#DEDEDE`（非活动标签） | 5.54:1 | AA |
| `#5A5A5A` / `#F7F7F7` | 6.44:1 | AA |
| `#767676` / `#FFFFFF`（placeholder） | 4.54:1 | AA |
| `#2F6FD0` / `#F7F7F7`（焦点） | 4.55:1 | AA（非文本 3:1） |
| `#FFFFFF` / `#C42B1C`（关闭悬停） | 5.66:1 | AA |
| `#F2F2F2` / `#2A2A2A` | 12.82:1 | AAA |
| `#9A9A9A` / `#1B1B1B`（深色 placeholder） | 6.12:1 | AA |
| `#6AA8FF` / `#2A2A2A`（深色焦点） | 5.92:1 | AA |
| `#A8A8A8`（禁用） | — | 装饰性豁免 |

- **旧 placeholder `#8A8A8A` 对白 3.45:1 不达标**，必须换 `#767676`。

### 14.4 可访问名称

- 每个图标按钮：`setAccessibleName` + tooltip（含快捷键）；`+`、reload、齿轮、导航段、标题按钮、搜索、地址。
- 状态页容器 accessible name "页面状态"；崩溃按钮"重新加载页面"。
- QTabBar 标签文字天然可读；关闭按钮不作为独立 Tab 焦点（在标签内用 Delete？不做，保持鼠标/Ctrl+W）。

### 14.5 命中尺寸

| 控件 | 逻辑 | 1.5x 物理 | 评价 |
|---|---|---|---|
| 标题按钮 | 46×30 | 69×45 | ≥ AA 24×24，接近原生 |
| 导航段 | 24×24 | 36×36 | 达标 |
| 图标按钮 | 24×24 | 36×36 | 达标 |
| 标签 | ≥144×30 | 216×45 | 达标 |
| 标签关闭 | 24×24 | 36×36 | 达标 |
| reload（输入框内） | 22×22 | 33×33 | 内嵌例外，可接受 |

---

## 15. 回归坐标：无需改动，但实现不能越线

### 15.1 保留表（1.5x 物理）

| 探针/约束 | 坐标 | 新布局下的命中 | 必须满足 |
|---|---|---|---|
| `navigation_probe.py` BACK_BTN | `(34,66)` = 逻辑 `(22.7,44)` | 后退段 `[8,32]×[32,56]` | 导航从 x=8 起、每段 24 宽、y 32..56 |
| `navigation_probe.py` FORWARD_BTN | `(68,66)` = 逻辑 `(45.3,44)` | 前进段 `[32,56]×[32,56]` | 同上 |
| `tombstone_probe.py` | `(170,20/22/25/28)` = 逻辑 `x≈113.3–117.3, y≈13.3–18.7` | tab0 `[8,≥152]×[0,30]`，标题区到 125 | 标签起点 8、最小宽 144、关闭热区 `[right-27,right-3]` |
| `engine_semantics_probe.py` CONTENT_TOP / 页面原点 | `(11,87)` | pageHost 左 7、chrome 58 | 不改 7、不改 58、客户区=窗口矩形 |
| 地址栏中心 | `y=66` = 逻辑 44 | 地址栏 `y32..56`，中心 44 | 工具栏内容中心 44 |
| `resize_probe` 页面识别 | 顶部 clamp ~100 逻辑 | chrome 58 < 100 | 不改页面尺寸（W-14 × H-62） |

### 15.2 明确声明

- **现有 20 个探针的坐标不需要改**（前提：§15.1 的几何红线全部满足）。
- **以下任一情况才算"必须改测试坐标"**（此时要在交付说明里逐条列出新坐标与原因）：
  1. chrome 总高 ≠ 58 逻辑（页面原点 y 变）；
  2. 页面左留白 ≠ 7 逻辑（页面原点 x 变）；
  3. 客户区原点 ≠ 窗口原点（出现非客户区）；
  4. 导航起点 > 22.7 逻辑、或后退/前进段不覆盖 `(22.7/45.3,44)`；
  5. tab0 起点 > 105 逻辑、宽度 < 137 逻辑、或关闭热区覆盖 x=113.3 逻辑；
  6. 工具栏内容中心 ≠ 44 逻辑（地址 `y=66` 落空）。
- 新行为（标题按钮、边缘缩放、方角、焦点环、hover）**新增探针**，不改旧探针：
  1. `caption`: 点最小化→`IsIconic`；点最大化→`IsZoomed`；再点还原；点关闭→进程退出（用本地页面，不联网）。
  2. `resize`: 左/右/上/下/四角各拖 120 物理 px，宽高变化 ≥50；`GetClientRect` 与 `GetWindowRect` 在拖动前后均相等；页面原点仍 `(11,87)`。
  3. `frame`: `GWL_STYLE & WS_THICKFRAME`；`DwmGetWindowAttribute(33)==1`；最大化后客户区 = 工作区、页面原点仍 `(11,87)`。
  4. `chrome`: 悬停标题按钮取色 `#E5E5E5`、关闭悬停 `#C42B1C`；chrome 区无饱和彩色像素（`engine_semantics` 的 chrome 排除逻辑更安全）。
  5. `focus`: Tab 到地址/搜索/按钮，截图存在 2px `#2F6FD0` 环。
  6. `drag`: 空白标签条/工具栏拖动窗口 rect 变化；双击最大化/还原；双击标签**不**最大化（沿用 HANDOVER 的真实 SendInput 方法）。

### 15.3 回归执行

- 构建后先跑单测：`python tests\tile_protocol_test.py`（非 MSYS2 环境）。
- 全量顺序回归（约 46 分钟，20 项）：
  `pwsh -File tests\run_regression.ps1`；离线可 `-Skip bing_ghost_check.py`。
- 关键项看：`navigation_probe`、`tombstone_probe`（623s）、`resize_probe`（601s）、`engine_semantics_probe`、`dpr_probe`（dpr=1.5）、`input_coord_probe`、`title_probe`、`tile_protocol_test`。
- 报告写 `tests/regression_report.txt`（脚本自动）；`bing_ghost_check` 允许 INCONCLUSIVE。

---

## 16. 实现落位（Lead 可直接照此改）

### 16.1 控件树/类

| 类/对象 | 角色 | 备注 |
|---|---|---|
| `KHtmlLiteWindow` | 宿主、拖拽/双击、窗口状态、状态页切换 | 保留现有业务 |
| `ChromeStrip`（QWidget） | 30 高、满幅、画底色/底边 | 原 `DragStrip` 可复用 |
| `ChromeToolbar`（QWidget） | 28 高、满幅 | 原 `DragStrip` 可复用 |
| `SafariTabBar` | 自绘标签 + 关闭 + 空白拖拽 | 保留 `tabAt/tabMoved/contextMenu`；改绘制与尺寸 |
| `CaptionButton` | 46×30，四种 glyph | 新增；`QAbstractButton` |
| `NavControl` | 48×24 分段（内含 2 个 24×24 `ChromeButton`） | 新增 |
| `ChromeButton` | 24×24（或 22）图标按钮 | 新增；hover/pressed/focus 自绘 |
| `AddressBar`（QLineEdit 子类或容器） | 地址 + 内嵌 reload | 新增 |
| `SearchField`（QLineEdit 子类） | 左侧放大镜 | 新增 |
| `EdgeBorder` | 1px 内描边 overlay | 新增 |
| `StatusPage` | 崩溃/恢复 | 新增；墓碑沿用 `TombstoneWidget` 改绘制 |
| `ChromePalette` | 颜色 token 结构体 | 新增；QSS 由它生成 |

**不新增文件**：全部放在 `src/host/khtml_browser.cpp` 内（CMake 已有 AUTOMOC，无需改构建）。

### 16.2 函数级改动清单

- `KHtmlLiteWindow()`：`setMinimumSize(720,480)`；`applyRoundedCorners` → `applySquareFrame`；保留默认尺寸算法（frameW/H 预算不改，保持 1805×927）；注释改成"客户区=窗口矩形"。
- `buildChrome()`：按 §4.2 改 root margins；重建 strip/toolbar；删除 `mkLight` 三个红绿灯、侧边栏、分享；加标题按钮/搜索/齿轮；`+` 保留；`m_addr` 改 `AddressBar`；`m_reload` 移入地址栏。
- `safariStyleSheet()` → `chromeStyleSheet(const ChromePalette&)`：token 化；新增菜单/状态页/按钮规则；删除旧 8px 胶囊规则与 `margin:0 6px`（QSS margin 会挪输入框几何，D5 的诱因之一）。
- `chromeGlyph()`/`mkToolButton()`/`mkGlyphButton()`：删除/替换为 `ChromeButton` 的矢量绘制。
- `SafariTabBar`：`tabSizeHint` 高 30、宽 `qBound(144, text+24, 240)`；`paintEvent` 按 §7.4；关闭热区 24×24；滚动按钮重绘。
- `DragStrip`：补手动双击检测，或并入宿主统一函数。
- `nativeEvent`：加 `WM_NCCALCSIZE`；`WM_NCHITTEST` 按 §12.2 交给默认（只覆盖标题按钮/最大化全屏）；补 `break;`；`WM_GETMINMAXINFO` 可选。
- `showEvent`（首次）：注入 `WS_THICKFRAME` + `SWP_FRAMECHANGED`；再 `applySquareFrame()`。
- `changeEvent`：`WindowStateChange` 更新标题按钮图标；`ScreenChangeInternal` 重绘/更新 dpr 缓存。
- `resizeEvent`：`EdgeBorder` 跟随；无布局副作用。
- `makeCrashWidget()`/`makeRecoveryWidget()`：改用 `StatusPage` + token；文案不动。
- `TombstoneWidget`：按 §13.5 改绘制。
- 新增 QAction：Ctrl+L/T/R、Alt+←/→（`WindowShortcut`）。
- 保留：`goBack/goForward/updateNavButtons/syncChrome/resolveInput/onAddressEntered/addTab/closeTab/retryCrashedTab/resumeTab/suspendTab/...` 全部业务不动。

### 16.3 QSS 骨架（浅色；深色替换 token）

```css
QWidget#central            { background: #FFFFFF; }
QWidget#chromeStrip        { background: #ECECEC; border-bottom: 1px solid #D5D5D5; }
QWidget#chromeToolbar      { background: #F7F7F7; border-bottom: 1px solid #E2E2E2; }
QStackedWidget#pageStack   { background: #FFFFFF; }
QLineEdit#addressField, QLineEdit#searchField {
    background: #FFFFFF; border: 1px solid #C2C2C2; border-radius: 4px;
    padding: 3px 10px; color: #1F1F1F; font-size: 12px;
    selection-background-color: #B3D7FF; selection-color: #1F1F1F;
}
QLineEdit#addressField     { padding-right: 32px; }
QLineEdit#searchField      { padding-left: 26px; }
QLineEdit#addressField:focus, QLineEdit#searchField:focus {
    border: 2px solid #2F6FD0; padding: 2px 9px;
}
QLineEdit#addressField:focus { padding-right: 31px; }
QLineEdit#searchField:focus  { padding-left: 25px; }
QPushButton#crashRetry {
    background: #EDEDED; border: 1px solid #C6C6C6; border-radius: 4px;
    color: #1F1F1F; font-size: 12px; padding: 0 16px; min-height: 28px;
}
QPushButton#crashRetry:hover    { background: #E3E3E3; }
QPushButton#crashRetry:pressed  { background: #D8D8D8; }
QPushButton#crashRetry:focus    { border: 2px solid #2F6FD0; }
QMenu {
    background: #FFFFFF; border: 1px solid #C9C9C9; border-radius: 4px; padding: 4px;
    color: #1F1F1F; font-size: 12px;
}
QMenu::item            { padding: 5px 26px 5px 10px; border-radius: 4px; }
QMenu::item:selected   { background: #EDF2FB; }
QMenu::separator       { height: 1px; background: #E5E5E5; margin: 4px 6px; }
QWidget#statusPage     { background: #FFFFFF; }
```

> `QLineEdit` 的 QSS `padding` 会参与控件几何（当前 `margin:0 6px` 就把输入框整体内缩了 6px）；新实现只用布局的 `setContentsMargins` 控制位置，**QSS 不写 margin**。

### 16.4 构建/验证顺序（建议）

1. token + `ChromePalette` + QSS 生成；**不改外观**先构建通过。
2. R1 边框缩放（style + NCCALCSIZE + 方角）→ 先验证页面原点与拖动缩放，再继续 UI（这是最大风险项）。
3. root margin 满幅 + pageHost 7px（页原点验证：红块 bbox `(11,87)`）。
4. 标签条（标签重绘、`+`、标题按钮）。
5. 工具栏（导航、地址+reload、搜索、齿轮）。
6. 拖拽/双击统一；标题按钮状态同步。
7. 状态页（崩溃/恢复/墓碑）。
8. 快捷键/无障碍/焦点环。
9. 新探针 + 全量回归。
10. 每一步保持可构建、可截图；截图存 `docs/assets/` 需 Lead 决定（ui-spec 不写资产）。

---

## 17. 验收清单（Definition of Done）

### 17.1 视觉/几何

- [ ] 外窗四角为方角；DWM corner pref = 1；无圆角白边；chrome 满幅。
- [ ] 标签条 30 + 工具栏 28；页面原点 `(11,87)`（1.5x）。
- [ ] 右上角依次为 最小化/最大化/关闭（46×30），无红黄绿。
- [ ] 后退/前进是 48×24 分段控件；地址栏 64..W-224、内嵌 reload；搜索 176×24；齿轮 24×24。
- [ ] 选中/悬停/按下状态符合 §7.4/§8.7；图标单色线性；无 emoji/系统标准图标。
- [ ] 1px 描边/分隔线在 150% 下不糊成粗灰带（设备栅格对齐）。
- [ ] 焦点环 2px `#2F6FD0`，可见；placeholder `#767676`。

### 17.2 行为

- [ ] 空白标签条/工具栏拖动移动窗口；双击最大化/还原；双击标签不最大化。
- [ ] 标题按钮：最小化/最大化(还原)/关闭全部有效；图标随状态切换；关闭走非阻塞清理。
- [ ] 四边/四角拖动可缩放；`GetClientRect == GetWindowRect`；Aero Snap 可用；最大化/还原无裁切、页面原点不变。
- [ ] F11 全屏、Ctrl+W、Ctrl+Shift+Del 行为不变；新增 Ctrl+L/T/R、Alt+←/→ 生效。
- [ ] 崩溃自动恢复（禁 JS 一次）→ 清缓存再试 → 崩溃页手动重试；墓碑挂起/恢复；tab 标题/窗口标题字符串不变。
- [ ] 20 个现有探针全绿（bing 可 INCONCLUSIVE），**坐标零改动**。

### 17.3 明确不变的既有行为

- [ ] 页面/渲染进程/引擎零改动；`khtml_renderer.exe` 不重建 UI。
- [ ] `khtml_browser.exe --clear-cache` 仍可用。
- [ ] 标签换序/右键菜单/`openNewTab`/内存压力驱逐/启动尺寸算法不变。

---

## 18. 风险与待决（Lead 拍板）

| # | 事项 | 建议 | 影响 |
|---|---|---|---|
| Q1 | 30+28（推荐）还是 26+32（原建议） | 30+28：标题按钮原生尺寸 + 地址中心精确 66 | 两者都保原点，只是观感/按钮高度 |
| Q2 | 地址+搜索双栏 vs 单栏"智能字段" | 双栏（经典 Safari 层级）；地址仍可搜 | 双栏需要 SearchField 约 176px，窄窗仍够用（720 下地址 432） |
| Q3 | 下载/设置入口 | 当前无功能，齿轮菜单只放真实动作；功能落地后再显形 | 避免假控件；见 §2.5 |
| Q4 | 深色模式 | token 先定；v1 可只浅色（页面仍白，需在文档注明） | 深色需要 QSS 重建与系统主题监听 |
| Q5 | R1（frameless+WS_THICKFRAME）能否稳住 Qt frame margin | 先做最小验证（§16.4 第 2 步）；不行切 R2 | 影响边缘缩放实现路径 |
| Q6 | 崩溃页文案整串 vs 拆分 | 默认保整串（兼容探针）；Lead 确认 task-2 探针匹配方式后再拆 | 仅视觉层级 |
| Q7 | 是否加"非激活窗口图标变淡" | v1 不做 | 少一处状态复杂度 |
| Q8 | 是否加右键系统菜单 | P2（原生 `GetSystemMenu` + `TrackPopupMenu`） | 非必需 |

---

## 附录 A：本审计的可复现测量（只读）

```python
# 启动 build/host/khtml_browser.exe 打开一个 margin:0 的红块页面
# 用 tests/win_shot.py 的 find_window/grab + ClientToScreen 截图，然后：
style = USER32.GetWindowLongPtrW(hwnd, -16) & 0xFFFFFFFF
print(hex(style))                      # 0x960B0000（无 WS_THICKFRAME）
print(USER32.GetDpiForWindow(hwnd))    # 144
# DWMWA_WINDOW_CORNER_PREFERENCE (33) -> 2（当前圆角）
# 红块 bbox 左上 -> (11,87)
# WM_NCHITTEST 在窗口左/右/上/下 2px 返回 10/11/12/15，但拖动不缩放
```

实现后应变为：

```
style & 0x00040000  != 0        # WS_THICKFRAME
DwmGetWindowAttribute(33) == 1  # DONOTROUND
红块 bbox 左上 == (11,87)
GetClientRect == GetWindowRect（非最大化/非全屏）
```

## 附录 B：关键参考行为（经典 Safari 4/5 Windows）

- 标签在标题条内，Windows 最小化/最大化/关闭在最右上；
- 一条统一工具栏：后退/前进分段 → 地址 → reload → 搜索 → 齿轮；
- 活动标签与工具栏同色并"连"在一起，非活动标签更暗、更矮；
- 单色细线图标、浅灰底、1px 分隔（我们以 Flat 实现，不做玻璃/渐变）。

> 本文件只定义外壳视觉与交互，不含源码改动。实现由 Lead 在 `src/host/khtml_browser.cpp` 内完成；完成后按 §15/§17 验证，并在 `docs/HANDOVER.md` 追加一节（由 Lead 负责，ui-spec 不碰 HANDOVER）。
