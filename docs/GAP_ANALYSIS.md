# KHtmlLite 对 Safari/WebKit 机制的全面差距盘点（GAP ANALYSIS）

> 依据 `docs/ARCHITECTURE.md`（章程）的章节结构逐条对标。红线仍然生效：只学机制，
> 不抄一行 WebKit 代码。每条落到具体文件/函数；有测量引测量（来自
> `docs/HANDOVER.md`，2025-10-14 本机热跑），没测量标"需测量"。
> 对齐成本分档：**小** ≤ 1 天；**中** ≈ 1 周；**大** > 1 周或动协议/引擎。

## 0. 修复批注（2026-10-14，竞态/文件层大修）

本批修复了一起三层叠加的"页面白/图全丢"故障，全部有像素级实测
（tests/bg_smoke.py：红背景 tile + 320×240 绿图权重断言，1.4s 内 PASS）：

1. **PageLoader 子资源时序竞态** → 初始文档引用的 file:// 尚不存在，
   CachedImage::error() 状态永久卡死，且渲染进程从不二次加载最终文档。
   修：engine `DocLoader::needReload` 对 `hadError()` 条目视为过期驱逐
   （misc/loader.cpp）；PageLoader 全量下载完成后发 `refreshed` 信号，
   渲染进程保滚动/缩放重喂最终文档（旧行为是每个资源完成时把 200KB 文档
   全文 regex 重写一遍——O(资源数) 次全页重写，一并改为恰好一次）。
2. **KIO file:// 全军覆没** → kioslave5.exe 在 MSYS2 部署中不存在，
   KIO 的 D-Bus worker 模型本来就被判出范围，所有 file:// 子资源加载
   永久失败。修：misc/loader.cpp `Loader::scheduleRequest` 对本地文件
   走 QFile 直读快路径（保持 KIO 异步契约：延迟一事件循环拍派发）。
3. **离屏渲染无损伤信号** → 渲染 widget 挂 WA_DontShowOnScreen，
   QWidget::update() 变 no-op，首帧后 KHTML 内部 repaint 全部无声丢失，
   页面可能永远停白。修：KHTMLView 新增 `contentsUpdated(QRect)` 信号
   （viewport 坐标，zoom/scroll 变换后），renderer 直连
   `m_dirtyRegion += r; scheduleFrame()` —— 这就是 WebKit 的
   damage-rect 上传机制，顺带把 2.2"输入路径 invalidateFull 造假"
   过渡到真脏矩形。

附：build.sh 现部署 Qt imageformats 插件（qwebp/qico/qsvg/qgif/qjpeg）
到 build/host/imageformats/；PageLoader cacheRoot 改写探针选路
（TEMP → exe 旁兜底），MSYS2 shell 把 TMP 重写成 /tmp 时缓存不再丢失。
诊断开关：KHTML_PERF=1（PageLoader 全链路 PERF_MARK）、
KHTML_FRAME_DUMP=1（渲染进程落 frame_dump.png）、
tests/renderer_solo.py（无浏览器直驱渲染进程+帧颜色断言）。

## 0b. 修复批注②（同日，高优 gap 三项）

- **3.2 墓碑缩略图**：suspendTab 在杀进程前抓取最后已合成画布存入
  TabPage::snapshot；挂起态改为 TombstoneWidget（快照缩放绘制+淡化+标题）；
  resumeTab 用快照 seed 新 PageSurface，真实 tile 到达前用户始终有视觉锚点。
- **1.6 渲染进程预热**：RenderTab::prewarm() 空闲时（首次冷标签后 600ms）
  起一个 about:blank 待命进程并常驻监听；start() 命中时直接收养
  （warm-hit STARTUP 标记可量化），收养后立即补起待命顶替者；孤儿看门狗
  10 分钟无客户端自杀。冷路径实测从 ~500ms 级降为毫秒级收养。
- **2.1 后台帧钟节流**：新增 `visible` IPC；非当前标签渲染进程停 60Hz 帧钟
  （damage 累积），回前台时 invalidateFull+一帧。WebKit 对应：隐藏页面收不到
  DisplayRefreshMonitor tick。

## 0c. 修复批注③（2026-10-15，UI/渲染质量轮，全部实测）

- **①窗口放大/全屏贴合（resize presentation）**：PageSurface::paintEvent 在
  "已提交帧尺寸 ≠ 表面尺寸"期间改为把旧帧**平滑拉伸铺满**（WebKit2 UI 进程
  拉伸既有 backing store 的 Windows 对应实现），取代旧的 1:1 锚定+棋盘格；
  真实新尺寸帧静默替换拉伸帧。新建 tests/resize_probe.py 量化（SW_MAXIMIZE/
  SW_RESTORE + BitBlt 网格采样）：修复前"首个 >90% content 采样 = NEVER、
  120ms 后白/棋盘格最差 62.89%"；修复后 **content 达标 0.0s、白闪 0.00%、
  新帧落地 0.17-0.19s**，restore 0.09-0.14s。探针自坑修正记录：旧探针页用
  flex/linear-gradient/hsl()/JS 建 DOM（KHTML 全不吐，页面纯白）；rgba 边框
  ~22% 像素落 other 桶使 90% 阈值永不可达；采样区含工具栏（#f0f0f0 被算成
  checkerboard，基线 ~14%）。新页为纯静态表格+hex 色，并加"最大化前 content
  ≥85%"预检门，页面没渲染直接判 PROBE INVALID。
- **②滚动渲染不及时**：两处真实根因，均已修——(a) 渲染端把 QWheelEvent 发给
  viewport()，而 KHTMLView::viewportEvent() 对 Wheel 显式 return false，
  **40 格滚轮 0px 位移**（死路）；改发 content widget（view->widget()）走
  KHTMLView::eventFilter → widgetEvent → wheelEvent 正路。(b) 引擎侧平滑滚动
  （SSMWhenEfficient，每格 ~128ms 动画）与浏览器端预测滚动互相打架，收敛尾
  ~260ms；改 SSMDisabled，引擎一步到位，收敛交给预测+权威帧对账
  （WebKit async scrolling 对应：compositor 立即滚动，引擎从不自己动画滚轮）。
  scroll_smoke 修正为 HTTP 加载（file:// 顶层导航在本项目不可用）+ 纯静态
  色带页（旧页 JS 全是模板字符串，KJS ES3 直接 SyntaxError → 0 色带白页），
  实测 **PASS：滚轮真实位移（签名 before≠after）、回滚精确还原
  （restored==before，无动画残留）**。
- **③墓碑改系统压力驱动**：checkMemoryBudget 从固定 400MB 阈值改为
  GlobalMemoryStatusEx.dwMemoryLoad 四级状态机（对应 WebKit
  MemoryPressureHandler 分级）：normal<70% 不动 / pressure 70-85% 只清磁盘
  缓存 / warning 85-93% 清缓存+按 lastActive **真 LRU** 逐个挂起后台标签 /
  critical≥93% 每 tick 持续驱逐直到系统恢复。空闲 5 分钟驱逐独立保留
  （Safari 空 RAM 也丢弃长闲置标签）。chrome 标签显示"系统内存%+等级"。
- **⑤KJS 移除评估结论**：**执行路径其实已是 QuickJS-only**——kjs_proxy.cpp
  无 KJS::ScriptInterpreter、无回退引擎（"QuickJS is the only JavaScript
  engine"），kjs 库根本不参与构建（CMakeLists 无 kjs 引用）。本轮清除残留：
  (a) kjs_proxy 的 kjlog 原来每次脚本求值都 fopen 写 khtml_qjs.log——改为
  KHTML_QJS_TRACE=1 才开；(b) PageLoader 不再把脚本降级到 ES3"给 KJS 解析"
  ——QuickJS 原生 ES2020，内联+外链脚本默认原文直通（跳过 Bublé 换pass），
  外链 256KB 上限随之解除（仅保留 8MB 资源门），KHTML_TRANSPILE=1 可回退
  A/B；try/catch 护栏保留。bing 采集确认现代脚本真实执行（17s 处仍有 JS
  触发的 render pass），TTFF 1694ms 在历史区间（1514-1694）内。
- CPU 自适应帧钟尝试被否决（中间轮已论证）：损伤驱动的 60Hz tick 空转是纯
  early-out，弱机应"丢帧不降预算"（Safari 同款原则），保持固定 16ms，
  KHTML_FPS_MS 仅用于诊断。

## 0d. 修复批注（2026-10-16，墓碑系统负载驱动 + UI 三连环 + KJS 移除落地）

