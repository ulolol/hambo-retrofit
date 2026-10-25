#define GL_GLEXT_PROTOTYPES 1
#include <GLES/gl.h>
#include <GLES/glext.h>

#include <mutex>
#include <unordered_map>

#include "core.h"

namespace {
u32 guest_str(const void* key, const char* s) {
    static std::mutex m;
    static std::unordered_map<std::string, u32> cache;
    if (!s) return 0;
    std::lock_guard lk(m);
    auto& slot = cache[std::to_string(reinterpret_cast<uintptr_t>(key)) + s];
    if (!slot) slot = mem::strdup(s);
    return slot;
}
}  // namespace

void init_gl() {
    IMPORT(glAlphaFunc);
    IMPORT(glBindBuffer);
    IMPORT(glBindTexture);
    IMPORT(glBlendFunc);
    IMPORT(glBufferData);
    IMPORT(glBufferSubData);
    IMPORT(glClear);
    IMPORT(glClearColor);
    IMPORT(glClearDepthf);
    IMPORT(glClearStencil);
    IMPORT(glColor4f);
    IMPORT(glColor4ub);
    IMPORT(glColorMask);
    IMPORT(glColorPointer);
    IMPORT(glCompressedTexImage2D);
    IMPORT(glCompressedTexSubImage2D);
    IMPORT(glCopyTexImage2D);
    IMPORT(glCopyTexSubImage2D);
    IMPORT(glCullFace);
    IMPORT(glDeleteBuffers);
    IMPORT(glDeleteTextures);
    IMPORT(glDepthFunc);
    IMPORT(glDepthMask);
    IMPORT(glDepthRangef);
    IMPORT(glDisable);
    IMPORT(glDisableClientState);
    IMPORT(glDrawArrays);
    IMPORT(glDrawElements);
    IMPORT(glEnable);
    IMPORT(glEnableClientState);
    IMPORT(glFinish);
    IMPORT(glFlush);
    IMPORT(glFrontFace);
    IMPORT(glFrustumf);
    IMPORT(glGenBuffers);
    IMPORT(glGenTextures);
    IMPORT(glGetBooleanv);
    IMPORT(glGetBufferParameteriv);
    IMPORT(glGetError);
    IMPORT(glGetFloatv);
    IMPORT(glGetIntegerv);
    IMPORT(glHint);
    IMPORT(glIsBuffer);
    IMPORT(glIsEnabled);
    IMPORT(glIsTexture);
    IMPORT(glLineWidth);
    IMPORT(glLoadIdentity);
    IMPORT(glLoadMatrixf);
    IMPORT(glMatrixMode);
    IMPORT(glMultMatrixf);
    IMPORT(glNormal3f);
    IMPORT(glNormalPointer);
    IMPORT(glOrthof);
    IMPORT(glPixelStorei);
    IMPORT(glPointSize);
    IMPORT(glPointSizePointerOES);
    IMPORT(glPolygonOffset);
    IMPORT(glPopMatrix);
    IMPORT(glPushMatrix);
    IMPORT(glReadPixels);
    IMPORT(glRotatef);
    IMPORT(glSampleCoverage);
    IMPORT(glScalef);
    IMPORT(glScissor);
    IMPORT(glShadeModel);
    IMPORT(glStencilFunc);
    IMPORT(glStencilMask);
    IMPORT(glStencilOp);
    IMPORT(glTexCoordPointer);
    IMPORT(glTexEnvf);
    IMPORT(glTexEnvfv);
    IMPORT(glTexEnvi);
    IMPORT(glTexEnviv);
    IMPORT(glTexImage2D);
    IMPORT(glTexParameterf);
    IMPORT(glTexParameterfv);
    IMPORT(glTexParameteri);
    IMPORT(glTexParameteriv);
    IMPORT(glTexSubImage2D);
    IMPORT(glTranslatef);
    IMPORT(glVertexPointer);
    IMPORT(glViewport);

    register_import("glGetString", [](Ctx& c) {
        c.ret(guest_str(nullptr, reinterpret_cast<const char*>(glGetString(c.arg(0)))));
    });
}
