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

## 7. 交付给下一个会话的操作步骤
1. `cd C:\Users\zhouzhehong\Desktop\KHtmlLite`
2. 先 `git status` 看有没有未提交的、主人没推的 commit，`git log --oneline -6` 验身世
3. 修代码前重读 **`docs/ARCHITECTURE.md`**（别造成条文冲突）
4. 改完必须跑 `python tests\tile_protocol_test.py`（非 MSYS2 下）+ 使用者确认构建
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
