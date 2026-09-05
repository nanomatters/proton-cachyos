/*
 * Native OpenGL Steam overlay adapter for WineWayland.
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 */

#define _GNU_SOURCE

#include <dlfcn.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <EGL/egl.h>
#include <GL/glx.h>

#include "wine/wayland_opengl_overlay.h"
#include "wine/wayland_vulkan_proxy.h"
#include "wineland_overlay_client.h"

#define EXPORT __attribute__((visibility("default")))

struct overlay_swap
{
    void *display, *surface, *context;
    wine_wayland_egl_swap_func swap;
    uint32_t result;
    bool swapped, in_driver;
};

static _Thread_local struct overlay_swap *current_swap;
static pthread_once_t glx_once = PTHREAD_ONCE_INIT;
static pthread_once_t overlay_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t overlay_mutex = PTHREAD_MUTEX_INITIALIZER;
static void (*real_glx_swap)(Display *, GLXDrawable);
static GLXContext (*real_glx_context)(void);
static void (*steam_glx_swap)(Display *, GLXDrawable);
static void (*stop_input)(unsigned int);
static EGLContext (*get_egl_context)(void);
static EGLDisplay (*get_egl_display)(void);
static EGLSurface (*get_egl_surface)(EGLint);
static int (*resize_window)(Display *, Window, unsigned int, unsigned int);
static int (*sync_display)(Display *, Bool);
static Bool (*check_configure)(Display *, Window, int, XEvent *);
static struct wine_wayland_vulkan_proxy proxy =
{
    sizeof(proxy), WINE_WAYLAND_VULKAN_PROXY_VERSION, NULL, 0,
};
static bool overlay_ready, logged_swap;
static int proxy_width, proxy_height;

EXPORT uint32_t __wineland_overlay_gl_swap_buffers_v1(
        void *display, void *surface, int width, int height,
        wine_wayland_egl_swap_func swap);

static void init_glx(void)
{
    real_glx_swap = dlsym(RTLD_NEXT, "glXSwapBuffers");
    real_glx_context = dlsym(RTLD_NEXT, "glXGetCurrentContext");
}

/* EGL context handles are opaque identities to Steam's GL renderer. Never
 * expose them to the real GLX implementation or to unrelated GLX calls. */
EXPORT GLXContext glXGetCurrentContext(void)
{
    if (current_swap && !current_swap->in_driver) return current_swap->context;
    pthread_once(&glx_once, init_glx);
    return real_glx_context ? real_glx_context() : NULL;
}

EXPORT void glXSwapBuffers(Display *display, GLXDrawable drawable)
{
    struct overlay_swap *frame = current_swap;
    bool in_driver = frame && frame->in_driver;

    if (frame && !frame->in_driver && display == proxy.display && drawable == proxy.window)
    {
        if (!frame->swapped)
        {
            frame->swapped = true;
            frame->in_driver = true;
            frame->result = frame->swap(frame->display, frame->surface);
            frame->in_driver = false;
        }
        return;
    }

    pthread_once(&glx_once, init_glx);
    if (frame) frame->in_driver = true;
    if (real_glx_swap) real_glx_swap(display, drawable);
    if (frame) frame->in_driver = in_driver;
}

