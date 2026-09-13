#include "kjs_webgl.h"
#include "kjs_context2d.h"
#include <imload/canvasimage.h>
#include <kjs/operations.h>
#include <kjs/value.h>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QOpenGLFramebufferObject>
#include <QOffscreenSurface>
#include <QSurfaceFormat>
#include <QImage>
#include <QPainter>
#include <QHash>
#include <QByteArray>

#include "kjs_webgl.lut.h"

namespace KJS {

class WebGLContext::Private {
public:
    QOpenGLContext *ctx = nullptr;
    QOffscreenSurface *surface = nullptr;
    QOpenGLFramebufferObject *fbo = nullptr;
    QOpenGLFunctions *gl = nullptr;
    int width = 0, height = 0;
    bool valid = false;
    GLuint currentProgram = 0;
    GLuint arrayBuffer = 0, elementBuffer = 0;
    ~Private() {
        if (fbo) delete fbo;
        if (ctx) { ctx->doneCurrent(); delete ctx; }
        if (surface) delete surface;
    }
};

KJS_DEFINE_PROTOTYPE(WebGLProto)
KJS_IMPLEMENT_PROTOFUNC(WebGLFunction)
KJS_IMPLEMENT_PROTOTYPE("WebGLRenderingContextProto", WebGLProto, WebGLFunction, ObjectPrototype)

/*
   @begin WebGLProtoTable 72
   clear                    WebGLContext::Clear                     DontDelete|Function 1
   clearColor               WebGLContext::ClearColor                DontDelete|Function 4
   clearDepth               WebGLContext::ClearDepth                DontDelete|Function 1
   clearStencil             WebGLContext::ClearStencil              DontDelete|Function 1
   viewport                 WebGLContext::Viewport                  DontDelete|Function 4
   enable                   WebGLContext::Enable                    DontDelete|Function 1
   disable                  WebGLContext::Disable                   DontDelete|Function 1
   createShader             WebGLContext::CreateShader              DontDelete|Function 1
   shaderSource             WebGLContext::ShaderSource              DontDelete|Function 2
   compileShader            WebGLContext::CompileShader             DontDelete|Function 1
   getShaderParameter       WebGLContext::GetShaderParameter        DontDelete|Function 2
   getShaderInfoLog         WebGLContext::GetShaderInfoLog          DontDelete|Function 1
   createProgram            WebGLContext::CreateProgram             DontDelete|Function 0
   attachShader             WebGLContext::AttachShader              DontDelete|Function 2
   linkProgram              WebGLContext::LinkProgram               DontDelete|Function 1
   getProgramParameter      WebGLContext::GetProgramParameter       DontDelete|Function 2
   getProgramInfoLog        WebGLContext::GetProgramInfoLog         DontDelete|Function 1
   useProgram               WebGLContext::UseProgram                DontDelete|Function 1
   createBuffer             WebGLContext::CreateBuffer              DontDelete|Function 0
   bindBuffer               WebGLContext::BindBuffer                DontDelete|Function 2
   bufferData               WebGLContext::BufferData                DontDelete|Function 3
   bufferSubData            WebGLContext::BufferSubData             DontDelete|Function 3
   enableVertexAttribArray  WebGLContext::EnableVertexAttribArray   DontDelete|Function 1
   disableVertexAttribArray WebGLContext::DisableVertexAttribArray  DontDelete|Function 1
   vertexAttribPointer      WebGLContext::VertexAttribPointer       DontDelete|Function 6
   drawArrays               WebGLContext::DrawArrays                DontDelete|Function 3
   drawElements             WebGLContext::DrawElements              DontDelete|Function 4
   getAttribLocation        WebGLContext::GetAttribLocation         DontDelete|Function 2
   getUniformLocation       WebGLContext::GetUniformLocation        DontDelete|Function 2
   uniform1f                WebGLContext::Uniform1f                 DontDelete|Function 2
   uniform2f                WebGLContext::Uniform2f                 DontDelete|Function 3
   uniform3f                WebGLContext::Uniform3f                 DontDelete|Function 4
   uniform4f                WebGLContext::Uniform4f                 DontDelete|Function 5
   uniform1i                WebGLContext::Uniform1i                 DontDelete|Function 2
   uniform2i                WebGLContext::Uniform2i                 DontDelete|Function 3
   uniform3i                WebGLContext::Uniform3i                 DontDelete|Function 4
   uniform4i                WebGLContext::Uniform4i                 DontDelete|Function 5
   uniformMatrix2fv         WebGLContext::UniformMatrix2fv          DontDelete|Function 3
   uniformMatrix3fv         WebGLContext::UniformMatrix3fv          DontDelete|Function 3
   uniformMatrix4fv         WebGLContext::UniformMatrix4fv          DontDelete|Function 3
   createTexture            WebGLContext::CreateTexture             DontDelete|Function 0
   bindTexture              WebGLContext::BindTexture               DontDelete|Function 2
   texParameteri            WebGLContext::TexParameteri             DontDelete|Function 3
   texParameterf            WebGLContext::TexParameterf             DontDelete|Function 3
   texImage2D               WebGLContext::TexImage2D                DontDelete|Function 9
   activeTexture            WebGLContext::ActiveTexture             DontDelete|Function 1
   pixelStorei              WebGLContext::PixelStorei               DontDelete|Function 2
   finish                   WebGLContext::Finish                    DontDelete|Function 0
   flush                    WebGLContext::Flush                     DontDelete|Function 0
   getError                 WebGLContext::GetError                  DontDelete|Function 0
   getParameter             WebGLContext::GetParameter              DontDelete|Function 1
   isEnabled                WebGLContext::IsEnabled                 DontDelete|Function 1
   blendFunc                WebGLContext::BlendFunc                 DontDelete|Function 2
   blendColor               WebGLContext::BlendColor                DontDelete|Function 4
   blendEquation            WebGLContext::BlendEquation             DontDelete|Function 1
   cullFace                 WebGLContext::CullFace                  DontDelete|Function 1
   frontFace                WebGLContext::FrontFace                 DontDelete|Function 1
   depthFunc                WebGLContext::DepthFunc                 DontDelete|Function 1
   depthMask                WebGLContext::DepthMask                 DontDelete|Function 1
   depthRange               WebGLContext::DepthRange                DontDelete|Function 2
   scissor                  WebGLContext::Scissor                   DontDelete|Function 4
   lineWidth                WebGLContext::LineWidth                 DontDelete|Function 1
   polygonOffset            WebGLContext::PolygonOffset             DontDelete|Function 2
   deleteShader             WebGLContext::DeleteShader              DontDelete|Function 1
   deleteProgram            WebGLContext::DeleteProgram             DontDelete|Function 1
   deleteBuffer             WebGLContext::DeleteBuffer              DontDelete|Function 1
   deleteTexture            WebGLContext::DeleteTexture             DontDelete|Function 1
   detachShader             WebGLContext::DetachShader              DontDelete|Function 2
   bindAttribLocation       WebGLContext::BindAttribLocation        DontDelete|Function 3
   getSupportedExtensions   WebGLContext::GetSupportedExtensions    DontDelete|Function 0
   getExtension             WebGLContext::GetExtension              DontDelete|Function 1
   readPixels               WebGLContext::ReadPixels                DontDelete|Function 7
   @end
*/

const ClassInfo WebGLContext::info = { "WebGLRenderingContext", nullptr, nullptr, nullptr };

JSValue *WebGLFunction::callAsFunction(ExecState *exec, JSObject *thisObj, const List &args)
{
    if (!thisObj || !thisObj->inherits(&WebGLContext::info))
        return throwError(exec, TypeError);
    return static_cast<WebGLContext*>(thisObj)->callMethod(exec, id, args);
}

WebGLContext::WebGLContext(ExecState *exec, DOM::HTMLCanvasElementImpl *canvas)
    : DOMObject(WebGLProto::self(exec)), m_canvas(canvas), d(new Private)
{
    d->width = canvas->width();
    d->height = canvas->height();
    if (d->width <= 0) d->width = 300;
    if (d->height <= 0) d->height = 150;
    d->valid = ensureGL();
}

WebGLContext::~WebGLContext() { delete d; }

bool WebGLContext::ensureGL()
{
    if (d->ctx) return true;
    QSurfaceFormat fmt;
    fmt.setRenderableType(QSurfaceFormat::OpenGLES);
    fmt.setMajorVersion(2);
    fmt.setMinorVersion(0);
    fmt.setDepthBufferSize(16);
    fmt.setStencilBufferSize(8);
    d->ctx = new QOpenGLContext;
    d->ctx->setFormat(fmt);
    if (!d->ctx->create()) {
        fmt.setRenderableType(QSurfaceFormat::OpenGL);
        d->ctx->setFormat(fmt);
        if (!d->ctx->create()) return false;
    }
    d->surface = new QOffscreenSurface;
    d->surface->setFormat(d->ctx->format());
    d->surface->create();
    if (!d->ctx->makeCurrent(d->surface)) return false;
    d->gl = d->ctx->functions();
    d->fbo = new QOpenGLFramebufferObject(d->width, d->height);
    if (!d->fbo->isValid()) return false;
    d->fbo->bind();
    d->gl->glViewport(0, 0, d->width, d->height);
    d->gl->glClearColor(0, 0, 0, 0);
    d->gl->glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    return true;
}

void WebGLContext::readPixelsToCanvas()
{
    if (!d->valid || !d->fbo) return;
    d->ctx->makeCurrent(d->surface);
    d->fbo->bind();
    QImage img(d->width, d->height, QImage::Format_ARGB32);
    d->gl->glReadPixels(0, 0, d->width, d->height, GL_RGBA, GL_UNSIGNED_BYTE, img.bits());
    img = img.mirrored(false, true);
    DOM::CanvasContext2DImpl *c2d = m_canvas->getContext2D();
    if (c2d && c2d->canvasImage) {
        QImage *dst = c2d->canvasImage->qimage();
        if (dst) {
            QPainter p(dst);
            p.drawImage(0, 0, img);
        }
    }
}

static const struct { const char *name; GLenum value; } kConsts[] = {
    {"VERTEX_SHADER", 35633}, {"FRAGMENT_SHADER", 35632},
    {"COMPILE_STATUS", 35713}, {"LINK_STATUS", 35714}, {"INFO_LOG_LENGTH", 35716},
    {"ARRAY_BUFFER", 34962}, {"ELEMENT_ARRAY_BUFFER", 34963},
    {"STATIC_DRAW", 35044}, {"DYNAMIC_DRAW", 35048}, {"STREAM_DRAW", 35040},
    {"FLOAT", 5126}, {"UNSIGNED_SHORT", 5123}, {"UNSIGNED_BYTE", 5121}, {"SHORT", 5122}, {"INT", 5124},
    {"TRIANGLES", 4}, {"TRIANGLE_STRIP", 5}, {"TRIANGLE_FAN", 6},
    {"LINES", 1}, {"LINE_STRIP", 3}, {"LINE_LOOP", 2}, {"POINTS", 0},
    {"TEXTURE_2D", 3553},
    {"TEXTURE0", 33984}, {"TEXTURE1", 33985}, {"TEXTURE2", 33986}, {"TEXTURE3", 33987},
    {"RGBA", 6408}, {"RGB", 6407}, {"ALPHA", 6406}, {"LUMINANCE", 6409},
    {"NEAREST", 9728}, {"LINEAR", 9729},
    {"TEXTURE_MAG_FILTER", 10240}, {"TEXTURE_MIN_FILTER", 10241},
    {"TEXTURE_WRAP_S", 10242}, {"TEXTURE_WRAP_T", 10243},
    {"CLAMP_TO_EDGE", 33071}, {"REPEAT", 10497},
    {"COLOR_BUFFER_BIT", 16384}, {"DEPTH_BUFFER_BIT", 256}, {"STENCIL_BUFFER_BIT", 1024},
    {"DEPTH_TEST", 2929}, {"BLEND", 3042}, {"CULL_FACE", 2884}, {"SCISSOR_TEST", 3089},
    {"FRONT", 1028}, {"BACK", 1029}, {"FRONT_AND_BACK", 1032},
    {"CW", 2304}, {"CCW", 2305},
    {"ONE", 1}, {"ZERO", 0}, {"SRC_ALPHA", 770}, {"ONE_MINUS_SRC_ALPHA", 771},
    {"DST_ALPHA", 772}, {"ONE_MINUS_DST_ALPHA", 773},
    {"NO_ERROR", 0}, {"INVALID_ENUM", 1280}, {"INVALID_VALUE", 1281},
    {"INVALID_OPERATION", 1282}, {"OUT_OF_MEMORY", 1285},
    {"MAX_TEXTURE_SIZE", 3379}, {"MAX_VIEWPORT_DIMS", 3386},
    {"MAX_VERTEX_ATTRIBS", 34921}, {"MAX_TEXTURE_IMAGE_UNITS", 34930},
    {"MAX_VERTEX_TEXTURE_IMAGE_UNITS", 35660}, {"MAX_COMBINED_TEXTURE_IMAGE_UNITS", 35661},
    {"CURRENT_PROGRAM", 35725}, {"ARRAY_BUFFER_BINDING", 34964},
    {"ELEMENT_ARRAY_BUFFER_BINDING", 34965}, {"TEXTURE_BINDING_2D", 32873},
    {"UNPACK_FLIP_Y_WEBGL", 37440}, {"UNPACK_PREMULTIPLY_ALPHA_WEBGL", 37441},
    {"BROWSER_DEFAULT_WEBGL", 37444},
    {"RED_BITS", 3410}, {"GREEN_BITS", 3411}, {"BLUE_BITS", 3412}, {"ALPHA_BITS", 3413},
    {"DEPTH_BITS", 3414}, {"STENCIL_BITS", 3415},
    {"SAMPLE_BUFFERS", 32936}, {"SAMPLES", 32937},
    {"MAX_RENDERBUFFER_SIZE", 34024},
    {nullptr, 0}
};

bool WebGLContext::getOwnPropertySlot(ExecState *exec, const Identifier &propertyName, PropertySlot &slot)
{
    for (int i = 0; kConsts[i].name; i++) {
        if (propertyName == kConsts[i].name) {
            slot.setValue(this, jsNumber(kConsts[i].value));
            return true;
        }
    }
    return DOMObject::getOwnPropertySlot(exec, propertyName, slot);
}

JSValue *WebGLContext::getValueProperty(ExecState *, int token) const
{
    return jsNumber(token);
}

static QByteArray extractArrayData(ExecState *exec, JSValue *val, int &elemSize)
{
    elemSize = 4;
    if (!val || val->type() != ObjectType) return QByteArray();
    JSObject *obj = val->getObject();
    int len = obj->get(exec, "length")->toInteger(exec);
    if (len <= 0 || len > 10000000) return QByteArray();
    JSValue *bpe = obj->get(exec, "BYTES_PER_ELEMENT");
    if (bpe && bpe->type() == NumberType) elemSize = bpe->toInteger(exec);
    QByteArray data(len * elemSize, 0);
    if (elemSize == 4) {
        float *fp = reinterpret_cast<float*>(data.data());
        for (int i = 0; i < len; i++) fp[i] = obj->get(exec, Identifier::from(i))->toNumber(exec);
    } else if (elemSize == 2) {
        unsigned short *sp = reinterpret_cast<unsigned short*>(data.data());
        for (int i = 0; i < len; i++) sp[i] = (unsigned short)obj->get(exec, Identifier::from(i))->toInteger(exec);
    } else if (elemSize == 1) {
        unsigned char *bp = reinterpret_cast<unsigned char*>(data.data());
        for (int i = 0; i < len; i++) bp[i] = (unsigned char)obj->get(exec, Identifier::from(i))->toInteger(exec);
    }
    return data;
}

JSValue *WebGLContext::callMethod(ExecState *exec, int id, const List &args)
{
    if (!d->valid) return jsNull();
    d->ctx->makeCurrent(d->surface);
    if (d->fbo) d->fbo->bind();
    QOpenGLFunctions *gl = d->gl;

    switch (id) {
    case Clear: gl->glClear(args[0]->toInteger(exec)); readPixelsToCanvas(); return jsUndefined();
    case ClearColor: gl->glClearColor(args[0]->toNumber(exec), args[1]->toNumber(exec), args[2]->toNumber(exec), args[3]->toNumber(exec)); return jsUndefined();
    case ClearDepth: gl->glClearDepthf(args[0]->toNumber(exec)); return jsUndefined();
    case ClearStencil: gl->glClearStencil(args[0]->toInteger(exec)); return jsUndefined();
    case Viewport: gl->glViewport(args[0]->toInteger(exec), args[1]->toInteger(exec), args[2]->toInteger(exec), args[3]->toInteger(exec)); return jsUndefined();
    case Enable: gl->glEnable(args[0]->toInteger(exec)); return jsUndefined();
    case Disable: gl->glDisable(args[0]->toInteger(exec)); return jsUndefined();
    case CreateShader: return jsNumber(gl->glCreateShader(args[0]->toInteger(exec)));
    case ShaderSource: {
        GLuint s = args[0]->toInteger(exec);
        QByteArray src = args[1]->toString(exec).qstring().toUtf8();
        const char *cs = src.constData();
        gl->glShaderSource(s, 1, &cs, nullptr);
        return jsUndefined();
    }
    case CompileShader: gl->glCompileShader(args[0]->toInteger(exec)); return jsUndefined();
    case GetShaderParameter: { GLint v=0; gl->glGetShaderiv(args[0]->toInteger(exec), args[1]->toInteger(exec), &v); return jsBoolean(v!=0); }
    case GetShaderInfoLog: {
        GLuint s = args[0]->toInteger(exec); GLint len=0; gl->glGetShaderiv(s, GL_INFO_LOG_LENGTH, &len);
        if (len<=0) return jsString("");
        QByteArray log(len,0); gl->glGetShaderInfoLog(s, len, nullptr, log.data());
        return jsString(QString::fromUtf8(log.constData()));
    }
    case CreateProgram: return jsNumber(gl->glCreateProgram());
    case AttachShader: gl->glAttachShader(args[0]->toInteger(exec), args[1]->toInteger(exec)); return jsUndefined();
    case DetachShader: gl->glDetachShader(args[0]->toInteger(exec), args[1]->toInteger(exec)); return jsUndefined();
    case LinkProgram: gl->glLinkProgram(args[0]->toInteger(exec)); return jsUndefined();
    case GetProgramParameter: { GLint v=0; gl->glGetProgramiv(args[0]->toInteger(exec), args[1]->toInteger(exec), &v); return jsBoolean(v!=0); }
    case GetProgramInfoLog: {
        GLuint p = args[0]->toInteger(exec); GLint len=0; gl->glGetProgramiv(p, GL_INFO_LOG_LENGTH, &len);
        if (len<=0) return jsString("");
        QByteArray log(len,0); gl->glGetProgramInfoLog(p, len, nullptr, log.data());
        return jsString(QString::fromUtf8(log.constData()));
    }
    case UseProgram: d->currentProgram = args[0]->toInteger(exec); gl->glUseProgram(d->currentProgram); return jsUndefined();
    case CreateBuffer: { GLuint b=0; gl->glGenBuffers(1,&b); return jsNumber(b); }
    case DeleteBuffer: { GLuint b=args[0]->toInteger(exec); gl->glDeleteBuffers(1,&b); return jsUndefined(); }
    case BindBuffer: {
        GLenum t = args[0]->toInteger(exec); GLuint b = args[1]->toInteger(exec);
        if (t==GL_ARRAY_BUFFER) d->arrayBuffer=b; else if (t==GL_ELEMENT_ARRAY_BUFFER) d->elementBuffer=b;
        gl->glBindBuffer(t,b); return jsUndefined();
    }
    case BufferData: {
        GLenum t = args[0]->toInteger(exec);
        GLenum usage = args.size()>2 ? args[2]->toInteger(exec) : GL_STATIC_DRAW;
        if (args[1]->type() == NumberType) {
            gl->glBufferData(t, args[1]->toInteger(exec), nullptr, usage);
        } else {
            int es; QByteArray data = extractArrayData(exec, args[1], es);
            gl->glBufferData(t, data.size(), data.constData(), usage);
        }
        return jsUndefined();
    }
    case BufferSubData: {
        GLenum t = args[0]->toInteger(exec);
        int es; QByteArray data = extractArrayData(exec, args[2], es);
        gl->glBufferSubData(t, args[1]->toInteger(exec), data.size(), data.constData());
        return jsUndefined();
    }
    case EnableVertexAttribArray: gl->glEnableVertexAttribArray(args[0]->toInteger(exec)); return jsUndefined();
    case DisableVertexAttribArray: gl->glDisableVertexAttribArray(args[0]->toInteger(exec)); return jsUndefined();
    case VertexAttribPointer:
        gl->glVertexAttribPointer(args[0]->toInteger(exec), args[1]->toInteger(exec),
            args[2]->toInteger(exec), args[3]->toBoolean(exec)?GL_TRUE:GL_FALSE,
            args[4]->toInteger(exec), (void*)(intptr_t)(args.size()>5?args[5]->toInteger(exec):0));
        return jsUndefined();
    case DrawArrays: gl->glDrawArrays(args[0]->toInteger(exec), args[1]->toInteger(exec), args[2]->toInteger(exec)); readPixelsToCanvas(); return jsUndefined();
    case DrawElements: gl->glDrawElements(args[0]->toInteger(exec), args[1]->toInteger(exec), args[2]->toInteger(exec), (void*)(intptr_t)(args.size()>3?args[3]->toInteger(exec):0)); readPixelsToCanvas(); return jsUndefined();
    case GetAttribLocation: { QByteArray n = args[1]->toString(exec).qstring().toUtf8(); return jsNumber(gl->glGetAttribLocation(args[0]->toInteger(exec), n.constData())); }
    case GetUniformLocation: { QByteArray n = args[1]->toString(exec).qstring().toUtf8(); return jsNumber((intptr_t)gl->glGetUniformLocation(args[0]->toInteger(exec), n.constData())); }
    case Uniform1f: gl->glUniform1f(args[0]->toInteger(exec), args[1]->toNumber(exec)); return jsUndefined();
    case Uniform2f: gl->glUniform2f(args[0]->toInteger(exec), args[1]->toNumber(exec), args[2]->toNumber(exec)); return jsUndefined();
    case Uniform3f: gl->glUniform3f(args[0]->toInteger(exec), args[1]->toNumber(exec), args[2]->toNumber(exec), args[3]->toNumber(exec)); return jsUndefined();
    case Uniform4f: gl->glUniform4f(args[0]->toInteger(exec), args[1]->toNumber(exec), args[2]->toNumber(exec), args[3]->toNumber(exec), args[4]->toNumber(exec)); return jsUndefined();
    case Uniform1i: gl->glUniform1i(args[0]->toInteger(exec), args[1]->toInteger(exec)); return jsUndefined();
    case Uniform2i: gl->glUniform2i(args[0]->toInteger(exec), args[1]->toInteger(exec), args[2]->toInteger(exec)); return jsUndefined();
    case Uniform3i: gl->glUniform3i(args[0]->toInteger(exec), args[1]->toInteger(exec), args[2]->toInteger(exec), args[3]->toInteger(exec)); return jsUndefined();
    case Uniform4i: gl->glUniform4i(args[0]->toInteger(exec), args[1]->toInteger(exec), args[2]->toInteger(exec), args[3]->toInteger(exec), args[4]->toInteger(exec)); return jsUndefined();
    case UniformMatrix2fv: case UniformMatrix3fv: case UniformMatrix4fv: {
        GLint loc = args[0]->toInteger(exec);
        GLboolean trans = args[1]->toBoolean(exec)?GL_TRUE:GL_FALSE;
        int es; QByteArray data = extractArrayData(exec, args[2], es);
        int dim = (id==UniformMatrix2fv)?4:(id==UniformMatrix3fv)?9:16;
        int count = data.size() / (es * dim);
        if (id==UniformMatrix2fv) gl->glUniformMatrix2fv(loc, count, trans, (const float*)data.constData());
        else if (id==UniformMatrix3fv) gl->glUniformMatrix3fv(loc, count, trans, (const float*)data.constData());
        else gl->glUniformMatrix4fv(loc, count, trans, (const float*)data.constData());
        return jsUndefined();
    }
    case CreateTexture: { GLuint t=0; gl->glGenTextures(1,&t); return jsNumber(t); }
    case DeleteTexture: { GLuint t=args[0]->toInteger(exec); gl->glDeleteTextures(1,&t); return jsUndefined(); }
    case BindTexture: gl->glBindTexture(args[0]->toInteger(exec), args[1]->toInteger(exec)); return jsUndefined();
    case TexParameteri: gl->glTexParameteri(args[0]->toInteger(exec), args[1]->toInteger(exec), args[2]->toInteger(exec)); return jsUndefined();
    case TexParameterf: gl->glTexParameterf(args[0]->toInteger(exec), args[1]->toInteger(exec), args[2]->toNumber(exec)); return jsUndefined();
    case TexImage2D: {
        if (args.size() >= 9) {
            gl->glTexImage2D(args[0]->toInteger(exec), args[1]->toInteger(exec),
                args[2]->toInteger(exec), args[3]->toInteger(exec), args[4]->toInteger(exec),
                args[5]->toInteger(exec), args[6]->toInteger(exec), args[7]->toInteger(exec), nullptr);
        }
        return jsUndefined();
    }
    case ActiveTexture: gl->glActiveTexture(args[0]->toInteger(exec)); return jsUndefined();
    case PixelStorei: gl->glPixelStorei(args[0]->toInteger(exec), args[1]->toInteger(exec)); return jsUndefined();
    case Finish: gl->glFinish(); readPixelsToCanvas(); return jsUndefined();
    case Flush: gl->glFlush(); return jsUndefined();
    case GetError: return jsNumber(gl->glGetError());
    case GetParameter: {
        GLenum pname = args[0]->toInteger(exec);
        GLint iv=0;
        switch(pname) {
        case GL_CURRENT_PROGRAM: return jsNumber(d->currentProgram);
        case GL_ARRAY_BUFFER_BINDING: return jsNumber(d->arrayBuffer);
        case GL_ELEMENT_ARRAY_BUFFER_BINDING: return jsNumber(d->elementBuffer);
        default: gl->glGetIntegerv(pname, &iv); return jsNumber(iv);
        }
    }
    case IsEnabled: return jsBoolean(gl->glIsEnabled(args[0]->toInteger(exec)));
    case BlendFunc: gl->glBlendFunc(args[0]->toInteger(exec), args[1]->toInteger(exec)); return jsUndefined();
    case BlendColor: gl->glBlendColor(args[0]->toNumber(exec), args[1]->toNumber(exec), args[2]->toNumber(exec), args[3]->toNumber(exec)); return jsUndefined();
    case BlendEquation: gl->glBlendEquation(args[0]->toInteger(exec)); return jsUndefined();
    case CullFace: gl->glCullFace(args[0]->toInteger(exec)); return jsUndefined();
    case FrontFace: gl->glFrontFace(args[0]->toInteger(exec)); return jsUndefined();
    case DepthFunc: gl->glDepthFunc(args[0]->toInteger(exec)); return jsUndefined();
    case DepthMask: gl->glDepthMask(args[0]->toBoolean(exec)?GL_TRUE:GL_FALSE); return jsUndefined();
    case DepthRange: gl->glDepthRangef(args[0]->toNumber(exec), args[1]->toNumber(exec)); return jsUndefined();
    case Scissor: gl->glScissor(args[0]->toInteger(exec), args[1]->toInteger(exec), args[2]->toInteger(exec), args[3]->toInteger(exec)); return jsUndefined();
    case LineWidth: gl->glLineWidth(args[0]->toNumber(exec)); return jsUndefined();
    case PolygonOffset: gl->glPolygonOffset(args[0]->toNumber(exec), args[1]->toNumber(exec)); return jsUndefined();
    case DeleteShader: gl->glDeleteShader(args[0]->toInteger(exec)); return jsUndefined();
    case DeleteProgram: gl->glDeleteProgram(args[0]->toInteger(exec)); return jsUndefined();
    case BindAttribLocation: { QByteArray n = args[2]->toString(exec).qstring().toUtf8(); gl->glBindAttribLocation(args[0]->toInteger(exec), args[1]->toInteger(exec), n.constData()); return jsUndefined(); }
    case GetSupportedExtensions: return exec->lexicalInterpreter()->builtinArray()->construct(exec, List());
    case GetExtension: return jsNull();
    case ReadPixels: return jsUndefined();
    default: break;
    }
    return jsUndefined();
}

} // namespace KJS
