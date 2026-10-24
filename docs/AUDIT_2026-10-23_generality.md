# KHtmlLite 内核通用性审计报告（只读 · 零改动）

- 审计时间：2026-10-23 19:45 – 2026-10-24 01:05（Asia/Shanghai）
- 对象：KHtmlLite @ commit f1cc5ff（src/ 全源码 + 项目文档；tests/ 只作旁证）
- 约束遵守：本轮**未修改任何文件、未构建、未启动任何 GUI**；仓库保持 f1cc5ff 原状（见附录 C 验证）。
- 审计方式：
  1) Lead 直读源码逐条取证（每条带 file:line）；
  2) 5 个独立只读代理对 12 条核心事实逐条复核（5/5 返回，判定见附录 A）；
  3) 代理对初判提出的 2 处修正（C10/C12）由 Lead 重读源码后采纳（见附录 B）。
  说明：重量级首轮扇出（每代理 5-8 项子调查）超过 2.5 小时未归，已终止并改为上述轻量核证轮；核心事实全部有 Lead 直读证据，不依赖首轮。
- 外部资料：本会话 web_search/web_fetch 均不可用（无 API key / DNS 限制），**Safari/WebKit 对比基于引擎谱系与公开常识**，其中"WebKit 2005 年自 KHTML/KJS 分叉、Safari 至今使用 WebKit"为标准历史事实，其余 WebKit 现状描述未在本会话逐条联网核实，特此声明。

---

## 一、直接回答（先给结论）

**Q1：内核里有没有"针对特定网站的特调"？**
有，但形态特殊：**没有一处"按域名分支"的代码**（全 src/ 无 if host==bing 类逻辑，5/5 代理确认）。特调的存在形式是：
1. **每页无条件注入 Bing 专属 CSS**（pageloader.cpp:1206-1223）——全仓库唯一一段按具体站点 DOM id 写死的样式；
2. 默认首页/新标签/搜索/地址栏全部硬编码 cn.bing.com（外加一个 "!y "→Yandex 的例外入口）；
3. 引擎内核、探针工具、需求文档、路线图里遍布以 Bing 现象为基准的修复痕迹与验收条目（证据见第二节）。
即：**不是"遇到 bing 就走特殊路径"，而是"整个兼容层是照着 bing 的形状长出来的"**。

**Q2：是不是"只对本地网页能跑"？**
不是"只对本地页"——本地测试页跑得最好是事实，但真实网站（cn.bing.com）的首屏也能渲染出来。准确的表述是：**"静态第一屏"能看，"动态的现代网页"结构性残缺**。残缺的根源不在测试页 vs 真实页，而在三层硬缺口（Web API 面、现代布局、动态网络请求），对 bing 这种首屏信息密度高、JS 崩了也能看的页面恰好"显得能用"。

**Q3：对比 Safari/WebKit 还差在哪，什么原因？**
差距是**代际级**的：本内核引擎是 WebKit 的直系祖先 KHTML（2005 年分叉前的血统），Safari/WebKit 用 20 年补齐的现代布局、CSS、Web API、网络栈在本内核**一件都没有**；本项目用"宿主层每页改写 + 假 polyfill"把其中一部分**伪装**了出来。逐层对照见第五节，根因归纳为三条：
- R1 引擎血统断代（现代布局/绘制/解析能力缺失，只能靠改写绕过）；
- R2 换 QuickJS 引擎时把旧 KJS 的整套 Web API 绑定（含 XHR/Window/Location/Navigator）删掉，新桥只重建到最小集——形成"ES2020 语法全支持、Web API 几乎全没有"的独特断层；
- R3 以 cn.bing.com 为验收北极星（项目文档自述），用正则改写把问题**静默化**（吞错、假 API、近似布局），而不是在引擎层解决。

---

## 二、特调证据清单（全部带证据，判定见附录 A）

