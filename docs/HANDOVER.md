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
`setTimeout/setInterval/rAF` 回调两者都不是 → 队列永不消费 → `recalcStyle` 不跑 →
渲染树里元素还停在旧坐标，之后的重绘和 damage 描述的都是旧位置。

**实测**（`KHTML_DAMAGE_TRACE=1`，tests/rg_diag4.py）：页面 JS 里
`[css] styleSetProp left=600px` 到达桥接层，**之后没有任何 `[setStyle]`**；而同样
的写入如果发生在 resize 事件处理器里就会出现 `[setStyle]`。

**修法**：qjs_dom_bridge.cpp 的 `jsWindowTimer` timeout lambda 里，回调返回后调用
`DocumentImpl::updateDocumentsRendering()`。HTML 语义上「update the rendering」本来
就在**每个 task 结束时**跑，不只在事件后。队列用 `takeFirst()` 消费，事件路径再
排空时是空的；`recalcStyle` 另有 `m_inStyleRecalc` 重入保护。

### 根因二（渲染进程）：部分帧被画到错误坐标，且未受损区域被刷白

1. **`QWidget::render(painter, targetOffset, sourceRegion)` 会抵消 region 自身的平移**
   （`QWidgetPrivate::render`：`offset -= paintRegion.boundingRect().topLeft()`）。
   旧代码传 `QPoint()`，于是**每个部分帧都把 damage 内容画到原点附近**，再把这些
   错误像素上传成 tile —— 这就是幽灵。逐像素实测（两标记 CSS (200,100)/(60,40)，
   dpr 1.5，与全幅渲染对比 damage 区内差异）：
   `offset=(0,0)` → **7236 个像素不同（错）**；
   `offset=unionTL` → **0**；逐矩形循环 → **0**。
   修法：`const QRect src = dirty.boundingRect(); w->render(&fp, src.topLeft(), QRegion(dirty));`
   （单次 paint，比逐矩形省 N 次引擎遍历。）
2. **部分帧以白色为底**：只重绘 damage 区，未受损像素留在 tile 里被一起上传，
   把画布上正确的内容擦白。修法：新增 `QImage m_lastFrame` 保存上一帧合成结果，
   部分帧以它为底；滚动 fast path 用与浏览器 blit 相同的 `(sdx,sdy)*dpr` 平移底图；
   全幅帧/首帧/尺寸变化仍从白底开始。导航/重载时 `dropFrameBase()` 丢弃底图，
   避免新文档叠在旧文档像素上。

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
