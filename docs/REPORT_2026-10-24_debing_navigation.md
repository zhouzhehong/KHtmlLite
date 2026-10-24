# KHtmlLite 去特调与通用化改造报告

- 日期：2026-10-24（Asia/Shanghai）
- 仓库：KHtmlLite @ f1cc5ff + 本轮工作树改动（未提交，见文末）
- 依据：上一轮《KHtmlLite内核通用性审计报告》+ 本轮 6 个独立只读调查代理的专项报告（WebKit 机制对比 / 引擎能力路线图 / 全量特调清单 / 点击链路 / 渲染速度 / 引擎缺口），全部结论由 Lead 逐条复核源码后落地。
- 说明：本会话 web_search/web_fetch 不可用，WebKit/Safari 对比基于桌面上 WebKit 源码（`C:\Users\zhouzhehong\Desktop\WebKit` 的 git 对象，HEAD 74894669，可按需读取 `Source/`）与 `WebKit-src` 的 TileGrid/TileController 源码；标准谱系事实（WebKit 2005 年自 KHTML 分叉）直接采用。

---

## 一、您的问题的答案（先给结论）

### Q1：为什么要对 bing.com 特调，而不是把引擎渲染做好？
- **不是"引擎渲染好了顺便加特调"，而是反过来**：引擎（KHTML 5.116，WebKit 2005 年分叉前的血统）缺少现代网页的布局/绘制/API 能力，项目用宿主层"每页正则改写 + 假 polyfill"把缺陷掩盖成"能看"，并把这套兼容层**以 cn.bing.com 为验收基准**定型（项目文档自述"核心验收目标：cn.bing.com 稳定运行"）。
- 全仓库**唯一一处真正的站点级渲染特调**是 `pageloader.cpp` 里向每个页面注入只对 bing DOM 有效的 CSS（`#sb_form` 等）——本轮已删除。其余"bing 相关"是产品默认值（默认首页/搜索引擎）、开发探针工具、测试与文档引用，不是渲染特调。
- Safari 不需要特调的原因：**WebKit 有完整的引擎**（布局有完整 flex/grid/multicol/sticky/aspect-ratio；绘制有渐变/transform/动画/滤镜/合成层；HTML 解析是 HTML5 规范树构建；网络/JS/媒体/字体齐全），网站写的标准语义它直接实现，没有需要"骗"的地方。

### Q2：点必应任何按钮都先跳本地地址、再跳真实网址——为什么？
**根因已定位并修复（三层证据）**：
1. `PageLoader::extractResources()` 把页面里**每一个 href/src 属性**都当子资源镜像，包括 `<a href>`；随后 `rewriteAndSave()`/`deliverInitialDocument()` 把页面中所有出现过的引用**全量字符串替换成本地缓存路径**。于是锚点 href 真的变成了 `file:///…/khtml_cache/…`。
2. 点击时 KHTML 解析出的就是 file:// 缓存副本；渲染进程 `RendererView::navigateRequested` 再用 `originalOf()` 把 file:// 映射回真实 URL 重取——所以用户看到"先本地、后真实"，实际是**多一次导航 + 一次下载**。
3. 叠加第二个来源：`rewriteAndSave()`（`refreshed` 之后的最终改写）**不注入 `<base>`**，源 HTML 无 base 时最终文档丢失 base，所有**相对 URL**（表单 action、JS 相对跳转）也会解析成 file://。
- **实测证据**：用户便携版 10/24 09:25 的必应缓存 index.html 里 52 个 href 有 **42 个是 file:///khtml_cache/…**。
- **修复**（见第三节）：资源采集按标签识别（跳过 a/area/form/base）、最终改写补 base、导航属性还原加固（含后缀/formaction/meta refresh）、originalOf 前缀兜底、地址栏/历史/load 入口三重 scheme 守卫。
- **行为级红/绿验证**：同版本旧二进制 3/3 锚点 file:// → 新二进制 0/3；旧版还会多抓 2 个锚点目标页面，新版只抓图片。

