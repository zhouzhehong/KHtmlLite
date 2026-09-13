#include "pageloader.h"
#include "js_transpile.h"
#include "css_anim_polyfill.h"
#include "perflog.h"
#include <QRegularExpression>
#include <QCryptographicHash>
#include <QFile>
#include <QTextStream>
#include <QStandardPaths>
#include <QDir>
#include <QColor>
#include <QDebug>

// Minimal ES5+ polyfill bundle. Kept small and ES3-only so KJS (2001-era)
// can parse and execute it without tripping over modern syntax. Wrapped in
// try/catch so a failure in one polyfill never takes down the page.
static const char *kJsPolyfill =
"(function(){"
"try{"
"var w=window;"
"if(!w.console)w.console={log:function(){},warn:function(){},error:function(){},info:function(){},debug:function(){}};"
"if(!w.performance)w.performance={now:function(){return new Date().getTime();}};"
"if(!w.requestAnimationFrame)w.requestAnimationFrame=function(cb){return setTimeout(function(){cb(new Date().getTime());},16);};"
"if(!w.cancelAnimationFrame)w.cancelAnimationFrame=function(id){clearTimeout(id);};"
"if(!Object.assign)Object.assign=function(t){for(var i=1;i<arguments.length;i++){var s=arguments[i];if(s){for(var k in s)t[k]=s[k];}}return t;};"
"if(!Array.from)Array.from=function(a){var r=[];if(a&&a.length){for(var i=0;i<a.length;i++)r.push(a[i]);}return r;};"
"if(!Array.of)Array.of=function(){return Array.prototype.slice.call(arguments);};"
"if(!Array.prototype.includes)Array.prototype.includes=function(v){return this.indexOf(v)!==-1;};"
"if(!Array.prototype.find)Array.prototype.find=function(fn){for(var i=0;i<this.length;i++){if(fn(this[i],i,this))return this[i];}return undefined;};"
"if(!Array.prototype.findIndex)Array.prototype.findIndex=function(fn){for(var i=0;i<this.length;i++){if(fn(this[i],i,this))return i;}return -1;};"
"if(!String.prototype.includes)String.prototype.includes=function(s){return this.indexOf(s)!==-1;};"
"if(!String.prototype.startsWith)String.prototype.startsWith=function(s){return this.indexOf(s)===0;};"
"if(!String.prototype.endsWith)String.prototype.endsWith=function(s){return this.lastIndexOf(s)===this.length-s.length;};"
"if(!String.prototype.repeat)String.prototype.repeat=function(n){var r='';for(var i=0;i<n;i++)r+=this;return r;};"
"if(!String.prototype.trim)String.prototype.trim=function(){return this.replace(/^\\s+|\\s+$/g,'');};"
"if(!Number.isNaN)Number.isNaN=function(v){return typeof v==='number'&&isNaN(v);};"
"if(!Number.isFinite)Number.isFinite=function(v){return typeof v==='number'&&isFinite(v);};"
"if(window.NodeList&&!NodeList.prototype.forEach)NodeList.prototype.forEach=Array.prototype.forEach;"
"if(!Element.prototype.matches)Element.prototype.matches=function(s){var m=(this.document||this.ownerDocument).getElementsByTagName('*');for(var i=0;i<m.length;i++){if(m[i]===this)return true;}return false;};"
"if(!Element.prototype.closest)Element.prototype.closest=function(s){var el=this;while(el){if(el.matches&&el.matches(s))return el;el=el.parentElement;}return null;};"
"if(!Element.prototype.remove)Element.prototype.remove=function(){if(this.parentElement)this.parentElement.removeChild(this);};"
"if(!w.localStorage){var _ls={};w.localStorage={getItem:function(k){return _ls[k]||null;},setItem:function(k,v){_ls[k]=String(v);},removeItem:function(k){delete _ls[k];},clear:function(){_ls={};},key:function(i){return Object.keys(_ls)[i]||null;};};}"
"if(!w.sessionStorage)w.sessionStorage=w.localStorage;"
"if(!w.Map){w.Map=function(){this._d={};};w.Map.prototype.set=function(k,v){this._d[k]=v;return this;};w.Map.prototype.get=function(k){return this._d[k];};w.Map.prototype.has=function(k){return k in this._d;};w.Map.prototype.delete=function(k){delete this._d[k];};w.Map.prototype.clear=function(){this._d={};};w.Map.prototype.forEach=function(fn){for(var k in this._d)fn(this._d[k],k,this);};}"
"if(!w.Set){w.Set=function(){this._d={};};w.Set.prototype.add=function(v){this._d[v]=v;return this;};w.Set.prototype.has=function(v){return v in this._d;};w.Set.prototype.delete=function(v){delete this._d[v];};w.Set.prototype.clear=function(){this._d={};};w.Set.prototype.forEach=function(fn){for(var k in this._d)fn(this._d[k],k,this);};}"
"if(!w.Symbol)w.Symbol=function(d){return 'jsym_'+d+'_'+Math.random().toString(36).slice(2);};"
"if(!Object.keys)Object.keys=function(o){var r=[];for(var k in o)if(Object.prototype.hasOwnProperty.call(o,k))r.push(k);return r;};"
"if(!Object.values)Object.values=function(o){var r=[];for(var k in o)if(Object.prototype.hasOwnProperty.call(o,k))r.push(o[k]);return r;};"
"if(!Object.entries)Object.entries=function(o){var r=[];for(var k in o)if(Object.prototype.hasOwnProperty.call(o,k))r.push([k,o[k]]);return r;};"
"if(!Array.isArray)Array.isArray=function(v){return Object.prototype.toString.call(v)==='[object Array]';};"
"if(!Function.prototype.bind)Function.prototype.bind=function(ctx){var fn=this,args=Array.prototype.slice.call(arguments,1);return function(){return fn.apply(ctx,args.concat(Array.prototype.slice.call(arguments)));};};"
"if(!Date.now)Date.now=function(){return new Date().getTime();};"
"if(!Array.prototype.filter)Array.prototype.filter=function(fn){var r=[];for(var i=0;i<this.length;i++)if(fn(this[i],i,this))r.push(this[i]);return r;};"
"if(!Array.prototype.map)Array.prototype.map=function(fn){var r=[];for(var i=0;i<this.length;i++)r.push(fn(this[i],i,this));return r;};"
"if(!Array.prototype.forEach)Array.prototype.forEach=function(fn){for(var i=0;i<this.length;i++)fn(this[i],i,this);};"
"if(!Array.prototype.reduce)Array.prototype.reduce=function(fn,iv){var acc=iv,start=0;if(iv===undefined){acc=this[0];start=1;}for(var i=start;i<this.length;i++)acc=fn(acc,this[i],i,this);return acc;};"
"if(!Array.prototype.some)Array.prototype.some=function(fn){for(var i=0;i<this.length;i++)if(fn(this[i],i,this))return true;return false;};"
"if(!Array.prototype.every)Array.prototype.every=function(fn){for(var i=0;i<this.length;i++)if(!fn(this[i],i,this))return false;return true;};"
"if(!Array.prototype.indexOf)Array.prototype.indexOf=function(v,s){s=s||0;for(var i=s;i<this.length;i++)if(this[i]===v)return i;return -1;};"
"if(!w.Promise){(function(){function P(exec){var self=this;self._state=0;self._value=null;self._cbs=[];function resolve(v){if(self._state!==0)return;self._state=1;self._value=v;for(var i=0;i<self._cbs.length;i++)self._cbs[i](v);}function reject(e){if(self._state!==0)return;self._state=2;self._value=e;}try{exec(resolve,reject);}catch(e){reject(e);}}P.prototype.then=function(onF,onR){var self=this;return new P(function(res,rej){function run(v){try{var r=(self._state===1)?(onF?onF(v):v):(onR?onR(v):v);if(r&&typeof r.then==='function')r.then(res,rej);else res(r);}catch(e){rej(e);}}if(self._state===0){self._cbs.push(function(v){self._state=1;self._value=v;run(v);});}else{setTimeout(function(){run(self._value);},0);}});};P.prototype.catch=function(onR){return this.then(null,onR);};P.resolve=function(v){return new P(function(r){r(v);});};P.reject=function(e){return new P(function(_,r){r(e);});};P.all=function(arr){return new P(function(res,rej){var results=[],done=0;if(arr.length===0){res(results);return;}for(var i=0;i<arr.length;i++){(function(idx){arr[idx].then(function(v){results[idx]=v;done++;if(done===arr.length)res(results);},rej);})(i);}});};P.race=function(arr){return new P(function(res,rej){for(var i=0;i<arr.length;i++)arr[i].then(res,rej);});};w.Promise=P;})();}"
"if(!w.fetch){w.fetch=function(url,opts){opts=opts||{};return new Promise(function(res,rej){var xhr=new XMLHttpRequest();xhr.open(opts.method||'GET',url);xhr.onload=function(){res({ok:xhr.status>=200&&xhr.status<300,status:xhr.status,statusText:xhr.statusText,json:function(){return Promise.resolve(JSON.parse(xhr.responseText));},text:function(){return Promise.resolve(xhr.responseText);}});};xhr.onerror=function(){rej(new TypeError('Network error'));};if(opts.headers){for(var k in opts.headers)xhr.setRequestHeader(k,opts.headers[k]);}xhr.send(opts.body||null);});};}"
"if(!w.atob)w.atob=function(s){var b='ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';var r='',i=0;s=s.replace(/[^A-Za-z0-9+/=]/g,'');while(i<s.length){var c1=b.indexOf(s.charAt(i++)),c2=b.indexOf(s.charAt(i++)),c3=b.indexOf(s.charAt(i++)),c4=b.indexOf(s.charAt(i++));var o1=(c1<<2)|(c2>>4),o2=((c2&15)<<4)|(c3>>2),o3=((c3&3)<<6)|c4;r+=String.fromCharCode(o1);if(c3!==64)r+=String.fromCharCode(o2);if(c4!==64)r+=String.fromCharCode(o3);}return r;};"
"if(!w.btoa)w.btoa=function(s){var b='ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';var r='',i=0;while(i<s.length){var c1=s.charCodeAt(i++),c2=s.charCodeAt(i++),c3=s.charCodeAt(i++);var o1=c1>>2,o2=((c1&3)<<4)|(c2>>4),o3=((c2&15)<<2)|(c3>>6),o4=c3&63;if(isNaN(c2))o3=o4=64;else if(isNaN(c3))o4=64;r+=b.charAt(o1)+b.charAt(o2)+b.charAt(o3)+b.charAt(o4);}return r;};"
"if(!w.IntersectionObserver)w.IntersectionObserver=function(){this.observe=function(){};this.unobserve=function(){};this.disconnect=function(){};};"
"if(!w.MutationObserver)w.MutationObserver=function(){this.observe=function(){};this.disconnect=function(){};};"
"if(!w.ResizeObserver)w.ResizeObserver=function(){this.observe=function(){};this.unobserve=function(){};this.disconnect=function(){};};"
"if(!w.AbortController)w.AbortController=function(){this.signal={aborted:false};this.abort=function(){this.signal.aborted=true;};};"
"if(!w.URLSearchParams)w.URLSearchParams=function(s){this._p={};if(s){var ps=s.split('&');for(var i=0;i<ps.length;i++){var kv=ps[i].split('=');this._p[decodeURIComponent(kv[0])]=decodeURIComponent(kv[1]||'');}}};w.URLSearchParams.prototype.get=function(k){return this._p[k]||null;};w.URLSearchParams.prototype.has=function(k){return k in this._p;};w.URLSearchParams.prototype.set=function(k,v){this._p[k]=v;};w.URLSearchParams.prototype.toString=function(){var r=[];for(var k in this._p)r.push(encodeURIComponent(k)+'='+encodeURIComponent(this._p[k]));return r.join('&');};"
"if(!w.getComputedStyle)w.getComputedStyle=function(el){return el.currentStyle||el.style;};"
"if(!w.matchMedia)w.matchMedia=function(){return {matches:false,addListener:function(){},removeListener:function(){}};};"
"if(!w.globalThis)w.globalThis=w;"
"if(!w.queueMicrotask)w.queueMicrotask=function(cb){setTimeout(cb,0);};"
"if(!w.crypto)w.crypto={getRandomValues:function(a){for(var i=0;i<a.length;i++)a[i]=Math.floor(Math.random()*256);return a;}};"
"if(!w.structuredClone)w.structuredClone=function(v){return JSON.parse(JSON.stringify(v));};"
"if(!Object.is)Object.is=function(a,b){if(a===b)return a!==0||1/a===1/b;return a!==a&&b!==b;};"
"if(!Array.prototype.fill)Array.prototype.fill=function(v,s,e){s=s||0;e=e||this.length;for(var i=s;i<e;i++)this[i]=v;return this;};"
"if(!String.prototype.padStart)String.prototype.padStart=function(l,c){c=c||' ';var s=this;while(s.length<l)s=c+s;return s;};"
"if(!String.prototype.padEnd)String.prototype.padEnd=function(l,c){c=c||' ';var s=this;while(s.length<l)s=s+c;return s;};"
"if(!Element.prototype.append)Element.prototype.append=function(){for(var i=0;i<arguments.length;i++)this.appendChild(arguments[i]);};"
"if(!Element.prototype.prepend)Element.prototype.prepend=function(){for(var i=0;i<arguments.length;i++)this.insertBefore(arguments[i],this.firstChild);};"
"if(!Element.prototype.before)Element.prototype.before=function(){var p=this.parentElement;if(p)for(var i=0;i<arguments.length;i++)p.insertBefore(arguments[i],this);};"
"if(!Element.prototype.after)Element.prototype.after=function(){var p=this.parentElement;if(p)for(var i=0;i<arguments.length;i++)p.insertBefore(arguments[i],this.nextSibling);};"
"if(!document.head)document.head=document.getElementsByTagName('head')[0];"
"if(!document.activeElement)document.activeElement=document.body;"
"}catch(e){}"
"})();";