static void init_overlay(void)
{
    void *wine_module = NULL, *input_module = NULL, *x11_module = NULL;
    int (*start_input)(void *, unsigned long, unsigned int);
    int (*get_proxy)(struct wine_wayland_vulkan_proxy *);
    Status (*get_attributes)(Display *, Window, XWindowAttributes *);
    int (*select_input)(Display *, Window, long);
    XWindowAttributes attributes;
    Dl_info info = {0};
    char path[PATH_MAX];
    const char *name, *slash, *enabled;
    const char *failure = "Steam GLX hook order";
    int length;

    enabled = getenv("PROTON_WAYLAND_OPENGL_OVERLAY");
    if (!enabled || !*enabled || !strcmp(enabled, "0")) return;
    enabled = getenv("PROTON_WAYLAND_STEAM_OVERLAY");
    if (!enabled || !*enabled || !strcmp(enabled, "0")) return;

    steam_glx_swap = dlsym(RTLD_DEFAULT, "glXSwapBuffers");
    if (!steam_glx_swap || !dladdr((void *)steam_glx_swap, &info) || !info.dli_fname)
        goto failed;
    name = strrchr(info.dli_fname, '/');
    if (strcmp(name ? name + 1 : info.dli_fname, "gameoverlayrenderer.so"))
        goto failed;
    /* Both hooks must precede libGL, while Steam's swap hook precedes ours. */
    if (dlsym(RTLD_DEFAULT, "glXGetCurrentContext") != (void *)glXGetCurrentContext)
        goto failed;

    failure = "EGL entrypoints";
    get_egl_context = dlsym(RTLD_DEFAULT, "eglGetCurrentContext");
    get_egl_display = dlsym(RTLD_DEFAULT, "eglGetCurrentDisplay");
    get_egl_surface = dlsym(RTLD_DEFAULT, "eglGetCurrentSurface");
    if (!get_egl_context || !get_egl_display || !get_egl_surface) goto failed;

    failure = "WineWayland input proxy";
    if (!(wine_module = dlopen("winewayland.so", RTLD_NOW | RTLD_LOCAL | RTLD_NOLOAD)))
        goto failed;
    get_proxy = dlsym(wine_module, WINE_WAYLAND_VULKAN_PROXY_SYMBOL);
    if (!get_proxy || !get_proxy(&proxy)) goto failed;

    failure = "X11 proxy geometry";
    if (!(x11_module = dlopen("libX11.so.6", RTLD_NOW | RTLD_LOCAL))) goto failed;
    get_attributes = dlsym(x11_module, "XGetWindowAttributes");
    resize_window = dlsym(x11_module, "XResizeWindow");
    sync_display = dlsym(x11_module, "XSync");
    select_input = dlsym(RTLD_DEFAULT, "XSelectInput");
    check_configure = dlsym(RTLD_DEFAULT, "XCheckTypedWindowEvent");
    if (!get_attributes || !resize_window || !sync_display || !select_input || !check_configure)
        goto failed;
    if (!get_attributes(proxy.display, proxy.window, &attributes)) goto failed;
    select_input(proxy.display, proxy.window, attributes.your_event_mask | StructureNotifyMask);

    /* Load the existing input client locally, not as a Vulkan preload: its
     * Vulkan entrypoints must not interpose the application's Vulkan loader. */
    failure = "shared overlay input client";
    if (!dladdr((void *)__wineland_overlay_gl_swap_buffers_v1, &info) ||
        !info.dli_fname || !(slash = strrchr(info.dli_fname, '/'))) goto failed;
#if defined(__x86_64__)
    name = "libVkLayer_WINELAND_translate_x86_64.so";
#elif defined(__i386__)
    name = "libVkLayer_WINELAND_translate_i386.so";
#else
    goto failed;
#endif
    length = snprintf(path, sizeof(path), "%.*s/../../vulkan/%s",
                      (int)(slash - info.dli_fname), info.dli_fname, name);
    if (length < 0 || (size_t)length >= sizeof(path)) goto failed;
    if (!(input_module = dlopen(path, RTLD_NOW | RTLD_LOCAL))) goto failed;
    start_input = dlsym(input_module, "__wineland_overlay_client_start_v1");
    stop_input = dlsym(input_module, "__wineland_overlay_client_stop_v1");
    if (!start_input || !stop_input) goto failed;
    if (!start_input(proxy.display, proxy.window, WINELAND_OVERLAY_INPUT_OPENGL)) goto failed;

    /* Retain these modules: the existing input worker is process-lifetime. */
    overlay_ready = true;
    fprintf(stderr, "wineland-opengl-overlay: ready for proxy window 0x%lx\n",
            (unsigned long)proxy.window);
    return;

failed:
    fprintf(stderr, "wineland-opengl-overlay: unavailable (%s); keeping ordinary EGL presentation\n",
            failure);
    if (input_module) dlclose(input_module);
    if (x11_module) dlclose(x11_module);
    if (wine_module) dlclose(wine_module);
}

EXPORT uint32_t __wineland_overlay_gl_swap_buffers_v1(
        void *display, void *surface, int width, int height,
        wine_wayland_egl_swap_func swap)
{
    struct overlay_swap frame = { .display = display, .surface = surface, .swap = swap };
    XEvent event;

    if (current_swap)
    {
        bool in_driver = current_swap->in_driver;
        current_swap->in_driver = true;
        frame.result = swap(display, surface);
        current_swap->in_driver = in_driver;
        return frame.result;
    }
    if (width <= 0 || height <= 0) return swap(display, surface);
    pthread_once(&overlay_once, init_overlay);
    if (!get_egl_context || !get_egl_display || !get_egl_surface ||
        !(frame.context = get_egl_context()) ||
        get_egl_display() != display || get_egl_surface(EGL_DRAW) != surface)
        return swap(display, surface);

    pthread_mutex_lock(&overlay_mutex);
    if (!overlay_ready)
    {
        pthread_mutex_unlock(&overlay_mutex);
        return swap(display, surface);
    }

    if (proxy_width != width || proxy_height != height)
    {
        /* InputOnly: this changes geometry for Steam, never allocates a game
         * framebuffer in X11. Deliver ConfigureNotify so Steam updates size. */
        resize_window(proxy.display, proxy.window, width, height);
        sync_display(proxy.display, False);
        while (check_configure(proxy.display, proxy.window, ConfigureNotify, &event)) {}
        proxy_width = width;
        proxy_height = height;
    }

    current_swap = &frame;
    steam_glx_swap(proxy.display, proxy.window);
    current_swap = NULL;

    if (!frame.swapped)
    {
        /* A conflicting preload must not drop a frame or cause two swaps. */
        overlay_ready = false;
        fprintf(stderr, "wineland-opengl-overlay: swap hook bypassed; disabling bridge\n");
    }
    else if (!logged_swap)
    {
        logged_swap = true;
        fprintf(stderr, "wineland-opengl-overlay: presenting EGL context %p (%dx%d)\n",
                frame.context, width, height);
    }
    pthread_mutex_unlock(&overlay_mutex);
    if (!frame.swapped) stop_input(WINELAND_OVERLAY_INPUT_OPENGL);
    return frame.swapped ? frame.result : swap(display, surface);
}