1. **3.4/4.5 内存压力模型已升级为系统事件驱动**（HANDOVER 5e）：本机常态
   物理内存 83-94% 实测 → 机械 400MB 阈值永远无法代表"系统紧张"。现为
   CreateMemoryResourceNotification(LowMemory) 内核事件线程（WebKit
   MemoryPressureHandler 的 Windows 对应）+ dwMemoryLoad 四级
   （≥70 清缓存 / ≥85 LRU 驱逐 / ≥93 全后台）+ GetSystemTimes 系统 CPU
   自适应空转超时（sysCpu≥85% → 60s，否则 5min）。事件"创建即 signaled"
   的坑用 15s 启动宽限解决（否则首页 PageLoader 缓存被清，bg_smoke 18.4s
   红屏）。真 LRU 已落地（lastActive 排序）。tombstone_probe 全绿。
2. **标题/_blank/file:// 三连环根因闭环**：PageLoader 预取并重写 `<a href>`
   为本地 sha1 副本（离线优先设计）——_blank 新标签因此拿到 file:///…cache
   路径 → KIO file 卡死 → 空标题、地址栏脏、purgeCache 后死链。修：PageLoader
   `originalOf()` 副本→原始 URL 反查，出口统一；file:// 主文档也走 PageLoader
   （QFile 读 + begin/write/end，弃 KIO）。3.2 的"恢复盯着占位标签 1s+"：
   resume 现仍无缩略图（下一批），但 URL/scroll/zoom/标题路径已净。
3. **⑥ KJS 移除落地**：src/khtml/src/bindings/ 全删（10 个从未编译的死文件：
   KJS SVG 定制绑定 + Perl 生成器）。kjs 库零参与构建维持；jsengine.h/
   quickjs_engine 是审计钦定的长期抽象故留；KJSProxy 命名残留文档化接受。
   needsFallback() = 优雅降级标志（非 KJS 回退），语义已正名。
   QuickJS 回归 PASS（file:// + document.title setter + inline handler 链路）。
4. **桥补齐**：window.innerWidth/innerHeight（KHTMLView::visibleWidth/Height
   活 getter，实测 W=638 H=478）。仍缺：setTimeout/setInterval、window.open、
   DPI 清晰度、F11。

---

## 0e. 修复批注（2026-10-17，系统监测与沙盒轮，全实测，本机 150% DPI 屏）

1. **1.4 渲染进程沙箱：Job Object 落地**（`RenderTab::assignJob`）——每渲染进程
   自有 Job Object：ProcessMemoryLimit=软预算×2（commit 硬顶，每 2s tick 动态
   调价）+ JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE（浏览器异常退出 → 内核即时回收
   全部渲染进程，10min 看门狗降为兜底）。软预算超限 → suspendTab 转墓碑
   （**不崩浏览器**），stderr `[tomb] renderer budget exceeded`。预算
   perRendererBudgetKB = 整机 RAM 档（≥16G→1G / ≥8G→768M / 否则 512M），
   系统压力 ≥85% 再减半。**1.4 从"重大差距"降级为"部分对齐"**：OS 级内存
   强制已闭环，restricted token / 完整性级别 / 文件系统白名单仍缺。
2. **③ GPU/CPU/内存实时监测**：PDH `\GPU Engine(*)\Utilization Percentage`
   （任务管理器同源）按引擎求和；无 GPU 计数器机器诚实降级为 0。状态栏
   "内存% MB | 内存x% CPUy% GPUz% (等级) · 后台‰"。调度决策位维持 CPU 自适应
   （GPU 未接规则——无实测收益不立规则，章程铁律）。
3. **3.2 墓碑缩略图闭环**：TombstoneWidget（快照缩放绘制 + 标题徽章 +
   "已挂起·点击标签恢复"）+ resumeTab seedFrame 预填首帧；tombstone_probe
   生命周期全绿（t+16s 驱逐、无自动复活、点击恢复、渲染进程存活）。
   3.5 持久化（冷重启回）仍缺。
4. **2.6 修正（checkerboard 已废除）**：Safari resize 呈现现为 WebKit2 连续性
   拉伸——失配期旧帧铺满 + framesize 提交"先整体拉伸旧画布再叠 1:1 搬运"
   （本轮修复的 maximize 半屏白闪真根因：渲染端 resize 后首帧部分瓦片把
   半白画布顶上屏，绕过 paintEvent 拉伸保护）。tile_protocol 静态断言禁止
   checkerBoard 回归。resize_probe 实测：maximize/restore 白闪 **0.00%**、
   内容 >90% @0.197s、settle 0.23s。
5. **桥定时器落地（0b 批注欠账清）**：setTimeout/setInterval/clearTimeout/
   clearInterval/requestAnimationFrame/cancelAnimationFrame 全家（global+window
   双挂，World 重置先杀定时器注册表）。连锁修复：CSS 动画 polyfill 硬依赖
   rAF（原先静默空转）、Promise shim 依赖 setTimeout（原先挂起态永不 resolve）。
   4.3 后台 JS 节流仍缺。
6. **DPI 清晰度（150% 屏）**：AA_EnableHighDpiScaling + 全物理像素管线
   （视口发 size×dpr、输入÷dpr、paintEvent `p.scale(1.0/dpr)` 设备像素 1:1
   直投）。像素级验证：色带 73-74 设备px（49 逻辑×1.5）、带内精确 hex 逐字
   面值、带界 1-2 行过渡。**真 deviceScaleFactor CSS-px 视口已在 0f 落地**
   （渲染端逻辑尺寸 KHTMLView + 物理画布 + QPainter scale(dpr)）。
7. **F11 全屏**（QAction 窗级注册）；**KJS 残留清零**：KJSProxy→ScriptProxy
   全仓改名（含 libkhtml.map）、js_transpile（Bublé ES3 降级机器）整体删除。
8. **探针工具链 DPI 化（方法论沉淀）**：DPI 无感知 harness 在 150% 屏抓到
   的是 DWM 降采样虚像（每像素 1.5×1.5 混合），精确调色板分类全废——高 DPI
   渲染曾因此被误判为回归。resize_probe 现为 PER_MONITOR_AWARE + 饱和像素
   包围盒自校准 + 纯白白闪指标 + 还原几何前置（先缩小再最大化）。新增
   tests/shot_tool.py。启动基线：bing 冷启 TTFF **1545ms**（同水位 1514-1694）。

## 0f. 修复批注（2026-10-19，真 deviceScaleFactor CSS-px 视口 + KJS 清零复查）

1. **CSS-px 视口落地（Safari web-process 语义）**：浏览器只发**逻辑尺寸**
   （CSS px）+ dpr；渲染端 KHTMLView 按逻辑尺寸布局，画布 = 逻辑 × dpr，
   渲染 pass 用 `QPainter::scale(dpr,dpr)` **直接光栅到设备网格**（文字在
   1.5× 下栅格化，不是逻辑图再放大）。damage 全程保持 CSS px，仅在瓦片
   命中测试处 `QTransform::fromScale(dpr,dpr)` 映射一次；滚动条带用逻辑
   坐标（sdx/sdy 是 CSS px），浏览器侧 scroll authority 把**绝对**偏移
   乘 dpr（用绝对值而非增量，长滚动不累积取整误差）。瓦片/画布尺寸不变。
   - 实测（tests/dpr_probe.py，直接驱动渲染进程）：resize {w:680,h:356,
     dpr:1.5} → 页面报 `innerWidth=680 innerHeight=356 r=1.5`，
     tiles 报 `fw=1020 fh=534`（=680×1.5 × 356×1.5）——四项全 OK。
   - 输入坐标隐性修复：旧模型 renderer 控件是物理像素、浏览器却已发 CSS px，
     点击落点偏 1/1.5；新模型下两者同为 CSS px。tests/input_coord_probe.py：
     在 (100,75) 与 (580,275) 两点点击，两个目标全中（PASS）。
   - 浏览器级回归：resize_probe PASS（maximize/restore 白闪 0.00%×2、
     内容 >90% @0.228s、settle 0.27s）、bg_smoke/scroll_smoke/tombstone/
     title_probe 全绿；bing 冷启 TTFF 1986ms（区间 1545-3106 内，网络主导）。
2. **window.devicePixelRatio**：KHTMLPart::setDevicePixelRatio + 桥的
   window 活 getter（magic 2），页面拿到 1.5；innerWidth/innerHeight 随
   逻辑视口（CSS px，WebCore 语义）。
3. **title 传播真缺陷（顺带修复）**：渲染进程原来只在 `completed` 时发一次
   "title" 消息，加载后 `document.title` 的任何变更（计时器回调、未读计数、
   媒体状态、测试上报）都被丢弃——浏览器标签永久停在加载时标题。现在
   caption 每次变化都发（m_lastSentTitle 去重，completed 同样去重）。
   实测：dpr_probe 页面 setTimeout(500) 里改 title，'timer-ok' 正确到达；
   旧版该消息丢失（探针即因此先判 FAIL，排查过程见 4）。
4. **计时器桥排查结论（不是桥的 bug）**：KHTML_QJS_TRACE=1 打开后逐点追踪
   （create/fire/callback ok/killall 带毫秒时间戳）确认 QTimer 正常触发、
   回调无异常、document.title 命中正确文档（view 有效、无 parent）——问题
   全在 3 的 title 出口。追踪点作为长期诊断保留（默认关闭）。
