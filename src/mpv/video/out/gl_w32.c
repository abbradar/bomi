/*
 * MPlayer is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * MPlayer is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with MPlayer; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * You can alternatively redistribute this file and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <assert.h>
#include <windows.h>
#include "w32_common.h"
#include "gl_common.h"

struct w32_context {
    HGLRC context;
    HDC hdc;
    int flags;
};

static bool create_dc(struct MPGLContext *ctx, int flags)
{
    struct w32_context *w32_ctx = ctx->priv;
    HWND win = vo_w32_hwnd(ctx->vo);

    if (w32_ctx->hdc)
        return true;

    HDC hdc = GetDC(win);
    if (!hdc)
        return false;

    PIXELFORMATDESCRIPTOR pfd;
    memset(&pfd, 0, sizeof pfd);
    pfd.nSize = sizeof pfd;
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;

    if (flags & VOFLAG_STEREO)
        pfd.dwFlags |= PFD_STEREO;

    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 24;
    pfd.iLayerType = PFD_MAIN_PLANE;
    int pf = ChoosePixelFormat(hdc, &pfd);

    if (!pf) {
        MP_ERR(ctx->vo, "unable to select a valid pixel format!\n");
        ReleaseDC(win, hdc);
        return false;
    }

    SetPixelFormat(hdc, pf, &pfd);

    int pfmt = GetPixelFormat(hdc);
    if (DescribePixelFormat(hdc, pfmt, sizeof(PIXELFORMATDESCRIPTOR), &pfd)) {
        ctx->depth_r = pfd.cRedBits;
        ctx->depth_g = pfd.cGreenBits;
        ctx->depth_b = pfd.cBlueBits;
    }

    w32_ctx->hdc = hdc;
    return true;
}

static void *w32gpa(const GLubyte *procName)
{
    HMODULE oglmod;
    void *res = wglGetProcAddress(procName);
    if (res)
        return res;
    oglmod = GetModuleHandle(L"opengl32.dll");
    return GetProcAddress(oglmod, procName);
}

static bool create_context_w32_old(struct MPGLContext *ctx)
{
    struct w32_context *w32_ctx = ctx->priv;

    HDC windc = w32_ctx->hdc;
    bool res = false;

    HGLRC context = wglCreateContext(windc);
    if (!context) {
        MP_FATAL(ctx->vo, "Could not create GL context!\n");
        return res;
    }

    if (!wglMakeCurrent(windc, context)) {
        MP_FATAL(ctx->vo, "Could not set GL context!\n");
        wglDeleteContext(context);
        return res;
    }

    w32_ctx->context = context;

    mpgl_load_functions(ctx->gl, w32gpa, NULL, ctx->vo->log);
    return true;
}

static bool create_context_w32_gl3(struct MPGLContext *ctx)
{
    struct w32_context *w32_ctx = ctx->priv;

    HDC windc = w32_ctx->hdc;
    HGLRC context = 0;

    // A legacy context is needed to get access to the new functions.
    HGLRC legacy_context = wglCreateContext(windc);
    if (!legacy_context) {
        MP_FATAL(ctx->vo, "Could not create GL context!\n");
        return false;
    }

    // set context
    if (!wglMakeCurrent(windc, legacy_context)) {
        MP_FATAL(ctx->vo, "Could not set GL context!\n");
        goto out;
    }

    const char *(GLAPIENTRY *wglGetExtensionsStringARB)(HDC hdc)
        = w32gpa((const GLubyte*)"wglGetExtensionsStringARB");

    if (!wglGetExtensionsStringARB)
        goto unsupported;

    const char *wgl_exts = wglGetExtensionsStringARB(windc);
    if (!strstr(wgl_exts, "WGL_ARB_create_context"))
        goto unsupported;

    HGLRC (GLAPIENTRY *wglCreateContextAttribsARB)(HDC hDC, HGLRC hShareContext,
                                                   const int *attribList)
        = w32gpa((const GLubyte*)"wglCreateContextAttribsARB");

    if (!wglCreateContextAttribsARB)
        goto unsupported;

    int gl_version = ctx->requested_gl_version;
    int attribs[] = {
        WGL_CONTEXT_MAJOR_VERSION_ARB, MPGL_VER_GET_MAJOR(gl_version),
        WGL_CONTEXT_MINOR_VERSION_ARB, MPGL_VER_GET_MINOR(gl_version),
        WGL_CONTEXT_FLAGS_ARB, 0,
        WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
        0
    };

    context = wglCreateContextAttribsARB(windc, 0, attribs);
    if (!context) {
        // NVidia, instead of ignoring WGL_CONTEXT_FLAGS_ARB, will error out if
        // it's present on pre-3.2 contexts.
        // Remove it from attribs and retry the context creation.
        attribs[6] = attribs[7] = 0;
        context = wglCreateContextAttribsARB(windc, 0, attribs);
    }
    if (!context) {
        int err = GetLastError();
        MP_FATAL(ctx->vo, "Could not create an OpenGL 3.x context: error 0x%x\n", err);
        goto out;
    }

    wglMakeCurrent(windc, NULL);
    wglDeleteContext(legacy_context);

    if (!wglMakeCurrent(windc, context)) {
        MP_FATAL(ctx->vo, "Could not set GL3 context!\n");
        wglDeleteContext(context);
        return false;
    }

    w32_ctx->context = context;

    /* update function pointers */
    mpgl_load_functions(ctx->gl, w32gpa, NULL, ctx->vo->log);

    return true;

