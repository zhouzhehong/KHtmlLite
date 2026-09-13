# KJS Modernization Roadmap — KHtmlLite

> 基于 KJS 5.115 兼容性审计的现代化路线图。
> 原则：不替换 KJS，不重写 parser，增量扩展，每阶段可独立编译回归测试。

---

## Phase 0：兼容性审计（已完成）

- [x] 扫描 KJS 头文件，建立 ECMAScript 能力矩阵
- [x] 分析 GC 架构
- [x] 对照现代网页需求建立优先级
- [x] 产出：kjs_ecmascript_compatibility.md, modern_web_js_requirements.md, kjs_gc_architecture.md

---

## Phase 1：纯 Builtin API 扩展（LOW 难度，高收益）

**目标**：不修改 parser/runtime，仅添加内置对象方法，快速提升兼容性。

### 1.1 Object 静态方法
- Object.assign
- Object.keys
- Object.values
- Object.entries
- Object.create
- Object.freeze / Object.seal / Object.preventExtensions
- Object.isExtensible / Object.isFrozen / Object.isSealed
- Object.getPrototypeOf
- Object.is

**涉及文件**：KF5JS object_object.cpp（需要 KF5JS 源码）

### 1.2 Array 静态/原型方法
- Array.from
- Array.of
- Array.isArray
- Array.prototype.includes
- Array.prototype.find
- Array.prototype.findIndex
- Array.prototype.fill
- Array.prototype.at

### 1.3 String 原型方法
- String.prototype.padStart
- String.prototype.padEnd
- String.prototype.trimStart / trimEnd（已有 TrimLeft/TrimRight，添加标准别名）
- String.prototype.at

### 1.4 验证
- tests/js_compat/object_builtins.html
- tests/js_compat/array_builtins.html
- tests/js_compat/string_builtins.html
- Bing 回归测试

---

## Phase 2：Template Literal（MEDIUM 难度，独立语法特性）

**目标**：支持 `string ${expr}` 模板字符串。

### 2.1 Lexer
- 添加 BACKTICK token
- 模板字符串内的 `${` 识别
- 模板字符串内容扫描

### 2.2 Parser (grammar.y)
- TemplateLiteral 产生式
- TemplateElement 产生式
- 嵌套表达式处理

### 2.3 AST (nodes.h/nodes.cpp)
- TemplateLiteralNode
- TemplateElementNode

### 2.4 运行时
- 模板字符串求值为字符串拼接
- 标签模板（tagged template）可选

**涉及文件**：KF5JS lexer.cpp, grammar.y, nodes.h, nodes.cpp

### 2.5 验证
- tests/js_compat/template.html

---

## Phase 3：Map + Set（HIGH 难度，runtime 新对象）

**目标**：实现 ES6 Map 和 Set 数据结构。

### 3.1 Map
- Map 构造函数（可传入 iterable，初期可只支持空）
- set(key, value)
- get(key)
- has(key)
- delete(key)
- clear()
- size (getter)
- forEach(callback)
- keys() / values() / entries()（初期可返回数组，不要求完整 iterator）

### 3.2 Set
- Set 构造函数
- add(value)
- has(value)
- delete(value)
- clear()
- size
- forEach
- keys() / values()

### 3.3 GC 集成
- Map/Set 内部用 WTF::HashMap/HashSet
- 必须实现 markChildren() 标记内部 key/value
- key 为对象时的相等性比较（SameValueZero）

### 3.4 验证
- tests/js_compat/map.html
- tests/js_compat/set.html

---

## Phase 4：Promise + Microtask（HIGH 难度，执行模型扩展）

**目标**：实现 Promise 和 microtask 队列，这是现代网页异步基础。

### 4.1 Promise
- Promise 构造函数 (executor)
- Promise.resolve / Promise.reject
- Promise.prototype.then
- Promise.prototype.catch
- Promise.prototype.finally
- Promise.all / Promise.race（初期）
- Promise.allSettled / Promise.any（后续）

### 4.2 Microtask Queue
- 在 Interpreter 中添加 microtask 队列
- 每次 script 执行完成后执行所有 pending microtask
- 每个 event handler / timer 回调执行后执行 microtask
- microtask 中的新 microtask 也在当前 tick 执行

### 4.3 GC 集成
- Promise 回调链必须 mark
- microtask queue 中的 job 必须 protect