### 2.1 宿主层（浏览器程序）
| # | 事实 | 证据 | 等级 |
|---|------|------|------|
| 1 | 每页无条件注入 Bing 表单 CSS：`.sbox`、`#sb_form`、`#sb_form_q`、`#sb_form_go`，颜色 #0078d4（Bing 蓝），无任何域名条件 | src/host/pageloader.cpp:1206-1223（注入点 :1219-1222） | Lead 直读 + 5/5 TRUE |
| 2 | 默认首页/主页按钮/新标签/末标签重建/关键词搜索 5 处硬编码 https://cn.bing.com | src/host/khtml_browser.cpp:1248, :1305, :1309, :1442, :1611 | Lead 直读 + 5/5 TRUE |
| 3 | 地址栏例外入口："!y " 前缀走 Yandex，其余关键词一律 Bing 搜索 | src/host/khtml_browser.cpp:1604-1611 | Lead 直读（代理发现） |
| 4 | 所有请求（主文档+全部子资源）用伪装 UA："Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) KHtml/5.116 Safari/537.36"；探针注释直言目的是"让服务器返回标准页" | src/host/pageloader.cpp:604-606, :762-764；src/host/khtml_qnam.cpp:48-51 | Lead 直读 + 5/5 TRUE |

### 2.2 引擎内核层的 Bing 痕迹（注释与动机）
| # | 事实 | 证据 |
|---|------|------|
| 5 | 瓦片脏区修复注释点名 "bing's blue search button / floating toggle" | src/khtml/src/rendering/render_layer.cpp:284-286 |
| 6 | SVG `<style>` 规则分支注明修 "Bing logo fills" 变黑问题 | src/khtml/src/xml/dom_docimpl.cpp:2470-2475 |
| 7 | 渲染进程注释记录 219KB Bing 首页缓存行为与拖动后 "stuck blue ghost block on bing" | src/host/khtml_renderer.cpp:492, :1586 |
| 8 | 专为中国版 Bing logo 造的 5 变体 SVG fill 探针（"reproduces the Bing-logo everything-gray bug"） | src/host/probe_svg.cpp:1-22 |

### 2.3 文档层的"北极星"自述
| # | 事实 | 证据 |
|---|------|------|
| 9 | 「核心验收目标：cn.bing.com 稳定运行 + 常见现代网站基础功能」 | src/khtml/docs/modern_web_js_requirements.md:4（另 :12-14 每个特性带"Bing 依赖"列、:71 起整节《Bing (cn.bing.com) 实际 JS 需求推断》） |
| 10 | 现代化路线图每阶段以「Bing 回归测试」收尾 | src/khtml/docs/kjs_modernization_roadmap.md:54, :149, :271 |
| 11 | 同一份文档同时写着「**不写 Bing 专用 hack**」——与事实 1 直接矛盾 | src/khtml/docs/qjs_backend_architecture.md:328 |

结论：特调是**事实**（事实 1 是硬证据），但它是"无条件注入"而非"按站分支"；文档既承认 bing 是验收基准（9/10），也自我要求不写 Bing 专用 hack（11）——代码与后一条自我承诺相违背，这个矛盾建议正视。

---

## 三、真实加载管线：为什么"通用加载"先天不足

真实网站不是被"打开"的，而是被**镜像→改写→重喂**的（src/host/pageloader.cpp 全文，Lead 直读 + 代理复核）：

1. **QNAM 抓主文档**（:603-607），UA 伪装（:604-606），15 秒页面看门狗（:601）。
2. **用正则**（不是 DOM 解析）从 HTML 抠 href/src/srcset/url()（:721-757），从 CSS 抠 url()/@import（:843-889）；子资源上限 150 个（:693）、单个 8MB（:796-799）、woff/woff2/eot 字体直接不下载（:703-706）；全部落盘缓存目录（1 小时清扫 :550-557）。
3. 把页面里所有引用**重写为本地 file:// 路径**（:891-928），`<base href>` 指回原站保导航（:930-933）。
4. **两段式交付**：先立即喂"初稿"（deliverInitialDocument :1433-1505），子资源收齐后再全量改写并 refreshed() 重喂（:1516-1533）——这就是文档里"同导航二次解析（脚本跑两次）"现象的来源。
5. 绕开引擎原生加载器的原因写在注释里：**KHTML 自带的 KIO 加载器在 Windows 上会挂死**（src/host/khtml_qnam.cpp:78-80 原话 "fetched by KHTML's own loader through KIO, which hangs on Windows"）；且并发 QNAM 下载会饿死 in-process 的 KIO file worker，把页面卡在首个 ~32KB（pageloader.cpp:648-652）。
   - 佐证：build/host 下确实没有任何 kio worker 二进制；但**便携包** release/KHtmlLite-portable/kf5/kio/ 里打包了 kio_file.dll、kio_http.dll（代理发现，Lead 认可其与"绕开 KIO"设计并存的事实）。
