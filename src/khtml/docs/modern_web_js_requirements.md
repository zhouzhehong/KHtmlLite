# Modern Web JavaScript Requirements — KHtmlLite

> 基于 KJS 兼容性审计，对照现代网页实际需求建立优先级。
> 核心验收目标：cn.bing.com 稳定运行 + 常见现代网站基础功能。

---

## 1. 现代网页 JS 特性使用频率分析

### P0 — 现代网页大量使用，必须优先

| 特性 | 为什么 P0 | Bing 依赖 | 实现类型 |
|------|-----------|----------|----------|
| Promise | 现代网页基础异步模型，fetch/setTimeout 封装都依赖 | 高（Bing 大量 Promise 链） | Runtime (HIGH) |
| let / const | 现代代码默认声明方式，几乎所有新代码使用 | 高 | Parser+Scope (HIGH) |
| arrow function | 现代代码默认函数写法，回调/事件处理大量使用 | 高 | Parser+Runtime (HIGH) |
| async/await | 现代异步代码标准写法，依赖 Promise | 中高 | Runtime (VERY HIGH) |
| Map | 现代数据结构，框架和工具库大量使用 | 中 | Runtime (HIGH) |
| Set | 现代数据结构，去重/集合操作 | 中 | Runtime (HIGH) |
| template literal | 现代字符串拼接，HTML 模板/动态内容 | 中高 | Lexer+Parser (MEDIUM) |
| Object.assign / Object.keys | 对象操作基础，polyfill 也常用 | 中 | Builtin (LOW) |
| Array.from / Array.isArray | 类数组转换，常见工具函数 | 中 | Builtin (LOW) |
| destructuring | 现代代码解构赋值/参数 | 中 | Parser+Runtime (HIGH) |
| spread syntax | 数组/对象展开，函数参数 | 中 | Parser+Runtime (MEDIUM) |

### P1 — 常见现代网页使用

| 特性 | 实现类型 |
|------|----------|
| Symbol | Runtime (HIGH) |
| for...of | Parser+Iterator (MEDIUM) |
| class | Parser+Runtime (HIGH) |
| String.padStart/padEnd | Builtin (LOW) |
| Array.includes/find/findIndex | Builtin (LOW) |
| optional chaining ?. | Parser+Runtime (HIGH) |
| nullish coalescing ?? | Parser (MEDIUM) |
| Object.entries/values | Builtin (LOW) |
| default parameters | Parser (MEDIUM) |
| rest parameters | Parser (MEDIUM) |
| microtask queue | Runtime (HIGH, 依赖 Promise) |

### P2 — 部分网站使用

| 特性 | 实现类型 |
|------|----------|
| Proxy | Runtime (VERY HIGH) |
| Reflect | Builtin (MEDIUM) |
| BigInt | Runtime (VERY HIGH) |
| TypedArray 完善 | Runtime (MEDIUM) |
| Generator | Parser+Runtime (HIGH) |
| computed properties | Parser (LOW) |
| shorthand properties | Parser (LOW) |
| Array.flat/flatMap | Builtin (MEDIUM) |
| String.replaceAll | Builtin (MEDIUM) |
| RegExp sticky/unicode | Regex engine (HIGH) |

### P3 — 暂时可以不实现

| 特性 | 原因 |
|------|------|
| ES Modules (import/export) | 大多数网站打包后不用原生 module |
| dynamic import() | 依赖 module loader |
| WeakMap/WeakSet | 需要 GC weak ref，成本极高 |
| Proxy | 实现成本极高，多数网站可降级 |
| async iterator | 依赖 generator + async |
| Intl | 国际化，多数网站可降级 |
| named capture / lookbehind regex | 少数网站使用 |

---

## 2. Bing (cn.bing.com) 实际 JS 需求推断

基于 Bing 是现代搜索引擎首页，其 JS  bundle 典型依赖：

