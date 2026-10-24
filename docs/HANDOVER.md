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
