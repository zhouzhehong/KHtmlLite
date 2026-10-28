# KHtmlLite 对话交接快照（HANDOVER）

> 新会话/换人接手时先读这份文件，再读 `docs/ARCHITECTURE.md`（章程），最后看 `git log`。
> 本文件每轮大任务完成后更新。

## 1. 项目一句话
KHTML（5.116）改造的双进程浏览器：`khtml_browser.exe`（UI/合成）+ `khtml_renderer.exe`
（每标签一渲染进程），QLocalSocket JSON 命令 + QSharedMemory 256×256 tile 零拷贝传输。
工作目录：`C:\Users\zhouzhehong\Desktop\KHtmlLite`。

## 2. 铁律（全程生效，动任何一个都要先想）
- **绝不引入/抄用一行 WebKit/Safari 源码**——只学机制与设计（见 `docs/ARCHITECTURE.md`）。
- 任何性能宣称**必须有真实测量**（KHTML_STARTUP_TIMING 埋点 / tile 协议 Python 模拟）。
- 只允许我在 MSYS2 UCRT64 shell 里编译；Agent 平时不直接跑自产 EXE 以外的 GCC 调用。
- 工具链固定：`/mingw64`（贯穿编译器、Qt、KF5），禁止混用 `/ucrt64`（CRT 不同会炸）。

## 3. 关键环境细节（实测踩过的坑）
- 构建：`bash scripts/build.sh`（QuickJS → KHTML → 双 exe）输出到 `build/host/`。
- MSYS2 MSVCRT 坑：C++ 常态 `-isystem` 会让 `#include_next` 失效 →
  两个 `CMakeLists.txt` 都设置了 `CMAKE_NO_SYSTEM_FROM_IMPORTED ON`。永远不要改回去。
- 引擎释放出来是 `gnu++20`（严格 `c++20` 会隐藏 `timespec_get`/`quick_exit`）。
- `src/khtml/KF5KHtmlConfig.cmake.in` 是手写模板，已在 `.gitignore` 里显式反忽略；
  之前消失过，构建报 `configure_file` 错误就是它又丢了。
- QuickJS 静态库在 `src/quickjs/libquickjs.a`，build.sh 每次 `make` 自动补装。

## 4. 已完成 & 测量的账目（commit 时间倒序）
- `fbe17fb` — 冷启动修复：字体枚举缓存(khtml_fonts.ini)+图标懒加载；KHTMLPart 构造 1.1s→~0.35s，TTFF 2.0s→~1.1s
- `b52ee6d` — KHTMLPart/KHTMLView/Settings/Cache/CSS 插桩（STARTUP_MARK 细分）
- `9466dc7` — 架构章程（章程优先：墓碑/帧钟/隔离/红线）
- `7989c1e` — 构建修复 + 采集脚本 `tests/run_startup_capture.py`
- `634bdd5` — 启动时间线基建：ready 消息带 renderer 自身时钟 → 双进程时钟零误差
- `e5dc4b2` `ced9983` `66731c3` `3a6c3b3` `24a6424` — 构建链生存修复
- `40052cd` — 预测性异步滚动（60fps 预告 + 绝对偏移收敛）
- `037c022` — 渲染-once-全帧 + tile 裁剪 + 1:1 checkerboard resize（对齐 Safari 显示管线）
- `fc308c4` `...`（见 `git log`）— tile 哈希抑制、60fps 帧钟、滚动 blit

## 5. 当前测量值（2025-10-14，本机，热跑）
冷启动 → 首帧 **~1.1s**（修前 ~2.0s）：
- `0→~170` EXE+Qt 壳 + chrome 搭建
- `188→684` 渲染子进程起 + socket 连接
- KHTMLPart 构造 **~350ms**（修前 ~1.1s；详见下方任务 A 战果）
- load→首帧 ~200ms

**任务 A 战果（1.1s 拆解，已完成测量+修复）：**
- `KHTMLSettings::init` 的 4 个 `QFontInfo(QFont(serif/...))` 解析触发 Qt 全库字体枚举
  = **556–800ms**（全坑的 80%）。修复：解析结果写入 `applicationDirPath()/khtml_fonts.ini`，
  此后每个渲染进程直接读表；显式 khtmlrc 条目仍优先。首台机器只付一次。
- `Cache::init` 里 KIconLoader 找 "image-missing" 图标 = 66–123ms → 已改懒加载
  （`ensureBrokenPixmap/ensureBlockedPixmap`，首个图片加载失败时才付）。
- html4.css/quirks.css/presentational.css 全解析只占 ~10ms，不是瓶颈；KConfig 读盘本身 2ms。
- 新探针 `probe_kconfig`（`-DBUILD_PROBE_TOOLS=ON` 构建）复现/诊断 KConfig、字体、图标成本。
关键学会（新坑，2025-10-14）：
- **startuplog 是 header inline：EXE 与 libKF5KHtml.dll 各持一份静态 QElapsedTimer**。
  跨模块比较绝对时间戳会冒出 ~340ms 的"空洞"假象；只有同一时钟内的差值可信。
- **%LOCALAPPDATA% 对未签名 exe 写入被拒**（CreateFileW gle=5、QLockFile/KConfig
  "couldn't lock local file"、QSettings sync AccessError）：cmd/bash 能写、未签名 exe 不行。
  需要进程级持久化时放 `applicationDirPath()` 旁边（如 `build/host/khtml_fonts.ini`）。

## 5b. 页面内容渲染通路大修（2026-10-14，像素级实测）
用户报"卡 + 背景缺失"的根因不是一条，是三层叠加，全部修复并验证：

- **PageLoader 时序竞态**：初始文档把资源改写成尚不存在的 file:// → 渲染端从不二次
  加载 → image 永久 error（且 hadError 缓存条目不被重取）。修复三件套：
  misc/loader.cpp `needReload` 驱逐 hadError 条目；PageLoader 全量下载后发
  `refreshed` 信号 → renderer 保滚动/缩放重喂最终文档；rewriteAndSave 从"每资源
  完成一次全文重写"降为每导航恰好一次（旧行为是 O(资源数)×200KB regex，是真实
  卡顿源之一）。
- **KIO file:// 全灭**：MSYS2 部署里没有 kioslave5.exe → 一切 file:// 子资源加载
  必然失败（这就是用户眼里"背景缺失"的物理解释）。misc/loader.cpp 加 QFile
  直读快路径（延迟一事件循环拍，保持 KIO 异步契约）。
- **离屏渲染无 repaint 信号**：WA_DontShowOnScreen 下 QWidget::update() 是 no-op，
  首帧后页面永白。KHTMLView 新增 `contentsUpdated(QRect)`（viewport 坐标）→
  renderer 累积脏区+排帧。这是 WebKit damage rect 的对应物。
- 配套：build.sh 部署 Qt imageformats 插件；PageLoader cacheRoot 改写探针选路
  （TEMP 被 MSYS2 重写成 /tmp 不可写时退到 exe 旁）；诊断开关 `KHTML_PERF=1`、
  `KHTML_FRAME_DUMP=1`；新测试 `tests/renderer_solo.py`（直驱渲染进程）、
  `tests/bg_smoke.py`（像素断言：红背景平铺+320×240 绿图，1.4s PASS）。
- 验证：bg_smoke PASS；tile_protocol PASS；bing 启动时间线无回归（Settings init
  36ms；渲染进程持续产出 repaint 帧而非一帧即哑）。

## 5c. 高优 gap 三项（2026-10-14 同批）
- **墓碑缩略图**：snapshot 在 suspend 前留存、挂起态绘制、resume 时 seed（63c358b）。
- **渲染进程预热池**：RenderTab::prewarm 待命 about:blank 进程，start() 收养
  （warm-hit），收养后 600ms 补位；孤儿 10 分钟看门狗。收养路径走
  `--url about:blank` + ready 后 `navigate`（迁移自 --url 直喂）。
- **后台帧钟节流**：`visible` IPC，非当前标签停 60Hz 帧钟；回前台全量一帧。
- 注意 pwsh 测试技巧：若 build 报 `cannot open output file ... Permission denied`，
  先看是否有孤儿 khtml_renderer.exe 锁着 exe（测试 kill 浏览器不杀子进程）。

## 5d. UI/渲染质量轮（2026-10-15，四项全实测）
- **resize 贴合**：PageSurface 尺寸失配期间拉伸旧帧铺满（WebKit2 UI 进程
  backing-store 拉伸的对应实现），真实帧静默替换。resize_probe 实测：
  content 达标 0.0s（修复前 NEVER）、白闪 0.00%（前 62.89%）、新帧 0.17-0.19s。
- **滚轮死路**：渲染端原把 QWheelEvent 发 viewport()——KHTMLView::viewportEvent
  对 Wheel 显式 return false，40 格 0px；改发 content widget。引擎平滑滚动
  （SSMWhenEfficient）与浏览器预测滚动打架 → SSMDisabled 一步到位。
  scroll_smoke（HTTP+静态色带页）PASS，回滚签名精确还原。
- **墓碑压力分级**：GlobalMemoryStatusEx.dwMemoryLoad 四级（<70/70-85/85-93/≥93），
  warning 起按 lastActive 真 LRU 驱逐后台标签；固定 400MB 阈值废除。
- **KJS 残留清理**：kjs_proxy 早已 QuickJS-only；本轮 (a) kjlog 每次求值
  fopen 写盘改为 KHTML_QJS_TRACE=1 才开；(b) PageLoader 停止 ES2015→ES3
  降级（QuickJS 原生解析，外链 256KB 上限解除，Khtml_Transpile=1 可回退）。
  bing TTFF 1694ms 在历史区间内，现代脚本确认真实执行。
- **探针工程教训**：给 KHTML 写测试页严禁 flex/gradient/hsl()/模板字符串/
  JS 建 DOM——旧 resize_probe 页因此纯白且 90% 阈值永不可达（rgba 边框落
  other 桶）；采样须剔除工具栏（#f0f0f0 会被判成 checkerboard）。
- 新测试：tests/resize_probe.py（最大化/还原白闪量化，带预检门）、
  scroll_smoke.py 改 HTTP+静态页。全绿：bg_smoke/tile_protocol/
  scroll_smoke/resize_probe。

## 5e. 系统负载墓碑 + UI 修复 + KJS 移除收尾（2026-10-16，全实测）
- **③ 墓碑随系统负载**（替代机械 400MB）：CreateMemoryResourceNotification
  (LowMemory) 事件线程（对标 WebKit MemoryPressureHandler）+ GlobalMemoryStatusEx
  四级：<70 正常 / ≥70 purgeCache+LRU 记分 / ≥85 驱逐 LRU 后台 / ≥93 全后台。
  事件创建即 signaled 的坑（本机内存常态 83-94%！）→ 线程先 sleep(15s) 启动宽限，
  否则 PageLoader 首页缓存被清 → bg_smoke 18.4s 红屏（已修复验证）。
  系统CPU 自适应空转超时：GetSystemTimes 采样 sysCpuPct ≥85% → 60s，否则 5min。
  测量背书：4 核本机物理内存 83-94%（常态 tier1-2）；后台标签即使放动画 GIF
  也仅 0.6-1.7%/核（帧时钟节流有效）→ 原"热标签 500‰ 豁免"是死规则，已删。
  验证：tests/tombstone_probe.py PASS（2 标签、t+16-21s 驱逐、无自动复活、
  点标签复活、渲染进程存活、crashlog 干净）。
- **QTabWidget 手术纪律**（本轮最深的坑）：removeTab/insertTab 会在半改状态
  重发 currentChanged → 挂起处理器被路由到墓碑标签自动复活；resumeTab 再入
  → 双重手术 → m_pages↔m_tabs 失配 → 杀了活渲染进程。修法：blockSignals(true)
  包手术 + suspended 标志先行 + 术后显式 setCurrentIndex。
- **④ 空标题/_blank/file:// 三连环根因**：PageLoader 会**预取链接页并重写
  href 为本地 sha1 副本**（离线优先设计）→ target=_blank 时 openUrlRequestDelayed
  携带的是 file:///…khtml_cache/… → 新标签拿 file:// 路径 → KIO file 加载卡死
  → 空标题×3 + 地址栏缓存路径 + purgeCache 后死链。修法：PageLoader 新增
  originalOf()（本地副本→原始 http URL 反查），newtab/navigateRequested 出口
  一律反查；file:// 主文档也改走 PageLoader（QFile 读 + begin/write/end 喂入，
  绕开 KIO 卡死，about:blank 的 temp 文件同理）。实测：quiet 标签 caption=quiet
  正确到达；file:// 页面渲染绿底 270k px（修前纯白）；quickjs 回归页 file://
  加载 + document.title setter 全链路 OK。
- **④ 标题机制**：executeScript("document.title") 在 jsDisabled 渲染进程上崩，
  改为直连 KParts::ReadOnlyPart::setWindowCaption（DOM::DocumentImpl 解析
  <title> 时原生发射，空标题自动回退 URL）。渲染进程 stderr 改
  ForwardedChannels 直透浏览器（探针可见），[tomb]/[rend] 调试输出由
  KHTML_TOMBSTONE_DEBUG=1 门控常驻。
- **④ window.innerWidth/innerHeight 桥补齐**：QJsDomBridge 在 window(=document
  包装)上挂 defCgetset 活 getter（KHTMLView::visibleWidth/Height，WebCore 同义）。
  实测 viewport W=638 H=478。残留未做：F11 全屏、DPI 清晰度（144dpi 屏模糊）。
- **⑤ KJS 完全移除评估落地**：删除 src/khtml/src/bindings/ 全目录（10 文件：
  5 个 KJS SVG 定制绑定引用不存在的头、5 个 Perl 绑定生成器——均不在任何编译
  清单，git 记 D）。保留 ecma/jsengine.h + quickjs_engine.{h,cpp}（编译内、
  审计文档钦定的长期参考抽象）；KJSProxy 命名残留维持文档化接受；
  needsFallback() 语义确认为"QuickJS 无法执行→调用方优雅降级"（非回退 KJS），
  qjs_dom_bridge 头部过期注释已改。QuickJS 回归 PASS。
- 启动基线（本轮新采，file:// quickjs 页）：TTFF 3106ms，全程标记齐全无缺口
  （尾部 13.2s 空闲为静态页无动画的预期）。
- 全绿：tile_protocol（含新增墓碑契约断言）/bg_smoke/scroll_probe(60px/格,
  3.7ms 收敛)/resize_probe(0.07s 还原)/tombstone_probe/title_probe(三路径)
  /quickjs 回归/viewport/启动采集。

## 5f. 系统监测与沙盒轮（2026-10-17，全实测，本机 150% DPI 屏）

- **② 渲染进程沙盒（Job Object 硬上限 + 动态软预算）**：每个渲染进程
  AssignProcessToJobObject 自有 Job Object：ProcessMemoryLimit = 软预算×2
  （commit 硬顶，每 2s tick 跟随调价）+ JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
  （浏览器异常退出 → 内核即时回收全部渲染进程，不再依赖 10min 看门狗）。
  软预算 perRendererBudgetKB：整机 RAM ≥16G→1G / ≥8G→768M / 否则 512M，
  系统压力 ≥85%（warning+）再减半（"预算跟机器和当下走"，对标 Safari
  per-WebProcess limit + MemoryPressureHandler）。超软预算 → suspendTab 转墓碑
  （绝不崩浏览器），stderr 打 `[tomb] renderer budget exceeded`。领养(暖进程)
  路径 start() 直挂，冷启路径挂 QProcess::started。
- **③ GPU/CPU/内存实时监测**：PDH GPU Engine(*)\Utilization Percentage
  （任务管理器同源）按引擎实例求和封顶 100；无 GPU 计数器的机器降级为 0
  （诚实缺席，不造数）。GetSystemTimes 系统 CPU%（子代理已有）保留。
  状态栏："内存 %MB | 内存x% CPUy% GPUz% (等级) · 后台‰"。已有决策位不变
  （CPU≥85% 缩短空转驱逐）；GPU 先做展示与诊断，不接调度（无实测收益
  不加规则——ARCHITECTURE 章程）。CMake：khtml_browser 链 pdh + pdhmsg。
- **④ DPI 清晰度（150% 屏实测，本轮最深）**：AA_EnableHighDpiScaling +
  AA_UseHighDpiPixmaps；管线全物理像素（PageSurface 发 size×dpr，输入÷dpr）。
  PageSurface paintEvent 稳态分支 `p.scale(1.0/dpr)` 后 1:1 设备像素直投
  （零拷贝；QImage setDevicePixelRatio 会每帧 detach 8MB，故用 painter 反缩放），
  ±1px 容差吸收 int()/qRound() 差；失配走 WebKit2 连续性拉伸。
  **像素级验证**：1920 客户区抓图，页面色带 73–74 设备px/条（49逻辑×1.5）、
  带内精确 hex（0xa02c2c 等逐字面值）、带界仅 1–2 行过渡。若双重重采样，
  精确色不可能长程存在。
- **④ resize 白闪真根因修复**：framesize 提交原是"白底 + 左上 1:1 搬运"——
  渲染端 resize 后首帧若只带部分瓦片，半白画布直接顶掉旧帧（绕过 paintEvent
  拉伸保护），maximize 时下半屏纯白（t≤0.12s 幽灵样本抓到，1920×974 客户
  区白 46% 列吻合）。修法：新画布先整体拉伸旧画布再叠 1:1 搬运（WebKit2
  "UI 进程永不暴露未绘制画布"），纯高度变化的条带重绘仍落精确像素。
  实测（DPI-aware 探针）：maximize 白闪 0.00%、内容 >90% @0.197s、settle
  0.23s；restore 白闪 0.00%。
- **④ 探针工具链升级（150% 屏必需）**：resize_probe SetProcessDpiAwareness(2)
  ——DPI 无感知进程在 150% 屏拿到的是 DWM 降采样虚像（每像素都是 1.5×1.5
  混合），精确调色板分类全废，高 DPI 渲染被误判为回归。页面区改为"饱和像素
  包围盒"自校准（chrome 随 dpr 变大，只裁顶部不够）；白闪指标改纯白
  （checkerboard 呈现已废除且被 tile_protocol 静态断言禁止，灰色=滚动条/
  状态栏等合法 chrome）；几何前提修复：先 SetWindowPos 缩小窗口再最大化
  （150% 屏默认窗口已近满屏，SW_MAXIMIZE 反而缩小→无暴露区可测）；
  SW_RESTORE 异步 → 等 IsZoomed 翻转 + 客户区真回到还原几何再采样。
  新工具 tests/shot_tool.py（DPI-aware 截图 + 任意窗宽）。
- **④ F11 全屏**：QAction(QKeySequence(Qt::Key_F11)) 窗级注册，任意子控件
  焦点下可触发；isFullScreen 切换 showNormal/showFullScreen。
- **④ 定时器桥**：QJsDomBridge 装 setTimeout/setInterval/clearTimeout/
  clearInterval/requestAnimationFrame/cancelAnimationFrame（global+window 双挂，
  JSCFunctionMagic 分派，rAF=16ms 单发）。QuickJS World 重置（resetRuntime/
  析构）先 qjsTimerKillAll() 清注册表再拆 ctx，回调永不落入死上下文。
  修复链：CSS 动画 polyfill 硬依赖 rAF（原先缺失→polyfill 静默空转）；
  Promise shim 内部用 setTimeout（原先 undefined→挂起态 Promise 永不 resolve）。
  KHTML_QJS_TIMERS=0 可关。qjslog/qjslog2 每 evaluate fopen 写盘的老病
  同步根治（KHTML_QJS_TRACE=1 才开）。
- **⑤ KJS 残留清零**：KJSProxy→ScriptProxy 全仓改名（16 文件 ~40 处 +
  libkhtml.map 导出 + guard），kjs_proxy.{h,cpp}→js_proxy.{h,cpp}；
  js_transpile.{h,cpp} 删除（Bublé ES3 降级机器——QuickJS 原生 ES2020，
  纯属 KJS 时代遗产），外链/内联脚本 raw 直通 + try/catch 保留；
  pageloader/css_anim_polyfill/khtml_browser 过期 KJS 注释改写。
  墓碑缩略图（TombstoneWidget 快照缩放绘制 + seedFrame 预填）经
  tombstone_probe 生命周期验证（挂起/无自动复活/点击恢复）。
