# KJS ECMAScript Compatibility Matrix — KHtmlLite

> 基于 KF5JS 5.115.0 头文件实际审计。KJS 源码为预编译库（MSYS2 mingw-w64-x86_64-kjs-qt5 5.115.0-1），头文件位于 `C:/msys64/mingw64/include/KF5/kjs/`。
> 标记：✓ 支持 / ◐ 部分支持 / ✗ 不支持 / ? 待确认
> 难度：LOW / MEDIUM / HIGH

---

## 1. 基础语法

| 特性 | 状态 | 证据 | 难度 |
|------|------|------|------|
| var | ✓ | grammar.h VAR token, nodes.h VarDeclNode | — |
| let | ✗ | grammar.h 无 LET token, nodes.h 无 LetDeclNode | HIGH (parser+scope) |
| const | ◐ | grammar.h 有 CONSTTOKEN 但 nodes.h 无 ConstDeclNode，可能解析为 var 或报错 | MEDIUM |
| function declaration | ✓ | grammar.h FUNCTION, nodes.h FuncDeclNode | — |
| function expression | ✓ | nodes.h FuncExprNode | — |
| arrow function | ✗ | 无 => token, 无 ArrowFuncNode | HIGH (parser+runtime) |
| class / extends / super | ✗ | 无 CLASS token, 无 ClassNode | HIGH (parser+runtime) |
| template literal | ✗ | 无 BACKTICK token, 无 TemplateLiteralNode | MEDIUM (lexer+parser) |
| object destructuring | ✗ | 无 DestructuringNode | HIGH (parser+runtime) |
| array destructuring | ✗ | 无 DestructuringNode | HIGH (parser+runtime) |
| default parameters | ✗ | 无对应 AST | MEDIUM (parser) |
| rest parameters | ✗ | 无对应 AST | MEDIUM (parser) |
| spread syntax | ✗ | 无 SPREAD token | MEDIUM (parser) |
| computed properties | ✗ | 无对应 AST | LOW (parser) |
| shorthand properties | ✗ | 无对应 AST | LOW (parser) |
| for...of | ✗ | 仅有 INTOKEN (for...in), 无 OF token | MEDIUM (parser+runtime) |
| for...in | ✓ | grammar.h INTOKEN | — |
| optional chaining ?. | ✗ | 无 token | HIGH (parser+runtime) |
| nullish coalescing ?? | ✗ | 无 token | MEDIUM (parser) |
| exponentiation ** | ✓ | grammar.h T_EXP=304, EXPEQUAL=309 | — |
| strict mode | ◐ | 有 function flags 机制，但完整 ES5 strict 支持待确认 | MEDIUM |
| debugger | ✓ | grammar.h DEBUGGER token | — |

**结论**：语法层面基本是 ES3 + 少量 ES5。const token 存在但无对应 AST 节点，可能是预留未实现。

---

## 2. 函数与异步

| 特性 | 状态 | 证据 | 难度 |
|------|------|------|------|
| Function | ✓ | function_object.h | — |
| ArrowFunction | ✗ | 无 | HIGH |
| Generator (function*) | ✗ | 无 YIELD token, 无 GeneratorNode | HIGH |
| async function | ✗ | 无 ASYNC token | HIGH |
| async/await | ✗ | 无 AWAIT token | VERY HIGH |
| Promise | ✗ | 全头文件无 Promise 引用 | HIGH (runtime) |
| Promise.all/race/allSettled/any | ✗ | 依赖 Promise | HIGH |
| Promise.prototype.finally | ✗ | 依赖 Promise | MEDIUM |
| microtask job queue | ✗ | 无 MicrotaskQueue/JobQueue | HIGH (runtime) |
| queueMicrotask | ✗ | 无 | MEDIUM (依赖 microtask) |

**结论**：完全没有异步能力。Promise 和 microtask 是现代网页最核心的缺失。

---

## 3. Object

| 特性 | 状态 | 证据 | 难度 |
|------|------|------|------|
| Object.assign | ✗ | object_object.h 无 | LOW (builtin) |
| Object.keys | ✗ | object_object.h 无 | LOW (builtin) |
| Object.values | ✗ | 无 | LOW (builtin) |
| Object.entries | ✗ | 无 | LOW (builtin) |
| Object.fromEntries | ✗ | 无 | LOW (builtin) |
| Object.create | ✗ | 无 | LOW (builtin) |
| Object.freeze | ✗ | 无 | LOW (builtin) |
| Object.seal | ✗ | 无 | LOW (builtin) |
| Object.defineProperty | ✗ | 无对应 Object 静态方法 | MEDIUM (builtin+property) |
| Object.getOwnPropertyDescriptor | ◐ | propertydescriptor.h 存在, StringInstance 有 getOwnPropertyDescriptor | MEDIUM |
| Object.getPrototypeOf | ✗ | 无 | LOW |
| Object.setPrototypeOf | ✗ | 无 | MEDIUM |
| Object.isExtensible / isFrozen / isSealed | ✗ | 无 | LOW |
| Object.is | ✗ | 无 | LOW |
| Object.getOwnPropertyNames | ◐ | StringInstance 有 getOwnPropertyNames | MEDIUM |
| Object.getOwnPropertySymbols | ✗ | 无 Symbol | HIGH |
| prototype methods (toString/hasOwnProperty/...) | ✓ | object_object.h enum | — |
| __defineGetter__ / __defineSetter__ | ✓ | object_object.h enum (非标准) | — |