// Parse a CSS color token (#rgb, #rrggbb, rgb(), rgba(), or named color) into
// QColor. Returns an invalid QColor if the token is not a color.
static QColor parseCssColor(const QString &tok)
{
    QString t = tok.trimmed();
    if (t.startsWith(QLatin1Char('#')))
        return QColor(t);
    if (t.startsWith(QLatin1String("rgb"), Qt::CaseInsensitive)) {
        // rgb(r,g,b) or rgba(r,g,b,a) 鈥?pull the numbers out.
        const QRegularExpression re(
            QStringLiteral("rgba?\\(\\s*([0-9.]+)[ ,]+([0-9.]+)[ ,]+([0-9.]+)[ ,]*([0-9.]*)\\s*\\)"),
            QRegularExpression::CaseInsensitiveOption);
        const QRegularExpressionMatch m = re.match(t);
        if (m.hasMatch()) {
            bool ok;
            const int r = m.captured(1).toInt(&ok);
            const int g = m.captured(2).toInt();
            const int b = m.captured(3).toInt();
            if (ok) return QColor(r, g, b);
        }
        return QColor();
    }
    const QColor named(t);
    return named.isValid() ? named : QColor();
}

// Replace every linear-gradient()/radial-gradient() (including vendor prefixes)
// with a flat rgb() color blended from its first and last color stops. KHTML
// has no CSS gradient painter; this keeps backgrounds opaque instead of
// transparent. Full gradient rendering is a future engine-level change.
static QString simplifyGradients(const QString &text)
{
    const QRegularExpression reFunc(
        QStringLiteral("(-(?:webkit|moz|o|ms)-)?(?:linear|repeating-linear|radial|repeating-radial)-gradient\\s*\\("),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpression reColor(
        QStringLiteral("#(?:[0-9a-f]{3}){1,2}\\b|rgba?\\([^)]*\\)|hsla?\\([^)]*\\)|"
                       "(?:transparent|black|silver|gray|white|maroon|red|purple|fuchsia|green|"
                       "lime|olive|yellow|navy|blue|teal|aqua|orange|aliceblue|antiquewhite|"
                       "aquamarine|azure|beige|bisque|blanchedalmond|blueviolet|brown|"
                       "burlywood|cadetblue|chartreuse|chocolate|coral|cornflowerblue|"
                       "cornsilk|crimson|darkblue|darkcyan|darkgoldenrod|darkgray|darkgreen|"
                       "darkgrey|darkkhaki|darkmagenta|darkolivegreen|darkorange|darkorchid|"
                       "darkred|darksalmon|darkseagreen|darkslateblue|darkslategray|"
                       "darkturquoise|darkviolet|deeppink|deepskyblue|dimgray|dodgerblue|"
                       "firebrick|floralwhite|forestgreen|gainsboro|ghostwhite|gold|"
                       "goldenrod|grey|greenyellow|honeydew|hotpink|indianred|indigo|"
                       "ivory|khaki|lavender|lavenderblush|lawngreen|lemonchiffon|"
                       "lightblue|lightcoral|lightcyan|lightgoldenrodyellow|lightgray|"
                       "lightgreen|lightgrey|lightpink|lightsalmon|lightseagreen|"
                       "lightskyblue|lightslategray|lightsteelblue|lightyellow|limegreen|"
                       "linen|mediumaquamarine|mediumblue|mediumorchid|mediumpurple|"
                       "mediumseagreen|mediumslateblue|mediumspringgreen|mediumturquoise|"
                       "mediumvioletred|midnightblue|mintcream|mistyrose|moccasin|"
                       "navajowhite|oldlace|olivedrab|orangered|orchid|palegoldenrod|"
                       "palegreen|paleturquoise|palevioletred|papayawhip|peachpuff|peru|"
                       "pink|plum|powderblue|rosybrown|royalblue|saddlebrown|salmon|"
                       "sandybrown|seagreen|seashell|sienna|skyblue|slateblue|slategray|"
                       "snow|springgreen|steelblue|tan|thistle|tomato|turquoise|"
                       "violet|wheat|whitesmoke|yellowgreen)\\b"),
        QRegularExpression::CaseInsensitiveOption);

    QString out;
    out.reserve(text.size());
    int last = 0;
    auto it = reFunc.globalMatch(text);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        out.append(text.midRef(last, m.capturedStart() - last));
        // Find the matching close paren, accounting for nesting (rgb() inside).
        int depth = 1;
        int pos = m.capturedEnd();
        while (pos < text.size() && depth > 0) {
            const QChar c = text.at(pos);
            if (c == QLatin1Char('(')) ++depth;
            else if (c == QLatin1Char(')')) --depth;
            ++pos;
        }
        const QString args = text.mid(m.capturedEnd(), pos - m.capturedEnd() - 1);
        // Pull colors out of the argument list.
        QList<QColor> colors;
        auto cit = reColor.globalMatch(args);
        while (cit.hasNext()) {
            const QColor c = parseCssColor(cit.next().captured(0));
            if (c.isValid()) colors.append(c);
        }
        if (colors.size() >= 2) {
            // Blend first and last stop.
            const QColor &a = colors.first();
            const QColor &b = colors.last();
            const int r = (a.red() + b.red()) / 2;
            const int g = (a.green() + b.green()) / 2;
            const int bl = (a.blue() + b.blue()) / 2;
            out.append(QStringLiteral("rgb(%1,%2,%3)").arg(r).arg(g).arg(bl));
        } else if (colors.size() == 1) {
            out.append(QStringLiteral("rgb(%1,%2,%3)")
                       .arg(colors.first().red())
                       .arg(colors.first().green())
                       .arg(colors.first().blue()));
        } else {
            out.append(m.captured(0)); // no colors found, leave as-is
        }
        last = pos;
    }
    out.append(text.midRef(last));
    return out;
}

