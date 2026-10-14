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

## 6. 正在/马上要做的
A. ~~拆那 1.1s~~ **已完成**（字体枚举缓存 + 图标懒加载，构造 1.1s→~0.35s，TTFF 2.0→1.1s）。
   下一步同部位残账：渲染进程镜像加载+socket 连接 ~500ms（每次新标签都付），
   可做渲染进程预热/常驻待命池（对齐 WebKit 的 process pre-warm）。
B. **墓碑机制落地**：挂起标签 → 缩略图+scroll/zoom/标题落 QSettings；冷重启可"秒回"。
   （注意：QSettings 写 AppData 在本开发机被 ACL 沙箱拒，落盘路径选 applicationDirPath。）
C. 空闲 CPU 归零（挂起标签的计时器/渲染进程冻结）。
D. 必应等现代重型站的 JS 转译流水线开销（Bublé 嵌入，异步/缓存化）。

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