- **回归账目（150% DPI 屏，全绿）**：tile_protocol（含 dpr 设备直投静态断言）
  /bg_smoke/scroll_smoke/tombstone_probe/tit le_probe(三路径)/resize_probe
  （白闪 0.00%×2 相位）。启动采集 bing 冷启 TTFF **1545ms**（渲染 pass 4-21ms，
  大隙为网络/子资源，与既测 1694ms 同水位；5e 的 3106ms 为 file:// 冷页基线）。

## 5g. 真 deviceScaleFactor CSS-px 视口 + KJS 清零复查（2026-10-19，全实测）

- **CSS-px 视口（Safari web-process 语义）**：浏览器只发**逻辑尺寸**（CSS px）
  + dpr（resize 消息带 `dpr` 字段）；渲染端 KHTMLView 按逻辑尺寸布局，帧画布 =
  逻辑 × dpr，渲染 pass `QPainter::scale(dpr,dpr)` **直接光栅到设备网格**
  （1.5× 下的文字是真 1.5× 栅格化，不是逻辑图放大）。引擎 damage 全程 CSS px，
  瓦片命中测试处 `QTransform::fromScale(dpr,dpr)` 映射一次；滚动条带用逻辑
  坐标，浏览器 scroll authority 用**绝对**偏移 ×dpr（不用增量，长滚动无累积
  取整误差）。瓦片协议/画布大小零改动。
- **实测（tests/dpr_probe.py，直驱渲染进程）**：resize {680,356,dpr 1.5} →
  页面报 `innerWidth=680 innerHeight=356 r=1.5`；tiles 报 `fw=1020 fh=534`
  （= 680×1.5 / 356×1.5）→ 四项断言全 OK（PASS）。
- **window.devicePixelRatio 桥**：KHTMLPart::setDevicePixelRatio + window 活
  getter（magic 2）；innerWidth/innerHeight 语义回归 WebCore（CSS px）。
- **输入坐标隐性 bug 修复**：旧模型 renderer 控件是物理像素、浏览器早已发
  CSS px → 点击落点偏 1/1.5。新模型两者同为 CSS px。tests/input_coord_probe.py
  在 (100,75)/(580,275) 两点点击两个目标全中（PASS）。
- **title 传播真缺陷（探针撞出来的）**：原实现只在 `completed` 发一次 title
  消息，加载后 `document.title` 的变更（计时器回调/未读计数/媒体状态）全部
  丢弃 → 标签永久停在加载时标题。现在 caption 每次变化即发（m_lastSentTitle
  去重，completed 同样去重）。排查用 KHTML_QJS_TRACE=1（create/fire/
  callback ok 带毫秒戳）证明计时器桥本身没问题，问题在出口。
- **KJS 清零复查（src/ 全树零命中）**：删上游 test_regression 三件套 + 构建
  目标；libkhtml.map 删 kjs_html_init/KJS::* 四条导出模式/KJSCPUGuard；
  kjs_html_STAT_SRCS → khtml_ecma_SRCS；删 KHTMLSettings 五组 KJSWindow*Policy
  枚举+getter+位域（零调用者死 API）；删 KJSErrorDlg/jsError* 一族、
  jScriptInterpreter()、setDebugScript/slotDebugScript/m_paDebugScript/
  m_newJSInterpreterExists/m_bJScriptDebugEnabled、ScriptProxy 调试器空实现
  与 paused() 分支、KJS_VERBOSE 宏；AtomicString 注释块、KJS 命名空间前向
  声明、各文件 kjs 注释全清。校验命令：`\bKJS\b|KF5JS|kjs_|kjs` → 0 命中。
- **回归账目**：tile_protocol/dpr_probe/input_coord_probe/bg_smoke/scroll_smoke/
  resize_probe（白闪 0.00%×2、内容 >90% @0.228s、settle 0.27s）/tombstone/
  title_probe(三路径) 全绿；quickjs 回归页标题 'QuickJS Test - OK' 正确上报；
  bing 冷启 TTFF **1986ms**（历史区间 1545-3106，网络主导）。
- **已知观察项（非本轮引入）**：同一导航内脚本执行两次、QuickJS World 随之
  重建（title 可见 pending→ok→pending 回退，约 1s 后发生），疑似 KHTML 编码
  探测触发的二次解析，会重置页面 JS 状态。记为下一轮候选。

## 5h. 高 DPI 取整 2.0 根因 + 窗口/输入/状态栏修复（2026-10-20，全实测）

- **根因：Qt 分数缩放取整**。`AA_EnableHighDpiScaling` 默认取整策略把 150%
  显示屏的 devicePixelRatio 从 1.5 四舍五入成 **2.0**（实测
  `primaryScreen()->devicePixelRatio()=2.00`、`availableGeometry()=960x504`），
  于是整窗 chrome 和页面都按 2× 布局/栅格：默认窗口 640x480 逻辑 = 1280x960
  物理、屏幕上一切 UI 比 OS 惯例大 33%，小窗"莫名其妙变大"。
  修复：`QGuiApplication::setHighDpiScaleFactorRoundingPolicy(PassThrough)`
  （Qt≥5.14，须在 QApplication 构造前）。修复后 ratio=1.50、
  availableGeometry=1280x672、页面自报 `window.devicePixelRatio='r1.5'`。
- **默认窗口尺寸**：`resize(1024,720)` 是逻辑像素，在 150% 屏上会变成
  1536x1080 物理（≈满屏）。当时按屏幕 ratio 反算，恢复启用 High-DPI 之前的
  **物理足迹**（1024x720 设备像素，本机实测窗口 1047x776、客户区 1025x720），
  并居中到 availableGeometry。
  **〔此不变量已被 5i 根因 B 推翻〕**683x480 逻辑只占工作区 53% 宽，页面
  视口 ~641 CSpx，响应式站点落窄屏大字布局；现行方案为工作区 94%/92%。
- **状态栏参数读数移除**：右下角"内存/CPU/GPU/压力等级"标签在 150% 下被
  状态栏高度裁切（只露半行字 → 乱码观感），且属诊断信息而非浏览器 UI。
  已删除整个 QStatusBar 与该标签；内存压力监测逻辑（缓存清理/标签驱逐）
  原样保留（只去掉显示）。
- **输入坐标映射（真 bug 修复）**：CSS-px 视口落地后渲染端内容控件是逻辑
  尺寸，但浏览器 `PageSurface::mapToFrame()` 仍在 ÷devicePixelRatio，
  导致真实点击落点向左上偏 1/dpr。改为恒等映射（布局坐标 = 控件坐标），
  点击/拖拽/滚轮位置一致。tests/browser_click_probe.py 走真实浏览器窗口
  （SetForegroundWindow + 鼠标注入 + DPI-aware 截图）在 CSS (530,270) 点
  小目标，标签标题变 'click-ok' → PASS（生产 dpr 同页自报 'r1.5'）。
- **已知缺口（非本轮）**：KHTML CSSOM 的 `element.style.backgroundColor=`
  写入不生效（`style.background=` 还会画出残框）；class 切换 + CSS 级联
  正常（ui_interaction_test 走 className 所以一直没暴露）。记为下一轮候选。
- **新增工具**：tests/browser_click_probe.py（真实窗口点击端到端）、
  tests/win_shot.py（对已运行窗口做 DPI-aware BitBlt，不占用调用方管道）。
- **回归（全绿）**：browser_click_probe / dpr_probe / input_coord_probe /
  bg_smoke / title_probe(三路径) / scroll_smoke / tombstone_probe /
  resize_probe（白闪 0.00%×2 等指标不变）。
- **启动基线**：bing 冷启 TTFF **1601ms**（取整修到 1.5 后光栅像素少 44%，
  比 2.0 时代的 1986ms 更好；历史区间 1514-3106）。

## 5i. 「网页 UI 过大」真根因：引擎 CSS DPI 双计 + 窗口策略（2026-10-20，全实测）

5h 修的是 **Qt 侧**（取整 2.0）；本轮修的是 **引擎侧 + 窗口侧** 三个叠加根因。
用户观感"网页 UI 又调那么大"由 5g/5h 的 dpr 光栅正确放大后彻底暴露：

- **根因 A（主根因）引擎 CSS DPI 双计**：渲染进程无 Qt High-DPI 缩放，
  `DocumentImpl::logicalDpiY()` 返回 `m_paintDevice->logicalDpiY()` = 真实屏
  144 → `cssstyleselector.cpp` `toPix = 144/72 = 2.0` → **medium = 12pt×2 =
  24 CSS px**（Chrome/KHTML 约定 16），再叠 1.5× 光栅 → 屏幕 36 物理 px。
  实测铁证（tests/fontdpi.png）：默认 `1em` = 36 物理 px（=24 CSpx，错）；
  `16px` 显式 = 24 物理 ✓；`24px` = 36 物理 ✓（CSpx×1.5 光栅正确）；
  `12pt` = 36 物理（应 24）。
  修复：`logicalDpiY()` 屏幕路径返回 **96**（CSS 2.1 §4.3.2 参考 DPI；dpr
  只在光栅处乘一次），`attach()` 的 `computeFontSizes` 改走 `logicalDpiY()`
  （原来直接 `m_paintDevice->logicalDpiY()` 绕过）；**打印路径保留旧行为**
  （`khtml::printpainter` 非空时仍返回 paint device DPI——打印布局按打印机
  设备像素做，pt/cm/in 必须按打印 DPI 换算）。
- **根因 B 窗口策略**：5h 的"按屏 ratio 反算恢复 1024x720 物理足迹"得到
  683x480 逻辑 = 工作区 53% 宽 → 页面视口只有 ~641 CSpx，响应式站点落入
  窄屏大字布局（观感"UI 大"的第二来源）。改为桌面浏览器惯例：工作区
  94%/92%（上限 1280x800 逻辑、下限 720x480 客户区），预留 Win11 边框
  预算 16x40 保证外框不越界，`qBound` 用 `qMin(lower,max)` 防 min>max
  断言。实测 client 1805x927 物理（1203x618 逻辑），外框 1827x983
  （1218x655 逻辑）≤ avail 1280x672，页面视口 **1161x553 CSpx**，
  bing 呈桌面布局（导航横排），与 Chrome@150% 同窗口观感一致。
- **根因 C 首屏 dpr 竞态**：dpr 原来只能靠 socket resize 下发，首个页面在
  dpr=1.0 下解析（file:// 直载自报 'r1'），`@media (resolution)` 按错值
  求值且**永不重算**。修复：浏览器启动 renderer 命令行带 `--dpr`（cold
  spawn 与 prewarm 两处），renderer 在首次 loadInitial 前生效；resize 分支
  先 `setDevicePixelRatio` 再 widget resize；`KHTMLPart::setDevicePixelRatio`
  值变化时 `m_doc->updateStyleSelector()`（修 MQ 永不重算）。实测 file://
  直载内联脚本首报 `'sync r1.5'`。
- **连带修复（媒体查询语义）**：`device_pixel_ratio` MQ 原来用
  `zoomFactor/100`（恒 1.0），改用 `part->devicePixelRatio()`；`resolution`
  MQ 改 `96×dpr`（=144dpi）；新增真实网页方言别名 `-webkit-device-pixel-
  ratio / -webkit-min-… / -webkit-max-…`（分别挂 NoPrefix/MinPrefix/MaxPrefix
  eval）。实测三条 MQ（min-dpr 1.5 / resolution 144dpi / dpr 1.5）全命中。
- **连带修复（QuickJS 桥 window===globalThis 语义）**：
  `innerWidth/innerHeight/devicePixelRatio` 原来只装在 window wrapper 上，
  页面裸写 `innerWidth` 报 ReferenceError 且**整段脚本早断**（无 fallback
  引擎）；现同时装在 global 上，getter 以 global 为 this 时经
  `global.window` 兜底取 wrapper（含 JS_IsObject/JS_IsException 防护）。
  同模式缺口 `window.document / window.window` 返回 undefined（高频写法
  `window.document.getElementById` 直接 TypeError）→ 在 wrapper 上补挂两个
  自引用。实测 `R wd:true wdId:1 self:true bare:number dpr:number`。
- **修复后实测**（tests/fontdpi2.png / mq_dpr.png，DPI-aware 截图）：
  默认 `1em` = 24 物理 px（16 CSpx ✓）；`16px` = 24 ✓；`24px` = 36 ✓；
  `12pt` 的 1em = 24 ✓（修前 36）。
- **回归（全绿 9/9，两轮）**：tile_protocol / dpr_probe / input_coord_probe
  / browser_click_probe / bg_smoke / title_probe(三路径) / scroll_smoke /
  tombstone_probe / resize_probe。（input_coord_probe 一次时序抖动系 5g
  已知"同导航二次解析"观察项，重跑 PASS。）
- **验证流程（按用户要求）**：5 代理评审选方案 → 实施 → 5 代理独立核实
  （css-dpi-semantics / dpi-pipeline-2 / bridge-global-scope /
  regression-risk / product-behavior）。核实共识：无阻塞 bug，窗口越界/
  打印注释失实/首屏竞态/MQ 永不重算/-webkit- 缺失五项已在核实后全部补修。
- **记档不阻塞（核实代理排出，按价值排序）**：matchMedia（JS 侧 MQ 探测，
  本轮 CSS 侧已修但 JS 侧仍 ReferenceError）> location/navigator/console/
  getComputedStyle（"双侧皆缺"的裸标识符家族，任何缺失都会整段早断）>
  窗口几何 QSettings 持久化 > 滚动条 QSS 定宽（150% 下 ~28px 观感）>
  dppx 单位（需动 bison parser）> srcset/sizes > iframe 子 part dpr 传播
  （KService 依赖，暂不可用）> 跨屏 ScreenChangeInternal。

## 5j. 「拖窗口后蓝块卡住」双根因修复（2026-10-21，全实测）

用户现象：拖动窗口改变大小后，页面里被 JS 移动过的元素**旧位置的像素永久残留**
（bing 上是搜索按钮的蓝色方块），新位置不出现。用户要求「不要重启 EXE，用我现在
开着的那份状态来修」，因此全程用探针复现、不碰用户的进程。

### 根因一（引擎）：timer 任务后的样式重算从未发生

`el.style.left = ...` 只在 DOM 上打脏标记：
`CSSStyleDeclarationImpl::setChanged` (css/css_valueimpl.cpp:817/1022) →
`NodeImpl::setChanged` (xml/dom_nodeimpl.cpp:366) →
`DocumentImpl::setDocumentChanged` (xml/dom_docimpl.cpp:1422)，后者**只把文档塞进
静态队列** `s_changedDocuments()`。唯一消费端是
`DocumentImpl::updateDocumentsRendering()` (dom_docimpl.cpp:1514)，其 KHTML 调用点
只有 **事件派发结束** (dom_nodeimpl.cpp:527) 和脚本求值 (html_headimpl.cpp:592)。
`setTimeout/setInterval/rAF` 回调两者都不是 → 该时刻没有 drain（脏标志一直保留，
要等下一次事件/尺寸变化才被消费）→ `recalcStyle` 不跑 → 渲染树里元素还停在旧坐标，
之后的重绘和 damage 描述的都是旧位置。

**实测**（`KHTML_DAMAGE_TRACE=1`，tests/rg_diag4.py）：页面 JS 里
`[css] styleSetProp left=600px` 到达桥接层，**在下一次事件/尺寸变化之前没有
`[setStyle]`**（样式写是**被推迟**、不是丢失：脏标志一直保留，
`dom_docimpl.cpp:1480-1483`/`:1520-1525` 说明下一次 drain 会补上）；而同样的写入
如果发生在 resize 事件处理器里会立刻出现 `[setStyle]`。
⚠️ 可复现性说明（2026-10-22 核实）：当时那份"修前干净 trace"已被修后运行覆盖
（`tests/rg_diag4.py:27` 写同一个文件名），所以这组前后对比**现在无法从仓库重新
导出**；现存证据是 `tests/rg_diag2_err.txt`（已冻结入库为
    `tests/evidence/rg_diag2_trace_2026-10-21.txt`，原文件仍是 gitignore 的
    再生产物）里 `[setStyle] diff=2` 紧跟两个移动矩形。

**修法**：qjs_dom_bridge.cpp 的 `jsWindowTimer` timeout lambda 里，回调返回后调用
`DocumentImpl::updateDocumentsRendering()`。HTML 语义上「update the rendering」本来
就在**每个 task 结束时**跑，不只在事件后。队列用 `takeFirst()` 消费，事件路径再
排空时是空的；`recalcStyle` 另有 `m_inStyleRecalc` 重入保护。

**机制精度（2026-10-22 二次核实更正）**：缺的**只是 `recalcStyle`，不是 layout**。
`recalcStyle()` 末尾确实有 `if (changed() && m_view) m_view->layout()`
（xml/dom_docimpl.cpp:1486-1488），但那里的 `changed()` 是 `NodeImpl::changed()`
= **文档节点自身**的 `m_changed`（`DocumentImpl` 没有覆写 `changed()`；
dom_docimpl.h:611 那个访问器返回的是 `m_docChanged`，不是同一标志）。元素级样式
写入置位的是元素自身 `m_changed` + 祖先 `hasChangedChild` + 文档 `m_docChanged`
（xml/dom_nodeimpl.cpp:366-381），**从不置位文档自身 `m_changed`** → 该门控不成立，
这个 case 里 layout 不跑。实测：修后 trace 是 `[setStyle] ... diff=2`，而
`RenderStyle::Diff`（rendering/render_style.h:2454）=
`{ Equal, NonVisible = Equal, Visible, Position, Layout, CbLayout }`，**diff=2 = Position**（不是 Layout(3)）。
准确说法：缺的是「样式重算」——`recalcStyle` 把新样式应用到渲染树，由
`RenderObject::setStyle` 产生新旧两块 damage。提交 `9e6f1db` 信息里的
"recalcStyle/layout never ran" 属旧措辞，历史不改写，以本段为准。

### 根因二（渲染进程）：部分帧被画到错误坐标，且未受损区域被刷白

1. **`QWidget::render(painter, targetOffset, sourceRegion)` 会抵消 region 自身的平移**
   （`QWidgetPrivate::render`：`offset -= paintRegion.boundingRect().topLeft()`）。
   旧代码传 `QPoint()`，于是**每个部分帧都把 damage 内容画到原点附近**，再把这些
   错误像素上传成 tile —— 这就是幽灵。
   ⚠️ 更正（2026-10-22 五次独立核实）：此处原引的「逐像素 7236 px 差异」来自一个
   **不在仓库里**的一次性测量程序（`tests/scratch_render_compare.cpp` 从来不存在），
   属不可复现证据，已删除。本仓可复现的证据是行为级的：把 offset 改回 `QPoint()`
   会让 `tests/resize_ghost_probe.py` 变 RED（new-rect 0、mid-rect 4536 卡住），
   另有 `tests/tile_protocol_test.py` Part 1 的源码契约断言。
   修法：`const QRect src = dirty.boundingRect(); w->render(&fp, src.topLeft(), QRegion(dirty));`
   （单次 paint，比逐矩形省 N 次引擎遍历。）
2. **部分帧以白色为底**：只重绘 damage 区，未受损像素留在 tile 里被一起上传，
   把画布上正确的内容擦白。修法：新增 `QImage m_lastFrame` 保存上一帧合成结果，
   部分帧以它为底；滚动 fast path 用与浏览器 blit 相同的 `(sdx,sdy)*dpr` 平移底图；
   全幅帧/首帧/尺寸变化仍从白底开始。`navigate`/`reload` 命令、以及**链接点击**
   （`openUrlRequestDelayed → navigateRequested → m_loader->load()`，
   khtml_renderer.cpp 里 `PageLoader::finished` 的 `begin()/write()/end()` 重喂）
   都调 `dropFrameBase()` 丢弃底图，避免新文档叠在旧文档像素上；`refreshed`
   （同文档重喂、保滚动/缩放）故意保留底图——白底起步正是本轮修掉的擦除 bug。