// Remove an at-rule block (@keyframes, @font-face, @supports, ...) starting at
// |pos| (the '@' character). Returns the text with the block removed.
static QString removeAtBlock(const QString &text, int pos)
{
    int brace = text.indexOf(QLatin1Char('{'), pos);
    if (brace < 0) return text;
    int depth = 0;
    int end = brace;
    while (end < text.size()) {
        const QChar c = text.at(end);
        if (c == QLatin1Char('{')) ++depth;
        else if (c == QLatin1Char('}')) {
            --depth;
            if (depth == 0) { ++end; break; }
        }
        ++end;
    }
    return text.left(pos) + text.mid(end);
}

// Strip @font-face rules whose src only references formats Qt cannot load
// (woff/woff2/eot). Rules with a ttf/otf/svg source are kept, and the
// unsupported-format entries are removed from their src list.
static QString stripUnsupportedFonts(const QString &css)
{
    QString out = css;
    int pos = 0;
    while ((pos = out.indexOf(QStringLiteral("@font-face"), pos, Qt::CaseInsensitive)) >= 0) {
        int brace = out.indexOf(QLatin1Char('{'), pos);
        if (brace < 0) break;
        int depth = 0, end = brace;
        while (end < out.size()) {
            const QChar c = out.at(end);
            if (c == QLatin1Char('{')) ++depth;
            else if (c == QLatin1Char('}')) {
                --depth;
                if (depth == 0) { ++end; break; }
            }
            ++end;
        }
        QString block = out.mid(pos, end - pos);
        const QString lower = block.toLower();
        const bool hasTtf = lower.contains(QLatin1String(".ttf")) || lower.contains(QLatin1String("truetype"));
        const bool hasOtf = lower.contains(QLatin1String(".otf")) || lower.contains(QLatin1String("opentype"));
        const bool hasSvgFont = lower.contains(QLatin1String(".svg")) && lower.contains(QLatin1String("font"));
        if (!hasTtf && !hasOtf && !hasSvgFont) {
            // No loadable format 鈥?drop the whole @font-face rule.
            out = out.left(pos) + out.mid(end);
        } else {
            // Keep the rule but drop woff/woff2/eot url() entries so the engine
            // falls through to the loadable format.
            const QRegularExpression reWoff(
                QStringLiteral("url\\([^)]*\\.(?:woff2?|eot)[^)]*\\)\\s*(?:format\\([^)]*\\))?\\s*,?"),
                QRegularExpression::CaseInsensitiveOption);
            block.remove(reWoff);
            out = out.left(pos) + block + out.mid(end);
            pos = pos + block.size();
        }
    }
    return out;
}

// Downgrade CSS features KHTML cannot render safely. Removing these prevents
// assertion trips in the layout engine and keeps pages stable.

// Extract @keyframes rules from CSS using brace-matching instead of regex.
// Regex backtracking on megabyte-scale CSS was causing stack overflow crashes.
static QString extractKeyframesJs(const QString &css)
{
    if (css.size() > 65536) return QStringLiteral("{}");
    QString result = QStringLiteral("{");
    bool first = true;
    int count = 0;
    int pos = 0;
    while (count < 30 && pos < css.size()) {
        // Find next @keyframes (optionally -webkit- prefix)
        int kfPos = css.indexOf(QStringLiteral("@keyframes"), pos, Qt::CaseInsensitive);
        int wkPos = css.indexOf(QStringLiteral("@-webkit-keyframes"), pos, Qt::CaseInsensitive);
        int atPos = -1;
        if (kfPos >= 0 && (wkPos < 0 || kfPos < wkPos)) atPos = kfPos;
        else if (wkPos >= 0) atPos = wkPos;
        if (atPos < 0) break;
        // Skip past the @keyframes keyword and whitespace to get the name
        int nameStart = atPos + (wkPos == atPos ? 17 : 10);
        while (nameStart < css.size() && (css.at(nameStart) == QLatin1Char(' ') || css.at(nameStart) == QLatin1Char('\t') || css.at(nameStart) == QLatin1Char('\n')))
            nameStart++;
        int nameEnd = nameStart;
        while (nameEnd < css.size() && css.at(nameEnd) != QLatin1Char('{') && css.at(nameEnd) != QLatin1Char(' ') && css.at(nameEnd) != QLatin1Char('\t'))
            nameEnd++;
        QString name = css.mid(nameStart, nameEnd - nameStart).trimmed();
        if (name.isEmpty()) { pos = atPos + 10; continue; }
        // Find opening brace
        int braceStart = css.indexOf(QLatin1Char('{'), nameEnd);
        if (braceStart < 0) break;
        // Brace-match to find the closing brace
        int depth = 1;
        int p = braceStart + 1;
        while (p < css.size() && depth > 0) {
            QChar ch = css.at(p);
            if (ch == QLatin1Char('{')) depth++;
            else if (ch == QLatin1Char('}')) depth--;
            p++;
        }
        if (depth != 0) break;
        QString body = css.mid(braceStart + 1, p - braceStart - 2);
        // Parse keyframe selectors within the body
        QString frames;
        bool firstFrame = true;
        int bp = 0;
        while (bp < body.size()) {
            int selStart = bp;
            while (selStart < body.size() && (body.at(selStart).isSpace() || body.at(selStart) == QLatin1Char(',')))
                selStart++;
            if (selStart >= body.size()) break;
            int selEnd = body.indexOf(QLatin1Char('{'), selStart);
            if (selEnd < 0) break;
            QString sel = body.mid(selStart, selEnd - selStart).trimmed().toLower();
            // Find matching close brace for this frame
            int fd = 1, fp = selEnd + 1;
            while (fp < body.size() && fd > 0) {
                if (body.at(fp) == QLatin1Char('{')) fd++;
                else if (body.at(fp) == QLatin1Char('}')) fd--;
                fp++;
            }
            if (fd != 0) break;
            QString props = body.mid(selEnd + 1, fp - selEnd - 2).trimmed();
            double offset = 0.0;
            if (sel == QStringLiteral("from")) offset = 0.0;
            else if (sel == QStringLiteral("to")) offset = 1.0;
            else {
                bool ok = false;
                double v = sel.replace(QStringLiteral("%"), QString()).toDouble(&ok);
                if (ok) offset = v / 100.0;
            }
            props.replace(QStringLiteral("\\"), QStringLiteral("\\\\"));
            props.replace(QChar('"'), QStringLiteral("\\\""));
            props.replace(QStringLiteral("\n"), QStringLiteral(" "));
            if (!firstFrame) frames += QLatin1Char(',');
            frames += QLatin1String("{\"offset\":") + QString::number(offset)
                   + QLatin1String(",\"cssText\":\"") + props + QLatin1String("\"}");
            firstFrame = false;
            bp = fp;
        }
        if (!frames.isEmpty()) {
            if (!first) result += QLatin1Char(',');
            result += QLatin1String("\"") + name + QLatin1String("\":[") + frames + QLatin1Char(']');
            first = false;
            count++;
        }
        pos = p;
    }
    result += QLatin1Char('}');
    return result;
}