### Q3：差什么？怎么对标 Safari？
按影响面排序的引擎级缺口（详见第五节路线图）：
- **P0（不做现代站必塌）**：`-webkit-` 前缀支持（伪装 UA 招来的 -webkit CSS 全被丢弃）、CSS 变量 var()、逻辑属性、calc()/min/max/clamp()、@supports 求值、HTML5 语义元素默认块级、编码链路（宿主强制 UTF-8 导致 GBK 必乱码）。
- **P1（布局）**：flexbox 完整化（当前只有单行子集，无 wrap/basis/order/align-self）、grid、aspect-ratio/object-fit、sticky。
- **P2（绘制）**：渐变（当前宿主压平成均值色）、transform（当前被改写为 left/top）、transition/animation、filter/blend、multicol。
- **P3（长线）**：HTML5 树构建、自定义元素/Shadow DOM、媒体（架构不兼容）、GPU 合成（建议不做）。
- 粗估：P0 约 25-35 人日，P1 约 50-70，P2 约 40-60，P3 100+。

---

## 二、本轮删除的"特调/过期补丁"

| # | 位置 | 内容 | 性质 | 处置 |
|---|---|---|---|---|
| 1 | `pageloader.cpp` 原 1206-1223 | 每页无条件注入 `.sbox/#sb_form/#sb_form_q/#sb_form_go/#0078d4` 的 Bing 专用 CSS | **站点特调** | **删除**（替换为说明注释） |
| 2 | `pageloader.cpp` 原 1119-1167 | 内联 `<svg>` 序列化为文件并换成 `<img>` | **过期补丁**（引擎已原生支持内联 SVG：`htmlparser.cpp:287-304/924-926`，提交 `70bb3a0`/`413e0ad`） | **整块删除**（恢复 CSS 着色/SMIL/滤镜） |
| 3 | `khtml_renderer.cpp:289` | 每个文档强制 `setEncoding("utf-8")` | **伪装**（掩盖编码缺陷） | 改为按响应 charset 解码，缺失才回退 UTF-8 |

保留并明确记录（非特调）：默认首页/搜索引擎/`!y` 前缀（产品默认值，类似 Edge 默认 msn，可后续配置化）、独立探针工具（不参与浏览器运行时）、通用兼容层（UA 伪装/polyfill/CSS 降级——是所有网页能渲染的承重墙，删除需先补引擎能力，按路线图分阶段）。

---

## 三、点击导航修复明细（本轮核心代码改动）

文件：`src/host/pageloader.{h,cpp}`、`src/host/khtml_renderer.cpp`、`src/host/khtml_browser.cpp`

1. **资源采集按标签识别**：`extractResources()` 正则改为捕获标签名，跳过 `<a>/<area>/<form>/<base>` 的导航属性，只镜像资源型标签（link/script/img/source/iframe/embed/object/video/audio/input 与 CSS url()/@import）。
2. **导航属性还原**：新增 `restoreNavigationTargets()`——
   - 支持 `<a|area href>`、`<form action>`、`<button|input formaction>`、`<meta http-equiv=refresh content="url=…">`；
   - 支持 `#frag`/`?query` 后缀（前缀匹配 + 保留后缀）、无引号属性；
   - 属性名要求前置空白（修复 `\bhref` 误配 `data-href` 的漏洞）；
   - 两处交付（首屏 + 最终改写）都调用。
3. **最终改写补 `<base>`**：新增 `installBase()`，`rewriteAndSave()` 与 `deliverInitialDocument()` 都调用；没有 base 就插入到 `<head>` 后（无 head 则 `<html>` 后），多个 base 归一。
4. **`originalOf()` 前缀兜底**：缓存目录下的一切 file://（写失败的镜像、带后缀的 URL、旧 loader 的副本）按"相对本页 URL"解析回真实 http(s)。
5. **三重 scheme 守卫**：`load()` 拒绝 cache 镜像作为文档 URL；渲染进程不再把 cache file:// 作为 `url` IPC 上报；浏览器 `urlChanged` 拒绝 `/khtml_cache/` 本地路径。
6. **`hashName()` 扩展名白名单**：修复无扩展名 URL 生成含 `/` 的本地名导致镜像写失败（必应 3 个 `.bin` 缺失即此类）。
7. **字符集透传**：PageLoader 从主响应 Content-Type 提取 charset（`mainCharset()`），渲染进程按它解码。