| 能力 | 优先级 | 说明 |
|------|--------|------|
| Promise | P0 | 搜索建议、异步加载、事件处理 |
| let/const | P0 | 现代代码基础 |
| arrow function | P0 | 回调/事件 |
| addEventListener | P0 | 已有（KJS 事件系统） |
| document.querySelector | P0 | 已有（KJS DOM 绑定） |
| classList | P1 | 可能需要扩展 KJS DOM 绑定 |
| fetch / XHR | P1 | 搜索建议异步请求（XHR 已有） |
| setTimeout | P0 | 已有（KJS Window） |
| JSON | P0 | 已有 |
| Map/Set | P1 | 数据缓存/去重 |
| template literal | P1 | 动态 HTML 生成 |
| Object.assign | P1 | 选项合并 |
| async/await | P1 | 异步代码 |
| Symbol | P2 | 框架内部使用 |
| Proxy | P3 | 通常不需要 |

---

## 3. ECMAScript vs Web API 分离

### ECMAScript（引擎本体，本次现代化重点）
Promise, Map, Set, Symbol, let/const, arrow, class, template, destructuring, spread, async/await, Object.*, Array.*, String.*, Proxy, BigInt, modules

### Web API（浏览器环境，KJS 绑定层已有大部分）
window, document, Element, Node, Event, MouseEvent, KeyboardEvent, XMLHttpRequest, fetch, URL, URLSearchParams, navigator, location, history, setTimeout, setInterval, console, MutationObserver, queueMicrotask, localStorage, Canvas, WebGL

**关键区分**：Web API 不需要修改 KJS 引擎，只需要在 KJS 绑定层（kjs_window.cpp, kjs_dom.cpp 等）扩展。ECMAScript 特性需要修改 KJS parser/interpreter/runtime。

---

## 4. 实现难度分类

### 类型 A：纯 Builtin API（最简单，优先实现）
- Object.assign, Object.keys, Object.values, Object.entries, Object.create, Object.freeze, Object.seal
- Array.from, Array.of, Array.isArray, Array.includes, Array.find, Array.findIndex, Array.fill, Array.at
- String.padStart, String.padEnd, String.trimStart, String.trimEnd, String.at, String.replaceAll
- Number.EPSILON（如果缺失）

**特点**：不需要改 parser，不需要改 runtime，只需要在对应 prototype/constructor 上添加函数。可以用 KJS 现有的 JSObject/InternalFunctionImp 体系实现。

### 类型 B：Parser/Syntax（中等，需要改 grammar.y + lexer + nodes）
- template literal（lexer + parser + AST node）
- computed properties（parser）
- shorthand properties（parser）
- default parameters（parser）
- rest/spread（parser + runtime 支持）
- nullish coalescing（parser）
- exponentiation **（已支持）

**特点**：需要修改 bison grammar（grammar.y）和 lexer（lexer.cpp），添加新的 AST 节点类型。每次只加一个语法特性。

### 类型 C：Runtime 对象（较难，需要新对象类型 + GC 集成）
- Map, Set（新 JSObject 子类，内部用哈希表）
- Symbol（新值类型或特殊对象，属性访问需识别）
- Promise（新对象类型 + 状态机 + then/catch 链）

**特点**：需要创建新的 JSObject 子类，注册到 global object，GC 自动管理。Map/Set 内部数据结构可以用 KJS 已有的 WTF::HashMap。

### 类型 D：执行模型（最难，需要改 interpreter + scope chain）
- let/const（块级作用域，需要 lexical environment）
- arrow function（this 绑定，无 prototype，无 arguments）
- class（原型链语法糖 + constructor）
- destructuring（模式匹配赋值）
- for...of（iterator 协议）
- async/await（Promise + generator 语法糖）
- microtask queue（事件循环集成）

**特点**：需要深入修改 interpreter 的执行上下文、scope chain、函数对象模型。风险最高。

---

## 5. 第一批最值得实现的 5 个功能

基于"影响面大 / 实现难度可控 / 现代网页依赖"三个维度：

1. **Promise + microtask queue** — P0，现代网页异步基础。虽然难度 HIGH，但没有它现代网页基本无法运行。
2. **let/const** — P0，现代代码基础语法。难度 HIGH 但影响面最大。
3. **arrow function** — P0，现代回调写法。难度 HIGH。
4. **template literal** — P1，动态字符串。难度 MEDIUM，lexer+parser 改动相对独立。
5. **Object/Array 纯 builtin 方法包**（Object.assign/keys/entries, Array.from/isArray/includes/find）— P0/P1，难度 LOW，一次可以加一批，立竿见影。

**备选**：Map/Set（P0/P1，难度 HIGH 但可以纯 runtime 实现，不改 parser）。