static QString simplifyUnsupportedCss(const QString &text)
{
    QString out = text;

    // CSS Grid -> Flexbox fallback. KHTML has no grid layout; flex with wrap
    // approximates the most common grid use cases (card rows, galleries).
    out.replace(QRegularExpression(
        QStringLiteral("display\\s*:\\s*grid"),
        QRegularExpression::CaseInsensitiveOption),
        QStringLiteral("display:flex;flex-wrap:wrap"));
    out.replace(QRegularExpression(
        QStringLiteral("display\\s*:\\s*inline-grid"),
        QRegularExpression::CaseInsensitiveOption),
        QStringLiteral("display:inline-flex;flex-wrap:wrap"));

    // position: sticky -> relative (layout stays valid, no scroll-pin)
    out.replace(QRegularExpression(
        QStringLiteral("position\\s*:\\s*sticky"),
        QRegularExpression::CaseInsensitiveOption),
        QStringLiteral("position:relative"));

    // transform: translate(X, Y) -> position:relative; left:X; top:Y
    // scale/rotate/skew have no CSS2 equivalent and are stripped below.
    out.replace(QRegularExpression(
        QStringLiteral("transform\\s*:\\s*translate\\(([^,)]+)(?:,\\s*([^)]+))?\\)"),
        QRegularExpression::CaseInsensitiveOption),
        QStringLiteral("position:relative;left:\\1;top:\\2"));
    out.replace(QRegularExpression(
        QStringLiteral("transform\\s*:\\s*translateX\\(([^)]+)\\)"),
        QRegularExpression::CaseInsensitiveOption),
        QStringLiteral("position:relative;left:\\1"));
    out.replace(QRegularExpression(
        QStringLiteral("transform\\s*:\\s*translateY\\(([^)]+)\\)"),
        QRegularExpression::CaseInsensitiveOption),
        QStringLiteral("position:relative;top:\\1"));

    // Properties KHTML has no pipeline for 鈥?remove the whole declaration.
    // Only properties that are genuinely absent or known-crashy are listed;
    // border-radius / box-shadow / opacity / box-sizing / text-shadow are
    // supported by KHTML 5.116 and must be kept.

    // CSS calc() -> approximate. Pure pixel arithmetic is evaluated;
    // percentage+pixel mixes drop the pixel term.
    {
        const QRegularExpression reCalc(QStringLiteral("calc\\(([^)]+)\\)"), QRegularExpression::CaseInsensitiveOption);
        auto it = reCalc.globalMatch(out);
        QString calcOut;
        int last = 0;
        while (it.hasNext()) {
            const QRegularExpressionMatch m = it.next();
            calcOut.append(out.midRef(last, m.capturedStart() - last));
            QString expr = m.captured(1).trimmed();
            const QRegularExpression rePct(QStringLiteral("([0-9.]+%)"));
            const QRegularExpressionMatch pm = rePct.match(expr);
            if (pm.hasMatch()) { calcOut.append(pm.captured(1)); }
            else {
                const QRegularExpression rePx(QStringLiteral("([0-9.]+)px"));
                QRegularExpressionMatchIterator pit = rePx.globalMatch(expr);
                double total = 0; bool ok = true; int pos = 0;
                while (pit.hasNext()) {
                    const QRegularExpressionMatch pxm = pit.next();
                    double val = pxm.captured(1).toDouble(&ok);
                    if (!ok) break;
                    int before = pxm.capturedStart() - 1;
                    while (before >= 0 && expr.at(before).isSpace()) --before;
                    if (before >= pos && expr.at(before) == QLatin1Char('-')) total -= val; else total += val;
                    pos = pxm.capturedEnd();
                }
                if (ok && pos > 0) calcOut.append(QString::number(total) + QStringLiteral("px"));
                else calcOut.append(m.captured(0));
            }
            last = m.capturedEnd();
        }
        calcOut.append(out.midRef(last));
        out = calcOut;
    }
    const QStringList props = {
        QStringLiteral("transform"), QStringLiteral("-webkit-transform"),
        QStringLiteral("-moz-transform"), QStringLiteral("-ms-transform"),
        QStringLiteral("backdrop-filter"), QStringLiteral("-webkit-backdrop-filter"),
        QStringLiteral("filter"), QStringLiteral("-webkit-filter"),
        QStringLiteral("animation"), QStringLiteral("animation-name"),
        QStringLiteral("animation-duration"), QStringLiteral("animation-timing-function"),
        QStringLiteral("animation-delay"), QStringLiteral("animation-iteration-count"),
        QStringLiteral("animation-direction"), QStringLiteral("animation-fill-mode"),
        QStringLiteral("animation-play-state"), QStringLiteral("-webkit-animation"),
        QStringLiteral("transition"), QStringLiteral("transition-property"),
        QStringLiteral("transition-duration"), QStringLiteral("transition-timing-function"),
        QStringLiteral("transition-delay"), QStringLiteral("-webkit-transition"),
        QStringLiteral("grid"), QStringLiteral("grid-template"),
        QStringLiteral("grid-template-columns"), QStringLiteral("grid-template-rows"),
        QStringLiteral("grid-template-areas"), QStringLiteral("grid-area"),
        QStringLiteral("grid-column"), QStringLiteral("grid-row"),
        QStringLiteral("grid-auto-flow"), QStringLiteral("grid-auto-columns"),
        QStringLiteral("grid-auto-rows"), QStringLiteral("place-items"),
        QStringLiteral("place-content"), QStringLiteral("place-self"),
        QStringLiteral("aspect-ratio"), QStringLiteral("clip-path"),
        QStringLiteral("-webkit-clip-path"), QStringLiteral("mask"),
        QStringLiteral("mask-image"), QStringLiteral("-webkit-mask"),
        QStringLiteral("-webkit-mask-image"), QStringLiteral("will-change"),
        QStringLiteral("contain"), QStringLiteral("scroll-behavior"),
        QStringLiteral("backface-visibility"), QStringLiteral("perspective"),
        QStringLiteral("transform-style"), QStringLiteral("object-fit"),
        QStringLiteral("object-position"), QStringLiteral("overflow-anchor"),
        QStringLiteral("overscroll-behavior"), QStringLiteral("touch-action"),
        QStringLiteral("scroll-snap-type"), QStringLiteral("scroll-snap-align"),
        QStringLiteral("scroll-margin"), QStringLiteral("scroll-padding"),
        QStringLiteral("inline-size"), QStringLiteral("min-inline-size"),
        QStringLiteral("max-inline-size"), QStringLiteral("block-size"),
        QStringLiteral("min-block-size"), QStringLiteral("max-block-size"),
        QStringLiteral("margin-inline"), QStringLiteral("padding-inline"),
        QStringLiteral("border-inline"), QStringLiteral("inset"),
        QStringLiteral("inset-block"), QStringLiteral("inset-inline"),
        QStringLiteral("text-decoration-thickness"), QStringLiteral("text-underline-offset"),
        QStringLiteral("text-underline-position"), QStringLiteral("font-variant-numeric"),
        QStringLiteral("font-feature-settings"), QStringLiteral("font-optical-sizing"),
        QStringLiteral("font-variation-settings"),
        QStringLiteral("text-wrap"), QStringLiteral("white-space-collapse"),
        QStringLiteral("box-decoration-break"),
        QStringLiteral("break-inside"), QStringLiteral("break-before"),
        QStringLiteral("break-after"), QStringLiteral("column-count"),
        QStringLiteral("column-gap"), QStringLiteral("column-rule"),
        QStringLiteral("column-width"), QStringLiteral("columns"),
        QStringLiteral("mix-blend-mode"), QStringLiteral("isolation"),
        QStringLiteral("shape-outside"), QStringLiteral("shape-margin"),
        QStringLiteral("accent-color"), QStringLiteral("color-scheme"),
        QStringLiteral("forced-color-adjust"),
        QStringLiteral("border-image"), QStringLiteral("border-image-source"),
        QStringLiteral("border-image-slice"), QStringLiteral("border-image-width"),
        QStringLiteral("border-image-outset"), QStringLiteral("border-image-repeat")
    };
    for (const QString &p : props) {
        const QRegularExpression re(
            QStringLiteral("(?:^|[;{}\\s])") + QRegularExpression::escape(p) +
            QStringLiteral("\\s*:[^;}]+[;}]?"),
            QRegularExpression::CaseInsensitiveOption);
        out.replace(re, QStringLiteral(";"));
    }

    // Remove @keyframes and @-webkit-keyframes blocks entirely.
    int pos = 0;
    while ((pos = out.indexOf(QRegularExpression(QStringLiteral("@-?[a-z]*keyframes")), pos)) >= 0)
        out = removeAtBlock(out, pos);

    // Remove @supports blocks (KHTML doesn't evaluate them; they may wrap
    // unsupported rules that crash the parser).
    pos = 0;
    while ((pos = out.indexOf(QStringLiteral("@supports"), pos, Qt::CaseInsensitive)) >= 0)
        out = removeAtBlock(out, pos);

    return out;
}

PageLoader::PageLoader(QObject *parent)
    : QObject(parent)
{
    m_nam.setRedirectPolicy(QNetworkRequest::NoLessSafeRedirectPolicy);
    m_timeout = new QTimer(this);
    m_timeout->setSingleShot(true);
    connect(m_timeout, &QTimer::timeout, this, &PageLoader::onTimeout);
}

QString PageLoader::cacheRoot()
{
    QString base = QStandardPaths::writableLocation(QStandardPaths::TempLocation)
                   + QStringLiteral("/khtml_cache");
    QDir().mkpath(base);
    // Sweep cache directories older than one hour so the temp folder does not
    // fill up after extended browsing. Each page gets its own subdirectory.
    const QDir d(base);
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    for (const QFileInfo &fi : d.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (now - fi.lastModified().toSecsSinceEpoch() > 3600)
            QDir(fi.absoluteFilePath()).removeRecursively();
    }
    return base;
}

QString PageLoader::hashName(const QUrl &u) const
{
    const QString s = u.toString(QUrl::FullyEncoded);
    const QByteArray h = QCryptographicHash::hash(s.toUtf8(), QCryptographicHash::Sha1).toHex();
    QString ext = u.path().section('.', -1).toLower();
    if (ext.isEmpty() || ext.length() > 5) ext = QStringLiteral("bin");
    // Keep css/js extensions so KHTML's MIME sniffing works.
    if (ext == QLatin1String("css") || ext == QLatin1String("js"))
        return QString::fromLatin1(h) + QLatin1Char('.') + ext;
    return QString::fromLatin1(h) + QLatin1Char('.') + ext;
}

