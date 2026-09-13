#ifndef KJS_WEBGL_H
#define KJS_WEBGL_H

#include "ecma/kjs_dom.h"
#include "html/html_canvasimpl.h"
#include <kjs/object.h>

namespace KJS {

class WebGLContext : public DOMObject {
    friend class WebGLFunction;
public:
    WebGLContext(ExecState *exec, DOM::HTMLCanvasElementImpl *canvas);
    ~WebGLContext();

    enum {
        Clear, ClearColor, ClearDepth, ClearStencil,
        Viewport, Enable, Disable,
        CreateShader, ShaderSource, CompileShader, GetShaderParameter, GetShaderInfoLog,
        CreateProgram, AttachShader, LinkProgram, GetProgramParameter, GetProgramInfoLog, UseProgram,
        CreateBuffer, BindBuffer, BufferData, BufferSubData,
        EnableVertexAttribArray, DisableVertexAttribArray, VertexAttribPointer,
        DrawArrays, DrawElements,
        GetAttribLocation, GetUniformLocation,
        Uniform1f, Uniform2f, Uniform3f, Uniform4f, Uniform1i, Uniform2i, Uniform3i, Uniform4i,
        UniformMatrix2fv, UniformMatrix3fv, UniformMatrix4fv,
        CreateTexture, BindTexture, TexParameteri, TexParameterf, TexImage2D,
        ActiveTexture, PixelStorei, Finish, Flush,
        GetError, GetParameter, IsEnabled,
        BlendFunc, BlendColor, BlendEquation, CullFace, FrontFace,
        DepthFunc, DepthMask, DepthRange, Scissor, LineWidth, PolygonOffset,
        DeleteShader, DeleteProgram, DeleteBuffer, DeleteTexture,
        DetachShader, BindAttribLocation,
        GetSupportedExtensions, GetExtension, ReadPixels
    };

    JSValue *callMethod(ExecState *exec, int id, const List &args);

    bool getOwnPropertySlot(ExecState *exec, const Identifier &propertyName, PropertySlot &slot) override;
    JSValue *getValueProperty(ExecState *exec, int token) const;

    static const ClassInfo info;
    const ClassInfo *classInfo() const override { return &info; }

private:
    bool ensureGL();
    void readPixelsToCanvas();

    DOM::HTMLCanvasElementImpl *m_canvas;
    class Private;
    Private *d;
};

} // namespace KJS

#endif