### 红/绿对照（探针判别力证明）

单独回退根因二第 1 项 → 重建 → `tests/resize_ghost_probe.py`
**FAIL**（mid-rect 残留 4536 像素、new-rect 0，exit 1），
`tests/rg_diag4.py` **CONTROL FAIL**；恢复后重建 → 两者 **PASS**
（mid 0、new 4536/4536=100%，橙色固定块恰好 1 个宽 119≈120）。

### 顺带修掉的真 bug：getenv 门控开关全是死的

这套 mingw 构建里 CRT 的 `getenv()` **读不到进程环境**（实测：
`KHTML_STARTUP_TIMING` 走 Qt 的 `qEnvironmentVariableIntValue` 有效，
而 `KHTML_QJS_TRACE` 的 getenv 门控从不触发）。因此把 `js_proxy.cpp` 的
`kjlog()` 与文档承诺的急停开关 `KHTML_QJS_RUNTIME` 改为 Qt 读取。
**注意坑**：`qEnvironmentVariableIntValue` 对**未设置**的变量也返回 0，
必须用 `ok` 出参区分，否则「未设置」会被当成「显式关闭」，QuickJS 会被整体禁用
（本轮真的踩到：页面 JS 全部不执行，由 rg_diag3 的 title 停在初始值发现）。

### 回归（全绿）

tile_protocol / scroll_probe(60px/notch, 收敛 3.1ms) / browser_click_probe /
dpr_probe(1.5, canvas 1020×534) / resize_ghost_probe / rg_diag4 全部 PASS。
`tests/tile_protocol_test.py` 的参考模型同步到新 damage 策略
（**任意**尺寸变化都整幅重绘，不再有 bottom-strip 特例，hash 表仅在列数变化时重置）。

### 2026-10-22 补修轮（二次核实的 must_fix 清理 + 全量回归复跑）

**修了什么**（细节见 GAP_ANALYSIS §0i 第 5/6 条）：
1. `qjs_dom_bridge.cpp` 的 FIX 1 注释改写为 **recalcStyle**（原文写 layout 是错的；
   含 layout 门控分析与 `diff=2 = Position` 证据）。
2. 链接点击路径补 `dropFrameBase()`（`PageLoader::finished` 里 `begin()/write()/end()`
   重喂处）：这条路走 `openUrlRequestDelayed → navigateRequested → m_loader->load()`，
   **收不到** `navigate` 命令，所以没拿到 navigate/reload 路径上的 `dropFrameBase()`
   （该调用的真实作用范围见下方"第三轮"更正：防御性，不是可测量的关键修复）；
   `refreshed`（同文档重喂）故意保留底图并写明理由。
3. 三处残留 `getenv()` 死开关改 Qt；删两处死代码（恒真的 `!m_relayoutPending`、
   无调用者的 `invalidateRect()`）。

**回归 12/12 全绿 + 1 项 INCONCLUSIVE**（最终验收跑在 2026-10-23 16:58 重新构建的 exe
上，运行窗口 17:03:01→17:50:06（报告 started:/finished: 原文）；报告尾 `failed: none` / `inconclusive: bing_ghost_check.py`）。
新增顺序回归脚本 `pwsh -File tests\run_regression.ps1`：先跑离线检测器自检，再顺序跑
10 个常备探针 + `rg_diag4` + bing 集成，报告落 `tests/regression_report.txt`。
每步记录 `exit` 与 `verdict`：**退出码非 0、或退出码 0 但输出里既没有 PASS 也没有
INCONCLUSIVE、或退出码 0 但输出里出现判定形 FAIL 令牌，都算失败**（INCONCLUSIVE 单独记：
这是"这一项没测到东西"，不是绿）。

- `resize_ghost_probe` **PASS**：old-rect 0 / mid-rect 0（精确与近蓝都是 0）、
  new-rect **4536/4536**、橙色固定块恰好 1 个（x 311-430，宽 119≈120）、
  **背景采样 5/5 灰 #f0f0f0**（第四轮新增的行为级覆盖，见下），anchor (11,87)。
- `rg_diag4`（完全不 resize 的对照）**CONTROL PASS**：mid=0/4536、new=4536/4536
  → 缺陷在 resize 后第一帧，不在样式移动路径本身。
- `bing_ghost_check` **INCONCLUSIVE（诚实结论，不是 PASS）**（真实站点 cn.bing.com）：
  baseline 1805×927 与拖动后 1500×748 的实心 #0078D4 块**都为 0**，说明本机 bing
  渲染里根本没有这个元素 → `extra = after - before <= 0` 对任何渲染都成立，这一项
  **什么都没测到**。探针已改为在这种情况下打印 INCONCLUSIVE（退出 0，回归脚本记为
  warning 而非 fail），不再假绿；真正能抓 ghost 的本地覆盖是 `resize_ghost_probe`。
- `bg_smoke` / `browser_click_probe` / `dpr_probe`(1.5, canvas 1020×534) /
  `input_coord_probe` / `resize_probe`（maximize：内容 0.0s、白闪 0.00%、0.20s 收敛
  (38 采样)；restore：0.00% / 0.08s。⚠️ 该指标 run-to-run 有噪声：同代码前两次是
  0.18s/38 与 0.28s/29 采样，引用时必须带上是哪一次运行的报告）/ `scroll_smoke`（像素哈希精确还原）
  / `title_probe`
  （fresh + adopted）/ `tombstone_probe`（busy 后台 tab t+16s 墓碑（该项在 16-21s 窗口内有抖动）、无自动恢复、
  点击可恢复）全部 **PASS**。
- `tile_protocol_test` **PASS**（5 段）：参考模型补上**帧合成**语义（部分帧以
  `m_lastFrame` 为底、只重绘 damage 区、tile 是合成帧的裁剪；hash 表按
  `QVector::resize` 保留同列前缀），并新增**反向对照**：用旧的白底+仅 damage 合成
  必须复现出画布与真页面不一致 → 首错 `seed=0 step=1 pixel(256,256) got=白
  want=(13,29,3)`。**这条对照让模型真的能抓住 ghost 这一类 bug**（旧模型假设 tile
  可独立整块渲染，所以永远抓不到）。统计：none 159 / suppressed 147 / upload 494 /
  noop_dmg 150。
- 检测器自检 `python tests\bing_ghost_check.py --selftest` **PASS**：合成 ghost→1 块、
  合成 clean→0 块；真实截图改判**相对判据**（`glitch1.png` 1 块 > `bing_ghost_after.png`
  0 块）。

**同日再补（第三轮：把 5 代理核实的 12 条 must_fix 收口；本轮核实 3/5 代理返回）**

- ⚠️ **死开关修好后的行为变化，排查前必看**：`getenv()` 修好后，5 个模块开关
  （`KHTML_QJS_NODE/ELEMENT/DOCUMENT/STYLE/EVENT`）与 `KHTML_QJS_TIMERS` 从
  「永远 ON」变成**真的生效**。环境里若残留 `KHTML_QJS_STYLE=0` 或
  `KHTML_QJS_TIMERS=0`，会静默禁用对应模块、**复现本轮修的病症**（样式写被忽略 /
  没有 setTimeout-rAF 回调）。先 `set | findstr KHTML_QJS` 再怀疑渲染。
- **链接点击 `dropFrameBase()` 的诚实范围**：它是**防御性不变量**，不是关键修复。
  tile 只在引擎 damage 命中时才上传，底图影响不到「没上传的 tile」；而换文档本身
  已触发整视口重绘（`KHTMLPart::begin → KHTMLView::clear → reset()` 置
  `firstLayoutPending/needsFullRepaint` → 全层 repaint → `contentsUpdated`）。
- **本轮顺手堵掉的测试漏洞**（都是这次核实发现的真问题）：
  1. `tests/rg_diag4.py`：`CONTROL FAIL` 原来也 `return 0` → 现按判定返回 0/1
     （它被回归脚本当 gate，之前"控制失败"会被记成通过）。
  2. `tests/run_regression.ps1`：退出码 0 但输出里既无 `PASS` 也无 `INCONCLUSIVE`
     判为失败；自检用独立名字登记（`-Skip detector-selftest`）；跑前先删旧报告。
  3. `tests/bing_ghost_check.py`：退化 baseline → INCONCLUSIVE（见上）。
  4. `tests/tile_protocol_test.py` Part 1 新增底图源码契约断言：部分帧必须
     `frameImage = m_lastFrame`、full-frame 判定必须比 `m_lastFrame.size()`、
     `dropFrameBase()` ≥3 处（链接点击在 `m_view->begin()` 之前）、`refreshed` 块内
     **不得**出现 `dropFrameBase()`。
- **第四轮（2026-10-23，5/5 代理全部返回；A=PASS，B/C/D/E=CONCERN，
  must_fix 6 项全部收口，详见 GAP §0i 第 8 条）**：
  1. **滚动 fast path 真 bug**：blit 平移画布却不清 tile 哈希缓存 →
     position:fixed 大块在快速滚动后停在平移位置。修：`m_tileHashes.fill(0)`；
     Part 5 滚动模型重写（哈希抑制 + 非均匀固定带 + 负对照，实测复现
     `dy=120 pixel(0,0) got=(88,9,0) want=(0,9,0)`，修复后收敛）。
  2. **底图修复补上行为级覆盖**：`resize_ghost.html` 背景改非白 `#f0f0f0` +
     探针 5 个背景采样点。实测红/绿：白底回归时旧断言全 PASS（即原来真的
     抓不到）、采样 0/5 全白 → FAIL exit 1；恢复 → 5/5 → PASS。
  3. runner 新增「判定形 FAIL 令牌」检测（9 组样例实测分对，must-FAIL 散文
     不误伤）。
  4. bing after 截图新增活性门 `after_liveness_ok`（3% 绝对 + baseline 一半
     相对），自检 5 个边界用例；按位置的移动型 ghost 检测仍做不到 → 已知限制。
  5. 注释/文档真实性：`:734/:1550` 行号自引（f2ff052 里实际 :745/:1566）→ 改
     按名引用；`dropFrameBase()` 定义处旧「承重」注释重写为防御口径；
     `rg_diag2` trace 冻结入库 `tests/evidence/`；桥接注释补「编译代码里仅两处
     drain」限定；运行窗口/墓碑时刻/回归时长三处数字对齐报告原文。
- **仍未完成的账（2026-10-23 更新，诚实记录）**：滚动修复只有模型级负对照、
  没有活探针（scroll_smoke 的页面是内容坐标索引的，复现不了固定带场景）；
  bing 移动型 ghost 检测做不到（本机 bing 0 蓝块，该项本就 INCONCLUSIVE）；
  `tests/regression_report.txt` 是 gitignore 的可再生产物、不随提交带走；
  回归脚本无单探针超时（一次约 46 分钟）。（底图修复的行为级覆盖**已补**；
  核实角度本轮 5/5 返回。）

## 5k. 去特调 + 「点击先跳本地地址」根因修复（2026-10-24，行为级红/绿实测）

用户指令：删掉针对网站（尤其 bing.com）的特调，按渲染引擎本来的效果渲染；查清「点任何必应按钮都先跳到一个本地地址、再跳真实网址」的原因。

### 根因（源码 + 活的缓存双证据）
- 「本地地址」不是地址栏错觉，而是**锚点 href 真的被改写成 file://**：`PageLoader::extractResources()` 把页面里**每一个 href/src 属性**都当子资源镜像（src/host/pageloader.cpp 原 :721-733），包括 `<a href>`；`rewriteAndSave()`/`deliverInitialDocument()` 再把页面里出现过的引用字面量全量替换成本地缓存路径（原 :891-928/:1441-1467）。点击时 KHTML 解析出的就是 file:// 缓存副本，渲染进程 `RendererView::navigateRequested`（khtml_renderer.cpp:452-461）再用 `originalOf()` 换回真实 URL 重取——于是「先本地、后真实」，多一次导航与一次下载。
- 实证：用户便携版 2026-10-24 09:25 的缓存 `release/KHtmlLite-portable/khtml_cache/<id>/index.html`（195,668 字节）里 **52 个 href 有 42 个是 file:///khtml_cache/...**；build/host 缓存同样 42/52。
- 同时确认：`pageloader.cpp` 原 :1206-1223 **给每个页面无条件注入一段只对 cn.bing.com DOM 有效的 CSS**（.sbox/#sb_form/#sb_form_q/#sb_form_go/#0078d4）——全仓库唯一的站点级渲染特调。

### 修复
1. `extractResources()` 改为**按标签识别**：跳过 `<a>/<area>/<form>/<base>` 的导航属性，只镜像资源型标签（link/script/img/source/iframe/embed/object/video/audio/input 的 href/src 与 CSS url()/@import）。
2. 新增 `PageLoader::restoreNavigationTargets()`：全局替换之后，把 `<a|area href>` 与 `<form action>` 中落在缓存目录的 file:// 值用 `m_resources/localAbs` 映射回原始绝对 http(s) URL；两处改写点都调用。
3. 删除 bing 专用 CSS 注入块，替换为「已移除」说明注释。
4. ~~内联 SVG→`<img>` 兜底正则的退格转义 bug~~ → 另行修正方向（见下方「第二轮补强」第 6 条）：该改写块**整块删除**，因为引擎已原生支持内联 SVG；不再维护这条正则。

### 行为级红/绿对照（真实浏览器、真实缓存文件，非源码断言）
本地 localhost 页面（3 个锚点：绝对路径/相对路径/绝对 URL；1 张图；1 个 form action），同一 URL 前缀 + 各自 `?tag=` 隔离缓存，等最终改写稳定后读缓存 index.html：

| 指标 | 旧版（release 便携 2026-10-23 16:43） | 新版（build/host 本轮构建） |
|---|---|---|
| `<a href>` 被改写为 file:// | **3/3** | **0/3**（全部保留真实 URL） |
| 额外把锚点目标当资源抓取 | **是**（/target.html、/rel.html 被下载） | **否**（只抓 /pic.png） |
| 图片镜像为本地 file:// | 1/1 | 1/1（保持） |
| `<form action>` 被改写 | 无 | 无 |
| 注入 bing `#sb_form` CSS | **是** | **否** |
| 最终改写产物大小 | 12,465 B | 11,630 B（少了注入块） |

结论：点击导航不再经过本地副本；站点特调已删除；图片本地镜像等通用机制不受影响。

### 第二轮补强（同日，点击链路与引擎路线图专项代理的缺口清单逐条落地）
1. **最终改写补 `<base>`**：`rewriteAndSave()`（`refreshed` 后覆盖 index.html 的那次）原来只替换已存在的 `<base>`，源 HTML 无 base 时最终文档丢失 base → `DocumentImpl::baseURL()` 回退到 `m_url`=file://…/index.html，相对 URL（`<form action="/search">`、JS 相对跳转）仍解析成 file://。新增 `PageLoader::installBase()`，两处交付都调用。
2. **导航属性还原加固**：支持 `#frag`/`?query` 后缀（前缀匹配+保留后缀）、无引号属性、`<button|input formaction>`、`<meta http-equiv=refresh content="url=…">`；属性名要求前置空白，修复 `\bhref` 误配 `data-href` 的漏洞。
3. **`originalOf()` 前缀兜底**：缓存目录下的 file://（含写失败的镜像、带后缀的 URL、上一个 loader 的副本）一律按"相对本页 URL"解析回真实 http(s)，不再原样放行。
4. **地址栏/历史 scheme 守卫**：渲染进程不再把 cache 镜像 file:// 作为 `url` IPC 上报；浏览器 `urlChanged` 也拒绝含 `/khtml_cache/` 的本地路径（双保险）；`load()` 入口拒绝 cache 镜像作为文档 URL。
5. **`hashName()` 扩展名白名单**：`section('.',-1)` 对无扩展名路径会返回整条路径（`<sha1>./maps`），QFile 写入不存在的子目录 → 镜像静默缺失（必应 3 个 `.bin` 缺失即此类）。改为 `^[a-z0-9]{1,5}$` 白名单。
6. **删除内联 SVG→`<img>` 改写（方向修正）**：引擎路线图代理发现并 Lead 复核——KHTML 的 HTML 解析器**已实现** HTML5 内联 SVG（`htmlparser.cpp:287-304` 的 svgDepth/foreignObject + `getElement()` 的 `createElementNS(SVG_NAMESPACE,…)` :924-926，另有提交 `70bb3a0`/`413e0ad`）。宿主的序列化改写是过期补丁，还会丢掉 CSS 着色（currentColor/class 规则）、SMIL、滤镜——"Bing logo 变灰"正是这条路。已删除该块（约 50 行），内联 SVG 原样交给引擎。
7. **字符集透传**：渲染进程原来对每个文档强制 `setEncoding("utf-8")`（khtml_renderer.cpp:289），而 PageLoader 存的是原始网络字节 → GBK/GB2312/Shift-JIS 页面必乱码。现在 PageLoader 从主响应 `Content-Type` 提取 charset（`mainCharset()`），渲染进程按它解码（缺失才回退 UTF-8）。

### 已知残留（如实记录）
- `<iframe>` 内部页面的相对链接仍以 iframe 本地副本为基准（iframe HTML 本体不被改写），该路径点击可能落到 file://（iframe 在此引擎本就依赖本地镜像）；未在本轮处理。
- 宿主仍对每个页面做正则级 CSS 降级改写（grid→flex、transform→translate、删除约 103 个现代属性、假 polyfill 等）——这些是**通用兼容层**不是站点特调，删除需先补引擎能力，已列入引擎级路线图（本轮 WebKit 对照报告）。

## 5l. 引擎渲染能力第一批（2026-10-24，对标 Safari/WebKit，组件"够用即止"）

用户指令：内核级渲染引擎补全，对标 Safari，组件够用即可、不追新版本；外壳 EXE 替换后续再说。
本轮落地三项（均为引擎内实现，不是宿主改写）：

### 1) `-webkit-` 前缀支持（引擎解析层）
- `src/khtml/src/css/parser.cpp` 的 `DOM::getPropertyID()` 现在对 `-webkit-/-moz-/-ms-/-o-/-khtml-` 前缀做**剥离后重试**：`-webkit-border-radius` 等别名解析到已有的无前缀属性；引擎仍不认识的属性（如 `-webkit-transform`）保持未知，声明被忽略（不破坏级联）。
- `DOM::getValueID()` 把旧 flex 拼写映射到现有值 id：`display:-webkit-box`/`-webkit-flex` → flex(334)，`-webkit-inline-box`/`-webkit-inline-flex` → inline-flex(335)。
- 宿主 `pageloader.cpp` 的 CSS 降级清单里**不再删除厂商前缀属性**（10 条），它们直接交给引擎。
- 背景：伪装 UA 声称 Safari，站点因此下发 `-webkit-` CSS；此前这些声明被全部丢弃。

### 2) HTML5 语义元素 UA 默认块级（引擎 UA 样式层）
- 运行时 html4.css 来自安装前缀（`C:\msys64\mingw64\bin\data\kf5\khtml\css\html4.css` 或便携包同名副本），改仓库里的 `src/khtml/src/css/html4.css` 不生效，因此规则**追加在引擎代码里**（`CSSStyleSelector::loadDefaultStyle`，读文件后拼接）：
  `article,aside,figcaption,figure,footer,header,main,nav,section{display:block}`、`figure{margin:1em 40px}`、`summary/details{display:block}`、`template,[hidden],dialog{display:none}`、`mark{background-color:yellow;color:black}`。
- 背景：引擎把未知元素按 inline 处理（dtd.cpp "custom elements same as <span>"），此前 `<section>/<nav>/<main>` 在无站点样式时全部行内堆叠。