void PageLoader::load(const QUrl &url)
{
    PERF_BEGIN(url.toString().toUtf8().constData());
    PERF_MARK("pageloader_load_start");
    m_original = url;
    m_finalUrl = QUrl();
    m_failed = false;
    m_timedOut = false;
    m_resources.clear();
    m_pendingMap.clear();
    m_cssRaw.clear();
    m_cssVars.clear();
    m_seenUrls.clear();
    m_pending = 0;
    m_criticalPending = 0;
    m_finished = false;
    m_timeout->start(15000);

    QNetworkRequest req(url);
    req.setRawHeader("User-Agent",
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
        "(KHTML, like Gecko) KHtml/5.116 Safari/537.36");
    QNetworkReply *r = m_nam.get(req);
    connect(r, &QNetworkReply::finished, this, &PageLoader::onMainFinished);
}

void PageLoader::onMainFinished()
{
    PERF_MARK("main_response_received");
    QNetworkReply *r = qobject_cast<QNetworkReply*>(sender());
    if (!r) return;
    if (r->error() != QNetworkReply::NoError) {
        fail(r->errorString());
        r->deleteLater();
        return;
    }
    m_mainHtml = r->readAll();
    PERF_MARK_D("main_html_downloaded", QString::number(m_mainHtml.size()));
    // Capture the URL after any HTTP redirects. Qt follows them automatically
    // (NoLessSafeRedirectPolicy), so r->url() is the final document URL.
    m_finalUrl = r->url();
    if (!m_finalUrl.isValid() || m_finalUrl.isEmpty())
        m_finalUrl = m_original;
    r->deleteLater();

    const QString id = QString::fromLatin1(
        QCryptographicHash::hash(m_original.toString().toUtf8(),
                                 QCryptographicHash::Sha1).toHex().left(16));
    m_cacheDir = cacheRoot() + QLatin1Char('/') + id;
    QDir().mkpath(m_cacheDir);

    PERF_MARK("extract_resources_start");
    extractResources();
    PERF_MARK_D("extract_resources_done", QString::number(m_resources.size()) + " resources");
    PERF_MARK("resource_downloads_start");
    startDownloads();
    maybeFinish(); // in case there are no resources
}

static bool isFetchable(const QUrl &u)
{
    if (!u.isValid()) return false;
    const QString s = u.scheme().toLower();
    return (s == QLatin1String("http") || s == QLatin1String("https"));
}

QString PageLoader::localAbs(const QString &localName) const
{
    return QUrl::fromLocalFile(m_cacheDir + QLatin1Char('/') + localName).toString();
}

void PageLoader::extractResources()
{
    const QString html = QString::fromUtf8(m_mainHtml);

    auto addRes = [this](const QUrl &u, const QString &raw) {
        if (!isFetchable(u)) return;
        const QString key = u.toString();
        if (m_seenUrls.contains(key)) return;
        // Hard cap on sub-resources so a heavy page cannot exhaust memory on
        // low-end devices. The first 150 are fetched; the rest are dropped
        // (the page still renders, just without some assets).
        if (m_resources.size() >= 150) return;
        m_seenUrls.insert(key);
        Res r{u, hashName(u), false};
        const QString path = u.path().toLower();
        if (path.endsWith(QLatin1String(".css")))
            r.isCss = true;
        if (path.endsWith(QLatin1String(".js")))
            r.isJs = true;
        // Skip font formats Qt cannot load. They would download, sit in the
        // cache, and then be ignored by the engine 鈥?pure waste.
        if (path.endsWith(QLatin1String(".woff"))
            || path.endsWith(QLatin1String(".woff2"))
            || path.endsWith(QLatin1String(".eot")))
            return;
        m_resources.append(r);
        // Keep the literal reference around so relative URLs in the page can
        // be patched too, not just absolute ones.
        const QString rt = raw.trimmed();
        if (!rt.isEmpty()
            && !rt.startsWith(QLatin1String("//"))
            && !rt.startsWith(QLatin1Char('#'))
            && !rt.startsWith(QLatin1String("data:"))
            && !rt.startsWith(QLatin1String("javascript:"))
            && !rt.startsWith(QLatin1String("mailto:")))
            m_rawToLocal.insert(rt, localAbs(r.localName));
    };

    // href="..." and src="..."
    const QRegularExpression reAttr(
        QStringLiteral("(?:href|src)\\s*=\\s*[\"']([^\"']+)[\"']"),
        QRegularExpression::CaseInsensitiveOption);
    auto it = reAttr.globalMatch(html);
    while (it.hasNext()) {
        const QString raw = it.next().captured(1).trimmed();
        if (raw.startsWith(QLatin1String("data:")) ||
            raw.startsWith(QLatin1String("javascript:")) ||
            raw.startsWith(QLatin1String("#")) ||
            raw.startsWith(QLatin1String("mailto:")))
            continue;
        addRes(m_finalUrl.resolved(QUrl(raw)), raw);
    }

    // srcset="a 1x, b 2x"
    const QRegularExpression reSrcset(
        QStringLiteral("srcset\\s*=\\s*[\"']([^\"']+)[\"']"),
        QRegularExpression::CaseInsensitiveOption);
    it = reSrcset.globalMatch(html);
    while (it.hasNext()) {
        const QStringList parts = it.next().captured(1).split(QLatin1Char(','));
        for (const QString &p : parts) {
            const QString raw = p.trimmed().section(QLatin1Char(' '), 0, 0);
            addRes(m_finalUrl.resolved(QUrl(raw)), raw);
        }
    }

    // inline style url(...)
    const QRegularExpression reUrl(
        QStringLiteral("url\\(\\s*[\"']?([^\"')]+)[\"']?\\s*\\)"),
        QRegularExpression::CaseInsensitiveOption);
    it = reUrl.globalMatch(html);
    while (it.hasNext()) {
        const QString raw = it.next().captured(1).trimmed();
        addRes(m_finalUrl.resolved(QUrl(raw)), raw);
    }
}

void PageLoader::startOne(const Res &res)
{
    QNetworkRequest req(res.url);
    req.setRawHeader("User-Agent",
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
        "(KHTML, like Gecko) KHtml/5.116 Safari/537.36");
    QNetworkReply *r = m_nam.get(req);
    m_pendingMap.insert(r, res);
    ++m_pending;
    if (res.isCss || res.isJs) ++m_criticalPending;
    connect(r, &QNetworkReply::finished, this, &PageLoader::onResourceFinished);
}

void PageLoader::startDownloads()
{
    for (const Res &res : m_resources)
        startOne(res);
}

void PageLoader::onResourceFinished()
{
    PERF_MARK("resource_finished");
    QNetworkReply *r = qobject_cast<QNetworkReply*>(sender());
    if (!r) return;
    Res res = m_pendingMap.value(r);
    m_pendingMap.remove(r);
    if (res.isCss || res.isJs) --m_criticalPending;

    if (r->error() == QNetworkReply::NoError) {
        const QByteArray data = r->readAll();
        // Reject oversized resources (8 MB cap). Huge images or video files
        // would otherwise balloon memory on low-end devices; the page simply
        // renders without that asset.
        if (data.size() > 8 * 1024 * 1024) {
            r->deleteLater();
            --m_pending;
            maybeFinish();
            return;
        }
        // Transpile external scripts before caching them: page JS is written in
        // ES2015+ but KJS only speaks ES3. The embedded Buble transformer lowers
        // the syntax in place; scripts that fail to transform are kept verbatim
        // so a single bad file never blanks the page.
        const QString resCt = QString::fromLatin1(r->header(QNetworkRequest::ContentTypeHeader).toByteArray()).toLower();
        const bool looksJs = res.url.path().toLower().endsWith(QLatin1String(".js"))
                             || resCt.contains(QLatin1String("javascript"));
        QByteArray outData;
        if (looksJs && data.size() < 4 * 1024 * 1024) {
            QString t = transpileJs(QString::fromUtf8(data));
            if (t.size() > 262144)
                t = QStringLiteral("/* skipped: external script too large */");
            else
                t = QStringLiteral("try{") + t + QStringLiteral("}catch(__kjs_e){}");
            outData = t.toUtf8();
        }
        QFile f(m_cacheDir + QLatin1Char('/') + res.localName);
        if (f.open(QIODevice::WriteOnly))
            f.write(outData.isEmpty() && !looksJs ? data : outData);

        // If this is a stylesheet, dig into it for background images, fonts
        // and @import sheets. Those references are relative to the CSS URL,
        // not the page URL.
        const QString ct = QString::fromLatin1(r->header(QNetworkRequest::ContentTypeHeader).toByteArray()).toLower();
        const bool looksCss = res.isCss || ct.contains(QLatin1String("css"))
                              || res.url.path().toLower().endsWith(QLatin1String(".css"));
        if (looksCss) {
            m_cssRaw.insert(res.localName, data);
            extractFromCss(data, res.url, res.localName);
        }
    }
    r->deleteLater();
    --m_pending;
    maybeFinish();
}