5. **KJS 清零复查（全仓代码零命中）**：删除上游 `test_regression.{h,cpp}` +
   `test_regression_fontoverload.cpp`（只引用已删的 kjs_binding/kjs_window/
   kjs_proxy 头，Windows 从不编译）+ tests/CMakeLists 的 testregression 目标；
   `libkhtml.map` 删 `kjs_html_init` 与 `KJS::*`/`Q*KJS::*`/typeinfo/vtable
   四条导出模式、`KJSCPUGuard*`；`kjs_html_STAT_SRCS` 变量改名
   `khtml_ecma_SRCS`；删除 `KHTMLSettings::KJSWindow{Open,Status,Move,Resize,
   Focus}Policy` 五组枚举 + 五个 getter + 位域/解析代码（全仓零调用者，
   死 API）；删 `KJSErrorDlg` 前向声明与 jsError* 一族
   （jsErrorExtension/removeJSErrorExtension/disableJSErrorExtension/
   jsErrorDialogContextMenu/launchJSErrorDialog + m_jsedlg/m_statusBarJSErrorLabel）、
   `jScriptInterpreter()` 遗留访问器、`setDebugScript`/`slotDebugScript`/
   `m_paDebugScript`/`m_newJSInterpreterExists`/`m_bJScriptDebugEnabled`；
   ScriptProxy 的调试器空实现（setDebugEnabled/debugEnabled/showDebugWindow/
   paused）删除并去掉 executeScript 里的 paused() 分支与 KJS_VERBOSE 宏；
   AtomicString 的 KJS 注释块、khtmlview/dom_node/html_canvasimpl 的
   `namespace KJS` 前向声明、dom_textimpl/dom_stringimpl/dom2_eventsimpl/
   pageloader/dom_docimpl/khtmladaptorpart 等处的 kjs 注释全部清除。
   校验：`\bKJS\b|KF5JS|kjs_|kjs` 在 **src/** 全树零命中。
6. **已知观察项（非本轮引入）**：同一次导航里脚本会执行两次、QuickJS World
   随之重建（title 序列可见 'pending'→'ok'→'pending' 回退，且约 1s 后发生）。
   现象是 KHTML 对同一文档的二次解析（疑似编码探测触发的重载），会重置页面
   JS 状态。未在本轮处理，记为下一轮候选（对计时器/表单状态有实际影响）。

## 0g. 修复批注（2026-10-20，高 DPI 取整 2.0 根因 + 窗口/输入/状态栏）

1. **"小窗变大 / UI 变大"的底层原因**：`AA_EnableHighDpiScaling` 的默认取整
   策略把 150% 屏的 dpr 由 1.5 四舍五入成 **2.0**（实测
   `primaryScreen()->devicePixelRatio()=2.00`、`availableGeometry()=960x504`），
   整窗按 2× 布局与光栅 → 比 OS 惯例大 33%，默认窗口 640x480 逻辑
   = 1280x960 物理（近满屏）。修复：
   `setHighDpiScaleFactorRoundingPolicy(PassThrough)`（QApplication 构造前）。
   实测修复后 ratio=1.50、available=1280x672、页面自报
   `window.devicePixelRatio=1.5`（生产链路，非探针注入）。
2. **默认窗口尺寸**：改为按屏 ratio 反算，恢复 High-DPI 之前的物理足迹
   （1024x720 设备像素；本机窗口 1047x776、客户区 1025x720）并居中。
   **〔已被 0h-2 推翻〕**该不变量错了：resize() 是客户区、窗口仍按逻辑
   尺寸计，683x480 逻辑仅占工作区 53% 宽 → 响应式站点落窄屏大字布局，
   恰是"网页 UI 又大"的第二根因。现行方案见 0h-2（工作区 94%/92%）。
3. **状态栏参数读数删除**：150% 下被状态栏高度裁切成半行字（乱码观感），
   且为诊断信息非 UI。删 QStatusBar + 内存/CPU/GPU 标签；压力监测逻辑
   （缓存清理、标签驱逐、墓碑）不动。
4. **输入映射真 bug**：CSS-px 视口后渲染端内容控件是逻辑尺寸，浏览器仍
   ÷dpr → 真实点击向左上偏 1/1.5。改恒等映射。tests/browser_click_probe.py
   真实窗口点击 CSS(530,270) 小目标，标题变 'click-ok'（PASS）。
5. **CSSOM 缺口（记为下一轮）**：`element.style.backgroundColor=` 不生效，
   `style.background=` 画残框；class 切换 + CSS 级联正常。
6. **工具**：tests/browser_click_probe.py、tests/win_shot.py（对已运行窗口
   DPI-aware BitBlt，避免子进程继承管道导致调用方挂起）。

---

## 0h. 修复批注（2026-10-20，引擎 CSS DPI 双计 + 窗口策略 + 首屏 dpr，全实测）

0g 修的是 Qt 侧取整；本轮修三个叠加根因（细节见 HANDOVER §5i）：

1. **引擎 CSS DPI 双计（主根因）**：`DocumentImpl::logicalDpiY()` 返回
   真实屏 144 → `toPix=2.0` → medium=24 CSS px（应 16）→ 叠 1.5× 光栅
   = 36 物理 px。修：屏幕路径返回 96（CSS 参考 DPI，dpr 只在光栅乘一次）；
   `attach()` 改走 `logicalDpiY()`；打印路径保留旧行为（printpainter 时
   用 paint device DPI）。实测默认 1em 24 物理 px（修前 36）。
2. **窗口策略**：683x480 逻辑（工作区 53%）→ 响应式站点落窄屏大字布局。
   修：工作区 94%/92%、上限 1280x800 逻辑、边框预算 16x40 防越界。实测
   client 1805x927 物理、外框 1827x983 ≤ avail、页面视口 1161x553 CSpx、
   bing 桌面布局。
3. **首屏 dpr 竞态 + MQ 永不重算**：`--dpr` 命令行（cold+prewarm 两处）、
   loadInitial 前生效、resize 先 setDevicePixelRatio、
   `setDevicePixelRatio` 变化触发 `updateStyleSelector()`。实测 file://
   直载首报 'sync r1.5'。
4. **MQ 语义**：`device_pixel_ratio` 由错误的 `zoomFactor/100` 改
   `devicePixelRatio()`；`resolution` = 96×dpr；新增 `-webkit-{,min-,max-}
   device-pixel-ratio` 别名（真实网页方言）。三条 MQ 实测全命中。
5. **桥 window===globalThis**：`innerWidth/innerHeight/devicePixelRatio`
   双挂 global + 裸名兜底（原来裸写 ReferenceError 且整段脚本早断）；
   补 `window.document/window.window` 自引用（原来 undefined →
   `window.document.getElementById` TypeError）。实测
   `wd:true wdId:1 self:true bare:number dpr:number`。
6. **回归**：9/9 全绿两轮（tile_protocol/dpr_probe/input_coord/
   browser_click/bg_smoke/title/scroll_smoke/tombstone/resize）。
7. **流程**：按用户要求 5 代理评审 → 实施 → 5 代理独立核实；核实指出的
   窗口越界、打印注释失实、首屏竞态、MQ 不重算、-webkit- 缺失五项全部
   补修后复测。
8. **记档候选（不阻塞，按价值）**：matchMedia > location/navigator/
   console/getComputedStyle（裸标识符家族，缺失即整段早断）> 窗口几何
   QSettings 持久化 > 滚动条 QSS 定宽 > dppx（需动 bison）> srcset >
   iframe dpr 传播 > 跨屏 ScreenChangeInternal。

---

## 0i. 修复批注（2026-10-21，拖窗口后蓝块卡住：引擎 timer 重算 + 渲染进程部分帧坐标）

两个独立根因叠加（细节、红绿对照、回归见 HANDOVER §5j）：

1. **timer 任务后的样式重算从未发生**：`setDocumentChanged()` 只把文档塞进静态
   队列，唯一消费端 `updateDocumentsRendering()` 只挂在事件派发结束和脚本求值上；
   `setTimeout/setInterval/rAF` 回调都不是 → 渲染树里元素停在旧坐标。
   修：`qjs_dom_bridge.cpp` 的 timer timeout lambda 回调返回后调用
   `updateDocumentsRendering()`（HTML 的 rendering update 本就在每个 task 结束时跑）。
   实测：`[css] styleSetProp left=600px` 之后、下一次事件/尺寸变化之前没有
   `[setStyle]`（推迟而非丢失：脏标志保留、下一次 drain 补上），修后有
   `diff=2` 且新旧两个矩形都产生 damage。⚠️ 当时的"修前干净 trace"已被修后运行
   覆盖（同一输出文件名），该前后对比现无法从仓库重导；现存证据已**冻结入库**：
   `tests/evidence/rg_diag2_trace_2026-10-21.txt`（原 `tests/rg_diag2_err.txt`
   仍是 gitignore 的可再生工件，`rg_diag2.py` 每次运行都会重写它——2026-10-23
   5 代理核实指出「把新克隆拿不到的文件当唯一证据」，此为收口）。
2. **部分帧坐标 + 白底**：
   a) `QWidget::render(painter, targetOffset, sourceRegion)` 会
   `offset -= paintRegion.boundingRect().topLeft()`，旧代码传 `QPoint()` 等于把
   damage 内容画到原点。修：传 `dirty.boundingRect().topLeft()`。
   ⚠️ 更正（2026-10-22）：原引「逐像素实测 7236 px 差异」来自一个**不在仓库里**的
   一次性程序（`tests/scratch_render_compare.cpp` 从来不存在），不可复现，已删除。
   可复现证据是行为级红/绿对照：offset 改回 `QPoint()` → `resize_ghost_probe` RED
   （new-rect 0、mid-rect 4536 卡住）。
   b) 部分帧以白为底，未受损像素随 tile 一起上传、擦掉正确内容。修：`m_lastFrame`
   作底图（滚动 fast path 同步平移；全幅/首帧/尺寸变化仍白底；导航时丢弃）。
   ✅ **覆盖缺口已关闭**（2026-10-23，两轮代理点名后收口）：页面背景改成非白
   `#f0f0f0`，`resize_ghost_probe.py` 新增 5 个背景采样点——落在延迟移动恰好
   损坏的两个 tile（(1,0)/(3,0)）**之内**、但**在两块 damage 矩形之外**的
   未受损余区。实测行为级红/绿对照（真实浏览器、真实构建）：
   - RED：把 `frameImage = m_lastFrame;` 改回白底 → 旧断言**全部仍然 PASS**
     （old/mid 0、new 4536/4536、orange 1——即此前真的抓不到），新采样
     **0/5、五个点全是纯白** → `RESIZE GHOST PROBE FAIL`，exit 1。
   - GREEN：恢复重建 → 采样 5/5 灰 → PASS，exit 0。
   源码契约断言（Part 1）另测过红/绿（删该行 → AssertionError，exit 1）。
3. **顺带修死开关**：mingw CRT 的 `getenv()` 读不到进程环境（`KHTML_QJS_TRACE` /
   `KHTML_QJS_RUNTIME` 从不起作用），改用 Qt 读取；注意
   `qEnvironmentVariableIntValue` 对未设置变量返回 0，必须用 `ok` 出参区分，
   否则会把「未设置」当成「显式关闭」而整体禁用 QuickJS（本轮踩过）。
   ⚠️ **行为变化（2026-10-22）**：这些开关修好后从「永远 ON」变成**真的生效**——
   环境里残留 `KHTML_QJS_STYLE=0` / `KHTML_QJS_TIMERS=0` 会静默禁用对应模块、
   复现本轮修的病症（样式写被忽略 / 没有 setTimeout-rAF 回调）。排查前先查环境变量。
   （变量设为空串的边界行为未实测，实际影响低。）
4. **流程**：按用户要求 5 代理独立核实；核实指出「render() 平移机制描述错误」
   「m_lastFrame 导航不清空」，两项在提交前改正（注释重写 + `dropFrameBase()`）。
   同一批核实里「FIX 1 因果链：drain 不 paint，缺的是 layout」这条**采纳错了**，
   见下条更正。
5. **机制更正（2026-10-22，二次核实）**：FIX 1 缺的是 **recalcStyle**，不是 layout。
   元素级样式写入置位的是元素自身 `m_changed` + 祖先 `hasChangedChild` + 文档
   `m_docChanged`（`NodeImpl::setChanged`，xml/dom_nodeimpl.cpp:366-381），
   **从不置位文档节点自身的 `m_changed`**；而 `recalcStyle()` 末尾的门控
   `if (changed() && m_view) m_view->layout()`（xml/dom_docimpl.cpp:1486-1488）
   用的正是文档自身 `m_changed`（`DocumentImpl` 未覆写 `changed()`；
   dom_docimpl.h:611 那个访问器返回的是 `m_docChanged`，不是同一标志）→ 该门控
   不成立，这个 case 里 layout 根本不跑。实测证据：修后 trace 为
   `[setStyle] ... diff=2`，而 `RenderStyle::Diff`（rendering/render_style.h:2454）
   = `{ Equal, NonVisible = Equal, Visible, Position, Layout, CbLayout }`，**diff=2 = Position**，
   不是 Layout(3)。准确表述：缺的是「样式重算」——`recalcStyle` 把新样式应用到
   渲染树，由 `RenderObject::setStyle` 产生新旧两块 damage，之后的重绘才描述新位置。
   提交 `9e6f1db` 信息里的 "recalcStyle/layout never ran" 属旧措辞，历史不改写，以本条为准。
6. **同日补修（2026-10-22，二次核实的 must_fix 清理）**：
   - `dropFrameBase()` 补到**链接点击路径**（`khtml_renderer.cpp` 里
     `PageLoader::finished` 的 `begin()/write()/end()` 重喂）：这条路走
     `BrowserExtension::openUrlRequestDelayed → RendererView::navigateRequested →
     m_loader->load()`，**不会**收到 `navigate` 命令，所以之前没有走到
     navigate/reload 路径上的 `dropFrameBase()`。
     ⚠️ 措辞更正（2026-10-22 核实）：原文「一直拿旧文档像素当底图」是**过度宣称**。
     底图影响不到**没被上传**的 tile（tile 只在引擎 damage 命中时才上传），而换文档
     本身就触发整视口重绘（`KHTMLPart::begin → KHTMLView::clear → reset()` 置
     `firstLayoutPending/needsFullRepaint` → 全层 repaint → `contentsUpdated`）。
     该调用是**防御性不变量**（引擎 flush 的是视口矩形，未必严格等于
     `QRegion(logicalRect)`），不是可测量的关键修复。
     `refreshed`（同文档重喂、保滚动/缩放）**故意不丢底图**——白底起步正是本轮修掉的
     擦除 bug，丢底图反而会让未受损区域变白。
   - **第三轮补丁（同日，5 代理核实的 must_fix 收口；本轮核实 3/5 代理返回）**：
     `rg_diag4.py` 的 `CONTROL FAIL` 原来也 `return 0` → 现按判定返回 0/1；
     `run_regression.ps1` 把「退出码 0 但无 PASS/INCONCLUSIVE」判为失败、自检用独立
     名字登记、跑前先删旧报告；`bing_ghost_check.py` 退化 baseline → INCONCLUSIVE
     且真实截图改相对判据；`tile_protocol_test.py` Part 1 增加底图契约断言；
     删除引用不存在文件的 7236 数字、链接点击块注释改为诚实范围（见本条上半）。
     ⚠️ 更正（2026-10-23 三轮核实）：当时写的「修正 `:720`→`:734`」**自己就是
     错的**——f2ff052 提交树里真正的清零/置位点其实是 `:745`/`:1566`（本轮
     新加的注释行把被引代码挤下去了；修前树里清零点确实在代码行 720、置位
     1539），且 `:720` 这个数字在 f2ff052^ 里从未以「引用文字」出现过。
     f2ff052 的提交信息重复了这个错数，历史不改写，以本条为准。该注释现已
     改为**按名引用**（cleared a few lines above），不再引用易变行号。
   - **仍未完成**（2026-10-23 更新）：回归报告不入库、回归脚本无单探针超时；
     底图修复的行为级覆盖**已补**（见第 2 条 b）；核实角度本轮 5/5 全部返回。
   - 三处残留的 `getenv()` 死开关改用 Qt（`src/khtml/src/ecma/qjs_dom_bridge.cpp`：
     `KHTML_QJS_TRACE`、模块级 kill switch（`flagOn`）、`KHTML_QJS_TIMERS`）。
   - 删死代码：scroll fast path 里恒真的 `!m_relayoutPending`（该标志在同一函数更早处
     已清零，且 relayout 帧本就整幅带 damage，`isEmpty()` 已能排除）、无调用者的
     `invalidateRect()`。
7. **记档不阻塞**：DOM0 `window.onresize=fn`、内联 window 属性
   （`<body onload>`，`InlineQJSListener` 仍用 `currentTarget()`）、world reset 后
   `g_qjsShimmed` 地址复用、元素路径 shim 去重、Layout diff 的移动元素、
   `m_dirtyRegion` 在帧末被清空、`readTiles` 丢弃分支无 resync、
   scroll-fast 帧同时 dimsChanged 时底图与画布可能差一帧。
 8. **第四轮（2026-10-23，5/5 代理返回的最终核实 + 收口）**：判定
    A(engine)=PASS，B(renderer)/C(test-gates)/D(evidence)/E(adversarial)
    =CONCERN。采纳并修复：
    a) **滚动 fast path 真 bug**（E 发现，Lead 读码复核确认）：blit 平移了
      浏览器画布与 `m_lastFrame`，但**不清 tile 哈希抑制缓存**——
      position:fixed 元素跨滚动渲染出**像素相同**的内容，会匹配**旧哈希**被
      永久跳过重传；250ms 收敛帧的「tiles render back identical → no upload」
      优化恰好把这个该修的场景优化掉了：≥256 物理像素见方的固定头/侧栏在
      快速滚轮后停在**平移后的位置**。修：blit 同步 `m_tileHashes.fill(0)`
      （每轮滚动突发多一次整视口重传，换正确性）。Part 5 滚动模型重写为带
      哈希抑制 + 非均匀固定带：负对照实测复现
      `dy=120 pixel(0,0) got=(88,9,0) want=(0,9,0)`，修复后收敛。
    b) 底图修复补**行为级覆盖**（见第 2 条 b 的 ✅，含实测红/绿）。
    c) runner 看不见 FAIL 令牌（E）：退出码 0 且同时打印 PASS 与 FAIL 判定词
      的探针旧规则会记 PASS。新增「判定形 FAIL」检测（行首 FAIL 或大写词后
      FAIL），9 组样例实测全部分对（tile 测试的 must-FAIL 散文不误伤）。
    d) bing after 截图无活性门（C）：resize 清空内容区而 baseline 有蓝块时
      `extra<0` 会 PASS。新增 `after_liveness_ok`（3% 绝对下限 + baseline
      一半的相对下限），自检补 5 个边界用例；**按位置**判别移动型 ghost
      （计数不变）仍做不到，记为已知限制（本机 bing 0 蓝块本就 INCONCLUSIVE）。
    e) 注释/文档真实性（B/D）：`:734/:1550` 行号自引（f2ff052 里实际
      :745/:1566）→ 改按名引用（见第 6 条更正）；`dropFrameBase()` 定义处旧
      「承重」注释重写为防御口径，与调用点注释一致；`rg_diag2` trace 冻结
      入库（见第 1 条）；桥接注释补「编译代码里仅两处 drain」限定（第三处
      文字匹配在不参与编译的 vendored SMIL 代码里）。
    f) 未采纳/记档：E 的「强转 fullFrame」回归形状行为上不可见（全幅帧本来就
      正确，只是变慢）；源码字符串断言对「注释掉而非删掉」不敏感——由行为级
      覆盖与 Part 2 模型兜底。
 9. **2026-10-24 去特调 + 点击导航本地地址修复**（详见 HANDOVER §5k）：
    a) 锚点 href 被镜像改写为 file:// 的根因修复（`extractResources` 按标签识别、
       `restoreNavigationTargets` 还原导航属性），行为级红/绿实测 3/3→0/3；
    b) 删除 pageloader 每页无条件注入的 bing 专用 CSS（#sb_form 等）；
    c) 第二轮补强：`rewriteAndSave` 补 `<base>`（installBase）、导航还原支持
       #frag/?query/formaction/meta-refresh/无引号、`originalOf` 前缀兜底、
       `hashName` 扩展名白名单、地址栏/历史/load 入口三重 scheme 守卫、
       字符集透传（不再强制 UTF-8）；**删除内联 SVG→`<img>` 改写**（引擎已原生
       支持内联 SVG：`htmlparser.cpp:287-304/924-926`，提交 70bb3a0/413e0ad）；
    d) 残留：iframe 内相对链接仍指向本地副本；宿主通用 CSS 降级层属引擎能力
       缺口，不在「站点特调」范畴，已按引擎路线图（P0-P3）分阶段处理。
 10. **2026-10-24 引擎渲染能力第一批**（详见 HANDOVER §5l，对标 Safari/WebKit，
     组件「够用即止」）：
    a) `-webkit-` 前缀支持：parser.cpp 的 getPropertyID 剥离 `-webkit-/-moz-/
       -ms-/-o-/-khtml-` 后重试，getValueID 把 `display:-webkit-box/-webkit-flex/
       -webkit-inline-flex` 映射到 flex 值；宿主不再删除厂商前缀属性；
    b) HTML5 语义元素 UA 默认块级：在 `CSSStyleSelector::loadDefaultStyle`
       追加（article/section/nav/main/aside/header/footer/figure 等 display:block，
       template/[hidden]/dialog display:none，mark 高亮）——运行时 html4.css 来自
       安装前缀，改仓库文件不生效，故放代码里；
    c) `@supports` 引擎级求值：`CSSStyleSheetImpl::parseString` 预处理阶段展开真
       条件/丢弃假条件（and/or/not、嵌套、注释跳过、规则边界、selector() 保守
       false），宿主不再删除该块；宿主也不再盲改 `display:grid`（会破坏条件文本）；
    d) 新增永久回归 `tests/engine_semantics.{html,py}`（像素级 6 项）并接入
       run_regression.ps1；下一批：var()/逻辑属性/calc()/flex 完整化/grid/绘制。
 11. **2026-10-24 引擎渲染能力第二批**（详见 HANDOVER §5m）：
    a) flex-wrap 换行布局：FlexConfig.wrap + config() 读取（style flexWrap 与
       行内 style），布局重写为按行分段（每行独立 grow/auto-margin/justify/
       align），行间 gap 作 row-gap；单行路径保持原行为；实测 3+2 两行；
    b) 逻辑属性别名：getPropertyID 增加 20 项逻辑→物理长属性映射（LTR、
       horizontal-tb）：margin/padding-inline-*、border-inline-*、inset-inline/
       block-*、inline-size/block-size 及 min/max；RTL 翻转未实现（后续项）；
    c) engine_semantics 探针扩到 8 项像素断言（含主聚类检测排除抗锯齿干扰）；
    d) 剩余 flex 项（flex-basis/order/align-self/inline-flex）与 var()/calc()
       仍在路线图。
 12. **2026-10-24 引擎渲染能力第三批**（详见 HANDOVER §5n）：
    a) 新 CSS 属性 align-self(188)/order(189)/flex-basis(190)：cssproperties.h
       扩展 + parser.cpp added[] + cssparser 值校验 + cssstyleselector 落盘 +
       StyleFlexibleBoxData 字段（含拷贝/相等），flex 数据变化返回 Layout diff；
    b) 布局：order 稳定排序、flex-basis 覆盖主轴尺寸（px/百分比）、align-self
       逐项交叉轴对齐；**单行容器行交叉尺寸**改为取确定高度（Length::isFixed）
       而非子项最大高，修复 center/flex-end 贴顶问题（实测 +90 物理像素落底）；
    c) 探针改为两页夹具（新增 engine_semantics2.html），共 11 项像素断言；检测
       用"主带"bbox 排除工具条抗锯齿污染；
    d) 剩余：flex-shrink、inline-flex 行内化、flex 简写三分量、var()/calc()。
 13. **2026-10-24 引擎渲染能力第四批**（详见 HANDOVER §5o）：
    a) `flex` 简写解析：新增 `CSSParser::parseFlexShorthand()`（none/auto/
       grow shrink? basis?），多值简写不再被整体拒绝；
    b) `flex-shrink` 属性 id=191 + StyleFlexibleBoxData.flexShrink（默认 1.0）
       + 值校验 + applyProperty；布局在主轴负剩余空间时按 shrink×基础尺寸
       加权收缩（实测 200px 容器内两个 flex:0 1 150px 收成各 100px）；
    c) 探针扩展至 12 项（新增 shrink 用例）；剩余：inline-flex 行内化、
       var()/calc()、grid/sticky/aspect-ratio、渐变/transform/动画。
 14. **2026-10-24 引擎渲染能力第五批**（详见 HANDOVER §5p）：
    a) CSS 变量 var() 引擎级实现：DocumentImpl 增加文档级自定义属性 map；
       新增 header-only `css/css_variables.h`（收集 --name 声明 + 递归展开
       var(--name, fallback)）；CSSStyleSheetImpl::parseString 在交给解析器前
       收集并展开（此前只是整块删除）；
    b) 宿主 pageloader 删除 m_cssVars 的收集/嵌套/展开逻辑，只保留 url()/@import
       本地化；变量语义由引擎负责（去掉了宿主"替引擎猜"的一层）；
    c) 探针扩至 14 项（新增 var() 应用与 fallback 两项）；已知限制：全局透传
       语义（无级联作用域）、style 属性内 var() 未展开、跨表依赖定义先于使用。
 15. **2026-10-24 引擎渲染能力第六批**（详见 HANDOVER §5q）：
    a) CSS 渐变引擎化：CSSParser::parseGradientSpec 解析 linear/radial（含
       -webkit-/repeating- 前缀、角度/方向关键字/rgb/hsl/hex 颜色/百分比色标），
       输出紧凑谱字符串随 background-image 进入样式层；
    b) BackgroundLayer 增加渐变字段（拷贝/相等/样式 diff 同步），applyProperty
       识别并落实，普通 background-image 覆盖时清除；
    c) RenderBox 用 QLinearGradient/QRadialGradient 绘制渐变背景；
    d) 宿主删除两处 simplifyGradients 压平，渐变交给引擎；探针新增 page3
       5 项断言（三页共 19 项）；已知限制：渐变不做 border-radius 裁剪、
       旧式 -webkit-gradient 不支持。
 16. **2026-10-24 引擎渲染能力第七批**（详见 HANDOVER §5r）：
    a) `aspect-ratio` 属性引擎化：id=192，StyleBoxData.aspect_ratio（0=无），
       RenderStyle 访问器（COW）；parseAspectRatio 支持 auto / <number>
       [/ <number>]（以 "w/h" 字符串传递），applyProperty 落盘为 float 比例；
    b) 布局：RenderBox::calcHeight 在 height:auto 且宽度已知时按比例推导高度
       （max(内容高, width/ratio)）；
    c) 探针新增 page2 断言（450×300 物理像素），两页合计 20 项；剩余：
       grid、sticky、calc()、inline-flex、transform/transition/animation。
 17. **2026-10-25 引擎渲染能力第八批**（详见 HANDOVER §5s）：
    a) `transform` 属性引擎化：id=193，StyleCSS3NonInheritedData.transform
       （含拷贝/相等），parseTransformSpec 支持 translate/translateX/
       translateY/scale/scaleX/scaleY/rotate（px/%/数字/deg/rad/grad）；
    b) 绘制：RenderBlock::paint 用 QPainter 围绕边框盒中心应用变换后绘制
       （paint-only，嵌套叠加）；宿主删除 transform→relative 重写；
    c) 探针新增 page4 三项断言（translate/rotate/scale），四页合计 23 项；
       已知限制：仅 block、无 hit-test 逆变换、无独立图层、origin 固定中心；
       剩余：grid、sticky、calc()、inline-flex、transition/animation。
 18. **2026-10-25 引擎渲染能力第九批**（详见 HANDOVER §5t）：
    a) `display:inline-flex` 行内化：isDisplayReplacedType/isOriginalDisplayInlineType
       增加 INLINE_FLEX（走 KHTML 既有 inline-block 行内布局通道，伪 replaced），
       RenderBox::setStyle 的 display switch 增加 INLINE_FLEX → setInline(true)；
       渲染器创建本就映射到 RenderFlexBox，flex 内部布局能力全部继承；
    b) 夹具 page2 新增两个 inline-flex 容器 + 探针 3 项断言（同行、子项横向行、
       第二个容器独立渲染），四页合计 26 项；剩余：grid、sticky、calc()、
       transition/animation。
 19. **2026-10-25 引擎渲染能力第十批**（详见 HANDOVER §5u）：
    a) CSS Grid 实用子集：新增 display 值 grid(342)/inline-grid(343) 与
       EDisplay.GRID/INLINE_GRID；新属性 grid-template-columns(194) 支持
       px/%/fr/auto/minmax/repeat(n)/repeat(auto-fit|auto-fill,minmax(px,1fr))；
    b) 新 RenderGrid（行优先自动放置、列宽分配、行高取最高项、gap、auto 宽度
       拉伸到列宽、容器高度求和）；render_object 映射 GRID/INLINE_GRID 到
       RenderGrid；@supports (display:grid) 改为 true；
    c) 新夹具 page5（3×3 网格）+ 探针 4 项断言，五页合计 30 项；已知未支持：
       grid-template-rows、span、命名线/区域、grid-auto-flow、非 px minmax 下限。
 20. **2026-10-25 引擎渲染能力第十一批**（详见 HANDOVER §5v）：
    a) `calc()` 实用子集：Length 增加 Calc 类型（percent + px 双分量），
       width/minWidth/minWidthRounded 支持按基准解析；parseCalcSpec 支持
       百分比与 px 的加减（输出 khtml-calc:pct:px），属性白名单为
       width/height/min/max、margin-*、padding-*；
    b) 布局：calcHeightUsing 增加 Calc 分支，padding 访问器把 isCalc 纳入
       百分比基准判断；calcWidthUsing/外边距经 Length::width 自动生效；
    c) 探针新增 page6 三项断言（六页合计 34 项）；未支持：calc 内乘除/嵌套/
       em/vh 等单位、left/top 的 calc；剩余：sticky、transition/animation。

---

## 1. 进程与沙箱模型

| # | 机制 | 我们的实现（落点） | WebKit 对应 | 差距 | 量化影响 | 优先级 | 对齐成本 |
|---|------|------------------|-------------|------|----------|--------|----------|
| 1.1 | UI/渲染进程分离 | 已实现：`khtml_browser.exe` 不碰渲染，`khtml_renderer.exe` 每标签一个（`RenderTab::start` L376，browser.cpp） | UIProcess / WebContent 进程 | **已对齐**。逐标签进程模型 = 早期 Chromium/现代 WebKit per-tab 粒度 | 崩溃只崩一格（章程北星指标已达成） | — | — |
| 1.2 | IPC 命令通道 | QLocalSocket JSON 行协议（`RenderTab::navigate/sendInput`；renderer `RendererHost::onMessage` L1076） | WebKit IPC（Mach/Unix 域套接字 + 二进制编码） | **部分对齐**。功能等价，但 JSON 文本序列化成本高、无消息优先级、无类型安全 schema | 每条消息一次 `QJsonDocument::toJson`+socket flush；帧外消息量级 10/s 内，**非瓶颈**；轮未实测吞吐，需测量 | 低 | 中 |
| 1.3 | 像素零拷贝通道 | QSharedMemory 256×256 tile 网格 + 脏 tile 索引表（renderer `grabFrame` L472，browser `readTiles` L766） | 共享内存 backing store / IOSurface（macOS） | **已对齐**（机制层）。单段、grow-only 复用、header 最后写保证读者不读半截帧 | tile 协议有 Python 模型测试（tests/tile_protocol_test.py） | — | — |
| 1.4 | 渲染进程沙箱（权限收紧） | **部分落地（0e）**：每渲染进程 Job Object（commit 硬顶=2×动态软预算 + KILL_ON_JOB_CLOSE 崩溃即回收）+ 超软预算转墓碑；内部降权照旧（Java/plugins 关、http(s) 经 PageLoader，`RendererView::openUrl` L314） | macOS sandbox profile / Windows GPU-less restricted token、低完整性级别 | **部分对齐**。OS 级内存强制已闭环；restricted token / 完整性级别 / 文件系统白名单仍缺 | 内存爆炸面已封（超限=墓碑不崩溃）；任意文件读写攻击面仍在 | **高** | 大→中 |
| 1.5 | 崩溃检测与诊断 | VEH + MiniDump + 模块解析 + QuickJS 诊断，全在无 Qt/无堆的处理器里（renderer `ccVehHandler` L95）；UI 侧 `onFinished` 判崩（browser.cpp L590） | WebKit 进程崩溃信号 + CrashReporter | **已对齐**且超额（minidump 自带 RVA 反解） | 崩溃日志落在 EXE 旁 khtml_renderer_crash.log | — | — |
| 1.6 | 渲染进程预热（process pre-warm） | **没有**。每开新标签从零起 QProcess + Qt 初始化 + socket 握手 | WebKit `prewarm` 的 WebContent：预先 fork、暂停在 main 之前，用时即接管 | **缺失**。这是当前冷路径明确的大头 | 实测：渲染进程起 + socket 连接 **~500ms**（HANDOVER 188→684），每次新标签都付 | **高** | 中 |
| 1.7 | 独立网络进程 | **没有**网络进程；PageLoader 在渲染进程内跑 | NetworkProcess（Cookie/缓存/DNS/TLS 全部独立） | **缺失**。网络层漏洞直接落在渲染进程；且各标签重复持连接/缓存，无法共享 | 需测量：每标签独立 socket/DNS 的内存与握手开销 | 中 | 大 |
| 1.8 | 进程退出编排 | 非阻塞 kill + deleteLater（`RenderTab::terminate` L531）；关窗不 wait（closeEvent L1170） | WebKit 的优雅退出 + 超时强杀 | **已对齐**方向，但无超时强杀梯度（我们直接 kill，不给渲染进程清盘机会；对无磁盘状态的本架构可接受） | 修复过 5s "Not Responding"（HANDOVER/注释） | 低 | 小 |

## 2. 渲染调度

| # | 机制 | 我们的实现（落点） | WebKit 对应 | 差距 | 量化影响 | 优先级 | 对齐成本 |
|---|------|------------------|-------------|------|----------|--------|----------|
| 2.1 | 显示帧钟 | 16ms 单一定时器驱动全部 layout/paint（renderer `m_frameTimer` L366；`scheduleFrame` L974 把输入合并进钟上） | DisplayRefreshMonitor / vsync 对齐 CADisplayLink | **部分对齐**。节奏对、但不与真实 vsync 锁相——定时器和显示器相位漂移，长期看每 ~1s 多/少一帧，理论上可 jitter | 60fps 手感实测达标（滚动预测 commit 40052cd）；相位抖动需测量 | 中 | 中 |
| 2.2 | 脏矩形重画 | QRegion 损伤累积 + tile 网格投影（`m_dirtyRegion`、`grabFrame` L584） | 渲染树 damage 传播 + TileGrid | **部分对齐**。输入路径偷懒：任何 click/key/mousemove 都 `invalidateFull()`（dispatchInput L859/896、L936）再靠 FNV-1a 哈希抑制救回来 | 全帧哈希 O(w·h) 每帧：1920×1080 ≈ 8MB 读取 ≈ 1–3ms/帧，**CPU 换带宽**；需测量哈希耗时占比 | **高** | 中 |
| 2.3 | 一次渲染、tile 裁剪 | 全帧只 `w->render()` 一次，tile 是裁剪（renderer L614–631） | display list 重放式 tile 绘制 | **已对齐**（修掉了逐 tile 重渲的大坑） | 该项完成于 commit 037c022，是主帧成本的第一大降 | — | — |
| 2.4 | 重复上传抑制 | FNV-1a per-tile 哈希（renderer L641–692；0 保留值防误抑制） | WebKit 靠 display list 而不是像素哈希；Chromium 有 damage tracker | **机制替代性对齐**。我们用像素哈希换"不知道哪里脏"的缺陷；WebKit 从源头知道 | 哈希本身有成本（见 2.2）；抑制命中率需测量 | 中 | 中 |
| 2.5 | 预测滚动 + 权威收敛 | 浏览器先挪画布（PageSurface `bumpScrollPreview` L133），渲染进程回绝对偏移收敛 + 250ms 防抖全收敛帧（`applyScrollAuthority` L731；renderer L566–581） | Async scrolling：Compositor 线程即时滚动 + 主线程稍后提交 | **已对齐**（双层职责分离是教科书式对应）。剩余 Gap：strip 外 fixed 元素靠 250ms 延迟收敛，WebKit 是逐帧命中 compositing 层 | 剩余"resize/缩小毛刺"已入章程候选池 C | 中 | 中 |
| 2.6 | Live-resize 不放大内容 | WM_ENTERSIZEMOVE/EXITSIZEMOVE 期间冻结渲染进程视口，结束才提交一次真实 resize（browser.cpp L1205–1224）；未画区呈现 = WebKit2 连续性拉伸（旧帧铺满，PageSurface），**checkerboard 已废除**（0e：framesize 提交"先整体拉伸旧画布再叠 1:1 搬运"，resize_probe 白闪 0.00%×2 相位） | Safari resize：内容 1:1 锚定 + 未画区呈现 | **已对齐且升级**。现代 Safari 同为连续性呈现而非棋盘格；tile_protocol 静态断言禁止 checkerBoard 回归 | 修掉了"照片拉伸"观感和 maximize 半屏白闪 | — | — |
| 2.7 | 合成层/硬件加速 | **没有**。软件 QImage 合成 + PageSurface::paintEvent 单次 drawImage（L195） | CALayer/硬加速合成树，滚动/视频/CSS transform 不进主渲染 | **缺失（机制层定论：现阶段不追）**。我们的页面复杂度（KHTML 引擎上限）决定了软件合成不是当前瓶颈；但纯滚动场景完全可以只靠 SHM blit + GPU 上传 | 需测量：全帧 drawImage 到屏幕的 paint 耗时；若 >4ms 那只有硬合成能救 | 低（章程第 6 节列明不追引擎现代化） | 大 |

## 3. 墓碑机制

| # | 机制 | 我们的实现（落点） | WebKit 对应 | 差距 | 量化影响 | 优先级 | 对齐成本 |
|---|------|------------------|-------------|------|----------|--------|----------|
| 3.1 | 挂起释放全部引擎内存 | 杀进程（`suspendTab` L1299：terminate + render=nullptr），内存 100% 归还 OS | iOS Safari 标签冷冻（先挂起，严重时 kill） | **部分对齐**。我们是"直接处决"——没有中间态（先冻结 CPU、内存还在）；iOS 是先 suspsend 再按压力 kill | 内存回收是彻底的；缺 CPU=0 但页面秒回的中间档 | 中 | 中 |
| 3.2 | 墓碑视觉状态保留 | **已落地（0e）**：TombstoneWidget 快照缩放绘制 + 标题徽章 + resumeTab seedFrame 预填首帧；scrollX/Y + zoom + URL + 标题照旧（TabPage；`restore` 命令） | 完整快照缩略图 + 所见即所得恢复 | **已对齐**（内存内路径）。tombstone_probe 生命周期验证（挂起/无自动复活/点击恢复） | 恢复即时有视觉锚点，不再是占位标签 | — | — |
| 3.3 | 恢复不丢未提交输入 | **没有**。墓碑不序列化 DOM 表单状态 | iOS Safari 恢复时表单数据经 NSUserActivity/会话状态保留 | **缺失**。长篇草稿在墓碑后全丢；这是最伤用户的 gap | 章程验收判据 #3 直接点名，无法豁免 | **高** | 大（要序列化 focus/表单/部分 DOM） |
| 3.4 | 处决策略 = LRU + 内存预算 | 10s 巡视线程（`checkMemoryBudget` L1376）：>400MB 超预算先墓碑一个非当前页 + purge 缓存；5 分钟 idle 也挂起（L1398–1404） | iOS: jetsam 事件 + foreground/background 分级；macOS: TabSuspender + memorypressure | **部分对齐**。LRU 用了但排序粗糙——只是"第一个满足条件的"（L1393/1400 循环 break），不是真 LRU 排序 | 400MB 阈值是否匹配现代 KHTML 站点分布，需测量挂起频次 | 中 | 小 |
| 3.5 | 墓碑持久化（冷重启可回） | **没有**。进程退了墓碑即蒸发 | Safari 会话恢复（Session Restore 跨重启） | **缺失**。HANDOVER 候选池 B 已列 | AppData 写盘在开发机被 ACL 拒（已知坑），落盘走 applicationDirPath | 中 | 中 |

## 4. CPU 调度与空闲律

| # | 机制 | 我们的实现（落点） | WebKit 对应 | 差距 | 量化影响 | 优先级 | 对齐成本 |
|---|------|------------------|-------------|------|----------|--------|----------|
| 4.1 | 周期性工作可证明节奏 | 三个钟：帧钟 16ms、内存巡视 10s、状态上报 10s（renderer `m_stateTimer` L433）——全部可说明理由 | WebKit timer throttling（对齐 1s/1min 分级） | **已对齐原则**。状态上报 10s 一条小 JSON，合理；但注意后台标签的三个钟都在跑 | 每标签每 10s 一次 `sendState` + 每次内存巡视遍历全部进程 WorkingSet 开销需测量（OpenProcess×N） | 低 | 小 |
| 4.2 | 挂起标签 CPU=0 | **部分**。suspendTab 直接杀进程，CPU 必然 0；但**后台未挂起的**活页标签帧钟照跑 60fps，只是无 damage 时 early-out（grabFrame L586） | WebKit: 后台 Tab page visibility → requestAnimationFrame 停、timer 节流 1Hz、最终 suspend | **差距明确**：我们给后台标签做的是"空转检查"（每 16ms 醒一次看有没有事），而不是真正的 sleep | 空转 wake 60Hz×标签数；章程北星"空闲 CPU ≈ 0"对后台标签未达标，需测量空闲 CPU% | **高** | 中 |
| 4.3 | 后台 JS 节流 | **没有**。QuickJS 端的 timer/JS 无任何可见性联动 | requestAnimationFrame 暂停、setTimeout 合并到 1Hz、hidden page JS 降权 | **缺失**。后台标签的页面 JS 随便转 | 需测量：后台 KHTML 页 10 分钟的 CPU 时间 | 中 | 中 |
| 4.4 | 大工作显式授权/可取消 | 部分：缓存清理分批非阻塞（`purgeCacheStep` L1355，每 tick 25 项）；字体枚举已懒加载并缓存（HANDOVER 任务 A） | WebKit WorkQueue 后台线程 + 优先级队列 | **部分对齐**。UI 主线程抢救完成；渲染进程内 KHTMLPart 构造 350ms 仍同步阻塞（可接受——进程隔离兜住了） | KHTMLPart 构造 1.1s→0.35s（实测） | 低 | 中 |
| 4.5 | 内存压力模型 | 简单双档：400MB 阈值 + 10s 巡视（L1376）| macOS memorypressure 四级（normal/warn/critical/jetsam）+ 内核异步事件 | **缺失深度**。OS 是主动告诉 WebKit"压力来了"，我们是自己算总和且只看自己 | 系统级内存紧张时我们不会提前收缩，需测量在整机 90% 内存下的行为 | 中 | 中（Windows 上 QueryMemoryResourceNotification） |

## 5. 渐进暴露与防御

| # | 机制 | 我们的实现（落点） | WebKit 对应 | 差距 | 量化影响 | 优先级 | 对齐成本 |
|---|------|------------------|-------------|------|----------|--------|----------|
| 5.1 | 渲染进程无特权动作 | 方向对：Java/plugins 关、http(s) 被劫持到 PageLoader（RendererView L314） | WebContent 默认无文件/网络/syscall 白名单 | **与同 1.4：名义降权但 OS 层 0 强制**。"协议上不做"≠"做不了" | 见 1.4 | **高** | 大 |
| 5.2 | 网络输入 schema 校验 | **没有**。PageLoader 落盘后 renderer 直接 `f.readAll()` 整段塞进 `m_view->write()`（renderer L398–414）；JSON IPC 侧 `QJsonDocument::fromJson` 解析失败只 `continue` | WebKit 解析器假设输入恶意的纵深（类型化 IPC 解码器主动拒绝畸形） | **部分对齐**。IPC 通道有"非 JSON 丢包"这一最低线；HTML 本体无任何预检直接给 KHTML | KHTML 是老引擎，fuzz 面大；需测量畸形 HTML 的崩溃率 | **高** | 大 |
| 5.3 | 崩溃自愈排序 | "首崩 → 禁 JS 自动重载；再崩 → 崩溃页 →完成"（browser.cpp `crashed` lambda L1084–1105） | Safari: 自动重载一次 → 成对崩溃 → 错误页 | **已对齐**，并且 crash 页点击恢复走 resumeTab | 逻辑含在崩溃路径测试范围内（章程验收 #2） | — | — |
| 5.4 | UI 对外部 URL 打开审批 | **缺失**。`newtab` IPC 直接 `addTab(u)` 无过滤（browser.cpp L1106） | WebKit PolicyDelegate + 用户手势绑定 | **差距**：页面可以让浏览器无限开标签（弹窗洪流） | 需测量（构造恶意场景） | 中 | 小 |
| 5.5 | IPC 消息认证 | socket 名按 PID+counter 生成但无随机性（L379）；本机任意进程连上就能驱渲染 | WebKit IPC 连接凭据（audit token） | **差距**：命名管道可猜（khtml_rt_<pid>_<n>） | 需测量（本地特权升级面） | 中 | 小 |

## 6. 章程外但 Safari/WebKit 标志性机制

| # | 机制 | 我们 | WebKit 对应 | 差距一句话 | 量化影响 | 优先级 | 对齐成本 |
|---|------|------|-------------|-----------|----------|--------|----------|
| 6.1 | 页面缓存（Page Cache / bfcache） | **没有**。后退 = 重新加载（`goBack` L1245 走 navigate → renderer reload） | WebKit PageCache：整棵 DOM/JS 状态冷冻，后退是 0ms 复活 | 后退延迟 = 完整重载 ~1s+；Safari 后退体感为瞬时 | 需测量；对带宽/CPU 都是纯浪费 | **高** | 大（DOM 冷冻需要引擎配合，KHTML 无此 API；只能先做"最近页面 URL+scroll+zoom 复合墓碑"弱化版） |
| 6.2 | TabSuspender（定时降级链） | 我们有 5 分钟 idle 一步到位的杀进程 | macOS Safari: 活跃 → 降级计时器 → suspend → 极限时 kill | 少中间档；等于跳到最终态 | 恢复成本 1.1s vs Safari 恢复近 0ms | 中 | 中 |
| 6.3 | 进程间消息优先级 | **没有**。所有 JSON 命令一条 FIFO；输入事件和 navigate/res.framesize 同权 | WebKit IPC 有同步/异步/优先通道（滚动抢占 navigate） | 用户狂输 + 后台 big dispatch 时，帧相关消息可能排在 navigate 后面 | 需测量（构造重负载下的输入延迟） | 中 | 中（双通道：控制通道 + 数据通道即可大批量解） |
| 6.4 | WebContent 进程预热 | 同 1.6，**缺失** | prewarmed WebContent：一个进程冻结池 | 每个新标签白付 ~500ms | 实测 188→684ms（HANDOVER） | **高** | 中 |
| 6.5 | Cookie/缓存/连接共享 | 每标签 PageLoader 自带 socket，无共享 | NetworkProcess 统一 Cookie 罐/HTTP 连接池/磁盘缓存 | 同源 N 标签重复 TCP 握手 + Cookie 不一致风险 | 需测量：开 3 个同源标签的总 TCP 握手数 | 中 | 大 |
| 6.6 | 首帧预算（time-to-first-frame 指标化） | 已埋点（STARTUP_MARK 全链，ready 消息带渲染进程时钟做双进程时钟零误差） | WebKit Speedometer/PLT 化验收 | **已对齐精神**：每轮改动要回答"哪个指标变了" | 1.1s 全链分解已入库（HANDOVER §5） | — | — |
| 6.7 | 崩溃后的 page-state 保留 | 崩溃 = 丢 DOM（只带 URL 重生）| Safari 崩溃恢复同样丢，但 bfcache 遮了一部分 | 与 WebKit 同等 | — | 低 | — |

---

## 7. 总结表

| 机制 | 状态 | 差距一句话 | 优先级 |
|------|------|-----------|--------|
| 进程隔离 per-tab | 已对齐 | — | — |
| 零拷贝像素通道 | 已对齐 | — | — |
| 显示帧钟节奏 | 部分 | 16ms 是对的但没锁 vsync，相位漂 | 中 |
| 脏矩形传播 | 部分 | 输入路径全帧 invalidate + 像素哈希补丁，WebKit 从源头知道 | **高** |
| 单次渲染 tile 裁剪 | 已对齐 | — | — |
| 预测滚动/权威收敛 | 已对齐 | 剩余 resize/缩小毛刺（章程候选池 C） | 中 |
| Live-resize 1:1 + 连续性呈现（拉伸，checkerboard 已废） | 已对齐且升级 | resize_probe 白闪 0.00%×2 相位（0e） | — |
| 合成层/硬加速 | 缺失 | 软件合成是当前架构上限，章程第 6 节已豁免 | 低 |
| **渲染进程 OS 层沙箱** | **部分落地（0e）** | Job Object 内存硬顶+崩溃回收闭环；restricted token/文件白名单仍缺 | **高** |
| 崩溃检测/诊断 | 已对齐 | 甚至超出 WebKit 基线 | — |
| **渲染进程预热** | **缺失** | 每新标签白付 ~500ms 进程启动 | **高** |
| 网络进程独立 | 缺失 | 网络漏洞落渲染进程；同源无共享 | 中 |
| **墓碑缩略图** | **已落地（0e）** | TombstoneWidget 快照+seedFrame，probe 验证 | — |
| **墓碑表单不丢** | **缺失** | 长篇草稿挂起即丢，违反章程验收 #3 | **高** |
| **后台活页 JS/帧钟节流** | **缺失** | 后台标签 60Hz 空转，违反"空闲 CPU ≈ 0" | **高** |
| 墓碑 LRU 排序 | 已对齐 | lastActive 真 LRU（0d 批注落地） | — |
| 墓碑持久化跨重启 | 缺失 | 冷启动墓碑蒸发 | 中 |
| 内存压力模型 | 已对齐 | 内核 LowMemory 事件 + 四级 + 动态 per-renderer 预算（0d/0e） | — |
| IPC 消息优先级 | 缺失 | 所有命令单通道 FIFO | 中 |
| 网络输入 schema 校验 | 部分 | IPC 有最低线，HTML 本体裸奔老引擎 | **高** |
| 崩溃自愈排序 | 已对齐 | 禁 JS 重载 → 崩溃页 → 用户触发 | — |
| 外部 URL 开标签审批 | 缺失 | 页面可无限开标签 | 中 |
| IPC 通道认证 | 缺失 | 命名管道可猜，本机任意进程能驱渲染 | 中 |
| **bfcache** | **缺失** | 后退 = 1s+ 整页重载，KHTML 无冷冻 API，只能做弱化版 | **高** |
| TabSuspender 中间档 | 缺失 | 直接跳最终态 | 中 |
| 首帧预算埋点 | 已对齐 | STARTUP_MARK + 双进程时钟零误差已闭环 | — |
| CPU 空闲律（活页） | 已对齐 | 无 damage 时 grabFrame 纯 early-out | — |

---

## 8. 下一步推荐顺序（高优先 × 成本/收益）

1. **渲染进程预热池**（中成本，~500ms/新标签）——测量值已在手，对齐第 1.6/6.4。
2. **后台活页 JS/帧钟停走**（中成本，直接冲"空闲 CPU ≈ 0"北星）——4.2 + 4.3。
3. **墓碑缩略图**（中成本，恢复视觉锚点）——3.2，落盘走 applicationDirPath 规避 ACL 坑。
4. **脏矩形从源头传播**（中成本，干 2.2 的全帧哈希）——在 KHTMLView 逐事件重挂 invalidateFull → invalidateRect。
5. **渲染进程 OS 层沙箱**（大成本， longest pole 但是安全的根本）——1.4，Windows Job Object + 受限 token 路线先调研。

*（每完成一项回来改这份文件的对账栏）*