6. 引擎侧的 KIO 作业代码（loader.cpp:1347 `KIO::get`、缓存策略、调度优先级）在树里保留着，但真实路径已被宿主完全接管。

**通用性后果**：JS 在运行期动态构建的 URL、data-src 懒加载图、<picture> 分支等正则看不见的资源永远加载不到；重资源页受 150 个/8MB/15s 截断；页面是"快照"而非"活的文档"。

---

## 四、兼容层的代价清单（每页都在发生）

### 4.1 CSS 改写（simplifyUnsupportedCss 等，pageloader.cpp）
- grid → `display:flex;flex-wrap:wrap`（:359-366）——**但自研 flex 子集根本没实现 wrap**（render_flexbox.h:11 "Not implemented (yet): wrap"），所以 grid 页实际塌成单行 flex（代理发现，与引擎头文件互证）；
- sticky → relative（:369-372）；transform 只保留 translate，scale/rotate/skew 删除（:374-387）；
- **103 项现代属性整段删除**（:429-483）：全部 animation/transition/filter/mask/columns/object-fit/逻辑属性/aspect-ratio/border-image…；
- @keyframes、@supports **整块删除**（:492-501），keyframes 被抽成 JSON 交给 rAF 驱动的 JS 动画 polyfill（:1225-1263 + css_anim_polyfill.h）；
- 渐变 → 首尾色平均纯色（:119-195）；
- CSS 变量 var(--x) 用**正则预展开**（:1276-1425），不做级联/继承/media 分支语义。

### 4.2 内联 SVG 改写是死代码（代理发现，Lead 复核坐实）
pageloader.cpp:1075 的正则 `"<svg\b([^>]*)>(.*?)</svg>"` 里 `\b` 是 **C++ 字符串里的退格转义（0x08）**，不是正则词边界（那要写 `\\b`）——真实 HTML 里 `<svg` 后面永远不会跟 0x08，**该正则永不匹配**。也就是说：为补 KHTML "HTML parser 没实现 HTML5 内联 SVG 集成"（:1070 注释原话）而写的内联 SVG→外部 .svg→<img> 改写**实际从不生效**；内联 SVG（现代网站的图标主力）直接进不了渲染，与 probe_svg.cpp 探针记录的 Bing logo 症状相互印证。

### 4.3 注入的假 Web API（kJsPolyfill，pageloader.cpp:18-87）
真桥接只暴露：document、事件、定时器、innerWidth/innerHeight/devicePixelRatio（qjs_dom_bridge.h:3-8 自述 "Minimal-but-sufficient…未暴露 API 读作 undefined"；qjs_dom_bridge.cpp:1341-1343 视口 getter、:747/:1368-1374 定时器）。其余全靠注入的假货：
- localStorage/sessionStorage = **内存假实现，无持久化**（:43-44）；
- IntersectionObserver/MutationObserver/ResizeObserver = **空操作**（:65-67）→ 懒加载内容永不触发；
- matchMedia **恒 matches:false**（:71）→ 响应式断点全落到基础分支；
- getComputedStyle 返回 `el.currentStyle||el.style`（:70，语义错误）；
- crypto.getRandomValues = **Math.random**（:74）；
- fetch 建立在 XMLHttpRequest 上（:62），**而 XHR 在引擎/桥接层根本不存在**（见 4.4）→ 站点 fetch 必然抛错（代理与 Lead 独立指出同一自相矛盾）。
- 再注入 `window.onerror=return true` **吞掉所有 JS 错误**（:1187-1190），并给每个外部 JS 包 try{}catch{}（<4MB，:805-817）、内联脚本同样（:1139-1181）→ 脚本在第一个裸标识符处整段死掉但页面无任何提示。**问题被静默化，不是被解决。**