void PageLoader::extractFromCss(const QByteArray &css, const QUrl &baseUrl, const QString &localName)
{
    const QString text = QString::fromUtf8(css);

    // url(...) references 鈥?backgrounds, fonts, cursor images, etc.
    const QRegularExpression reUrl(
        QStringLiteral("url\\(\\s*[\"']?([^\"')]+)[\"']?\\s*\\)"),
        QRegularExpression::CaseInsensitiveOption);
    auto it = reUrl.globalMatch(text);
    while (it.hasNext()) {
        const QString raw = it.next().captured(1).trimmed();
        if (raw.startsWith(QLatin1String("data:"))) continue;
        QUrl u = baseUrl.resolved(QUrl(raw));
        if (!isFetchable(u)) continue;
        const QString key = u.toString();
        if (m_seenUrls.contains(key)) continue;
        m_seenUrls.insert(key);
        Res r{u, hashName(u), u.path().toLower().endsWith(QLatin1String(".css"))};
        m_resources.append(r);
        startOne(r);
        // Relative references inside this sheet resolve against the sheet URL;
        // record them per-sheet so the rewrite pass can patch them later.
        if (!raw.startsWith(QLatin1String("//")) && !raw.startsWith(QLatin1String("data:"))
            && !raw.startsWith(QLatin1Char('#')))
            m_cssRawToLocal[localName].insert(raw, localAbs(r.localName));
    }

    // @import "sheet.css" or @import url(sheet.css)
    const QRegularExpression reImport(
        QStringLiteral("@import\\s+(?:url\\(\\s*)?[\"']([^\"']+)[\"']"),
        QRegularExpression::CaseInsensitiveOption);
    it = reImport.globalMatch(text);
    while (it.hasNext()) {
        const QString raw = it.next().captured(1).trimmed();
        QUrl u = baseUrl.resolved(QUrl(raw));
        if (!isFetchable(u)) continue;
        const QString key = u.toString();
        if (m_seenUrls.contains(key)) continue;
        m_seenUrls.insert(key);
        Res r{u, hashName(u), true};
        m_resources.append(r);
        startOne(r);
        if (!raw.startsWith(QLatin1String("//")) && !raw.startsWith(QLatin1String("data:"))
            && !raw.startsWith(QLatin1Char('#')))
            m_cssRawToLocal[localName].insert(raw, localAbs(r.localName));
    }
}