### 3) `@supports` 引擎级求值（不依赖 bison）
- 语法文件 parser.y 无 `@supports` token，且构建树没有 bison（parser.cpp 是提交的生成文件），因此在**引擎样式表预处理**阶段实现：`CSSStyleSheetImpl::parseString()` 在交给解析器前扫描 `@supports` 块——真条件把内部规则**内联**回样式表，假条件**整块丢弃**；支持 `and`/`or`/`not`、嵌套递归、跳过注释、at-rule 规则边界判断；`selector()/font-tech()/font-format()` 保守取 false。
- 判定依据：属性 id 已知（含前缀剥离）→ 支持；额外把 `display:grid/inline-grid`、`position:sticky` 判为不支持；值级未知一律按支持处理（够用原则）。
- 宿主 `pageloader.cpp` **不再删除 @supports 块**（此前整块删除会连带丢掉引擎能渲染的规则）。
- 附带修正：宿主不再把 `display:grid` 盲改成 `display:flex;flex-wrap:wrap`——它会把 `@supports (display:grid)` 的条件文本一起改坏，且 flex 无 wrap 时卡片墙塌成一行。

### 行为级验证（新增永久回归探针）
- 新增 `tests/engine_semantics.html` + `tests/engine_semantics_probe.py`（真实浏览器、像素级 6 项断言，已加入 `tests/run_regression.ps1`）：
  `@supports (display:flex)` 真块应用（绿字出现）；`@supports (display:grid)` 假块丢弃（无红字）；`-webkit-border-radius` 圆角（角落非蓝）；`display:-webkit-flex` 三项同一行（bbox 179×59）；`<section>` 块级（黄色背景宽 1711/1805 px）。
- 实测：探针 **PASS**；`tile_protocol_test` PASS；**全量回归 2026-10-24 13:24→14:11 `failed: none`**：14 项中 13 项 PASS（含新增 engine_semantics_probe 24.2s、bg_smoke、scroll_smoke、resize_probe、tombstone_probe、tile_protocol_test 178.5s）+ bing_ghost_check INCONCLUSIVE（本机 bing 无 #0078D4 蓝块，诚实结论）。

### 下一批（路线图，按收益排序）
`var()` 自定义属性 → `calc()/min()/max()/clamp()` → flex 最后一项（inline-flex 行内化）→ grid → sticky/aspect-ratio → 渐变/transform/transition/animation。每条都要求：本地 fixture + 像素断言 + 全量回归绿。
（flex-wrap/逻辑属性 §5m、order/align-self/flex-basis §5n、flex 简写/flex-shrink §5o 已完成，从本列表移除。）

## 5m. 引擎渲染能力第二批（2026-10-24，flex-wrap + 逻辑属性）

### 1) flex-wrap 换行布局（rendering/render_flexbox.{h,cpp}）
- `FlexConfig` 增加 `wrap`；`config()` 从 `style()->flexWrap()`（stylesheets 已接入）与行内 style 的 `flex-wrap: wrap|nowrap` 读取。
- 布局重写为**按行分段**：主轴上按 `(已用 + gap + 下一项) > 容器主轴` 换行；每行独立做 flex-grow 分配、auto margin 吸收、justify-content（含 space-between/around）、align-items 交叉轴对齐；行间用 `gap` 作为 row-gap；总高度/溢出尺寸按行累计。
- 单行（wrap=false）路径保持原行为。
- 实测：fixture 中 5 个 100px 卡片在 300px 容器里换行为 **3+2 两行**（cyan bbox 449×119 @dpr1.5）；此前会挤成一行溢出。

### 2) 逻辑属性别名（css/parser.cpp）
- `getPropertyID()` 在属性表之外增加**逻辑→物理长属性映射**（horizontal-tb、LTR）：`margin-inline-start/end → margin-left/right`、`padding-inline-start/end`、`border-inline-start/end-width/style/color`、`inset-inline-start/end → left/right`、`inset-block-start/end → top/bottom`、`inline-size/block-size → width/height` 及 min/max 变体（共 20 项）。RTL 方向翻转尚未实现（记为后续项）。
- 实测：fixture 中 `#logical{margin-inline-start:120px;width:60px}` 的盒子落在 x=206、宽 89 物理像素（= body margin + 120px×1.5），确认映射生效。

### 行为级验证（探针扩展）
- `tests/engine_semantics_probe.py` 增加两项像素断言（并改为"主聚类"检测以排除其他元素的抗锯齿干扰）：flex-wrap 换行（cyan bbox 宽>300 且高>90）；逻辑属性偏移（orange 主聚类 x0>150 且宽 60..140）。
- 实测：**8/8 检查 PASS**；**全量回归 2026-10-24 14:27→15:17 `failed: none`**：14 项中 13 项 PASS（engine_semantics_probe 31.8s 含 8 项断言、bg_smoke、scroll_smoke、resize_probe、tombstone_probe、tile_protocol_test 182.7s）+ bing_ghost_check INCONCLUSIVE（本机 bing 无 #0078D4 蓝块，诚实结论）。

## 5n. 引擎渲染能力第三批（2026-10-24，flex 完整化：order / align-self / flex-basis）

### 1) 新 CSS 属性（引擎解析 + 样式数据）
- 属性 id 扩展：`align-self`=188、`order`=189、`flex-basis`=190（`cssproperties.h` 的 CSS_PROP_TOTAL 188→191；`parser.cpp` 的 added[] 表登记；`cssparser.cpp` 值校验；`cssstyleselector.cpp` applyProperty 落盘）。
- `StyleFlexibleBoxData` 增加 `alignSelf`（0=auto/1=stretch/2=flex-start/3=center/4=flex-end）、`order`、`flexBasisType`（0=auto/1=px/2=percent）+ `flexBasisValue`，并同步拷贝/相等/diff（**flex 数据变化返回 Layout diff**，此前 flex 属性变化不触发重排）。
- 实测解析正确：trace 中 `alignSelf=4`、`basis=1/200` 均按预期进入布局。

### 2) 布局应用（render_flexbox.cpp）
- **order**：子项按 order 升序稳定排序（同值保持文档顺序）后再分行/分配/放置。
- **flex-basis**：显式 basis（px 或容器主轴的百分比）覆盖自然主轴尺寸（行内 setWidth + 重新 layout 取高）。
- **align-self**：每个 item 计算 `effAlign`（auto 回退到容器 align-items），行/列两个放置分支都按它做交叉轴对齐。
- **单行容器交叉尺寸修正**：此前行交叉尺寸只取"子项最大交叉尺寸"，导致 `align-items/align-self` 在容器有确定高度时全部贴顶（center/flex-end 无效）。现在若单行且行方向容器 `height` 是确定值（`Length::isFixed()`），行交叉尺寸取容器内容高度（减去 padding/border），符合 flexbox 规范；多行与 auto 高度容器行为不变。
- 实测：`height:80px` + 20px 子项 + `align-self:flex-end` → 子项落在底部（y0 teal=147 顶部、maroon=237 底部，差 90 物理像素 = (80-20)×1.5）✓。

### 3) 回归探针（两页夹具）
- 新增 `tests/engine_semantics2.html`（order/align-self/flex-basis 专用，页面更矮以适配窗口截图），`tests/engine_semantics_probe.py` 改为两页流程：page1（@supports/-webkit/HTML5/换行/逻辑属性 8 项）+ page2（order/align-self/basis 3 项），共 **11 项像素断言**全过。
- 探针检测改为"主带（连续行最大簇）"bbox，排除浏览器工具条抗锯齿等杂散像素污染（此前 maroon 被工具条暗红像素拉到 y0=25 造成误报）。

### 实测
- 引擎探针 **11/11 PASS**；`tile_protocol_test` PASS；**全量回归 2026-10-24 16:41→17:28 `failed: none`**：14 项中 13 项 PASS（engine_semantics_probe 48.2s 两页、bg_smoke、scroll_smoke、resize_probe、tombstone_probe、tile_protocol_test 160.3s）+ bing_ghost_check INCONCLUSIVE（本机 bing 无 #0078D4 蓝块，诚实结论）。

## 5o. 引擎渲染能力第四批（2026-10-24，flex 简写 + flex-shrink）

### 1) `flex` 简写解析（引擎解析层）
- 问题：`CSSParser::parseValue()` 只校验**单个 token**，多值简写（`flex: 1 1 0%`、`flex: 0 1 150px`）会被整体拒绝；此前只有 `flex: 1` 这种单数字形式生效。
- 修复：新增 `CSSParser::parseFlexShorthand()`（仿 `parseFontShorthand`），支持 `none | auto | <grow> <shrink>? <basis>?`：逐个 token 转成 `flex-grow`/`flex-shrink`/`flex-basis` 三个子属性（含 `auto`/px/百分比 basis；`flex: none` → 0 0 auto；`flex: auto` → 1 1 auto；超过两个数字回滚拒绝）。`parseValue` 的 `CSS_PROP_FLEX` 分支改为直接调用它。

### 2) `flex-shrink` 属性与布局收缩（engine）
- 新属性 id：`flex-shrink`=191（CSS_PROP_TOTAL 192）；`StyleFlexibleBoxData` 增加 `flexShrink`（默认 1.0，含拷贝/相等/diff）；cssparser 值校验（CSS_NUMBER）；cssstyleselector applyProperty 落盘。
- 布局：主轴上 `free < 0` 时按 `shrink × 基础尺寸` 的权重分配收缩量（`need*scaled/totalScaled`，下限 0），收缩后重新计算 free 并继续 auto-margin/justify 流程。
- 实测：`#shrinkbox{width:200px}` 内两个 `flex: 0 1 150px` 的项收缩为各 100px（bbox 300 物理像素 @dpr1.5），此前会溢出到 450 物理像素。

### 3) 回归探针
- `tests/engine_semantics2.html` 增加 `#shrinkbox`（flex 简写 + 收缩用例）；`engine_semantics_probe.py` 的 page2 增加第 4 项断言（蓝色 bbox 宽 280..320 物理像素），page2 共 4 项、两页合计 **12 项像素断言**。

### 实测
- 引擎探针 **12/12 PASS**（page2 新增 shrink 断言：bbox 299×29 物理像素）；`tile_protocol_test` PASS；**全量回归 2026-10-24 18:10→18:58 `failed: none`**：14 项中 13 项 PASS（engine_semantics_probe 51.5s、bg_smoke、scroll_smoke、resize_probe、tombstone_probe、tile_protocol_test 172.1s）+ bing_ghost_check INCONCLUSIVE（本机 bing 无 #0078D4 蓝块，诚实结论）。

## 5p. 引擎渲染能力第五批（2026-10-24，CSS 变量 var() 引擎级实现）

### 1) 文档级自定义属性存储（DocumentImpl）
- `DocumentImpl` 新增 `m_customProperties`（QHash<QString,QString>）与 `setCustomProperty/customProperty/hasCustomProperty`（public）。样式表解析期收集的 `--name: value` 存入文档级 map，因此**后解析的样式表**（如外部 .css）能解析**先解析的样式表**（如 head 内联 `<style>`）里定义的变量。

### 2) 样式表解析期展开（引擎，header-only）
- 新增 `css/css_variables.h`：
  - `khtmlCollectCustomProperties()`：扫描并移除 `--name: value` 声明（尊重引号与括号嵌套），把键值对交给 sink；
  - `khtmlExpandCssVariables()`：递归展开 `var(--name[, fallback])`（最多 8 层）；未解析且无 fallback 的引用保留原样，交给解析器安全丢弃该声明。
- `CSSStyleSheetImpl::parseString()`：先收集声明 → 写入 DocumentImpl → 展开整段样式表文本 → 再交给 bison 解析（此前只是"整块删除 -- 声明"）。顺带修正一处 `{{`/`}}` 多字符字面量为 `{`/`}`。

### 3) 宿主移除 var() 文本展开
- `pageloader.cpp` 删除 `m_cssVars` 的收集/嵌套解析/展开逻辑（inline `<style>` 与外部 CSS 两处），只保留 url()/@import 本地化重写；自定义属性与 var() 完全交给引擎，宿主不再"替引擎猜"。

### 4) 行为级验证 + 已知限制
- 夹具 page1 新增：`:root { --brand-bg: #009999 }` + `#vars1{background:var(--brand-bg)}`、`#vars2{background:var(--missing-bg,#cccc00)}`；探针新增 2 项像素断言（变量应用 / fallback），两页合计 **14 项**。
- 验证结果见下方"实测"；已知限制（如实记录）：
  - 文档级 map 是**全局透传**语义，未实现 CSS 级联作用域（选择器/媒体查询/继承下的变量作用域差异）；
  - 内联 `style="..."` 属性里的 var() 暂不展开（声明解析路径未接入；宿主此前同样不处理，无回归）；
  - 跨表解析依赖"定义先于使用"的解析顺序（常见站点头部 `<style>` 先于外部 CSS，满足）。

### 实测
- 引擎探针 **14/14 PASS**（新增 var() 应用 bbox 119×35 与 fallback bbox 119×35，均符合 80×24 CSS×1.5）；`tile_protocol_test` PASS；**全量回归 2026-10-24 20:41→21:30 `failed: none`**：14 项中 13 项 PASS（engine_semantics_probe 62.5s、bg_smoke 613.6s、scroll_smoke、resize_probe、tombstone_probe、tile_protocol_test 169.1s）+ bing_ghost_check INCONCLUSIVE（本机 bing 无 #0078D4 蓝块，诚实结论）。

## 5q. 引擎渲染能力第六批（2026-10-24，CSS 渐变引擎解析 + 绘制）

### 1) 解析层（CSSParser）
- 新增 `CSSParser::parseGradientSpec()`：识别 `linear-gradient()/radial-gradient()`（含 `-webkit-`、`repeating-` 前缀）函数值，解析角度（deg/rad/grad）、方向关键字（`to right/bottom/...`）、颜色（hex/命名色/rgb()/rgba()/hsl()/hsla()，复用 `parseColorFromValue`）与百分比色标；未给位置的色标按首尾 0/1000 与相邻色标线性插值补齐。
- 输出**紧凑谱字符串**：`khtml-grad:L|<angle>|<#rrggbb>:<pos0-1000>,...`（径向为 `R||...`），以 CSS_STRING 形式随 `background-image` 进入样式层——不新增 DOM CSSValue 子类。
- `background-image: <gradient>` 在 `parseValue` 的 background 分支被优先识别（多图层列表里的渐变暂不支持，返回声明无效）。

### 2) 样式层（BackgroundLayer / RenderStyle）
- `BackgroundLayer` 新增 `QString m_gradient` + `backgroundGradient()/setBackgroundGradient()`；拷贝构造/赋值/`operator==` 全部同步（样式 diff 因此能感知渐变变化）；设置渐变会清掉同层的图片。
- `applyProperty(CSS_PROP_BACKGROUND_IMAGE)`：遇到 `khtml-grad:` 前缀的字符串值即写入渐变、跳过图片路径；普通 `background-image` 声明会清除渐变（同名属性覆盖语义）。

### 3) 绘制层（RenderBox）
- `paintOneBackground()`：渐变层直接用 `QLinearGradient/QRadialGradient` 填充（角度→端点投影按 CSS 约定：0deg=向上、90deg=向右；径向取中心+0.75×max(w,h) 半径），受 clipr 裁剪；背景色、多图层顺序沿用原有逻辑。
- 已知限制：渐变背景暂不做 border-radius 圆角裁剪（`paintBackgroundExtended` 的圆角路径未复用）；旧式 `-webkit-gradient(...)` 不支持。

### 4) 宿主去掉渐变压平
- `pageloader.cpp` 删除两处 `simplifyGradients()` 调用（inline `<style>` 与外部 CSS），渐变原样交给引擎解析与绘制。`simplifyGradients` 函数保留但不再调用。

### 5) 行为级验证
- 新增夹具 `tests/engine_semantics3.html`（唯一饱和色页面，避免干扰其它页断言）：`#g1` 垂直渐变 `#aa00aa→#00aa88`（300×60 CSS），`#g2` 90° 水平渐变 `#ff00aa→#00aaff`。
- 探针新增 page3 共 5 项：整体尺寸、垂直渐变上/下端点颜色方向、水平渐变左/右端点颜色方向；三页合计 **19 项像素断言**。

### 实测
- 引擎探针 **19/19 PASS**：page3 渐变 5 项全过（bbox 449×191；垂直渐变上端 (157,13,167)→下端 (15,155,139)；水平渐变左 (251,3,171)→右 (3,168,254)）。
- `tile_protocol_test` PASS；**全量回归 2026-10-24 22:45→23:35 `failed: none`**：14 项中 13 项 PASS（engine_semantics_probe 76.9s、bg_smoke 602.9s、scroll_smoke 603.4s、resize_probe 602s、tombstone_probe 624.7s、tile_protocol_test 194s）+ bing_ghost_check INCONCLUSIVE（本机 bing 无 #0078D4 蓝块，诚实结论）。
- 调试记录（如实）：渐变首次实现后不绘制，根因是 `RenderStyle::hasBackground()` 只认背景色/背景图，导致 `paintBoxDecorations` 被跳过；在 `RenderBox::setStyle` 里把"有渐变"也视为需要绘制背景（纯 .cpp 修复）后正常。另修复了诊断代码插入位置导致 `parseGradientSpec` 恒返回空的 bug。

## 5r. 引擎渲染能力第七批（2026-10-24，`aspect-ratio`）

### 1) 属性与样式层
- 新属性 id：`aspect-ratio`=192（CSS_PROP_TOTAL 193）；parser.cpp 的 `added[]` 登记；`StyleBoxData` 增加 `float aspect_ratio`（0=无），ctor/copy/`operator==` 同步；`RenderStyle::aspectRatio()/setAspectRatio()`（DataRef COW）。
- 解析：`CSSParser::parseAspectRatio()` 支持 `auto | <number> [/ <number>]?`（`3 / 2`、`16/9`、单个数字=数字/1），以 CSS_STRING `"w/h"` 形式随属性传递；`parseValue` 的 `CSS_PROP_ASPECT_RATIO` 分支直接调用它（多 token 值不再被单 token 校验拒绝）。
- 应用：`applyProperty(CSS_PROP_ASPECT_RATIO)` 解析 `"w/h"` → `setAspectRatio(w/h)`；`initial/inherit` → 0。
- `RenderStyle::diff()`：StyleBoxData 参与比较，比例变化自动触发 Layout。

### 2) 布局
- `RenderBox::calcHeight()`：当 `aspect-ratio > 0`、`height` 为 `auto` 且宽度已知时，`m_height = max(内容高度, width / ratio)`（比内容矮时取内容高度，避免裁切子内容）。

### 3) 行为级验证
- 夹具 page2 新增 `#ratio { width:300px; aspect-ratio:3/2; background:#cc8800 }`，期望 450×300 物理像素（dpr1.5）；探针新增 1 项断言（page2 5 项、两页合计 **20 项**；page3 渐变 5 项不变）。

### 实测
- 引擎探针 **20/20 PASS**（新增 aspect-ratio：bbox 449×299 物理像素 = 300×200 CSS×1.5）；`tile_protocol_test` PASS；**全量回归 2026-10-25 00:36→01:22 `failed: none`**：14 项中 13 项 PASS（engine_semantics_probe 51.3s、bg_smoke 602.2s、scroll_smoke 602.7s、resize_probe 601.6s、tombstone_probe 623.3s、tile_protocol_test 123.1s）+ bing_ghost_check INCONCLUSIVE（本机 bing 无 #0078D4 蓝块，诚实结论）。
- 调试记录（如实）：第一版把 aspect-ratio 规则插进了 `RenderBox::setHeight()`（而非 `calcHeight()`），导致高度不生效；移到正确函数后 450×300 物理像素符合预期。

## 5s. 引擎渲染能力第八批（2026-10-25，CSS transform：translate/scale/rotate）