---

## 四、行为级验证（真实浏览器、真实缓存产物，非源码断言）

本地 localhost 夹具（4 锚点 / 1 图 / form+button formaction / meta refresh / 内联 SVG），缓存隔离，等最终改写稳定后读缓存 index.html：

| 指标 | 旧版（release 便携 10-23 16:43） | 新版（build/host 本轮构建） |
|---|---|---|
| `<a href>` 被改写为 file:// | **3/3** | **0/3**（全部真实 URL） |
| 额外把锚点目标当资源抓取 | **是**（/target.html、/rel.html） | **否**（只抓 /pic.png） |
| 图片镜像为本地 file:// | 1/1 | 1/1（机制保持） |
| form action / formaction / meta refresh 被改写 | — | **无** |
| 注入 bing `#sb_form` CSS | **是** | **否** |
| 内联 `<svg>` | 被改写成 `<img>`（丢 CSS 着色） | **原样保留，引擎原生渲染** |

（最终全量回归结果见文末“验证记录”。）

---

## 五、对标 Safari/WebKit 的引擎路线图（摘要）

完整版见调查代理报告与 `docs/HANDOVER.md`。核心判断：
- **与 Safari 的差距是代际级的**：KHTML 属性表约 187 个（含项目自增 8 个）vs WebKit ≈687；HTML 标签约 110 个且无任何 HTML5 语义标签；CSS 优先级链无 `-webkit-` 前缀；无 grid/multicol/sticky/transform/渐变/filter/动画（有未接线的脚手架）。
- **本项目相对最健全的是合成层**（tile 机制 + 近期修复的 resize/scroll/dpr，回归全绿）；分辨率正确性与 Safari 的 deviceScaleFactor 思路一致，不是差距。
- **宿主可继续合法兜底**（站点无关、像素等价、可移除）：资源镜像/URL 本地化、进程隔离/崩溃恢复、图片与字体格式转码、视频解码（须走"帧→纹理"接口）。
- **必须引擎层做**（宿主继续兜底＝特调）：渐变、grid、transform、@supports、var()、@keyframes、内联 SVG（本轮已回归引擎）、matchMedia/getComputedStyle 真实实现。

分阶段路线（每阶段 1.5-6 周）：
- **阶段 0 通真值**：宿主 CSS/HTML 改写全面开关化（默认关，保留 URL 重写）、编码链路修复、删内联 SVG 改写（本轮已完成）、补 getBoundingClientRect/offsetWidth 等几何 API。
- **阶段 1 CSS 现代化**：-webkit- 前缀、var()、逻辑属性、calc()/min/max/clamp、@supports、HTML5 语义元素 UA 块级、srcset/picture。
- **阶段 2 布局**：flex 完整化、grid、aspect-ratio/object-fit、sticky。
- **阶段 3 绘制**：渐变、transform、transition/animation、filter。
- **阶段 4 健壮性**：HTML5 树构建、template/自定义元素、媒体帧接口、multicol。
- 验收站点（非 bing）：zh.wikipedia.org、news.ycombinator.com、people.com.cn(GBK)、getbootstrap.com、tailwindcss.com、github.com、stackoverflow.com、apple.com.cn、stripe.com 等 + 本地 fixture 矩阵；量化判据：无注入断言、几何偏差 ≤2px、首屏像素差 <2%。

---

## 六、渲染速度瓶颈（对标 Safari 的机制差，摘要）