void PageLoader::rewriteAndSave()
{
    PERF_MARK("rewrite_start");
    QString html = QString::fromUtf8(m_mainHtml);

    // Build the full replacement table: absolute URLs, protocol-relative URLs
    // and every literal relative reference seen in the page. Targets are
    // absolute file:// paths so the injected <base href> (which must point at
    // the original site for navigation) can never hijack resource loading.
    QStringList needles;
    QHash<QString, QString> repl;
    for (const Res &res : m_resources) {
        const QString la = localAbs(res.localName);
        repl.insert(res.url.toString(), la);
        repl.insert(QLatin1String("//") + res.url.host() + res.url.path(), la);
    }
    // Relative references are only protected against each other: a short name
    // ("a.png") must not be patched inside a longer one ("logo-a.png"), but an
    // absolute URL containing the same name is a legitimately separate match.
    {
        const QStringList raws = m_rawToLocal.keys();
        QSet<QString> subsumed;
        for (int i = 0; i < raws.size(); ++i)
            for (int j = 0; j < raws.size(); ++j)
                if (i != j && raws.at(i).size() < raws.at(j).size()
                    && raws.at(j).contains(raws.at(i)))
                    subsumed.insert(raws.at(i));
        for (auto it = m_rawToLocal.begin(); it != m_rawToLocal.end(); ++it)
            if (!subsumed.contains(it.key()))
                repl.insert(it.key(), it.value());
    }
    needles = repl.keys();
    std::sort(needles.begin(), needles.end(), [](const QString &a, const QString &b) {
        return a.size() > b.size();
    });
    for (const QString &n : needles)
        html.replace(n, repl.value(n));
    PERF_MARK("rewrite_urls_done");

    // Rewrite <base> so relative links resolve against the final (post-redirect) URL.
    const QRegularExpression reBase(
        QStringLiteral("<base[^>]*>"), QRegularExpression::CaseInsensitiveOption);
    html.replace(reBase, QStringLiteral("<base href=\"%1\">").arg(m_finalUrl.toString()));

    // Pre-collect custom property definitions from inline <style> blocks so
    // that external stylesheets (processed next) can resolve their var()
    // references against them. Many modern sites define --vars in inline
    // <style> and consume them in external .css files.
    {
        const QRegularExpression rePreStyle(
            QStringLiteral("<style[^>]*>(.*?)</style>"),
            QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption);
        const QRegularExpression rePreVarDef(
            QStringLiteral("(--[a-zA-Z0-9_-]+)\s*:\s*([^;!{}]+?)(?:\s*!important)?\s*[;}]"),
            QRegularExpression::CaseInsensitiveOption);
        auto pit = rePreStyle.globalMatch(html);
        while (pit.hasNext()) {
            const QRegularExpressionMatch pm = pit.next();
            const QString block = pm.captured(1);
            auto dit = rePreVarDef.globalMatch(block);
            while (dit.hasNext()) {
                const QRegularExpressionMatch dm = dit.next();
                m_cssVars.insert(dm.captured(1).toLower(), dm.captured(2).trimmed());
            }
        }
    }

    // Process inline <style> blocks: collect their custom properties, expand
    // var() references, and rewrite url() to local files. External stylesheets
    // were already handled by rewriteCssFiles(); we run it first so its
    // variable definitions are available to inline styles.
    PERF_MARK("rewrite_css_start");
    rewriteCssFiles();
    PERF_MARK("rewrite_css_done");

    const QRegularExpression reStyle(
        QStringLiteral("<style[^>]*>(.*?)</style>"),
        QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption);
    const QRegularExpression reVarDef(
        QStringLiteral("(--[a-zA-Z0-9_-]+)\\s*:\\s*([^;!{}]+?)(?:\\s*!important)?\\s*[;}]"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpression reVarRef(
        QStringLiteral("var\\(\\s*(--[a-zA-Z0-9_-]+)\\s*(?:,\\s*([^)]+))?\\)"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpression reUrl(
        QStringLiteral("(url\\(\\s*[\"']?)([^\"')]+)([\"']?\\s*\\))"));

    QHash<QString, QString> urlToLocal;
    for (const Res &res : m_resources) {
        const QString la = localAbs(res.localName);
        urlToLocal.insert(res.url.toString(), la);
        urlToLocal.insert(QLatin1String("//") + res.url.host() + res.url.path(), la);
    }
    for (auto it = m_rawToLocal.begin(); it != m_rawToLocal.end(); ++it)
        urlToLocal.insert(it.key(), it.value());

    // Process inline <style> blocks.
    QString htmlOut;
    htmlOut.reserve(html.size());
    int lastPos = 0;
    auto styleIt = reStyle.globalMatch(html);
    while (styleIt.hasNext()) {
        const QRegularExpressionMatch sm = styleIt.next();
        htmlOut.append(html.midRef(lastPos, sm.capturedStart() - lastPos));
        QString css = sm.captured(1);
        // Collect variable definitions from this block.
        auto dit = reVarDef.globalMatch(css);
        while (dit.hasNext()) {
            const QRegularExpressionMatch dm = dit.next();
            m_cssVars.insert(dm.captured(1).toLower(), dm.captured(2).trimmed());
        }
        // Expand var() references.
        QString out;
        out.reserve(css.size());
        int last = 0;
        auto vit = reVarRef.globalMatch(css);
        while (vit.hasNext()) {
            const QRegularExpressionMatch vm = vit.next();
            out.append(css.midRef(last, vm.capturedStart() - last));
            const QString name = vm.captured(1).toLower();
            if (m_cssVars.contains(name))
                out.append(m_cssVars.value(name));
            else if (vm.lastCapturedIndex() >= 2 && !vm.captured(2).isEmpty())
                out.append(vm.captured(2).trimmed());
            else
                out.append(vm.captured(0));
            last = vm.capturedEnd();
        }
        out.append(css.midRef(last));
        css = out;
        // Rewrite url() references.
        out.clear();
        last = 0;
        auto uit = reUrl.globalMatch(css);
        while (uit.hasNext()) {
            const QRegularExpressionMatch um = uit.next();
            out.append(css.midRef(last, um.capturedStart() - last));
            const QString raw = um.captured(2).trimmed();
            auto hit = urlToLocal.constFind(raw);
            if (hit != urlToLocal.constEnd())
                out.append(um.captured(1) + hit.value() + um.captured(3));
            else
                out.append(um.captured(0));
            last = um.capturedEnd();
        }
        out.append(css.midRef(last));
        // Rewrite @import "sheet.css" (the url() form was already patched by
        // the url() pass above) to the local sheet.
        {
            const QRegularExpression reImp(
                QStringLiteral("@import\\s+(?:url\\(\\s*)?[\"']([^\"']+)[\"']"),
                QRegularExpression::CaseInsensitiveOption);
            auto iit = reImp.globalMatch(out);
            QString io;
            int ilast = 0;
            while (iit.hasNext()) {
                const QRegularExpressionMatch im = iit.next();
                io.append(out.midRef(ilast, im.capturedStart() - ilast));
                auto hit = urlToLocal.constFind(im.captured(1).trimmed());
                if (hit != urlToLocal.constEnd())
                    io.append(QStringLiteral("@import url(\"%1\");").arg(hit.value()));
                else
                    io.append(im.captured(0));
                ilast = im.capturedEnd();
            }
            io.append(out.midRef(ilast));
            out = io;
        }
        out = stripUnsupportedFonts(out);
        out = simplifyGradients(out);
        out = simplifyUnsupportedCss(out);
        htmlOut.append(QStringLiteral("<style>") + out + QStringLiteral("</style>"));
        lastPos = sm.capturedEnd();
    }



    htmlOut.append(html.midRef(lastPos));
    // --- Inline SVG -> external .svg file + <img> ---
    // KHTML's HTML parser does not implement HTML5 inline SVG integration.
    // External SVG files load fine via <img>, so serialize each inline <svg>
    // to a cached .svg file and replace it with an <img> reference.
    {
        const QRegularExpression reSvg(
            QStringLiteral("<svg\b([^>]*)>(.*?)</svg>"),
            QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption);
        QString svgOut;
        svgOut.reserve(htmlOut.size());
        int svgLast = 0;
        int svgIdx = 0;
        auto svgIt = reSvg.globalMatch(htmlOut);
        while (svgIt.hasNext()) {
            const QRegularExpressionMatch sm = svgIt.next();
            svgOut.append(htmlOut.midRef(svgLast, sm.capturedStart() - svgLast));
            const QString attrs = sm.captured(1);
            const QString inner = sm.captured(2);
            const QString fullSvg = QStringLiteral("<svg xmlns=\"http://www.w3.org/2000/svg\"") + attrs + QStringLiteral(">") + inner + QStringLiteral("</svg>");
            const QString svgName = QStringLiteral("inline_svg_%1.svg").arg(svgIdx++);
            const QString svgPath = m_cacheDir + QLatin1Char('/') + svgName;
            {
                QFile sf(svgPath);
                if (sf.open(QIODevice::WriteOnly))
                    sf.write(fullSvg.toUtf8());
            }
            // Preserve width/height/class/id from the original <svg>
            QString w, h, cls, id;
            const QRegularExpression reW(QStringLiteral("width\\s*=\\s*[\x22\x27]([^\x22\x27]+)"), QRegularExpression::CaseInsensitiveOption);
            const QRegularExpression reH(QStringLiteral("height\\s*=\\s*[\x22\x27]([^\x22\x27]+)"), QRegularExpression::CaseInsensitiveOption);
            const QRegularExpression reC(QStringLiteral("class\\s*=\\s*[\x22\x27]([^\x22\x27]+)"), QRegularExpression::CaseInsensitiveOption);
            const QRegularExpression reI(QStringLiteral("id\\s*=\\s*[\x22\x27]([^\x22\x27]+)"), QRegularExpression::CaseInsensitiveOption);
            QRegularExpressionMatch mw = reW.match(attrs); if (mw.hasMatch()) w = mw.captured(1);
            QRegularExpressionMatch mh = reH.match(attrs); if (mh.hasMatch()) h = mh.captured(1);
            QRegularExpressionMatch mc = reC.match(attrs); if (mc.hasMatch()) cls = mc.captured(1);
            QRegularExpressionMatch mi = reI.match(attrs); if (mi.hasMatch()) id = mi.captured(1);
            QString img = QStringLiteral("<img src=\"") + QUrl::fromLocalFile(svgPath).toString() + QStringLiteral("\"");
            if (!w.isEmpty()) img += QStringLiteral(" width=\"") + w + QStringLiteral("\"");
            if (!h.isEmpty()) img += QStringLiteral(" height=\"") + h + QStringLiteral("\"");
            if (!cls.isEmpty()) img += QStringLiteral(" class=\"") + cls + QStringLiteral("\"");
            if (!id.isEmpty()) img += QStringLiteral(" id=\"") + id + QStringLiteral("\"");
            img += QStringLiteral(" alt=\"\">");
            svgOut.append(img);
            svgLast = sm.capturedEnd();
        }
        svgOut.append(htmlOut.midRef(svgLast));
        htmlOut = svgOut;
    }
    html = htmlOut;

    // Rewrite url(...) elsewhere in the page (style="..." attributes, anything
    // outside <style> blocks) using the same table.
    QString htmlTail;
    htmlTail.reserve(html.size());
    lastPos = 0;
    auto uit2 = reUrl.globalMatch(html);
    while (uit2.hasNext()) {
        const QRegularExpressionMatch um = uit2.next();
        htmlTail.append(html.midRef(lastPos, um.capturedStart() - lastPos));
        const QString raw = um.captured(2).trimmed();
        auto hit = urlToLocal.constFind(raw);
        if (hit != urlToLocal.constEnd())
            htmlTail.append(um.captured(1) + hit.value() + um.captured(3));
        else
            htmlTail.append(um.captured(0));
        lastPos = um.capturedEnd();
    }
    htmlTail.append(html.midRef(lastPos));
    html = htmlTail;

    // Lower ES2015+ syntax in inline <script> blocks (no src attribute).
    // JSON, templates and other data blocks are skipped so they stay intact.
    {
        const QRegularExpression reScript(
            QStringLiteral("<script([^>]*)>(.*?)</script>"),
            QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption);
        const QRegularExpression reSrc(
            QStringLiteral("\\bsrc\\s*="), QRegularExpression::CaseInsensitiveOption);
        const QRegularExpression reType(
            QStringLiteral("\\btype\\s*=\\s*[\"']?([^\"'\\s>]+)"),
            QRegularExpression::CaseInsensitiveOption);
        QString out;
        out.reserve(html.size());
        int lastPos = 0;
        auto sit = reScript.globalMatch(html);
        while (sit.hasNext()) {
            const QRegularExpressionMatch sm = sit.next();
            out.append(html.midRef(lastPos, sm.capturedStart() - lastPos));
            const QString attrs = sm.captured(1);
            const QString body = sm.captured(2);
            const bool hasSrc = reSrc.match(attrs).hasMatch();
            QString stype = reType.match(attrs).captured(1).toLower();
            const bool isJs = !hasSrc && (stype.isEmpty()
                || stype.contains(QLatin1String("javascript"))
                || stype == QLatin1String("module"))
                && !body.trimmed().isEmpty();
            if (isJs) {
                QString t = transpileJs(body);
                if (t.size() > 131072)
                    t = QStringLiteral("/* skipped: inline script too large */");
                else
                    t = QStringLiteral("try{") + t + QStringLiteral("}catch(__kjs_e){}");
                out.append(QStringLiteral("<script") + attrs + QStringLiteral(">")
                          + t + QStringLiteral("</script>"));
            } else {
                out.append(sm.captured(0));
            }
            lastPos = sm.capturedEnd();
        }
        out.append(html.midRef(lastPos));
        html = out;
    }

    // Inject the ES5+ polyfill bundle before any page script so modern APIs
    // (Promise, fetch, Array.from, etc.) are available when site code runs.
    // Global error guard: prevents an uncaught JS exception from tearing
    // down subsequent script blocks on the page.
    const QString kErrorGuard = QStringLiteral(
        "<script>window.onerror=function(){return true;};"
        "if(window.addEventListener){window.addEventListener('error',function(e){e.preventDefault&&e.preventDefault();return true;},true);}"
        "</script>");
    const QString polyfill = kErrorGuard + QStringLiteral("<script>") + QString::fromLatin1(kJsPolyfill) + QStringLiteral("</script>");
    int headPos = html.indexOf(QRegularExpression(QStringLiteral("<head[^>]*>"), QRegularExpression::CaseInsensitiveOption), 0);
    if (headPos >= 0) {
        int headEnd = html.indexOf(QLatin1Char('>'), headPos) + 1;
        html.insert(headEnd, polyfill);
    } else {
        int bodyPos = html.indexOf(QRegularExpression(QStringLiteral("<body[^>]*>"), QRegularExpression::CaseInsensitiveOption), 0);
        if (bodyPos >= 0) {
            int bodyEnd = html.indexOf(QLatin1Char('>'), bodyPos) + 1;
            html.insert(bodyEnd, polyfill);
        } else {
            html.prepend(polyfill);
        }
    }

    // Minimal compatibility CSS: modern sites often hide core UI behind
    // JS-controlled classes. Keep form inputs visible and usable.
    {
        QString kCompat = QStringLiteral(
            "<style>"
            ".sbox{display:block;text-align:center;margin:40px 0}"
            "#sb_form{display:inline-block}"
            "#sb_form_q{display:inline-block;width:420px;height:38px;font-size:16px;"
            "padding:4px 10px;border:1px solid #999;border-radius:4px;background:#fff;color:#000}"
            "#sb_form_go{display:inline-block;width:54px;height:38px;background:#0078d4;"
            "color:#fff;border:0;cursor:pointer;margin-left:4px}"
            "input[type=search]{display:inline-block}"
            "</style>");
        int he = html.indexOf(QRegularExpression(QStringLiteral("</head>"),
            QRegularExpression::CaseInsensitiveOption));
        if (he >= 0) html.insert(he, kCompat);
        else html.prepend(kCompat);
    }

    // Collect @keyframes from cached stylesheets + inline <style> blocks,
    // then inject the CSS animation polyfill (driven by requestAnimationFrame).
    QString allKeyframes = QStringLiteral("{");
    bool firstKf = true;
    for (auto it = m_cssRaw.begin(); it != m_cssRaw.end(); ++it) {
        const QString kfJs = extractKeyframesJs(QString::fromUtf8(it.value()));
        if (kfJs != QStringLiteral("{}")) {
            if (!firstKf) allKeyframes += QStringLiteral(",");
            allKeyframes += kfJs.mid(1, kfJs.length() - 2);
            firstKf = false;
        }
    }
    const QRegularExpression reStyleKf(QStringLiteral("<style[^>]*>(.*?)</style>"),
        QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption);
    auto sit = reStyleKf.globalMatch(html);
    while (sit.hasNext()) {
        const QRegularExpressionMatch sm = sit.next();
        const QString kfJs = extractKeyframesJs(sm.captured(1));
        if (kfJs != QStringLiteral("{}")) {
            if (!firstKf) allKeyframes += QStringLiteral(",");
            allKeyframes += kfJs.mid(1, kfJs.length() - 2);
            firstKf = false;
        }
    }
    allKeyframes += QStringLiteral("}");
    // Only inject when there is actual keyframe data AND it is small enough
    // for KJS. Empty "{}" means no animations were found — skip injection
    // entirely so the polyfill does not scan getComputedStyle on every element.
    if (allKeyframes.size() > 3 && allKeyframes.size() < 65536) {
        const QString kfScript = QStringLiteral("<script>window.__cssKeyframes=") + allKeyframes +
            QStringLiteral(";</script>\n<script>") + QString::fromLatin1(kCssAnimPolyfill) + QStringLiteral("</script>");
        int kfInsert = html.indexOf(QStringLiteral("</script>"), headPos >= 0 ? headPos : 0);
        if (kfInsert >= 0) {
            html.insert(kfInsert + 9, kfScript);
        } else {
            html.append(kfScript);
        }
    }




    QFile f(m_cacheDir + QStringLiteral("/index.html"));
    if (f.open(QIODevice::WriteOnly)) {
        QTextStream ts(&f);
        ts.setCodec("UTF-8");
        ts << html;
    }
}

void PageLoader::rewriteCssFiles()
{
    // --- Pass 1: collect custom property (--name: value) definitions ---
    // m_cssVars already pre-collected from inline <style>; do not clear.
    const QRegularExpression reVarDef(
        QStringLiteral("(--[a-zA-Z0-9_-]+)\\s*:\\s*([^;!{}]+?)(?:\\s*!important)?\\s*[;}]"),
        QRegularExpression::CaseInsensitiveOption);
    for (auto it = m_cssRaw.begin(); it != m_cssRaw.end(); ++it) {
        const QString css = QString::fromUtf8(it.value());
        auto mit = reVarDef.globalMatch(css);
        while (mit.hasNext()) {
            const QRegularExpressionMatch m = mit.next();
            m_cssVars.insert(m.captured(1).toLower(), m.captured(2).trimmed());
        }
    }

    // Resolve variables that reference other variables (up to 8 deep to avoid
    // infinite loops from circular references).
    const QRegularExpression reVarRef(
        QStringLiteral("var\\(\\s*(--[a-zA-Z0-9_-]+)\\s*(?:,\\s*([^)]+))?\\)"),
        QRegularExpression::CaseInsensitiveOption);
    for (int depth = 0; depth < 8; ++depth) {
        bool changed = false;
        for (auto it = m_cssVars.begin(); it != m_cssVars.end(); ++it) {
            QString val = it.value();
            QString out;
            out.reserve(val.size());
            int last = 0;
            auto mit = reVarRef.globalMatch(val);
            while (mit.hasNext()) {
                const QRegularExpressionMatch m = mit.next();
                out.append(val.midRef(last, m.capturedStart() - last));
                const QString name = m.captured(1).toLower();
                if (m_cssVars.contains(name)) {
                    out.append(m_cssVars.value(name));
                    changed = true;
                } else if (m.lastCapturedIndex() >= 2 && !m.captured(2).isEmpty()) {
                    out.append(m.captured(2).trimmed());
                } else {
                    out.append(QStringLiteral("inherit"));
                }
                last = m.capturedEnd();
            }
            out.append(val.midRef(last));
            it.value() = out;
        }
        if (!changed) break;
    }

    // --- Pass 2: build URL rewrite map ---
    QHash<QString, QString> baseUrlToLocal;
    for (const Res &res : m_resources) {
        const QString la = localAbs(res.localName);
        baseUrlToLocal.insert(res.url.toString(), la);
        const QString protoRel = QLatin1String("//") + res.url.host() + res.url.path();
        baseUrlToLocal.insert(protoRel, la);
    }
    for (auto it = m_rawToLocal.begin(); it != m_rawToLocal.end(); ++it)
        baseUrlToLocal.insert(it.key(), it.value());

    // --- Pass 3: rewrite each stylesheet (var expansion + url rewrite) ---
    const QRegularExpression reUrl(
        QStringLiteral("(url\\(\\s*[\"']?)([^\"')]+)([\"']?\\s*\\))"));
    const QRegularExpression reImp(
        QStringLiteral("@import\\s+(?:url\\(\\s*)?[\"']([^\"']+)[\"']"),
        QRegularExpression::CaseInsensitiveOption);

    for (auto it = m_cssRaw.begin(); it != m_cssRaw.end(); ++it) {
        QString css = QString::fromUtf8(it.value());
        // This sheet's own relative references resolve against the sheet URL,
        // not the page URL; merge them in for this sheet only.
        QHash<QString, QString> urlToLocal = baseUrlToLocal;
        const QHash<QString, QString> sheetMap = m_cssRawToLocal.value(it.key());
        for (auto sit = sheetMap.begin(); sit != sheetMap.end(); ++sit)
            urlToLocal.insert(sit.key(), sit.value());

        // Expand var() references in this stylesheet.
        {
            QString out;
            out.reserve(css.size());
            int last = 0;
            auto mit = reVarRef.globalMatch(css);
            while (mit.hasNext()) {
                const QRegularExpressionMatch m = mit.next();
                out.append(css.midRef(last, m.capturedStart() - last));
                const QString name = m.captured(1).toLower();
                if (m_cssVars.contains(name))
                    out.append(m_cssVars.value(name));
                else if (m.lastCapturedIndex() >= 2 && !m.captured(2).isEmpty())
                    out.append(m.captured(2).trimmed());
                else
                    out.append(m.captured(0)); // leave unresolved var() as-is
                last = m.capturedEnd();
            }
            out.append(css.midRef(last));
            css = out;
        }

        // Rewrite url() references to local files.
        QString out;
        out.reserve(css.size());
        int last = 0;
        auto mit = reUrl.globalMatch(css);
        while (mit.hasNext()) {
            const QRegularExpressionMatch m = mit.next();
            out.append(css.midRef(last, m.capturedStart() - last));
            const QString raw = m.captured(2).trimmed();
            auto hit = urlToLocal.constFind(raw);
            if (hit != urlToLocal.constEnd())
                out.append(m.captured(1) + hit.value() + m.captured(3));
            else
                out.append(m.captured(0));
            last = m.capturedEnd();
        }
        out.append(css.midRef(last));

        // Rewrite @import statements to the local sheet.
        {
            auto iit = reImp.globalMatch(out);
            QString io;
            int ilast = 0;
            while (iit.hasNext()) {
                const QRegularExpressionMatch im = iit.next();
                io.append(out.midRef(ilast, im.capturedStart() - ilast));
                auto hit = urlToLocal.constFind(im.captured(1).trimmed());
                if (hit != urlToLocal.constEnd())
                    io.append(QStringLiteral("@import url(\"%1\");").arg(hit.value()));
                else
                    io.append(im.captured(0));
                ilast = im.capturedEnd();
            }
            io.append(out.midRef(ilast));
            out = io;
        }

        // Drop @font-face rules that only reference unloadable formats.
        out = stripUnsupportedFonts(out);
        // Collapse CSS gradients to flat colors (KHTML has no gradient painter).
        out = simplifyGradients(out);
        // Drop properties KHTML cannot parse (sticky/transform/filter).
        out = simplifyUnsupportedCss(out);

        QFile f(m_cacheDir + QLatin1Char('/') + it.key());
        if (f.open(QIODevice::WriteOnly)) {
            QTextStream ts(&f);
            ts.setCodec("UTF-8");
            ts << out;
        }
    }
}

void PageLoader::maybeFinish()
{
    PERF_MARK("maybe_finish_check");
    if (m_failed || m_finished) return;
    if (m_criticalPending > 0 && !m_timedOut) return;
    m_finished = true;
    m_timeout->stop();
    rewriteAndSave();
    const QUrl local = QUrl::fromLocalFile(m_cacheDir + QStringLiteral("/index.html"));
    emit finished(local, m_finalUrl, QString());
}

void PageLoader::onTimeout()
{
    m_timedOut = true;
    // Copy keys first: abort() triggers finished() which removes from
    // m_pendingMap, invalidating iterators if we walk the map directly.
    const QList<QNetworkReply *> replies = m_pendingMap.keys();
    for (QNetworkReply *r : replies)
        r->abort();
    maybeFinish();
}

void PageLoader::fail(const QString &msg)
{
    if (m_failed) return;
    m_failed = true;
    emit finished(QUrl(), m_original, msg);
}