### 4.4 验证
- tests/js_compat/promise.html
- tests/js_compat/microtask.html
- Bing 回归测试（Promise 是 Bing 核心依赖）

---

## Phase 5：let / const（HIGH 难度，scope chain 扩展）

**目标**：支持块级作用域声明。

### 5.1 Parser
- LET token（grammar.h 添加）
- LexicalDeclaration 产生式
- 块级作用域跟踪

### 5.2 Runtime
- LexicalEnvironment（块级作用域对象）
- ScopeChain 扩展支持块级作用域
- const 赋值保护
- TDZ（Temporal Dead Zone）初期可简化

### 5.3 AST
- LetDeclNode / ConstDeclNode（可复用 VarDeclNode 加标志）

### 5.4 验证
- tests/js_compat/let_const.html
- tests/js_compat/block_scope.html

---

## Phase 6：Arrow Function（HIGH 难度，函数模型扩展）

**目标**：支持 `() => expr` 和 `() => { ... }`。

### 6.1 Parser
- ARROW token (=>)
- ArrowFunction 产生式
- 简洁体（concise body）vs 块体

### 6.2 Runtime
- ArrowFunctionImp（无 prototype，无 arguments，this 词法绑定）
- this 从定义时的 scope chain 捕获

### 6.3 验证
- tests/js_compat/arrow.html

---

## Phase 7：Destructuring + Spread（HIGH 难度）

### 7.1 Array destructuring
- `const [a, b] = arr`
- `const [a, ...rest] = arr`

### 7.2 Object destructuring
- `const {a, b} = obj`
- `const {a: x, b: y} = obj`
- 默认值

### 7.3 Spread
- 函数调用 `f(...args)`
- 数组字面量 `[...arr]`
- 对象字面量 `{...obj}`（后续）

### 7.4 验证
- tests/js_compat/destructuring.html
- tests/js_compat/spread.html

---

## Phase 8：async/await（VERY HIGH 难度，依赖 Promise + generator）

### 8.1 Generator 基础
- function* 语法
- yield 表达式
- iterator 协议（next/return/throw）

### 8.2 async/await
- async function 语法
- await 表达式
- 基于 Promise 的状态机转换

### 8.3 验证
- tests/js_compat/async.html
- tests/js_compat/generator.html

---

## Phase 9：剩余现代特性（按优先级）

### 9.1 Symbol
- Symbol() 构造
- Symbol.iterator / Symbol.toPrimitive 等 well-known symbols
- 作为属性 key

### 9.2 for...of
- iterator 协议
- for...of 语句

### 9.3 class
- class 声明/表达式
- extends / super
- 方法定义

### 9.4 optional chaining / nullish coalescing

### 9.5 Object.fromEntries / Array.flat / String.replaceAll

---

## Phase 10：高级特性（可选，长期）

- Proxy / Reflect
- ES Modules
- TypedArray 完善
- BigInt
- WeakMap/WeakSet（需要 GC weak ref，可能不可行）
- RegExp 现代特性（sticky/unicode/named capture）

---

## 每阶段通用规则

1. **每阶段开始前**：获取 KF5JS 源码（当前只有预编译库和头文件，需要下载源码才能修改）
2. **每阶段结束后**：编译 KF5JS → 编译 KHTML → 运行测试 → Bing 回归
3. **不回退**：已实现的功能不删除
4. **不破坏 KJS**：所有修改是增量扩展，不修改现有 ES3/ES5 行为
5. **QuickJS 作为参考**：行为不确定时用 QuickJS 验证标准行为，但不复制 QuickJS 源码
6. **测试先行**：每个功能先写 tests/js_compat/*.html 测试

---

## 前置条件：获取 KF5JS 源码

当前 KF5JS 是 MSYS2 预编译库（mingw-w64-x86_64-kjs-qt5 5.115.0），只有头文件和 .dll/.a，没有 .cpp 源码。

**必须先获取 KF5JS 源码才能修改 parser/interpreter/runtime**：
- KDE 官方：https://download.kde.org/stable/frameworks/5.115/porting/kjs-5.115.0.tar.xz
- 或 MSYS2 源码包：pacman -S mingw-w64-x86_64-kjs-qt5 (source)

获取源码后，需要将 KF5JS 纳入 KHtmlLite 构建体系，从源码编译而非链接系统库。
