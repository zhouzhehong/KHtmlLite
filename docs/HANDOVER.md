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
- `9466dc7` — 架构章程（章程优先：墓碑/帧钟/隔离/红线）
- `7989c1e` — 构建修复 + 采集脚本 `tests/run_startup_capture.py`
- `634bdd5` — 启动时间线基建：ready 消息带 renderer 自身时钟 → 双进程时钟零误差
- `e5dc4b2` `ced9983` `66731c3` `3a6c3b3` `24a6424` — 构建链生存修复
- `40052cd` — 预测性异步滚动（60fps 预告 + 绝对偏移收敛）
- `037c022` — 渲染-once-全帧 + tile 裁剪 + 1:1 checkerboard resize（对齐 Safari 显示管线）
- `fc308c4` `...`（见 `git log`）— tile 哈希抑制、60fps 帧钟、滚动 blit

## 5. 当前测量值（2025-10-13，本机）
冷启动 → 首帧 **~2.0s**：
- `0→62ms` EXE+Qt 壳加载
- `63→192` UI chrome/tab 搭建（130ms，健康）
- `244→645` 渲染子进程起 + socket 连接（~400ms 镜像加载）
- `620→1725` **★ KHTMLPart/RendererView 构造 1.1s**（唯一的巨型时间块）
- `1827→1965` load→首帧（渲染仅 ~4ms，IPC ~10ms）
结论：引擎**渲染速度足够**，慢的是冷启初始化与重型现代站的 JS/CSS 处理能力天花板。

## 6. 正在/马上要做的
A. 拆那 1.1s：RendererView 构造内部分块（html4.css 解析 / 字体枚举 / KJS / 缺省样式 / 插件）。
B. **墓碑机制落地**：挂起标签 → 缩略图+scroll/zoom/标题落 QSettings；冷重启可"秒回"。
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