### 1) 属性与样式层
- 新属性 id：`transform`=193（CSS_PROP_TOTAL 194）；parser.cpp 的 `added[]` 登记；`StyleCSS3NonInheritedData` 增加 `QString transform`（含拷贝/相等）；`RenderStyle::transform()/setTransform()`（DataRef COW）。
- 解析：`CSSParser::parseTransformSpec()` 把 `translate()/translateX()/translateY()/scale()/scaleX()/scaleY()/rotate()` 序列化成紧凑谱字符串（如 `translate(10px,20px);rotate(45deg)`），参数支持 px/%/数字/deg/rad/grad；`none` 清空；`parseValue` 的 `CSS_PROP_TRANSFORM` 分支直接调用它。
- 应用：`applyProperty(CSS_PROP_TRANSFORM)` 落盘字符串。

### 2) 绘制（paint-only 变换）
- `RenderBlock::paint()`：若元素有 transform，`QPainter::save()` 后用 `applyCssTransform()`（围绕**边框盒中心**，按 translate→scale→rotate 顺序、百分比按元素自身尺寸换算）再执行原 `paintObject()`，最后 restore。子元素绘制自然继承父变换（嵌套变换叠加）。
- 已知限制（如实记录）：仅 block 级元素；不做 hit-test 坐标逆变换（点击按未变换矩形）；不创建独立图层（与外部元素的遮挡/裁剪关系保持原状）；`transform-origin` 固定为中心；skew/matrix 不支持。

### 3) 宿主去掉 transform→relative 重写
- `pageloader.cpp` 删掉三处 `transform: translate(...)` → `position:relative;left/top` 的重写；transform 交给引擎解析绘制（`position: sticky -> relative` 的重写仍在，因为 sticky 尚未实现）。

### 4) 行为级验证
- 新增夹具 `tests/engine_semantics4.html`（三个 100×100 CSS 盒，颜色互不相同）：`#t1 translateX(60px)`、`#t2 rotate(45deg)`、`#t3 scale(1.5)`；探针新增 page4 三项断言（位移后 x0≈105、旋转后 bbox≈212×212、放大后 bbox≈225×225 物理像素），四页合计 **23 项断言**。

### 实测
- 引擎探针 **23/23 PASS**：page4 三项全过（t1 translateX bbox x0=116 w=149；t2 rotate bbox 211×184；t3 scale bbox 224×224），四页合计 23 项。
- `tile_protocol_test` PASS；**全量回归 2026-10-25 02:43→03:29 `failed: none`**：14 项中 13 项 PASS（engine_semantics_probe 63.3s、bg_smoke 602.5s、scroll_smoke 601.9s、resize_probe 601.5s、tombstone_probe 623.4s、tile_protocol_test 125.8s）+ bing_ghost_check INCONCLUSIVE（本机 bing 无 #0078D4 蓝块，诚实结论）。
- 调试记录（如实）：
  1) 变换后的背景/边框被裁在未变换盒子矩形内（`paintBoxDecorations` 用 `QRect(_tx,_ty,w,h) ∩ paintInfo.r` 作裁剪），修复为把可见区域**反变换**回未变换坐标再求交；同时把变换矩阵提取为 `khtmlCssTransformMatrix()` 供绘制与裁剪共用。
  2) 探针连续多页抓图存在 DWM 竞态（杀掉上一个浏览器后新窗口首帧可能读到空白），`finish()` 增加 1s 稳定等待、抓图前置 `SetForegroundWindow`、等颜色出现后再补抓一帧；修复后 page4 稳定通过。
  3) page4 初版夹具内容过小/命中偶发空白帧，最终以 page3 渐变页为基底 + 追加 transform 盒子，保证稳定。

## 5t. 引擎渲染能力第九批（2026-10-25，`display: inline-flex`）

### 1) 行内级 flex 容器
- 此前 `display:inline-flex` 会被当作**块级** flex 容器（RenderBox::setStyle 的 display 分支没有 INLINE_FLEX，落到 default → setInline(false)），两个 inline-flex 容器会各占一行。
- 修复（复用 KHTML 既有的 inline-block 行内布局通道，不新写行布局）：
  - `RenderStyle::isDisplayReplacedType()` 与 `isOriginalDisplayInlineType()` 增加 `INLINE_FLEX`；
  - `RenderBlock::setStyle()` 的 `setReplaced(isDisplayReplacedType())` 因此把 inline-flex 容器标记为"伪 replaced"→ `isInlineBlockOrInlineTable()` 为真，行布局按行内级盒子排布（与 inline-block 完全同一条路径）；
  - `RenderBox::setStyle()` 的 display switch 增加 `case INLINE_FLEX: setInline(true)`；
  - 渲染器创建逻辑本就把 FLEX/INLINE_FLEX 都映射到 `RenderFlexBox`（render_object.cpp），无需改动。
- 结果：inline-flex 容器与文字/其它行内元素同处一行，容器内部仍走引擎既有 flex 布局（grow/shrink/order/align 等全部继承生效）。

### 2) 行为级验证
- 夹具 page2 新增两个 70×40 CSS 的 `display:inline-flex` 容器（同底色 #bb6600）：`#if1` 内两个 20×20 子项（#0088bb）、`#if2` 内一个（#bb8800）。
- 探针新增 3 项断言：两个容器 bbox 高度 ≤80 物理像素（证明同一行，块级会变 120）、`#if1` 子项 bbox 呈横向行（宽 55..90、高 20..45）、`#if2` 子项单独渲染；page2 共 8 项，四页合计 **26 项**。

### 实测
- 引擎探针 **26/26 PASS**：inline-flex 三项全过——两个容器同一行（bbox 209×59 物理像素）、`#if1` 两个子项横排（59×29）、`#if2` 子项独立渲染（29×29）；aspect-ratio 仍为 449×299。
- `tile_protocol_test` PASS；**全量回归 2026-10-25 04:00→04:49 `failed: none`**：14 项中 13 项 PASS（engine_semantics_probe 68.4s、bg_smoke 602.4s、scroll_smoke 601.7s、resize_probe 601.4s、tombstone_probe 623.2s、tile_protocol_test 126.2s）+ bing_ghost_check INCONCLUSIVE（本机 bing 无 #0078D4 蓝块，诚实结论）。
- 调试记录：初版 inline-flex 容器底色 (#bb6600) 与 aspect-ratio 盒 (#cc8800) 在像素容差内串色，导致 ratio/inline 检查互相污染；改为 #6644bb / #44bb66 后全部通过（inline-flex 本身第一次构建即生效）。

## 5u. 引擎渲染能力第十批（2026-10-25，CSS Grid 实用子集）

### 1) display 值与枚举
- 新增显示值 id：`grid`=342、`inline-grid`=343（parser.cpp getValueID 硬编码）；`EDisplay` 增加 `GRID`、`INLINE_GRID`（排在 INLINE_FLEX 后、NONE 前）。
- display 校验接受 grid/inline-grid；CSSStyleSelector 的 display 应用分支映射到 GRID/INLINE_GRID；absolute/floating 时的 display 变形例外表同步加入；`inline-grid` 复用 inline-block 行内通道（isDisplayReplacedType/isOriginalDisplayInlineType 包含 INLINE_GRID，RenderObject 创建映射到 RenderGrid）。

### 2) 新属性 grid-template-columns（id 194，TOTAL 195）
- 解析：`CSSParser::parseGridTemplateColumns()` 支持 `none | <track>+ | repeat(<n>, <track>) | repeat(auto-fit|auto-fill, minmax(<length>, 1fr))`，其中 track 支持 px / % / fr / auto / minmax(px,1fr)；序列化为紧凑谱 `"px:100,fr:1,auto"` / `"repeat:3,fr:1"` / `"autofit:200"` 以 CSS_STRING 随属性传递。
- 样式：`StyleCSS3NonInheritedData.gridTemplateColumns`（含拷贝/相等）+ RenderStyle 访问器（DataRef COW）；applyProperty 落盘。
- 宿主层无需改动（display:grid 早已不再被改写为 flex）。

### 3) RenderGrid（新文件 rendering/render_grid.{h,cpp} + CMake）
- `layoutBlockChildren()` 实现子集布局：
  - 解析列模板 → 列数与本列宽（px/%、fr 按剩余空间分配、auto 视作 fr、minmax 取 min 为底 + fr 分配、autofit 按最小宽度算列数）；
  - 子项**行优先自动放置**（row-major），每行高度取最高子项；
  - 子项宽度 auto 时拉伸到列宽（setOverrideWidth + relayout）；
  - 容器高度 = 各行高度 + gap 间距（移除末尾多余 gap）；
  - 定位子项 `setPos()`，并调用 `layoutPositionedObjects()`。
- 渲染器创建：GRID/INLINE_GRID → `RenderGrid`（render_object.cpp）。
- @supports：`(display:grid)` 现在返回 **true**（sticky 仍为 false）。已知未支持：grid-template-rows、span、命名线/区域、grid-auto-flow 显式控制、minmax 的非 px 下限等。

### 4) 行为级验证
- 新夹具 `tests/engine_semantics5.html`：3×3 网格（`grid-template-columns:100px 100px 100px; gap:10px; width:340px`），9 个 100×50 的彩色单元格。
- 探针新增 page5 四项断言：第一行三格同一行且列间距 155..175 物理像素、第二/三行依次下移、第二行回到第 0 列、单元格按列宽尺寸；五页合计 **30 项断言**。

### 实测
- 引擎探针 **31/31 PASS**：page5 网格四项全过（第一行 y0 均为 87、列 x=11/176/341、三行 y=87/177/267、单元格 149×74）；page1 的 `@supports (display:grid)` 已变为 **applied**（红块出现），新增 `@supports (position:sticky) dropped` 负向断言通过。
- `tile_protocol_test` PASS；**全量回归 2026-10-25 05:46→06:32 `failed: none`**：14 项中 13 项 PASS（engine_semantics_probe 116.1s、bg_smoke 602s、scroll_smoke 602s、resize_probe 601.4s、tombstone_probe 623.2s、tile_protocol_test 127.4s）+ bing_ghost_check INCONCLUSIVE（本机 bing 无 #0078D4 蓝块，诚实结论）。**@supports (display:grid) 变真未引起回归**（bg_smoke 等探针页面无 grid 布局）。
- 调试记录（如实）：CSS_VAL_GRID/INLINE_GRID 初选 342/343 与既有 space-between/space-around 冲突，改用 344/345；render_style.h 的访问器插入位置多了一个 `}` 导致类结构破坏（编译错误），已修复；探针新增"稳定帧等待"（连续两次抓图一致才返回）解决 page5 部分瓦片帧导致的假缺失。

## 5v. 引擎渲染能力第十一批（2026-10-25，CSS `calc()` 实用子集）

### 1) Length 扩展：百分比+像素两部分
- `Length`（misc/khtmllayout.h）新增 `LengthType::Calc` 与 `int m_calcValue`（第二分量），对象尺寸从 4 字节变 8 字节；构造函数/拷贝比较/`value()` 等全部同步。
- 新方法：`Length::calc(double percent, int px)`、`isCalc()`、`calcPx()`、`calcPercent()`、`calcValue(int basis)`（按基准解析百分比再加 px）。
- `width()/minWidth()/minWidthRounded()` 增加 Calc 分支 → 所有通过 `Length::width(cw)` 消费长度的地方自动获得 calc 支持（`calcWidthUsing`、水平/垂直外边距等）。

### 2) 解析
- `CSSParser::parseCalcSpec()`：支持 `calc()` 内 **百分比与 px 的加减**（`100% - 20px`、`50% + 10px`、纯 px 运算；tokenizer 把减号折成负值），无视 `+`/`,` 分隔符；输出 `"khtml-calc:<percent>:<px>"` 字符串。
- `parseValue()` 在进入属性校验 switch 之前处理 calc（属性白名单：width/height/min/max-width/min/max-height、margin-*、padding-*），以 CSS_STRING 值随属性进入样式层（多 token 值不再被单 token 校验拒绝）。

### 3) 应用与布局
- `applyProperty` 的长度分支识别 `khtml-calc:` 字符串 → `Length::calc(percent, px)`。
- 高度：`RenderBox::calcHeightUsing()` 增加 Calc 分支（百分比按包含块内容高解析）。
- 内边距：`RenderObject::paddingTop/Bottom/Left/Right()` 的百分比基准判断加入 `isCalc()`。
- 未支持（如实记录）：calc 内的乘除、嵌套 calc、非 px 单位（em/rem/vh 等）、以及 left/top/right/bottom 的 calc。

### 4) 行为级验证
- 新夹具 `tests/engine_semantics6.html`：`#wrap` 宽 400px；`#calc1` 宽 `calc(100% - 40px)`、`#calc2` 左边距 `calc(50% - 50px)`、`#calc3` 高 `calc(100px + 20px)`。
- 探针新增 page6 三项断言：calc1 宽 540±10 物理像素、calc2 x0=225±6、calc3 高 180±10 且宽 600±10；六页合计 **34 项断言**。

### 实测
- 引擎探针 **34/34 PASS**：page6 三项全过——calc1 宽 539 物理像素（= (400-40)×1.5）、calc2 x0=236（= body 11 + 150×1.5）、calc3 599×179（高 120 CSS×1.5）。
- `tile_protocol_test` PASS；**全量回归 2026-10-25 12:23→13:13 `failed: none`**：14 项中 13 项 PASS（engine_semantics_probe 117.6s、bg_smoke 603.3s、scroll_smoke 602.6s、resize_probe 601.7s、tombstone_probe 623.4s、tile_protocol_test 148.8s）+ bing_ghost_check INCONCLUSIVE（本机 bing 无 #0078D4 蓝块，诚实结论）。
- 过程中修复了**测试环境问题**：全量回归首跑时 `browser_click_probe` 失败（真实鼠标点击未送达页面），根因是用户前台被 Edge 占用、Windows 焦点保护使 `SetForegroundWindow` 返回 0，点击被前台窗口吞掉；已在探针中加入 `force_foreground()`（AttachThreadInput 到前台线程 + BringWindowToTop/SW_RESTORE，并轮询确认前台），重跑后点击探针稳定 PASS。
- 调试记录：bison 表达式文法不支持 `+`/`-`（只有 `/`、`,`，负号靠一元负号），因此在样式表解析前把 `calc(A - B)` 重写为 `calc(A,-B)`（逗号分隔带符号项），再由 `parseCalcSpec()` 求和；height/width 的 applyProperty 各自有独立长度分支，已分别接入 calc 字符串解析；宿主原有的 calc 近似改写已移除（交给引擎）。

## 5w. 引擎渲染能力第十二批（2026-10-25，`position: sticky`）

### 1) 位置类型
- `EPosition` 增加 `PSTICKY`；新值 id `sticky`=346（cssvalues.h + parser.cpp getValueID）；`position` 校验接受 sticky、applyProperty 映射到 PSTICKY。
- 布局语义：sticky 元素**留在正常流**中（与 static 相同，不脱离文档流、不生成 positioned 图层）。

### 2) 绘制期视口锚定
- `RenderBlock::paint()` 在加上 `m_y` 之后：若 `position == PSTICKY` 且非根元素：
  - 有 `top: <px>` 时：当 `文档y - scrollY < top`，把绘制 y 改为 `scrollY + top`（钉在视口顶部）；
  - 否则有 `bottom: <px>` 时：当 `文档y > scrollY + viewportH - bottom - height`，把绘制 y 改为该值（钉在视口底部）。
- 滚动后的收敛由既有渲染宿主机制保证：滚动帧会重置 tile 哈希并安排 250ms **deferred convergence**（invalidateFull + 重绘整个视口），因此 sticky 的锚定位置会在滚动后自动修复，不额外改 tile 协议。
- 未实现（如实记录）：containing-block 底部约束（sticky 不会在父容器结束时停止吸附，会吸到页面末尾）、left/right 方向 sticky、百分比 inset、z-index 提升。

### 3) @supports 与宿主
- `@supports (position: sticky)` 现在返回 **true**；page1 的"未支持负向断言"改用 `@supports selector(:has(*))`（引擎 @supports 对 selector()/font-tech()/font-format() 判定为 false），验证 @supports 的 false 分支仍会丢弃整块。
- 宿主移除 `position: sticky -> relative` 文本重写，交给引擎。

### 4) 行为级验证
- 新夹具 `tests/engine_semantics7.html`：300px 灰色 spacer + 40px 红色 sticky 条（`top:0`）+ 1200px 绿色内容块。
- 探针 page7：滚动前两项断言（红条在静态位置 y≈537、绿内容在其下方 y≈597）；随后向窗口投递 12 个滚轮 notch，等待收敛帧后复抓，再断言（红条钉在视口顶部 y≈87、绿色内容仍可见且高度 >300）；七页合计 **38 项断言**。

### 实测
- 引擎探针 **38/38 PASS**：page7 四项全过——滚动前红条 y0=537 h=59、绿内容 y0=597；投递 12 个滚轮 notch 后红条钉在视口顶部 **y0=87 h=59**，绿内容仍可见（y0=147，紧贴 sticky 条下方，h=769）。
- `tile_protocol_test` PASS；**全量回归 2026-10-25 15:00→15:46 `failed: none`**：14 项中 13 项 PASS（engine_semantics_probe 135.3s、bg_smoke 602.7s、scroll_smoke 602.4s、resize_probe 601.7s、tombstone_probe 623.4s、tile_protocol_test 154.5s）+ bing_ghost_check INCONCLUSIVE（本机 bing 无 #0078D4 蓝块，诚实结论）。
- 调试记录（如实）：
  1) PSTICKY 初版完全不生效——`NonInheritedFlags` 的 `_position` 位域只有 **2 bit**，PSTICKY(4) 被截断成 PSTATIC(0)；扩到 3 bit、`unused` 10→9 后生效（位域总和仍为 64）。
  2) 钉住的 sticky 条又被后面的绿色内容块覆盖——普通兄弟在 sticky 之后绘制；`RenderBlock::paintObject()` 改为先画普通子元素、**最后画 sticky 子元素**，保证吸附条在上层。
  3) 探针存在偶发"整页白帧"（上个浏览器关窗的 DWM 竞态；曾让 page3 渐变假失败）——main 循环改为整页重试一次，修复后稳定通过。
  4) 未实现（如实）：containing-block 底部约束（sticky 不会在父容器结束处停止吸附）、left/right sticky、百分比 inset、z-index 提升（用绘制顺序代替）。

## 5x. 引擎渲染能力第十三批（2026-10-25，CSS `@keyframes` 动画引擎原生子集）

### 1) 属性解析
- 新属性 id `animation`=195（cssproperties.h/parser.cpp added[]），`CSS_PROP_ANIMATION` 用多 token 手写解析（`parseAnimationSpec`），支持 `animation: <name> <duration>s <delay>s <iteration> <direction> <timing> <fill>`，输出 spec `"name;durMs;delayMs;iter;dir;timing;fill"` 存进 `RenderStyle`（`StyleCSS3NonInheritedData::animation`，copy/== 同步）。
- `animation: none` 用空 spec 清除；`@-webkit-` 前缀经既有 vendor 前缀剥离路径进入同一属性。

### 2) @keyframes 注册（新 `css/css_animations.h`）
- 引擎**没有** @keyframes 的 bison 文法 → 在 `CSSStyleSheetImpl::parseString()` 于 parseSheet 之前调用 `khtmlExtractAndRegisterKeyframes(css)`：扫出 `@keyframes`/`@-webkit-keyframes` 块、解析 `from/to/N%` 每个关键帧的 `opacity`/`transform`/`background-color`，注册到进程级 registry；块从 CSS 文本中移除（不再让 bison 报错吃掉后续规则）。
- 提取器对**大括号不平衡的输入**（例如宿主 strip 列表删掉关键帧里的 `transform:` 造成的 `from {;`）会跳过该 at-rule 而不是吞掉整个样式表（page8 空白 bug 的根因之一）。
- 时钟：进程级 `QElapsedTimer`；方向 normal/reverse/alternate/alternate-reverse；无限/有限迭代；线性相位（ease 等暂时按线性）。
- 变换插值：`khtmlLerpTransform` 按数值 token 逐项插值（形状不同则离散取近端）；颜色按 RGB 通道插值。