**结论**：Object 静态方法几乎全部缺失。这些是纯 builtin API，实现难度 LOW，但需要先确认 property descriptor 基础设施。

---

## 4. Array

| 特性 | 状态 | 证据 | 难度 |
|------|------|------|------|
| Array.from | ✗ | array_object.h 无 | LOW (builtin) |
| Array.of | ✗ | 无 | LOW (builtin) |
| Array.isArray | ✗ | 无 | LOW (builtin) |
| includes | ✗ | array_object.h enum 无 | LOW (builtin) |
| find | ✗ | 无 | LOW (builtin) |
| findIndex | ✗ | 无 | LOW (builtin) |
| findLast / findLastIndex | ✗ | 无 | LOW (builtin) |
| flat / flatMap | ✗ | 无 | MEDIUM (builtin) |
| entries / keys / values | ✗ | 无 iterator 基础设施 | HIGH (依赖 iterator) |
| fill | ✗ | 无 | LOW (builtin) |
| copyWithin | ✗ | 无 | LOW (builtin) |
| at | ✗ | 无 | LOW (builtin) |
| ES5 methods (forEach/map/filter/reduce/every/some/indexOf/lastIndexOf/slice/splice/concat/join/push/pop/shift/unshift/sort/reverse) | ✓ | array_object.h enum 完整列出 | — |

**结论**：ES5 数组方法完整，ES6+ 全部缺失。大部分是纯 builtin，难度 LOW。entries/keys/values 需要 iterator 协议支持。

---

## 5. String

| 特性 | 状态 | 证据 | 难度 |
|------|------|------|------|
| includes | ✓ | string_object.h enum Includes | — |
| startsWith | ✓ | enum StartsWith | — |
| endsWith | ✓ | enum EndsWith | — |
| repeat | ✓ | enum Repeat (标注 ES6 Draft 08.11.2013) | — |
| padStart | ✗ | 无 | LOW (builtin) |
| padEnd | ✗ | 无 | LOW (builtin) |
| trimStart / trimEnd | ◐ | 有 TrimLeft/TrimRight (非标准名, KJS_PURE_ECMA 外) | LOW (builtin) |
| at | ✗ | 无 | LOW (builtin) |
| replaceAll | ✗ | 无 | MEDIUM (builtin) |
| matchAll | ✗ | 无 iterator | HIGH |
| codePointAt | ✗ | 无 | MEDIUM (builtin) |
| fromCodePoint | ✗ | 无 | LOW (builtin) |
| normalize | ✗ | 无 | MEDIUM (unicode) |
| raw (String.raw) | ✗ | 无 template literal | MEDIUM |
| trim | ✓ | enum Trim | — |
| ES3 methods (charAt/concat/indexOf/match/replace/search/slice/split/substr/substring/toLowerCase/...) | ✓ | enum 完整 | — |

**结论**：String 是 KJS 中 ES6 支持最好的内置对象（includes/startsWith/endsWith/repeat 已实现）。缺少的主要是 ES2017+ 方法。

---

## 6. 现代数据结构

| 特性 | 状态 | 证据 | 难度 |
|------|------|------|------|
| Map | ✗ | 全头文件无 Map 类 | HIGH (runtime+GC) |
| Set | ✗ | 无 | HIGH (runtime+GC) |
| WeakMap | ✗ | 无 | VERY HIGH (GC weak refs) |
| WeakSet | ✗ | 无 | VERY HIGH |
| Symbol | ✗ | 无 Symbol 类 (SymbolTable.h 是内部符号表, 非 ES6 Symbol) | HIGH (runtime+property) |
| BigInt | ✗ | 无 | VERY HIGH (new value type) |
| TypedArray (Int8Array/Uint8Array/...) | ◐ | KHTML 有 kjs_arraybuffer.cpp 绑定, 但 KJS 核心支持待确认 | MEDIUM |
| ArrayBuffer | ◐ | 同上 | MEDIUM |
| DataView | ✗ | 无 | MEDIUM |

**结论**：Map/Set/Symbol 完全缺失。这些需要 runtime 层面的新对象类型和 GC 集成。WeakMap/WeakSet 需要 GC weak reference 支持，KJS GC 当前不支持。

---

## 7. Object Meta Programming

| 特性 | 状态 | 证据 | 难度 |
|------|------|------|------|
| Proxy | ✗ | 无 Proxy 类 | VERY HIGH (runtime trap 机制) |
| Reflect | ✗ | 无 Reflect 类 | MEDIUM (builtin, 但依赖 Proxy 语义) |
| Proxy.get/set/has/deleteProperty/apply/construct | ✗ | 无 | VERY HIGH |