### 4.4 Web API 断层的根因（换引擎清零）
- ecma/ 目录只有 4 组源文件（quickjs_engine、js_proxy、qjs_dom_bridge、jsengine.h）——**grep 不到 navigator/location/history/XMLHttpRequest/WebSocket/Worker 的任何实现**（5/5 代理确认）；
- 旧 KJS 引擎的整套绑定（kjs_window.cpp 118KB、kjs_dom.cpp 93KB、**kjs_xmlhttprequest.cpp 29KB**——里面有真的 XHR）**已整体不在树内**（src/khtml/docs/kjs_architecture_audit.md:167 记录其存在；全树 glob 零命中）；
- 桥接头明言 "QuickJS is the ONLY engine…instead of hallucinating an engine that no longer exists"（qjs_dom_bridge.h:4-6）——旧引擎已删；
- 新桥的待办清单把 location/navigator/XHR/fetch/localStorage 全列为"需要扩展"（qjs_backend_architecture.md:177-203）——即设计文档自己承认这些至今没建。
- 语义代价：外层 try{} 包裹还会让脚本里的顶层 `let/const/class` 变成块作用域，跨 `<script>` 引用会因此断链（结构性风险，属【推断】级，未实测）。

---

## 五、引擎真实能力 vs Safari/WebKit（2026）

谱系：KHTML 是 KDE 的引擎；Apple 2005 年将其分叉为 WebKit（渲染层 WebCore 出自 khtml，JS 层 JavaScriptCore 出自 kjs），Safari 至今使用 WebKit。所以本对比是"祖先 vs 走了 20 年的直系后代"。KDE 侧 KHTML 维护到本仓库的 5.116 打包版本，但从未追平 WebKit 的现代特性。

| 层 | Safari/WebKit 现状 | KHtmlLite 实测现状 | 差距根因 |
|---|---|---|---|
| HTML 解析 | HTML5 规范树构建；template、custom elements、shadow DOM、**内联 SVG/MathML** | CSS2.1 时代解析器；无内联 SVG 集成（官方注释自认）；改写补丁本身还是死正则 | R1 |
| CSS 语言 | @supports/@container/@layer、带级联继承的自定义属性、全量 calc、逻辑属性、现代色彩空间 | @supports/@keyframes 被整删、var() 正则预展开、逻辑属性被剥离 | R1+R3 |
| 布局 | 完整 flex（wrap/order/align-self…）+ 完整 grid（含 subgrid）+ multicol + sticky + 表格现代化 | 自研**最小 flex**（已接入 CSS 管道：cssstyleselector.cpp:4606-4652、render_object.cpp:153-155；但无 wrap/order/align-self）；无 grid（改写近似）、无 sticky、无 multicol | R1；wrap 缺失使 grid 兜底失效 |
| 绘制/效果 | GPU 合成、2D/3D transform、filter、mask、渐变、硬件加速动画过渡 | transform 仅 translate 近似为 left/top；filter/mask/渐变删除或平色；动画是 rAF JS polyfill | R1（无现代绘制后端） |
| JS 引擎 | JavaScriptCore 多级 JIT + WebAssembly | QuickJS 解释器，ES2020 语法原生全支持 | 只差性能不差语法——但见下行 |
| Web API | fetch/XHR（真网络）、WebSocket、Worker、Service Worker、IndexedDB、WebRTC、canvas 2D/WebGL/WebGPU、真实 storage、真 IntersectionObserver、真 getComputedStyle、完整事件 | **只有 document/事件/定时器/视口尺寸**；XHR/fetch/location/navigator/history/WebSocket/Worker/canvas 全无；storage/observer/matchMedia/getComputedStyle 是假货 | R2（本审计最大发现） |
| 网络 | 独立网络进程、HTTP/2+HTTP/3、brotli/zstd、TLS 1.3、磁盘缓存重验证、CORS/CSP/SRI、cookie 完整策略、健壮编码探测 | QNAM 单进程整页镜像（TLS/gzip 由 Qt 支撑，基础可用）；KIO 原生路径 Windows 无 worker 会挂；无 CORS/CSP 生效痕迹；**编码探测存在但中文分支是空 FIXME**（见下） | R1+R3 |
| 媒体 | 完整 video/audio（硬解）、MSE/EME、WebRTC | 无任何媒体管线（无 html_video/audio 文件、桥接无 canvas/mediaContext） | R1 |
| 字体 | woff2/可变字体/彩色字体全支持 | 仅 ttf/otf/svg；woff/woff2 主动跳过 | Qt 加载器限制 |
| 合成/进程 | 多进程沙箱、站点隔离、异步滚动合成 | 自研双进程 tile 合成（resize/scroll/dpr 层，近期修复且全量回归 12/12 全绿） | **本内核相对最健全的一层** |