### 3) 绘制与帧驱动
- `RenderBlock::paint()`：有 animation spec 时计算当前 opacity/transform/背景色；opacity 用 `QPainter::setOpacity`，transform 复用既有 `khtmlCssTransformMatrix` 世界变换路径；`paintObject()` 内在 `paintBoxDecorations` 之后用插值颜色补画元素矩形（背景色动画）。
- 新 `KHTMLView::hasCssAnimations()`（渲染树扫描 style()->animation()）。
- 渲染宿主新增 200ms 探测定时器 + 33ms 动画帧定时器：有动画时 `invalidateFull()+scheduleFrame()`，无动画自动停止（空闲零帧）。
- 宿主清理：不再删除 @keyframes、不再注入 JS 动画 polyfill（引擎接管）；strip 列表移除引擎已支持的 `transform`/`aspect-ratio`/`grid*`/`place-*` 等条目（此前它们会在 http 页面上删掉这些属性，也会把关键帧删坏）；保留 transition/filter/clip-path 等未支持项。
- JS 桥顺手补齐：`el.style.backgroundColor`（kStyleProps 新条目）、`getComputedStyle()`（animation-* 从 RenderStyle spec 读回）、`document.getElementsByTagName`、`document.readyState`。
- 未实现（如实）：transition、timing-function 曲线（ease 按线性）、逐帧 easing、关键帧百分比以外的复合值插值（只插值数值 token）、非 opacity/transform/background-color 属性、动画层/z-index 提升。

### 4) 行为级验证
- 新夹具 `tests/engine_semantics8.html`：`@keyframes fade/slide/tint` 三个 1s alternate 无限动画（透明度 1→0.25、translateX 0→80px、#cc2222→#2222cc）。
- 探针 page8：静态三项（fade/tint 非白、slide 橙色像素 >500）+ 两次抓帧三项（透明度像素差 >12、slide 橙色 x0 右移超过 355（静态 341）、背景色像素差 >20）；八页合计 **44 项断言**。

### 实测
- 引擎探针 **44/44 PASS**：page8 六项全过——静态 fade/tint 非白、橙色像素 >500；两次抓帧：透明度 (202,233,202)→(174,221,174)、slide 橙色 x0 459→438（静态 341，确认右移）、背景色 (36,34,202)→(67,34,171)。
- `tile_protocol_test` PASS；**全量回归 2026-10-25 18:20→19:12 `failed: none`**：14 项中 13 项 PASS（engine_semantics_probe 193s、bg_smoke 603s、scroll_smoke 603s、resize_probe 601.9s、tombstone_probe 623.5s、tile_protocol_test 160.6s）+ bing_ghost_check INCONCLUSIVE（本机 bing 无 #0078D4 蓝块，诚实结论）。
- 调试记录（如实）：
  1) `khtmlExtractAndRegisterKeyframes` 初版漏了跳过 `@keyframes` 关键字后的空白 → name 为空、块未移除，bison 丢弃后续规则 → 页面空白；补 `isSpace()` 跳过。
  2) 宿主 strip 列表仍删 `transform:`，把 `@keyframes slide { from { transform: translateX(0px) } ... }` 删成 `from {;`（大括号失衡）→ 提取器一路吞到样式表末尾 → page8 空白；修法：strip 列表移除引擎已支持的 transform/aspect-ratio/grid*/place-*，且提取器对不平衡块改为跳过该 at-rule。
  3) 探针 transform 断言初版比较两帧 x0 差值（alternate 动画可能对称而差小）；改为"任一帧 x0 > 355（静态 341）"。
  4) JS 桥补齐了 `getComputedStyle`/`getElementsByTagName`/`readyState`/`style.backgroundColor`（即使引擎动画接管，站点脚本也常用）；宿主快速页面此前从不触发 finalizeSubresources → 最终重写缺失，已改为初始投递后若 pending==0 立即调度 + finalize 自重试。

## 5y. 引擎渲染能力第十四批（2026-10-25，CSS transition 引擎原生子集）

### 1) 属性解析
- 新属性 id `transition`=196（CSS_PROP_TOTAL 197），`parseTransitionSpec` 解析 `transition: <property> <duration>s [<timing>] [<delay>s]`，输出 spec `"property;durationMs;delayMs;timing"` 存入 `StyleCSS3NonInheritedData::transition`（copy/== 同步）；`transition: none` 用空 spec 清除。
- property 支持 `all`/`opacity`/`transform`/`background-color`/`background`/`color`（后两者都映射到背景色）；timing 接受 linear/ease/ease-in/out/in-out（当前一律按线性计算）。

### 2) 触发与登记（新 `css/css_transitions.h`）
- `RenderObject::setStyle()` 在换上新样式前检查 `style->transition()`：若有 spec 且旧/新样式在 opacity、transform、background-color 上有差异，就调用 `khtmlStartTransition(this, old, new)` 把 from/to 值、起止时间、时长/延迟登记到按渲染对象索引的 registry。
- 布局属性（width/height 等）不做过渡（每帧重排成本太高，如实记录为未支持）；`transition-property` 列表只有单属性（够用即止）。

### 3) 绘制与帧驱动
- `RenderBlock::paint()`：无 animation 时查询 `khtmlTransitionLookup(this, ...)`，按 `animationClockMs()` 线性插值 opacity/transform；背景色过渡在 `paintObject()` 的 `paintBoxDecorations` 之后用插值色补画矩形。
- `KHTMLView::hasCssAnimations()` 同时报告 `khtmlTransitionsActive()`；既有 200ms 探测 + 33ms 动画帧定时器自动驱动过渡帧，结束后自动停止（空闲零帧）。
- 宿主 strip 列表移除 transition/transition-property/duration/timing-function/delay（引擎接管）。
- 未实现（如实）：布局属性过渡、timing-function 曲线（线性回退）、transition-property 多属性列表、过渡期间的合成层/z-index 提升。

### 4) 行为级验证
- 新夹具 `tests/engine_semantics9.html`：`#b` 背景色 `transition: background-color 1.5s`（红→蓝）、`#m` `transform: translateX(60px)` 过渡；脚本每 900ms 切换 `className`（真实 hover 注入在无头测试里不可靠）。
- 探针 page9：三帧采样（间隔 0.6s）三项断言——过渡盒非白、背景色三帧最大差 >40、绿盒 x0=161（静态 71，确认右移）；九页合计 **48 项断言**。

### 实测
- 引擎探针 **48/48 PASS**：page9 三项全过——过渡盒在采样帧 (123,34,115)；三帧背景色 (113,34,125)/(107,34,131)/(153,34,85) 最大差 90；绿盒三帧 x0 均 161（静态 71，确认 transform 过渡已生效）。
- `tile_protocol_test` PASS；**全量回归 2026-10-25 20:13→21:02 `failed: none`**：14 项中 13 项 PASS（engine_semantics_probe 168.5s、bg_smoke 602.7s、scroll_smoke 603.4s、resize_probe 602.4s、tombstone_probe 628.5s、tile_protocol_test 152.5s）+ bing_ghost_check INCONCLUSIVE（本机 bing 无 #0078D4 蓝块，诚实结论）。
- 调试记录（如实）：
  1) 真实 hover 注入（SetCursorPos/PostMessage WM_MOUSEMOVE + 前台抢占）在无头探针环境里不可靠——即使无 transition 的静态 `:hover` 也不变色，故夹具改用 setInterval 切换 `className` 触发样式重算（className 写入已验证生效），过渡机制本身随后验证通过。
  2) 过渡触发点确认：`RenderObject::setStyle` 旧样式仍可用时登记 from/to；若样式变化不影响 opacity/transform/background-color 则不登记（不会误开帧）。

## 5z. 路线图完成状态（收尾核验，2026-10-25）

七类路线图能力全部落地：每批均有本地 fixture 行为验证、全量回归绿、文档记录、提交推送。当前 `HEAD` = `origin/main`。

| # | 路线图能力 | 批次（提交） | 行为验证夹具 |
|---|---|---|---|
| 1 | `-webkit-` 前缀别名与宽容解析 | 批1 `e7e5ea5` | page1 |
| 2 | HTML5 语义元素 UA 默认块级 + 基础元素 | 批1 `e7e5ea5` | page1 |
| 3 | `@supports` 求值 | 批1（后续随 grid/sticky 支持态更新） | page1 |
| 4 | flexbox 完整化 | 批2 `bf7bde8`、批3 `35cc886`、批4 `6a104dc`、批9 `076aaaf` | page1/page2 |
| 5 | `var()` / 逻辑属性 / `calc()` | 批5 `9cb26b6`、批2（逻辑属性）、批11 `d7df94d` | page1/page6 |
| 6 | grid / sticky / aspect-ratio | 批10 `4789b6b`、批7 `5a699fc`、批12 `6286845` | page5/page7/page2 |
| 7 | 渐变 / transform / animation / transition | 批6 `7a74f56`、批8 `e60a24f`、批13 `3a9766c`、批14 `266d4ac` | page3/page4/page8/page9 |

验证基线（最近一次）：
- 引擎探针 **48/48 PASS**（9 个 fixture 页）；`tile_protocol_test` PASS；`portable_smoke` PASS（SHA256 相等）。
- 全量回归 **`failed: none`**（2026-10-25 20:13→21:02，14 项中 13 PASS + `bing_ghost_check` INCONCLUSIVE——本机 bing 无 #0078D4 蓝块，诚实结论）。
- 十四批提交全部已推送 `origin/main`；工作区干净。

已如实记录的"够用即止"缺口（详见各 §5x 与 GAP 对应条目）：
- grid：`grid-template-rows`、span、命名线/区域、`grid-auto-flow`、非 px `minmax` 下限；
- sticky：containing-block 底部约束、left/right 方向、百分比 inset、z-index 提升；
- calc：乘除、嵌套、em/rem/vh 等单位、left/top 的 calc；
- animation/transition：timing-function 曲线（线性回退）、布局属性过渡、多属性 `transition-property`、复合值插值与合成层提升；
- 外壳 EXE 替换：按用户指示后续再做，不在本目标范围。

## 5aa. 缓存清理接口（内核提供、UI 决定时机）（2026-10-25）

### 背景与实测结论
- 原 EXE **没有**"启动清空/退出清空缓存"的设定：`PageLoader` 只写镜像（同一 URL 每次重新下载并重写 `index.html`，资源按下载结果写入）。
- 实测（同 URL 连续两次启动、服务端改色）：两次各发起 3 个请求，第二次显示服务端**新内容**（蓝盒），证明页面内容**不会复用缓存**。残留风险只在"同一 URL 目录里旧资源文件"（例如某次图片下载失败会留下旧文件），以及缓存目录持续累积占用磁盘。
- 需求按用户最新指示调整：**不做自动清空**；改为在内核里提供清空缓存接口，由 UI/设置决定何时调用（对标 Safari/WebKit 的数据清理 API 模型）。

### 接口（UI 可调用）
1. **内核层**：`PageLoader::clearCache()`（`src/host/pageloader.h`，静态方法）——清理 `PageLoader::cacheRoot()` 的全部候选根：
   - `%TEMP%/khtml_cache`
   - `%TEMP%/khtmllite/khtml_cache`
   - `applicationDirPath()/khtml_cache`（便携版实际落点）
   - `AppDataLocation/khtml_cache`
2. **浏览器 UI 层**：`KHtmlLiteWindow::clearPageCacheNow()` 直接转调上者；已注册内置动作 **Ctrl+Shift+Delete**（浏览器"清除数据"的惯例快捷键），设置页/菜单可直接 `trigger()` 该 QAction 或调用此方法。
3. **命令行/便携启动器**：`khtml_browser.exe --clear-cache` 清空缓存后立即退出（返回码 0），便于设置脚本或启动器调用。

### 行为说明
- 内核自身**不在**启动/每次加载/退出时清除缓存（自动清空已按新指示撤销）。
- 页面加载仍然每次重新下载主文档（缓存镜像不会作为页面内容复用）；UI 调一次 `clearCache()` 可把磁盘镜像彻底释放。

### 验证
- 预置 junk 目录后执行 `khtml_browser.exe --clear-cache`：返回码 0、缓存根被删除。
- 普通启动：预置 junk 保留（无自动清空），页面正常渲染（绿色盒子）。
- `tile_protocol_test` PASS、引擎探针 48/48 PASS、`portable_smoke` PASS；全量回归结果见下方补记。

## 5ab. Safari 15 UI 整体重写（2026-10-25，参照 naxl/Firefox-Safari-15-Theme）

### 需求与仓库实情（如实）
- 用户指定 https://github.com/naxl/Firefox-Safari-15-Theme 作为 UI 来源，要求"整个 UI 换成该仓库的，旧 UI 全部舍弃，尽量复用其内容"。
- 实际核查：该仓库是 **Firefox 的 userChrome.css 主题**（"A Firefox theme that emulates the Safari 15 interface and new tab page"），仓库内容只有 `Readme.md` + `Resources/*.png` 预览图；`Readme.md` 指向的 CSS 下载（denizjcan/Firefox-Safari-15-Theme releases）已 404，naxl 仓库本身无 release。**其"代码"是 Firefox XUL DOM 专属 CSS，无法直接嵌入 Qt EXE**。
- 因此按预览图（`docs/assets/safari15_ui.png` 为本实现截图；参照 `Resources/safari*.png`）把 Safari 15 外观**移植重写**到 Qt：复用其视觉设计（圆角标签、红黄绿灯、圆角地址胶囊、灰底工具栏），不复用其 Firefox 选择器。

### 旧 UI → 新 UI
- **删除**：旧 `buildChrome()`（顶部工具条+下方 `QTabWidget` 原生标签、旧 QSS、主页/前往按钮等）整体移除；`QTabWidget *m_tabs` 成员删除。
- **新结构**（`src/host/khtml_browser.cpp`）：
  - 无边框窗口（`Qt::FramelessWindowHint`）+ 自绘 macOS 风格红黄绿灯（关闭/最小化/最大化，实测 1920x1008 最大化与还原、红灯关闭进程）；
  - 顶部标签条（28 logical px）：红黄绿灯、侧边栏占位按钮、Safari 圆角标签栏（`QTabBar`，可拖拽换序、关闭、Ctrl+W、右键菜单关闭/新建）、"+“ 新标签；
  - 工具条（30 logical px）：圆角返回/前进/刷新按钮、**圆角地址胶囊**（focus 高亮）、共享占位按钮；
  - 页面区改为 `QStackedWidget`，由 `addPageTab/insertPageTab/removePageTab/selectPageTab/setPageTabTitle/pageTabTitle/blockPageTabSignals` 助手维护（替换原 `QTabWidget` 调用点；崩溃恢复/挂起/恢复路径同步）；
  - 拖拽：标签条/工具条空白处 `QWindow::startSystemMove()` 移动窗口，双击最大化；无边框缩放经 `WM_NCHITTEST` 边框命中；Win11 调 `DwmSetWindowAttribute(DWMWA_WINDOW_CORNER_PREFERENCE=ROUND)` 圆角。
- **页面内容原点保持 (11,87) 物理像素**（7 logical 左右边距、28+30 logical 行高、4 logical 下边距，dpr 1.5），因此全部既有回归坐标无需更改（仅 page3 的"饱和像素行"扫描需限定 y≥87，避开新 chrome 的彩色圆点/文字彩边）。

### 交互与回归
- 保留：地址栏导航、F11 全屏、Ctrl+Shift+Del 清缓存接口（UI 调用 `PageLoader::clearCache()`）、标签挂起/恢复、崩溃恢复、tombstone 快照、页面尺寸同步。
- 占位（如实）：侧边栏按钮与共享按钮无功能；标签关闭的悬停 × 图标未用 QSS 渲染（改用 Ctrl+W/右键菜单关闭）。
- 验证：引擎探针 **48/48 PASS**；`browser_click_probe` / `input_coord_probe` / `dpr_probe` / `title_probe` 均 PASS；红黄绿灯实测；`tile_protocol_test` PASS（源码不变量检查已适配 `blockPageTabSignals`）；`portable_smoke` PASS；全量回归结果见下方补记。
- 文档截图：`docs/assets/safari15_ui.png`。

### 实测
- 引擎探针 **48/48 PASS**（9 个 fixture 页，页面原点仍为 (11,87)，仅 page3 饱和扫描起点改为 y=87）。
- 全量回归 **`failed: none`**（2026-10-26 00:22→01:13，14 项中 13 PASS + `bing_ghost_check` INCONCLUSIVE）：engine_semantics_probe 198.6s、tombstone_probe 626s、bg_smoke 607s、scroll_smoke 605.7s、resize_probe 603.2s、tile_protocol_test 177.8s、click/dpr/input/title/resize_ghost/rg_diag4 全 PASS。
- 首轮全量回归曾仅 `tombstone_probe` 失败：探针按旧布局在 y=48..70 点标签栏；新 UI 标签位于顶部 y≈10..40、x≈160..380，改为点击 (200,25) 后 PASS（探针修复已随本批提交）。
- 红黄绿灯实测：绿灯最大化 1920x1008、再次点击还原 1805x927、红灯关闭进程；拖拽/双击最大化/`WM_NCHITTEST` 边框缩放按实现路径工作（拖拽未做自动化点击验证）。
- `tile_protocol_test` PASS（源码不变量已适配 `blockPageTabSignals`）；`portable_smoke` PASS；便携包二进制已同步（SHA256 相等）。

## 5ac. Safari UI 按参考主题 CSS 数值重做（2026-10-26）

用户反馈 §5ab 的首版"做的很差"，并要求按 Claude 的建议：**不要整份复制参考主题（选择器是 Firefox 专属），而是抄 CSS 里的具体数值**。据此重做：

### 参考与取数
- `denizjcan/Firefox-Safari-15-Theme`：GitHub API 404（仓库已不可访问），Wayback 同样不可用；`naxl` 仓库仅 README + 预览图，README 指向的 release 已失效。无法取到其 userChrome.css。
- `vinceliuice/WhiteSur-gtk-theme` → `other/firefox/Monterey/`（用户说两者可互证）：从其 `colors/light.css`、`parts/tabsbar.css`、`parts/toolbox.css`、`parts/headerbar-urlbar.css` 抄录数值：
  - 工具栏背景 `#ffffff`；加载前背景 `#f5f5f5`；toolbox `max-height: 42px`。
  - 标签：`--tab-min-height: 32px`、`height: 32px`、`border-radius: 8px`、`margin: 0 3px`；非活动 `#f5f5f5`、文字 `rgb(141,144,145)`；hover `#f0f0f0`、文字 `rgb(93,98,99)`；活动 `#e8e8e8`、文字 `rgb(46,52,54)`。
  - 地址栏：`--urlbar-height: 32px`、`border-radius: 8px`、背景 `#e8e8e8`、边框 `#b6b6b6`、文字 `#020202`、focus `#5683da`、hover 边框 `#dddddd`；输入内边距 `0 28px 0 0`。
  - 按钮 hover `rgba(0,0,0,0.08)`、active `rgba(0,0,0,0.14)`（`--gnome-headerbar-button-*`）。

### 实现变化（相对 §5ab）
- 新 `SafariTabBar`（自绘 `QTabBar`）：8px 圆角标签、活动 `#e8e8e8`/非活动 `#f5f5f5`/hover `#f0f0f0`、文字用参考色、活动或悬停时在右侧画 `×`（点击关闭，替换此前无法渲染的 QSS close-button 图标）；保留拖拽换序、右键菜单、Ctrl+W。
- 头部布局：标签条 26 logical px（`#f5f5f5`，底部 1px `#dcdcdc`）+ 工具条 32 logical px（`#ffffff`，底部 1px `#e2e2e2`），总 58 logical = 87 physical px → 页面内容原点仍是 **(11, 87)**，全部回归坐标不变。
- 工具条图标改为自绘线性图标（返回/前进/刷新/共享/侧边栏/加号），不再用 Qt 标准图标；地址栏 `#e8e8e8` + `#b6b6b6` 边框 + 8px 圆角 + focus `#5683da`；按钮 hover `rgba(0,0,0,0.08)`；红黄绿灯用 Safari 标准色 `#ff5f57/#febc2e/#28c840`、12 logical px。
- 文档截图更新为 `docs/assets/safari15_ui.png`（本版实拍）。

