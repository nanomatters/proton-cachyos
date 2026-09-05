/*
 * Isolated swap-adapter tests; no display, driver, Steam or Wine required.
 * cc -std=gnu11 -Iwine/include vklayers/test_overlay_gl.c -pthread -ldl -o test-overlay-gl
 */
#include "wineland_overlay_gl.c"
#include <assert.h>

static char egl_display_token, egl_surface_token, egl_context_token, glx_context_token;
static bool have_context = true, correct_display = true, correct_surface = true;
static bool duplicate_swap, bypass_swap, check_driver_scope;
static unsigned int egl_swaps, glx_swaps, steam_swaps, resizes, nested_swaps;
static unsigned int input_stops;
static uint32_t swap_result = EGL_TRUE;

static void initialized(void) {}
static void mock_stop_input(unsigned int source)
{
    assert(source == WINELAND_OVERLAY_INPUT_OPENGL);
    assert(!current_swap && !overlay_ready);
    assert(!pthread_mutex_trylock(&overlay_mutex));
    pthread_mutex_unlock(&overlay_mutex);
    input_stops++;
}
static EGLContext mock_egl_context(void) { return have_context ? &egl_context_token : NULL; }
static EGLDisplay mock_egl_display(void) { return correct_display ? &egl_display_token : NULL; }
static EGLSurface mock_egl_surface(EGLint which)
{
    assert(which == EGL_DRAW);
    return correct_surface ? &egl_surface_token : NULL;
}
static GLXContext mock_glx_context(void) { return (void *)&glx_context_token; }
static void mock_glx_swap(Display *display, GLXDrawable drawable)
{
    (void)display;
    (void)drawable;
    glx_swaps++;
}
static uint32_t nested_swap(void *display, void *surface)
{
    assert(display == &egl_display_token && surface == &egl_surface_token);
    assert(glXGetCurrentContext() == (void *)&glx_context_token);
    nested_swaps++;
    return EGL_TRUE;
}
static uint32_t mock_egl_swap(void *display, void *surface)
{
    assert(display == &egl_display_token && surface == &egl_surface_token);
    assert(glXGetCurrentContext() == (void *)&glx_context_token);
    if (check_driver_scope)
    {
        assert(current_swap && current_swap->in_driver);
        assert(__wineland_overlay_gl_swap_buffers_v1(display, surface, 800, 600, nested_swap));
    }
    egl_swaps++;
    return swap_result;
}
static void mock_steam_swap(Display *display, GLXDrawable drawable)
{
    steam_swaps++;
    assert(glXGetCurrentContext() == (void *)&egl_context_token);
    if (check_driver_scope)
        assert(__wineland_overlay_gl_swap_buffers_v1(&egl_display_token, &egl_surface_token,
                                                     800, 600, nested_swap));
    if (!bypass_swap)
    {
        glXSwapBuffers(display, drawable);
        if (duplicate_swap) glXSwapBuffers(display, drawable);
    }
    assert(glXGetCurrentContext() == (void *)&egl_context_token);
}
static int mock_resize(Display *display, Window window, unsigned int width, unsigned int height)
{
    assert(display == proxy.display && window == proxy.window);
    assert(width > 0 && height > 0);
    resizes++;
    return 1;
}
static int mock_sync(Display *display, Bool discard)
{
    assert(display == proxy.display && !discard);
    return 1;
}
static Bool mock_configure(Display *display, Window window, int type, XEvent *event)
{
    assert(display == proxy.display && window == proxy.window && type == ConfigureNotify);
    (void)event;
    return False;
}
static uint32_t present(int width, int height)
{
    uint32_t result = __wineland_overlay_gl_swap_buffers_v1(
            &egl_display_token, &egl_surface_token, width, height, mock_egl_swap);
    assert(!current_swap);
    assert(glXGetCurrentContext() == (void *)&glx_context_token);
    return result;
}

int main(void)
{
    pthread_once(&glx_once, initialized);
    pthread_once(&overlay_once, initialized);
    real_glx_context = mock_glx_context;
    real_glx_swap = mock_glx_swap;
    get_egl_context = mock_egl_context;
    get_egl_display = mock_egl_display;
    get_egl_surface = mock_egl_surface;
    steam_glx_swap = mock_steam_swap;
    stop_input = mock_stop_input;
    resize_window = mock_resize;
    sync_display = mock_sync;
    check_configure = mock_configure;
    proxy.display = (void *)&proxy;
    proxy.window = 123;
    overlay_ready = true;

    assert(present(800, 600));
    assert(egl_swaps == 1 && steam_swaps == 1 && resizes == 1);
    duplicate_swap = true;
    assert(present(800, 600));
    assert(egl_swaps == 2 && steam_swaps == 2 && resizes == 1);
    duplicate_swap = false;

    swap_result = EGL_FALSE;
    assert(!present(1024, 768));
    assert(egl_swaps == 3 && steam_swaps == 3 && resizes == 2);
    swap_result = EGL_TRUE;

    check_driver_scope = true;
    assert(present(1024, 768));
    assert(egl_swaps == 4 && steam_swaps == 4 && nested_swaps == 2);
    check_driver_scope = false;

    have_context = false;
    assert(present(800, 600));
    have_context = true;
    correct_display = false;
    assert(present(800, 600));
    correct_display = true;
    correct_surface = false;
    assert(present(800, 600));
    correct_surface = true;
    assert(present(0, 600));
    assert(egl_swaps == 8 && steam_swaps == 4);

    glXSwapBuffers(NULL, 0);
    assert(glx_swaps == 1);
    bypass_swap = true;
    assert(present(800, 600));
    assert(!overlay_ready && egl_swaps == 9 && steam_swaps == 5 && input_stops == 1);
    assert(present(800, 600));
    assert(egl_swaps == 10 && steam_swaps == 5 && input_stops == 1);

    /* Partial initialization must fail open without dereferencing missing procs. */
    get_egl_display = NULL;
    assert(present(800, 600));
    assert(egl_swaps == 11);
    puts("OpenGL overlay adapter tests passed");
    return 0;
}