**"渲染 UI 哪些渲染不出来、什么原因"一句话版**：一切靠 flex-wrap/grid/transform/动画/渐变/滤镜/内联 SVG 摆出来的现代 UI 都会走样或消失，一切靠 JS 拿数据/改 DOM 的交互都不会发生——原因不是某个 bug，而是引擎血统里没有这些能力，宿主改写只能近似其中一小部分。

---

## 六、两个由代理核证修正的重要事实（诚实记录）

1. **flex 比我初判的要好**：flex-direction/justify-content/align-items/gap 已接入真实 CSS 管道（cssstyleselector.cpp:4606-4652 解析、:2746-2747 映射 display:flex、render_object.cpp:153-155 创建 RenderFlexBox）——**外部样式表里的 flex 是生效的**；render_flexbox.h:12-14 "需要真正 CSS 管道"的自述已滞后。但 wrap/order/align-self/flex-basis% 仍缺失（render_flexbox.h:11），grid→flex-wrap 兜底因此无效。
2. **中文编码探测是坏的**：KEncodingDetector 框架存在（meta charset/BOM/@charset 链路齐全），但 automaticDetectionFor* 只实现了 9 个语种；**ChineseSimplified/ChineseTraditional 连同 Korean/Thai 落在上游遗留的空分支里，源码注释原话 "huh. somethings broken in this code ### FIXME"**（kencodingdetector.cpp:1102-1113）。对中文老站（GBK/Big5 且无显式声明）意味着乱码风险；现代 UTF-8 站不受影响。

---

## 七、"打开任意网页"分层失败模型与评分

| 层 | 现状 | 证据 |
|---|---|---|
| L1 网络获取 | 主文档 + 正则可见子资源可达（TLS/gzip 经 Qt）；JS 运行期发起的一切请求不可达；15s/150 个/8MB 截断 | pageloader.cpp 全文 |
| L2 资源完整性 | 静态引用可达；懒加载/JS 动态 URL/woff2 字体/内联 SVG 丢失 | :703-706、:1075（死正则）、:721-757 |
| L3 JS 执行 | 语法层全过（QuickJS）；API 层一碰 navigator/location/fetch 即死，错误被全局吞掉 | ecma/ grep + polyfill |
| L4 CSS | 现代特性被降级/删除/近似后才进引擎 | :353-504 |
| L5 布局 | CSS2.1 + 最小 flex；grid 近似失效、sticky 失效 | render_flexbox.* |
| L6 合成交互 | 相对健全（本项目近期全部修复与 12/12 全绿回归都压在这一层） | tests/ |

**通用性评分：静态阅读 4/10**（新闻/文档/维基类"能看但残缺"——无渐变、无动画、图标缺失、布局走样）；**动态站点 1/10**（搜索建议、登录、无限滚动、SPA 全部结构性不可用）。这个分数与"测试 12/12 全绿"并不矛盾：现有探针验证的是 L6 与 L4/L5 的一小片（且多为本地测试页），没有覆盖 L1-L3 的真实网站需求面——绿证明的是"修过的东西没坏"，不是"通用性达标"。