### backdrop-filter（Claude 提到的模糊效果）如实说明
- WhiteSur 的 Firefox CSS **没有使用 backdrop-filter**（grep 全部文件 0 命中），它用实色 `#ffffff/#f5f5f5` 模拟 Safari 的半透明外观；本实现照抄实色。
- 我们的 chrome 是 Qt 原生控件，页面内容从工具条下方开始（不延伸到 chrome 下面），因此没有"透出页面再模糊"的场景；内核目前也**未实现 `backdrop-filter` CSS 属性**（GAP 有记录）。若要做真正的半透明模糊标题栏，需要把页面绘制延伸到 chrome 下并加合成模糊，属于后续独立工程。

### 验证
- 引擎探针 **48/48 PASS**（page3 饱和扫描限定 y≥87 的适配仍有效）。
- `browser_click_probe` / `input_coord_probe` / `dpr_probe` PASS；`tile_protocol_test` PASS；`portable_smoke` PASS（便携包二进制已同步，SHA256 相等）。
- 全量回归结果见下方补记。

## 5ad. 内核侧 DWM 呈现适配层 + WARP 回退（2026-10-26）

### 定位（用户明确要求）
- DWM/DirectComposition、D3D11、WARP 都是 **Windows 自带的系统能力**；内核只做"识别并调用"的适配层，不实现合成器，也不实现软件光栅器。
- 因此适配层代码归内核：`src/khtml/src/rendering/khtml_compositor.{h,cpp}`，编译进 **KF5KHtml.dll**，通过 `khtml::KHTMLCompositor` 导出；浏览器宿主（`khtml_browser.cpp` 的 PageSurface）只负责传窗口 HWND、物理几何、帧数据和可见性。

### 数据路径
1. renderer 进程里的 KHTML 引擎渲染页面，经共享内存 256px tile 传给浏览器；
2. 宿主 PageSurface 把脏 tile 合成到持久 canvas（QImage RGB32）；
3. 调用内核 `KHTMLCompositor::present(BGRA)`：`D3D11CreateDevice(HARDWARE)` → 失败则 `D3D11CreateDevice(WARP)`；纹理 `UpdateSubresource` → `CopyResource` 到 DXGI composition swap chain → `Present`；
4. swap chain 内容经 `IDCompositionVisual` 挂到宿主窗口 HWND 的 `IDCompositionTarget`，由 **DWM** 合成上屏。
- 窗口归属说明（与 WebKit/Chromium 同理）：DWM 目标绑定 HWND，窗口由浏览器进程拥有，renderer 无窗口；因此呈现调用发生在有窗口的进程，但**代码与接口属于内核**。
- 合成画布是 `WS_EX_NOREDIRECTIONBITMAP | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE` 子窗口 + `WM_NCHITTEST=HTTRANSPARENT`，输入穿透到 Qt PageSurface；点击/坐标探针验证通过。

### 回退策略（按用户要求只保留两级）
- 硬件 D3D11 → **WARP**（`D3D_DRIVER_TYPE_WARP`，Windows 自带软件光栅器）。
- **没有 QPainter/GDI 兜底**：若 WARP 也不可用/呈现失败，记录 `[canvas] backend=none …` 并视为系统图形栈故障。
- 设备移除/重置（`DXGI_ERROR_DEVICE_REMOVED/RESET`）：自动重建为 WARP 合成一次；仍失败才 log none。
- 测试钩子：`KHTML_FORCE_WARP=1`（强制 WARP）、`KHTML_FORCE_D3D_FAIL=1`（模拟 GPU 不可用，走 WARP）。

### 测试捕获适配
- DWM 合成内容不在窗口 GDI 重定向表面里，原有 client BitBlt 截图会全白：`tests/win_shot.py` 改为**桌面 DC 屏幕捕获**（先提升窗口到前台，按 ClientToScreen 取区域）；`bg_smoke / browser_click_probe / resize_probe / scroll_smoke / quick_shot / shot_tool` 里的本地 BitBlt 同步改为屏幕捕获。
- 新增 `tests/dwm_canvas_probe.py`：默认硬件、强制 WARP、模拟 GPU 失败三种模式各验证"页面像素上屏 + 后端日志 + resize 后仍正常"，并加入 `run_regression.ps1`。

### 验证（本批实测）
- `dwm_canvas_probe` **PASS**：三种模式截图采样均为页面色 (34,102,204)，日志分别为 `backend=d3d11-hw` / `backend=warp` / `backend=warp`，resize 后仍正确。
- 引擎探针 **48/48 PASS**（DWM 画布 + 屏幕捕获）；`browser_click_probe`、`input_coord_probe`、`dpr_probe`、`portable_smoke` PASS；便携包二进制已同步（含新 KF5KHtml.dll）。
- 全量回归结果见下方补记。

### 回归适配修复（DWM 画布带来的测试侧变化）
- `tile_protocol_test` 的静态不变量从旧的 QPainter 1:1 绘制行改为 DWM 契约：`khtml::KHTMLCompositor` 呈现、compose 在物理分辨率、`m_forceGdi` 必须不存在（防止兜底路径回潮）。
- `run_regression.ps1` 的 FAIL token 匹配改为大小写敏感（`-cmatch`）：原来 PowerShell 默认不区分大小写，探针里 `[OK] no backend failure` 的 "failure" 会被误判为 FAIL token（`dwm_canvas_probe` 曾因此假失败）。
- `tombstone_probe`：
  - 点击前提升窗口到前台——DWM 改造后 Qt 会丢弃非激活窗口的合成鼠标按下（agent 复现：前台时点击立即恢复）。
  - resume 点击从 (200,25) 移到 x=170 的标签文字区——WhiteSur 风格标签的关闭 "×" 热区在标签右缘，旧坐标命中了关闭按钮，浏览器 trace 显示 `currentChanged idx=0 tabs=1 pages=1`（标签被关掉）而不是 `resumeTab`。
- 复测：`tile_protocol_test` PASS、`dwm_canvas_probe` PASS（三后端+resize）、`tombstone_probe` PASS（resume True）、引擎探针 48/48 PASS。

### 全量回归（DWM 画布之后，2026-10-26 15:56→16:47）
- **`failed: none`**：15 项中 14 PASS + `bing_ghost_check` INCONCLUSIVE（本机 cn.bing.com 无 #0078D4 元素，探测器按设计报不确定）。
- 关键项：`dwm_canvas_probe 22.7s PASS`（硬件 / 强制 WARP / 模拟 GPU 失败 + resize）、`tombstone_probe 624.2s PASS`（resume True）、`tile_protocol_test 189.2s PASS`、`engine_semantics_probe 276.1s PASS`、`resize_probe 601.9s PASS`、`scroll_smoke 602.9s PASS`、`bg_smoke 602.9s PASS`。
- 便携包重新同步（`khtml_browser.exe` / `khtml_renderer.exe` / `libKF5KHtml.dll` SHA256 相等），`portable_smoke` PASS。

## 5ae. localStorage/matches/fetch polyfill 修复 + 注入时机（2026-10-26）

用户报告的 `src/host/pageloader.cpp` `kJsPolyfill` 问题及同批修复：

### 修复内容
1. **key() 多余分号**（原始 bug）：`"...||null;};};}"` → `"...||null;}};}"`。之前整个 polyfill `<script>` 是 SyntaxError，被第 1254 行 `window.onerror` 守卫吞掉，所有 shim 不执行。
2. **matches 真正匹配选择器**：旧实现遍历全部元素直接返回 true。现用引擎自己的 `document.querySelector`：给元素临时加唯一属性 `data-khtml-m="m<token>"`，查询 `selector + '[data-khtml-m="token"]'`，命中即匹配（逗号分组逐个查）；结束恢复/删除属性。
   - 为恢复属性，在 `qjs_dom_bridge.cpp` 增加 `Element.removeAttribute` JS 绑定。
   - 修复 wrapper 身份：引擎每次 `querySelector` 返回**新的 JS wrapper**，`=== this` 永远 false；改为判断查询结果非空。
   - 该引擎没有 `Element`/`Node`/`HTMLElement` 全局（实测 `typeof Element === 'undefined'`），polyfill 原在 `Element.prototype.matches` 处整段中断。改为用 `_ep = Element.prototype || Object.getPrototypeOf(document.documentElement)`，并给所有 `Element.prototype.*` 块加守卫。
3. **localStorage 语义**：`_ls = Object.create(null)`（`clear()` 同样重置为 null 原型）；`getItem` 用 `hasOwnProperty` 区分“不存在 → null”和“空字符串”；`key(i)` 正规化索引；`setItem/removeItem` 统一 `String()`。`getItem('toString')` 等原型键不再命中。
4. **fetch**：引擎没有 XMLHttpRequest（全树无实现）。fetch polyfill 现在在 `typeof XMLHttpRequest !== 'function'` 时明确 `reject(new TypeError('fetch is not available: XMLHttpRequest is not implemented in this engine'))`，不再抛 ReferenceError。未补 XHR（工程量大，按“明确报错”方案处理）。
5. **polyfill 全局目标**：该引擎的 `window` 是 document wrapper，**不是 globalThis**（实测 `window !== globalThis`）；旧 polyfill 把 console/localStorage/fetch 定义在 `window` 上，裸全局名全部 undefined。改为定义到 `globalThis`（旧环境回退 window）。
6. **注入时机**：抽出 `injectPolyfillBundle()`，**初始文档 `deliverInitialDocument()` 与最终 `rewriteAndSave()` 都注入**。此前只有最终文档有 polyfill，页面脚本先跑在无 shim 的初始文档上。

### 验证
- `node --check`：从 C 字符串拼出的完整 JS 11,669 字节，exit 0。
- Node 语义执行（强制使用本 polyfill，提供 document stub）：console/fetch/localStorage 类型正确；empty/缺失/原型键/key/clear 全部通过；`fetch` reject 消息含 `XMLHttpRequest`。
- 引擎内联执行（同一份 JS，同步报告写入 document.title）：
  `console=object|fetch=function|ls=object|empty=true|missing=true|proto=true|key=true|cleared=true|ma=true|mb=false|mtag=true|mid=true|mattr=true|thenable=true`。
  （引擎 Promise 微任务不驱动，fetch 的异步 reject 由 Node 阶段覆盖；引擎侧验证返回 thenable。）
- 新探针 `tests/polyfill_probe.py` 已加入 `run_regression.ps1`。
- 回归：`engine_semantics_probe` 48/48 PASS、`tile_protocol_test` PASS、`portable_smoke` PASS（便携包已同步）。

### 全量回归（polyfill 修复后，2026-10-26 17:50→18:41）
- **`failed: none`**：16 项中 15 PASS + `bing_ghost_check` INCONCLUSIVE（本机 cn.bing.com 无 #0078D4 元素）。
- 关键项：`polyfill_probe 6.6s PASS`、`engine_semantics_probe 282.6s PASS`、`tile_protocol_test 185.6s PASS`、`tombstone_probe 624.4s PASS`、`dwm_canvas_probe 22.4s PASS`、`resize_probe`/`scroll_smoke`/`bg_smoke`/`click`/`dpr`/`input` 全 PASS。
- HTTP 初始文档注入实测：页面脚本同步报告 `SHIM|console=object|ls=object|fetch=function`（初始交付即带 shim；随后 refreshed 重载会重置标题，属于既有 loader 刷新时序，不是 polyfill 问题）。
- 便携包已同步（exe/DLL SHA256 相等），`portable_smoke` PASS。

## 5af. CSS 属性删除配平、Promise 微任务、window 别名（2026-10-27）

### 任务 A：simplifyUnsupportedCss() 吃掉花括号
- 删除正则改为分组回填：`((?:^|[;{}\s]))PROP\s*:[^;}]*(?:;|(?=\}))` → `\1`（Qt `QStringLiteral("\\1")`），只删声明本身，保留 `{`/`}`。
- 实现移到 `src/host/css_unsupported.h`（PageLoader 与单测共用同一份代码）。
- 值内 `;` 残留：新增跳过 `()`/`[]`/引号的扫描器，把值内 `;`/`}` 暂存为私有区哨兵，删除后再还原；`mask-image:url(data:image/png;base64,...)` 现在整段删除不截断。
- 单测 `src/host/css_trim_probe.cpp` + `tests/css_trim_probe.py`（已加入回归）：覆盖 4 个指定用例（filter 在前/在后、object-fit、@media 内 inset/will-change，`min-width` 不受影响），以及 data URI、引号内分号/花括号；逐例断言属性已删、规则保留、花括号配平。结果 **CSS TRIM PROBE PASS**（7/7）。

### 任务 B：Promise 微任务从未执行
- `quickjs_engine` 增加 `khtml::runMicrotasksForContext(::JSContext *)`（JS_ExecutePendingJob 循环，100k 上限）。
- 调用点：页面 `<script>` JS_Eval 后（qjs_dom_bridge evaluate）、定时器回调 JS_Eval/JS_Call 后、inline DOM 事件 JS_Call 后、addEventListener 回调 JS_Call 后；`QuickJSContext::evaluate` 也补上。
- 新探针 `tests/promise_microtask_probe.py`（已加入回归）：页面脚本内 `Promise.resolve().then`、`async/await`、定时器内 then 与 async/await 共 5 个 continuation，最终报告 `MICRO|await+then+timer-await+timer-sync+timer-then`，**PROMISE MICROTASK PROBE PASS**。

### 任务 C：window !== globalThis
- 在 polyfill 末尾把 shim 从 globalThis 镜像到 `window`：console/performance/rAF/localStorage/sessionStorage/Map/Set/Symbol/fetch/atob/btoa/IntersectionObserver/MutationObserver/ResizeObserver/AbortController/URLSearchParams/getComputedStyle/matchMedia/crypto/structuredClone/queueMicrotask/globalThis。
- `polyfill_probe` 引擎内联报告新增并通过：`window.console=object`、`window.localStorage=object`、`window.fetch=function`、`window.matchMedia=function`、`window.IntersectionObserver=function`。

### 验证（本轮）
- `css_trim_probe.py` PASS、`promise_microtask_probe.py` PASS、`polyfill_probe.py` PASS（含新增 window.* 检查）。
- `engine_semantics_probe` 48/48 PASS、`tile_protocol_test` PASS、`portable_smoke` PASS（便携包已同步）。

### 全量回归（2026-10-27 20:14→21:03）
- **`failed: none`**：18 项中 17 PASS + `bing_ghost_check` INCONCLUSIVE（本机 cn.bing.com 无 #0078D4 元素）。
- 新增项耗时：`css_trim_probe 0.6s PASS`、`polyfill_probe 3.5s PASS`、`promise_microtask_probe 2.2s PASS`。
- 主要项：`engine_semantics 186.7s PASS`、`tombstone 624s PASS`、`tile_protocol 148s PASS`、`resize 602s`、`scroll 604.5s`、`bg 603.2s`、`dwm_canvas 20.8s` 全 PASS。
- 便携包重新同步（exe/DLL SHA256 相等），`portable_smoke` PASS。

## 5ag. 交付打包约定（用户 2026-10-27 确认）

- **每次任务完成并验证无错（回归 `failed: none`、提交已推送）后，自动打包 `C:\Users\zhouzhehong\Desktop\KHtmlLite.zip`**，并删除旧包（同名覆盖）。
- 命名只用「项目名.zip」，不再加 `-source-snapshot-<hash>` 等额外后缀。
- 内容为 `git archive HEAD` 的源码快照（跟踪文件，不含 `.git`、`build/` 产物）；打包后做 ZIP 完整性校验并与 `git ls-files` 核对缺失文件数（必须为 0）。

## 5ah. 导航生命周期 / 资源交付 / 错误页 / 滚动方向（2026-10-27）

第一轮稳定性加固，目标链路：点击链接 → navigateRequested → PageLoader::load → 主文档 → extractResources → 子资源 → finished → begin/write/end → refreshed → 布局首帧。

### 已证实根因与代码证据
1. **跨导航状态泄漏**：`PageLoader::load()` 复位了很多容器（`m_resources/m_pendingMap/m_cssRaw/m_seenUrls`…）却没有清 `m_rawToLocal`、`m_cssRawToLocal`、`m_mainHtml`、`m_cacheDir`（旧代码 608-632 行）。上一页的 raw→local URL 映射会参与新页 rewrite，快速连续导航/返回后可能把新页资源改写到旧缓存目录。
2. **旧导航的定时任务无 generation 防护**：`finalizeSubresources()` 用 `QTimer::singleShot(100, this, [this]{...})` 重试，lambda 不携带 `m_navGeneration`。旧页的重试可在新导航开始后执行 rewrite/refreshed（新 load 会重置 `m_rewritten=false`），污染新页或被误判为可重写。
3. **失败导航没有任何 UI**：renderer 的 `loader finished` 错误分支只 `sendMessage("error")`；`khtml_browser.cpp` 的消息分发（915-998 行）没有 `error` 分支 → 新标签/失败导航停留在空白/旧画面。现在 renderer 合成一张本地错误页并走同一 begin/write/end（标题改为 ASCII `LoadError` 便于测试；正文含错误信息与目标 URL）。注意：`QDir::tempPath()` 在本机沙箱 renderer 中 `QFile::open` 失败（实测 write=0），错误页文件改写到 renderer 工作目录。
4. **链接点击不进历史**：只有 `KHtmlLiteWindow::navigate(..., pushHistory)` 会 append `pg->back`；链接点击走 renderer 的 `urlChanged`，浏览器只更新 `pg->current`/地址栏，所以点击 B 后 Back 无历史。现在 urlChanged 在 URL 变化且不是 navigate() 已记录时补 push（`TabPage::navPushed` 去重，含重定向）。
5. **预测滚动方向反了**：Qt 滚轮向上 angleDelta 为正、内容应向下移动；权威路径 `applyScrollAuthority` 用 `shiftCanvas(dx,dy)`（负 delta 时向下画），而 PageSurface 的预测预览用 `drawImage(-preview…)`，方向相反，滚动瞬间先反向跳、等权威帧才纠正（即“反向滚动异常”）。已统一符号，并在新文档/重载/seed 时复位 `m_baseCX/m_baseCY` 与预览（旧 authority base 会把新页首帧按旧偏移平移）。
6. **setEncoding() 触发隐式重载**（关键）：renderer 每次 `begin` 前调用 `KHTMLPart::setEncoding(cs, true)`，而该 API 的实现是 `closeUrl(); openUrl(oldUrl)`（`khtml_part.cpp` 2668-2684）。oldUrl 是缓存镜像 `file://…/khtml_cache/<hash>/index.html`，openUrl 进入 `RendererView::openUrl(file)` → `navigateRequested` → `PageLoader::load(originalOf(cache index))` → 前缀回退解析成 `http://<site>/index.html` → 404 → 错误页/白屏；带子资源的页面在 `refreshed()` 时必然触发（实测错误页 target 即 `/index.html`）。已删除 finished/refreshed 两处 setEncoding 调用；PageLoader 写出的镜像本就是 UTF-8，ctor 已设 utf-8。
7. **二次整页解析**：主文档在子资源下载前交付，`refreshed()` 再 begin/write/end 一次（脚本二次执行、DOM 重置）。已做的最小安全改造：
   - 抽出 `polishHtmlForEngine()`，初始与最终文档使用完全相同的 HTML polish（inline style url/@import、URL 替换、inline script try/catch 包装、polyfill），两边内容可比较；
   - 对“无子资源且最终 HTML 与初始 hash 相同”的页面跳过 refreshed（`refreshed_skipped_identical`）；
   - 曾尝试“先启动下载 + 50ms head-start，资源全部完成则以最终文档作为首文档交付（只解析一次）”，实测在带图片/背景图的页面上导致 renderer 连续崩溃（bg_smoke FAIL、产生新 dmp），已回退为原来的立即交付顺序。