unsupported:
    MP_ERR(ctx->vo, "The OpenGL driver does not support OpenGL 3.x \n");
out:
    wglMakeCurrent(windc, NULL);
    wglDeleteContext(legacy_context);
    return false;
}

static void create_ctx(void *ptr)
{
    struct MPGLContext *ctx = ptr;
    struct w32_context *w32_ctx = ctx->priv;

    if (!create_dc(ctx, w32_ctx->flags))
        return;

    create_context_w32_gl3(ctx);
    if (!w32_ctx->context)
        create_context_w32_old(ctx);
    wglMakeCurrent(w32_ctx->hdc, NULL);
}

static bool config_window_w32(struct MPGLContext *ctx, int flags)
{
    struct w32_context *w32_ctx = ctx->priv;
    if (!vo_w32_config(ctx->vo, flags))
        return false;

    if (w32_ctx->context) // reuse existing context
        return true;

    w32_ctx->flags = flags;
    vo_w32_run_on_thread(ctx->vo, create_ctx, ctx);

    if (w32_ctx->context)
        wglMakeCurrent(w32_ctx->hdc, w32_ctx->context);
    return !!w32_ctx->context;
}

static void destroy_gl(void *ptr)
{
    struct MPGLContext *ctx = ptr;
    struct w32_context *w32_ctx = ctx->priv;
    if (w32_ctx->context)
        wglDeleteContext(w32_ctx->context);
    w32_ctx->context = 0;
    if (w32_ctx->hdc)
        ReleaseDC(vo_w32_hwnd(ctx->vo), w32_ctx->hdc);
    w32_ctx->hdc = NULL;
}

static void releaseGlContext_w32(MPGLContext *ctx)
{
    struct w32_context *w32_ctx = ctx->priv;
    if (w32_ctx->context)
        wglMakeCurrent(w32_ctx->hdc, 0);
    vo_w32_run_on_thread(ctx->vo, destroy_gl, ctx);
}

static void swapGlBuffers_w32(MPGLContext *ctx)
{
    struct w32_context *w32_ctx = ctx->priv;
    SwapBuffers(w32_ctx->hdc);
}

void mpgl_set_backend_w32(MPGLContext *ctx)
{
    ctx->priv = talloc_zero(ctx, struct w32_context);
    ctx->config_window = config_window_w32;
    ctx->releaseGlContext = releaseGlContext_w32;
    ctx->swapGlBuffers = swapGlBuffers_w32;
    ctx->vo_init = vo_w32_init;
    ctx->vo_uninit = vo_w32_uninit;
    ctx->vo_control = vo_w32_control;
}