---

## 八、如果要继续投入（只建议，本轮不动手）

按"对通用性回报 / 实现成本"排序：
1. **Web API 面**（最大断崖）：location/navigator/getComputedStyle 真实现 + **XHR 直连 QNAM**（绕开 KIO，让 fetch polyfill 变成真的）——这一项做完，动态站点从"完全不可用"变成"部分可用"；
2. **修死正则**：pageloader.cpp:1075 `\b`→`\\b` 一行修复，内联 SVG 兜底立刻生效（同时建议给该路径补一个探针）；
3. **编码探测中文分支**：补 ChineseSimplified/Traditional 启发式（上游 FIXME 位），中文老站乱码问题；
4. **flex 子集补 wrap**：让 grid→flex-wrap 兜底真正起效；
5. **布局现代化**（grid/sticky/transform）与**网络进程化**：这是引擎级工程，建议在 1-4 之后另立项；
6. **流程建议**：把"每页无条件注入 #sb_form CSS"改为条件化或移除，与 qjs_backend_architecture.md:328 的自我承诺对齐；测试面建议新增 1-2 个非 bing 的真实站点探针（如维基/新闻站），避免"绿灯基准"继续单一化。

---

## 附录 A：5 代理核证判定汇总（2026-10-24，5/5 返回）

| 主张 | verify-1 | verify-2 | verify-3 | verify-4 | verify-5 |
|---|---|---|---|---|---|
| C1 Bing CSS 注入 | TRUE | TRUE | TRUE | TRUE | TRUE |
| C2 5 处硬编码默认页 | TRUE | TRUE | TRUE | TRUE | TRUE |
| C3 伪装 UA / 无按站分支 | TRUE | TRUE | TRUE | TRUE | TRUE |
| C4 镜像-改写-重喂管线 | TRUE | TRUE | TRUE | TRUE | TRUE |
| C5 150/8MB/15s/字体跳过 | TRUE | TRUE | TRUE | TRUE | TRUE |
| C6 CSS 降级改写 | TRUE | TRUE | TRUE | TRUE | TRUE |
| C7 内联 SVG 改写注释 | TRUE | TRUE | TRUE | TRUE | TRUE |
| C8 假 polyfill/吞错 | TRUE | TRUE | TRUE | TRUE | TRUE |
| C9 Web API 断层 | TRUE | TRUE | TRUE | TRUE | TRUE |
| C10 flex 最小子集 | TRUE | **PARTIAL**（已入 CSS 管道） | TRUE | TRUE | TRUE |
| C11 文档自述矛盾 | TRUE | TRUE | TRUE | TRUE | TRUE |
| C12 编码探测完整 | **PARTIAL** | PARTIAL | **PARTIAL** | **PARTIAL** | **PARTIAL**（中文分支为空 FIXME） |

代理另有独立发现已并入正文并经 Lead 复核：内联 SVG 死正则（C7 延伸）、便携包含 kio worker、"!y"→Yandex 入口、innerWidth/innerHeight/devicePixelRatio 已暴露、4MB 脚本包裹上限、路线图"Bing 回归测试"条目。

## 附录 B：审计过程的两处自我修正
- 初判"flex 只支持行内样式"过于悲观 → 实为已接 CSS 管道（采纳 verify-2 的 PARTIAL，Lead 重读 cssstyleselector.cpp 坐实）；
- 初判"中文简繁编码启发式存在"过于乐观 → 实为空 FIXME 分支（5 个代理全部指出，Lead 重读 kencodingdetector.cpp:1102-1113 坐实）。
这正是 5 代理独立核证流程的价值：两条初判都被纠正。

## 附录 C：仓库零改动验证
审计期间仓库无任何写入；工作树与 f1cc5ff 完全一致（本报告与草稿均写在仓库外的 Desktop，不入库）。验证记录见报告交付时的 git status 输出。