- **每帧固定开销估算 13-18ms**（1280×720@dpr1.5）：最大单项是逐字节 FNV 全瓦片哈希 ≈8-13ms（`khtml_renderer.cpp:1002-1053`）；每 tile 两次拷贝 + 一次 memset；宿主每帧整幅 `detach()`（`khtml_browser.cpp:1039`）。
- **滚动路径每帧 4 次整帧像素拷贝**（渲染平移 + QPainter detach + 宿主平移 + 宿主 detach），1440×900@1.5 下约 44 MiB/帧 ≈6-9ms；Safari 滚动 0 次（只改 layer transform）。
- **每次 mousemove 都 `invalidateFull()`** 把 20×20 的 hover 放大成整屏重绘+全屏哈希。
- 60Hz 定时器不是上限而是下限：`scheduleFrame()` 用 0ms singleShot 且 pending 在 grabFrame 入口清零 → 加载期可背靠背出帧。
- WebKit 可借鉴：tile 默认 512 且按可滚动方向选整宽条带（`TileController.mm:660-689`）；只标脏 tile 内被命中的子矩形（`TileGrid.cpp:191-216`）；覆盖区预取（2×宽/3×高 + 方向感知）；每 tick 最多一帧（`DisplayRefreshMonitorWin`）。
- 优先建议：① 子矩形 memcmp 替代整瓦片 FNV（12-19ms → <0.5ms）；② 输入路径去 invalidateFull + 帧时钟单源化；③ 滚动 4 次拷贝减到 1 次（就地 memmove + 裸 buffer 双缓冲）。

---

## 七、已知残留（如实记录）

1. iframe 内部页面的相对链接仍以 iframe 本地副本为基准（iframe HTML 本体不被改写），点击可能落到 file://——iframe 在此引擎本就依赖本地镜像，未在本轮处理。
2. 宿主通用兼容层（UA 伪装、ES5+ polyfill、CSS 降级、假 matchMedia/getComputedStyle）仍在：它们是"所有网页能渲染"的承重墙，删除需按路线图先补引擎能力。
3. 编码修复只覆盖服务器声明 charset 的页面；纯 meta charset 且无头的页面仍按 UTF-8 解码（后续交引擎探测器）。
4. 渲染速度优化（第六节）尚未实施，仅完成瓶颈定位与方案设计。
5. 探针/测试/文档里仍有大量 bing 引用（E 类），属测试资产与历史记录，未经本轮清理。

---

## 八、验证记录

- 行为级红/绿（真实浏览器、真实缓存文件，新旧二进制同夹具）：
  - 第一轮（导航/特调）：旧 3/3 锚点 file:// → 新 0/3；旧版多抓 2 个锚点目标页面 → 新版只抓图片；旧有 `#sb_form` 注入 → 新版无。
  - 第二轮（10 项断言全 PASS）：`<base>` 唯一且等于文档 URL、4/4 锚点真实 URL、图片仍镜像本地、form action / button formaction / meta refresh 均非 file://、内联 `<svg>` 原样保留（`n_svg=1`，无 `inline_svg_N.svg`）、无 bing 注入 CSS、最终改写 >2000 字节。
- 完整回归（`tests/run_regression.ps1`，2026-10-24 10:19→11:08，报告 `tests/regression_report.txt`）：
  **`failed: none`** —— 13 项中 12 项 PASS、bing_ghost_check INCONCLUSIVE（本机 bing 无 #0078D4 蓝块，诚实结论）。关键项：bg_smoke PASS（必应页面在删除注入 CSS + 内联 SVG 回归引擎后仍正常渲染）、scroll_smoke PASS、tile_protocol_test PASS（166s）、resize_ghost_probe PASS、tombstone_probe PASS、resize_probe PASS。
  - 过程记录：首次回归抓到 tile_protocol 的源码契约断言失败（我把字符集设置插在了 `dropFrameBase();` 与 `m_view->begin(localFile)` 之间）→ 把字符集设置移到 `dropFrameBase()` 之前后恢复；最终回归全绿。
- 便携包同步 + 冒烟：`release/KHtmlLite-portable` 三个二进制与 build/host **SHA256 全部相等**；`PORTABLE SMOKE PASS`。
- 提交：**6534d67** "Remove site-specific tuning and fix click navigation going through a local mirror"（6 文件，+335/−92），工作树干净；**用户自行 push**。