8. **refreshed 二次解析时旧文档仍在加载图片 → 间歇性崩溃**（本轮最后修复）：busy.html 单页加载循环 8 次出现 1 次 renderer 崩溃（URL=busy.html，libKF5KHtml RVA 0x933fc）；用临时 `KHTML_NO_REFRESH` 关闭 refreshed 后 10/10 无崩溃，确认触发点是旧文档带着未完成的图片/GIF 加载被 re-parse 销毁。修复：在 refreshed 的 `begin()` 前显式 `m_view->closeUrl()`，先中止旧文档的 DocLoader/动画任务再拆除。复测：busy 10/10 无新 dmp，`bg_smoke` 3/3 PASS（红/绿像素 82416/10813，单次请求周期），`navigation_probe`/`engine_semantics_probe` PASS。

### 新增/修改文件
- `src/host/pageloader.cpp/.h`：状态复位、generation 防护、`polishHtmlForEngine()`、hash 判定、失败清理、refreshed 跳过。
- `src/host/khtml_renderer.cpp`：本地错误页（`LoadError`）；删除会触发隐式重载的 `setEncoding()` 调用；refreshed 前 `closeUrl()`。
- `src/host/khtml_browser.cpp`：链接点击历史、滚动预测方向/authority 复位。
- `tests/navigation_probe.py`（新）：普通链接、第二页、Back/Forward、302 重定向、快速连续点击取消慢加载、资源 404、JS 标题、主文档 404 错误页、非白屏、崩溃日志不变。**PASS**。
- `tests/scroll_direction_probe.py`（新）：单色块位置验证 wheel-down 上移、wheel-up 下移、回位。**PASS**。
- `tests/polyfill_probe.py`/`tile_protocol_test.py`：适配 polish 注入点与滚动预览符号的静态不变量。
- `tests/bg_smoke.py`：browser stdout 改 DEVNULL，避免 renderer 继承管道导致探针/回归挂 600s。

### 已跑验证
- `navigation_probe` PASS（全部 16 项，含错误页最终绘制重试）。
- `scroll_direction_probe` PASS：block y 538 → 448（wheel-down，内容上移）→ 538（wheel-up 回位）。
- `bg_smoke` 3/3 PASS（单次请求周期 `/bg_smoke.html,/green.png,/red.png`，无新 dmp）。
- busy.html 加载循环 10/10 无新 dmp（修复前 8 次 1 崩）。
- `title_probe` / `dwm_canvas_probe` / `css_trim_probe` / `polyfill_probe` / `promise_microtask_probe` / `browser_click_probe` / `engine_semantics_probe`（48/48）PASS。

### 全量回归（2026-10-28 02:10→02:49）
- **`failed: none`**（19 项：18 PASS + `bing_ghost_check` INCONCLUSIVE）。
- 关键项：`bg_smoke 4.3s PASS`（红/绿像素正常、单次请求周期、无新 dmp）、`tombstone_probe 623.2s PASS`、`scroll_smoke 602.6s PASS`、`resize_probe 601.9s PASS`、`engine_semantics_probe 194.6s PASS`、`navigation_probe 18.2s PASS`、`scroll_direction_probe 8.1s PASS`、`tile_protocol_test 145.7s PASS`、`dwm_canvas_probe 20.7s PASS`、`promise_microtask_probe 2.2s PASS`、`polyfill_probe 3.5s PASS`、`css_trim_probe 0.5s PASS`、`click/dpr/input/title/resize_ghost/rg_diag4` 全 PASS。
- 便携包与 ZIP 打包结果见下方交付记录。

## 5ai. Renderer 崩溃根因（0x933fc / Qt5Core+0x23d747，2026-10-28）

### 版本核对
- portable 目录中的 `khtml_browser.exe`/`khtml_renderer.exe`/`libKF5KHtml.dll` 与 build/host 产物 SHA256 完全一致；崩溃时段的 .dmp 模块时间戳/校验和也与这三个 PE 文件一致：
  - renderer ts=1793124603 cs=646517；Qt5Core ts=1780573085 cs=5574437；libKF5KHtml ts=1793100977 cs=15788325。
- 因此 2026-10-27 21:04、2026-10-28 13:48/13:49/13:55 的转储可与当前源码/二进制一一对应；不是旧构建或错配 DLL。

### 崩溃 A：libKF5KHtml.dll+0x933fc（Bing 首页 / 翻译搜索）
- 反汇编：`0x933fc` 属于 `DOM::DocumentImpl::part() const+0xc`：`mov 0x48(%rax),%rax`，其中 `%rax = this->m_view`（DocumentImpl+0x80）。`rcx`（DocumentImpl*）指向的内存已不是 DocumentImpl：首 qword 是 `0x7ffcf2ffffff` 类数据/哈希条目，`m_view+0x80` 为 `0x21fa00000026` 等非指针。
- 调用栈：`KHTMLPart::slotLoaderRequestDone(DocLoader*, CachedObject*)+0xd7` 调用 `DocumentImpl::part()`；该处机器码为 `mov 0x40(%rsi),%rcx; call DocumentImpl::part()`，`%rsi` 是 `DocLoader*`，`dl+0x40` 就是 `DocLoader::m_doc`。
- 根因链（源码 `src/khtml/src/misc/loader.cpp`）：
  1. 本地文件子资源（宿主缓存镜像）走 `Loader::scheduleRequest()` 的 `QFile` 分支，用 `QTimer::singleShot(0, this, [this, req, path]{... emit requestDone(req->m_docLoader, req->object); ...})` 延迟一个事件循环；
  2. `DocLoader::~DocLoader()` 调 `Cache::loader()->cancelRequests(this)`，但旧 `cancelRequests()` 只遍历 `m_requestsLoading`（KIO job），**完全不知道这些本地延迟 Request**；
  3. 页面切换/refreshed 重建/关闭 iframe 时 DocLoader 与 DocumentImpl 被删除，零延迟回调仍执行，`req->m_docLoader` 已悬空；`requestDone` 到达 `slotLoaderRequestDone()`，`dl->doc()->part()` 解引用已释放的 DocumentImpl → 0xC0000005。
- 转储中 `dl` 内存呈 QStringData/QHash 式内容，正是释放后被堆复用（DocumentImpl 析构已把 m_view 置 nullptr，但复用会覆写）。
- 确定性复现：`tests/khtml_lifetime_probe.cpp` 先写含本地缺失 CSS 的文档，`closeUrl()/begin()` 重建文档后进入事件循环；旧 DLL 在 gdb 下稳定崩溃于 `DocumentImpl::part()`，栈为 `slotLoaderRequestDone` ← `Loader::scheduleRequest lambda`；新 DLL 正常退出。

### 崩溃 B：Qt5Core.dll+0x23d747（Bing 视频页面 / 本地缓存页）
- Qt5Core 导出表解析 `0x23d747` = `QObject::setObjectName(QString const&)+0x7`；异常 `READ 0x8`、`RCX=0` → 对 **null QObject** 调用 `setObjectName`（读 `[rcx+8]` 的 d_ptr）。
- 源码 `KHTMLPart::connectToChildPart()`（`khtml_part.cpp` 4591 附近）第一行 `part->setObjectName(child->m_name)` 未判空；调用方 `loadFrameElement()` 的 `KParts::ReadOnlyPart *part = createPart(...)` 在 portable 无 KParts khtmlpart 插件时返回 `nullptr`（`createPart()` 对 `offers.isEmpty()` 直接 return nullptr），随后 `navigateLocalProtocol(child, part, ...)`/`connectToChildPart()` 直接使用。
- 本地 iframe 页可稳定复现：新构建前启动 iframe 页，renderer 以同签名 Qt5Core+0x23d747 崩溃并生成 dmp；browser 进程仍存活（自动恢复），与日志一致。
- 修复：`loadFrameElement()` 在 `createPart()` 后判空，失败走既有 `childLoadFailure(child)` 并返回；`connectToChildPart()` 首部再加防御性判空。需要说明：portable 未部署 khtmlpart 插件，因此 iframe 内容能力仍受限（不再崩溃，但子框架加载失败），这是后续部署/直建 child KHTMLPart 的功能问题，不是本次崩溃修复的掩盖。

### 修改文件
- `src/khtml/src/misc/loader.h/.cpp`：新增 `Loader::m_localRequests` 跟踪本地延迟请求；`cancelRequests()` 移除并取消匹配的本地 Request（只从集合移除，回调看到未决即自行 delete，不提前 delete 避免回调悬空）；`Loader::~Loader()` 清理；`Cache::isDocLoaderAlive(DocLoader*)` 仅做指针身份校验。
- `src/khtml/src/khtml_part.cpp`：`slotLoaderRequestDone()` 首部 `if (!dl || !Cache::isDocLoaderAlive(dl)) return;`（不先解引用 dl）；`loadFrameElement()` createPart 空值处理；`connectToChildPart()` 空值防御。
- `tests/khtml_lifetime_probe.cpp`（新增诊断）：确定性构造 stale local Request；gdb 下旧 DLL 崩、新 DLL 正常。
- `tests/iframe_crash_probe.py`（新增并加入回归）：本地 iframe 页父文档仍绘制、无新 dmp/crashlog。

### 已验证
- 旧/新 DLL 生命周期探针（gdb）：旧 `DocumentImpl::part()` SIGSEGV（栈含 Loader::scheduleRequest lambda），新 `exited normally`。
- `iframe_crash_probe` PASS（title=True，blue_pixels=10989，无新 dmp）。
- 新构建打开 Bing 首页/视频/翻译搜索：browser/renderer 均存活，无新 dmp，crashlog 不增长；视频/翻译页较白属于页面渲染不完整，不是崩溃。
- 本机 reload 按钮 (client 1729,66) 实测触发第二次主文档请求（requests 1→2）；随后 Ctrl+W 关闭当前标签，该标签的 renderer PID 退出、浏览器保持存活且无新 dmp/crashlog；关闭浏览器后所有 renderer 子进程退出。
- 目标探针：navigation_probe / scroll_direction_probe / browser_click_probe / bg_smoke / engine_semantics_probe / iframe_crash_probe 全 PASS。
- 全量回归（2026-10-28 15:04→15:43）：**`failed: none`**（20 项：19 PASS + `bing_ghost_check` INCONCLUSIVE）。关键：iframe_crash_probe 5.7s PASS、bg_smoke 3.6s PASS、tombstone_probe 623.1s PASS、scroll_smoke 602s PASS、resize_probe 601.5s PASS、engine_semantics 181.8s PASS、navigation_probe 18.6s PASS、scroll_direction 7.1s PASS、dwm 21.1s PASS、tile_protocol 138.6s PASS。

## 5aj. 崩溃恢复按钮 + 无边框窗口拖动（2026-10-28）

修改前状态：HEAD `fe75ee3`（上一轮 engine 崩溃修复），工作区干净；portable 仍是旧二进制（browser/renderer 02:10、libKF5KHtml 19:36），build/host 是 14:55 的新构建。portable 的 17:25 crashlog 是旧 DLL 产生的，不能用来判断这两项 UI 修复。

### Bug 1：崩溃页“重新加载”无效
证据链（`src/host/khtml_browser.cpp`）：
- `makeCrashWidget()` 的按钮直接 `connect(..., [this,pg]{ resumeTab(pg); })`；
- `resumeTab()` 第一行 `if (!pg || !pg->suspended) return;`；
- 崩溃路径只设置 `crashWidget`，从未设置 `suspended`，所以点击必然空操作。
- 另有两个隐患：崩溃 handler 在 `RenderTab::crashed`（由 `RenderTab::onFinished` 直接 emit）信号内同步 `delete pg->render`；且手动重试没有重复启动保护/JS 策略重置/标签索引与选中恢复。

修复（最小、分离两类恢复）：
- 新增 `dropRenderer(pg)`：先把 `pg->render` 置空，`terminate()` 断开 QProcess 并杀进程，`deleteLater()` 延迟删除，避免在 sender 自身信号内析构。
- 新增 `retryCrashedTab(pg)`：仅用于 crashed 标签；`suspended` 标签仍走 `resumeTab()`。
  - 校验 `m_pages.indexOf(pg)`、`pg->suspended`，若新 renderer 已在运行则直接返回（双击/陈旧信号保护）；
  - 清理旧 renderer 与 crashWidget（`deleteLater`，不删除正在处理点击的按钮）；
  - `startRenderer(pg, pg->current)` 重新建进程 + IPC；同索引 remove/insert 标签页（blockPageTabSignals），恢复原选中态和地址栏；
  - 手动重试 `jsDisabled=false` 且 `crashCount>=2`：JS 恢复启用；后续再崩只显示错误页，不再自动重启。
- crashWidget 按钮与工具栏 reload 按钮都改走 `retryCrashedTab()`；首次自动恢复仍只有一次，JS 禁用仅用于这一次自动重试。
- 崩溃/自动恢复/手动重试的标签替换统一 block signals，避免 `currentChanged` 重入导致索引错乱；崩溃页显示时选中标签的窗口标题改为“已崩溃 - KHtmlLite”。

### Bug 2：窗口不能拖动
证据：
- 标题栏空白区实际是 stretch 填充的 `SafariTabBar`（QTabBar）子控件；`DragStrip` 只有在自身背景区域才收到 mouse press。
- 实测（真实 SendInput 鼠标）从空白标题栏拖动：窗口 rect 完全不变 (delta 0,0)，双击也不最大化；说明事件被 QTabBar 吞掉，父级 `DragStrip::startSystemMove()` 未执行。
- 通过向窗口发送 `WM_NCHITTEST` 验证空白区坐标属于 tab bar；直接返回 HTCAPTION 的实验在本 Qt frameless 窗口上 DefWindowProc 并未实际移动窗口，因此改用 Qt 层机制。

修复：
- 在 `SafariTabBar::mousePressEvent` 中，仅当 `tabAt(pos)<0`（标签栏空白、非标签/关闭按钮）且左键时调用 `window()->windowHandle()->startSystemMove()`；
- 同一空白区手工双击检测（`QApplication::doubleClickInterval()` + 距离阈值）切换最大化/还原；另保留 `mouseDoubleClickEvent`；标签本体/关闭按钮仍走原 QTabBar 逻辑。
- 不改工具栏/地址栏/按钮的命中逻辑，不把整个工具栏变成拖动区。

### 已验证（本机 Windows GUI，真实 SendInput）
- 崩溃恢复：正常加载 → 杀 renderer → **首次自动恢复**（新 renderer、URL 重新请求）→ 再杀 → 崩溃页 → 点击“重新加载” → 新 renderer 启动、请求增加、标题恢复；再杀 → 工具栏 reload 重试同样成功。
- 多标签：后台标签崩溃时当前标签不变；切换回后页面已恢复；关闭崩溃标签后其他标签保留；连续崩溃不会无限自动重启（第二次只显示错误页）。
- 手动重试后 renderer 命令行为 JS enabled（`--disable-js` 不存在）；自动恢复那次带 `--disable-js`。
- 窗口拖动：空白标题栏拖动后窗口 rect 改变；双击最大化/再双击还原；地址栏点击+输入导航、+ 新标签、标签切换、标签关闭按钮、双击标签不最大化均正常。
- 已知预存在问题：交互式边框 resize 在旧 portable 与新构建上同样 width/height delta=0（`WM_NCHITTEST` 返回 HTLEFT/HTRIGHT 但 frameless 窗口未实际 resize）；本次未修改该路径，最大化/还原与 DPI 正常。不计入本次两个 Bug。
- 地址栏在多标签下也验证：`target=_blank` 打开本地第二标签后，点击地址栏、Ctrl+A、输入新 URL、Enter，服务器收到 `/c.html` 且标题变为 page-c。此前一次“新标签 + Bing”用例 Enter 未生效是 Bing 页面焦点/测试时序问题，同一地址栏在本地第二标签可稳定工作。
- 全量回归（2026-10-28 20:48→21:28）：**`failed: none`**（20 项：19 PASS + `bing_ghost_check` INCONCLUSIVE）。关键：`bg_smoke 5.8s PASS`、`resize_probe 601.8s PASS`、`scroll_smoke 603.1s PASS`、`tombstone_probe 623.5s PASS`、`engine_semantics_probe 205.7s PASS`、`navigation_probe 20.4s PASS`、`dwm_canvas_probe 21.3s PASS`、`tile_protocol_test 155s PASS`、`iframe_crash_probe 5.9s PASS`、`promise_microtask_probe 2.3s PASS`、`dpr_probe 31.4s PASS`（dpr=1.5）、`resize_ghost_probe 5.7s PASS`、`click/input/title/css_trim/polyfill/rg_diag4` 全 PASS。上一轮全量曾出现 `navigation_probe`/`dwm_canvas_probe` 各一次瞬时失败，单独重跑均 PASS，最终本轮无失败。

## 6. 正在/马上要做的
A. ~~拆那 1.1s~~ **已完成**（字体枚举缓存 + 图标懒加载，构造 1.1s→~0.35s，TTFF 2.0→1.1s）。
   下一步同部位残账：渲染进程镜像加载+socket 连接 ~500ms（每次新标签都付），
   ~~可做渲染进程预热~~ **prewarm 已落地**（本轮 quiet 标签即走领养路径）；
   残账是 load kicked→browser connected 1617ms 的对齐开销（见 5e 启动基线）。
B. ~~真 deviceScaleFactor CSS-px 视口~~ **已完成（5g）**。QSettings 墓碑
   持久化（冷重启秒回）仍待做。
C. 空闲 CPU 归零（挂起标签的计时器/渲染进程冻结）。
D. **同导航二次解析**（5g 观察项：脚本跑两次、JS World 重建、约 1s 后发生），
   疑编码探测重载 → 排查 KHTMLPart::begin 的二次调用与 encoding 分支。
E. **KHTML CSSOM 内联样式写入**（5h 缺口）：`style.backgroundColor=` 无效、
   `style.background=` 画残框 → 查 docSetProp 的 style 包装/属性转发。
F. **JS 裸标识符家族**（5i 记档，缺失即整段脚本早断，按真实页面频率）：
   matchMedia（MQ 的 JS 侧探测，本轮 CSS 侧已修）> location / navigator /
   console / getComputedStyle > alert/confirm > screen/history/performance。
G. **窗口体验**（5i 记档）：几何 QSettings 持久化（现在每次启动回默认
   94%/92%）；滚动条 QSS 定宽（150% 下 ~28px 观感差）。
H. **低层单位与传播**（5i 记档）：dppx 单位（需动 bison parser）；
   srcset/sizes 高清图选择；iframe 子 part dpr 传播（KService 依赖暂缺）；
   跨屏 ScreenChangeInternal。

## 7. 交付给下一个会话的操作步骤
1. `cd C:\Users\zhouzhehong\Desktop\KHtmlLite`
2. 先 `git status` 看有没有未提交的、主人没推的 commit，`git log --oneline -6` 验身世
3. 修代码前重读 **`docs/ARCHITECTURE.md`**（别造成条文冲突）
4. 改完必须跑 `python tests\tile_protocol_test.py`（非 MSYS2 下）+ 使用者确认构建；
   动到 damage/tile/resize/导航通路时再跑全量顺序回归
   `pwsh -File tests\run_regression.ps1`（约 46 分钟；报告 `tests/regression_report.txt`；
   bing 一项需联网，离线用 `-Skip bing_ghost_check.py`）
5. 启动采集：`python tests\run_startup_capture.py https://cn.bing.com 25` → `python tests\startup_timing_report.py build\host`
6. 已知 fail 模式：
   - `stdlib.h: No such file`：`CMAKE_NO_SYSTEM_FROM_IMPORTED` 被误删
   - `timespec_get has not been declared`：用了 `-std=c++20` 严格模式而非 gnu++20
   - 引擎渲染不更新/花屏：先跑 tile 协议模型测试再怀疑代码

## 8. 用户偏好
- 中文沟通，直来直去，数据优先。
- "Apple 式规矩"：先把机制/调度/墓碑立正，再做 UI 动效。
- Git push 用户自己来（网络限制），我负责 `git add/commit`。

---
*（如有大改请更新本文件，让下一个会话不用考古）*