**结论**：Proxy 是实现成本最高的特性之一，需要在属性访问的所有路径插入 trap。现代框架（Vue 3 等）依赖 Proxy，但很多网站仍可在没有 Proxy 的情况下运行（通过降级或 polyfill）。

---

## 8. RegExp

| 特性 | 状态 | 证据 | 难度 |
|------|------|------|------|
| basic (g/i/m flags) | ✓ | regexp_object.h, regexp.h | — |
| exec / test / compile | ✓ | regexp_object.h enum | — |
| String.match / replace / search / split | ✓ | string_object.h enum | — |
| named capture groups (?<name>) | ✗ | 无 | HIGH (regex engine) |
| lookbehind (?<=...) | ✗ | 无 | HIGH (regex engine) |
| unicode flag (u) | ✗ | 无 | HIGH (regex engine) |
| dotAll flag (s) | ✗ | 无 | LOW (regex engine) |
| sticky flag (y) | ✗ | 无 | MEDIUM (regex engine) |
| matchAll | ✗ | 无 iterator | HIGH |
| replaceAll | ✗ | 无 | MEDIUM |

**结论**：RegExp 是 ES3 水平。现代 regex 特性需要修改底层 regex 引擎（KJS 有自己的 regexp.cpp）。

---

## 9. Modules

| 特性 | 状态 | 证据 | 难度 |
|------|------|------|------|
| import / export (ES modules) | ✗ | IMPORT token 存在但用于 package imports (非 ES modules) | VERY HIGH (parser+runtime+loader) |
| dynamic import() | ✗ | 无 | VERY HIGH |
| `<script type="module">` | ✗ | KHTML 不识别 module 类型 | HIGH (HTML+JS) |

**结论**：完全不支持 ES modules。`<script type="module">` 在 KHTML 中会被忽略或当作普通脚本处理。

---

## 10. Error / JSON / Date / Math / Number

### Error
| 特性 | 状态 |
|------|------|
| Error | ✓ |
| TypeError / RangeError / ReferenceError / SyntaxError / URIError / EvalError | ✓ (NativeErrorImp) |
| Error.captureStackTrace | ✗ |

### JSON
| 特性 | 状态 |
|------|------|
| JSON.parse | ✓ (json_object.h enum Parse) |
| JSON.stringify | ✓ (enum Stringify) |

### Date
| 特性 | 状态 |
|------|------|
| Date (full ES5) | ✓ (date_object.h enum 完整) |
| Date.now | ? (可能在 constructor 静态方法中) |
| Date.parse | ? |
| toISOString / toJSON | ✓ |

### Math
| 特性 | 状态 |
|------|------|
| ES3 (abs/ceil/floor/max/min/pow/round/sqrt/random/...) | ✓ |
| ES6 (trunc/sign/cbrt/hypot/log2/log10/imul/clz32/acosh/asinh/atanh/cosh/sinh/tanh/expm1/log1p/fround) | ✓ (math_object.h enum 标注 ES6) |

### Number
| 特性 | 状态 |
|------|------|
| ES5 (toFixed/toExponential/toPrecision/NaN/Infinity/MAX_VALUE/MIN_VALUE) | ✓ |
| ES6 (isFinite/isInteger/isNaN/isSafeInteger/parseInt/parseFloat/MAX_SAFE_INTEGER/MIN_SAFE_INTEGER/EPSILON?) | ✓ (number_object.h enum 标注 ES6) |

---

## 11. 总结：KJS 当前 ECMAScript 版本

**大致相当于 ES5 + 部分 ES6 内置方法（无 ES6 语法）**

- 语法：ES3 水平（var/function/for-in/try-catch），const token 预留
- 内置对象：ES5 完整 + ES6 部分方法（String.includes/startsWith/endsWith/repeat, Math.* ES6, Number.* ES6）
- 完全缺失：let/const 块级作用域、arrow function、class、template literal、destructuring、spread、for...of、Promise、Map/Set/Symbol、Proxy、async/await、modules、microtask
- GC：保守式 mark-sweep，cell 分配，无 weak reference

---

## 12. 与 QuickJS 对比（作为参考实现）

| 维度 | KJS 5.115 | QuickJS 2026-06-04 |
|------|-----------|---------------------|
| 语法 | ES3 + const token | ES2020 完整 |
| Promise | ✗ | ✓ |
| Map/Set | ✗ | ✓ |
| Symbol | ✗ | ✓ |
| async/await | ✗ | ✓ |
| Proxy | ✗ | ✓ |
| BigInt | ✗ | ✓ |
| Modules | ✗ | ✓ |
| microtask | ✗ | ✓ (JS_ExecutePendingJob) |
| TypedArray | ◐ | ✓ |
| GC | 保守 mark-sweep | 引用计数 + 周期 GC |
