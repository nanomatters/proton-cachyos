/*
 * Wineland renderer local Steam overlay input client.
 * Copyright 2026 Erhan Bilgili
 *
 * This library is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation; either version 2.1 of the License, or (at your option)
 * any later version.
 */

#define _GNU_SOURCE

#include <dlfcn.h>
#include <linux/input-event-codes.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <X11/Xlib.h>

#include "wine/wayland_external_input.h"

#include "wineland_overlay_client.h"

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define OVERLAY_INPUT_QUEUE_SIZE 1024

struct overlay_x11_funcs
{
    void *x11_module;
    void (*lock_display)(Display *);
    void (*unlock_display)(Display *);
    Window (*default_root_window)(Display *);
    int (*put_back_event)(Display *, XEvent *);
    Bool (*check_if_event)(Display *, XEvent *,
                          Bool (*)(Display *, XEvent *, XPointer), XPointer);
};

struct overlay_event_match
{
    Window window;
    unsigned long serial;
    int type;
};

struct overlay_input_completion
{
    bool done;
    bool handled;
};

struct overlay_input_queue_event
{
    struct wine_wayland_external_input_event event;
    struct overlay_input_completion *completion;
};

struct overlay_pointer_state
{
    int x, y, width, height;
    unsigned int modifiers, buttons;
    bool positioned;
};

static pthread_mutex_t overlay_input_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t overlay_input_cond = PTHREAD_COND_INITIALIZER;
static struct overlay_input_queue_event overlay_input_events[OVERLAY_INPUT_QUEUE_SIZE];
static unsigned int overlay_input_head, overlay_input_count;
static unsigned int overlay_focus_serial;
static bool overlay_focus_valid;
static bool overlay_input_running;
static bool overlay_input_initialized;
static bool overlay_input_owned;
static bool overlay_authoritative_known;
static bool overlay_authoritative_active;
static bool overlay_state_update_running;
static unsigned int overlay_state_serial, overlay_applied_serial;
static const struct wine_wayland_external_input_api *overlay_input_api;
static Display *overlay_display;
static Window overlay_window;
static void *overlay_self_module;
static pthread_once_t trace_once = PTHREAD_ONCE_INIT;
static int trace_active;

static void init_trace(void)
{
    const char *env = getenv("WINELAND_VK_TRANSLATE_DEBUG");

    trace_active = env && *env != '0';
}

static int trace_enabled(void)
{
    pthread_once(&trace_once, init_trace);
    return trace_active;
}

#define TRACE(fmt, ...) do { \
    if (trace_enabled()) fprintf(stderr, "wineland-overlay-input[%ld]: " fmt, \
                                 (long)getpid(), ##__VA_ARGS__); \
} while (0)

static bool overlay_active_locked(void)
{
    if (overlay_authoritative_known) return overlay_authoritative_active;
    return overlay_input_owned;
}

static bool load_overlay_x11_funcs(struct overlay_x11_funcs *funcs)
{
    Dl_info info = {0};

    if (!(funcs->x11_module = dlopen("libX11.so.6", RTLD_NOW | RTLD_LOCAL)))
    {
        TRACE("could not open libX11: %s\n", dlerror());
        return false;
    }

#define LOAD_X11_FUNC(member, symbol) \
    funcs->member = (__typeof__(funcs->member))dlsym(funcs->x11_module, symbol)
    LOAD_X11_FUNC(lock_display, "XLockDisplay");
    LOAD_X11_FUNC(unlock_display, "XUnlockDisplay");
    LOAD_X11_FUNC(default_root_window, "XDefaultRootWindow");
    LOAD_X11_FUNC(put_back_event, "_XPutBackEvent");
#undef LOAD_X11_FUNC
    funcs->check_if_event = (__typeof__(funcs->check_if_event))
            dlsym(RTLD_DEFAULT, "XCheckIfEvent");

    if (!funcs->lock_display || !funcs->unlock_display ||
        !funcs->default_root_window || !funcs->put_back_event ||
        !funcs->check_if_event)
    {
        TRACE("could not resolve the X11 entry points\n");
        dlclose(funcs->x11_module);
        funcs->x11_module = NULL;
        return false;
    }
    if (!dladdr((void *)funcs->check_if_event, &info) || !info.dli_fname ||
        !strstr(info.dli_fname, "gameoverlayrenderer.so"))
    {
        TRACE("XCheckIfEvent is not interposed by gameoverlayrenderer.so (%s)\n",
              info.dli_fname ? info.dli_fname : "unknown");
        dlclose(funcs->x11_module);
        funcs->x11_module = NULL;
        return false;
    }

    TRACE("using XCheckIfEvent interposed by %s\n", info.dli_fname);
    return true;
}

static const struct wine_wayland_external_input_api *load_wayland_external_input(
        void **module)
{
    const struct wine_wayland_external_input_api *api;

    if (!(*module = dlopen("winewayland.so", RTLD_NOW | RTLD_NOLOAD))) return NULL;
    api = (const struct wine_wayland_external_input_api *)
            dlsym(*module, WINE_WAYLAND_EXTERNAL_INPUT_SYMBOL);
    if (!api || api->size < sizeof(*api) ||
        api->version != WINE_WAYLAND_EXTERNAL_INPUT_VERSION ||
        !api->set_handler || !api->set_active)
    {
        dlclose(*module);
        *module = NULL;
        return NULL;
    }
    return api;
}

static Bool match_overlay_event(Display *display, XEvent *event, XPointer arg)
{
    const struct overlay_event_match *match = (const struct overlay_event_match *)arg;

    (void)display;
    return event->xany.window == match->window &&
           event->xany.serial == match->serial && event->type == match->type;
}

static bool send_overlay_event(const struct overlay_x11_funcs *funcs, Display *display,
                               Window proxy, XEvent *event, unsigned long *serial)
{
    struct overlay_event_match match;
    XEvent returned;

    event->xany.display = display;
    event->xany.window = proxy;
    event->xany.send_event = False;
    event->xany.serial = ++*serial;
    match.window = proxy;
    match.serial = event->xany.serial;
    match.type = event->type;

    funcs->lock_display(display);
    funcs->put_back_event(display, event);
    funcs->unlock_display(display);
    return !funcs->check_if_event(display, &returned, match_overlay_event,
                                  (XPointer)&match);
}

static bool send_overlay_focus_event(const struct overlay_x11_funcs *funcs,
                                     Display *display, Window proxy,
                                     unsigned long *serial, bool focused)
{
    XEvent event = {0};

    event.type = focused ? FocusIn : FocusOut;
    event.xfocus.mode = NotifyNormal;
    event.xfocus.detail = NotifyNonlinear;
    return send_overlay_event(funcs, display, proxy, &event, serial);
}

static unsigned int overlay_button_from_linux(unsigned int code)
{
    switch (code)
    {
    case BTN_LEFT: return Button1;
    case BTN_MIDDLE: return Button2;
    case BTN_RIGHT: return Button3;
    case BTN_SIDE:
    case BTN_BACK: return 8;
    case BTN_EXTRA:
    case BTN_FORWARD: return 9;
    default: return 0;
    }
}

static unsigned int overlay_button_mask(unsigned int button)
{
    switch (button)
    {
    case Button1: return Button1Mask;
    case Button2: return Button2Mask;
    case Button3: return Button3Mask;
    case Button4: return Button4Mask;
    case Button5: return Button5Mask;
    default: return 0;
    }
}

static void update_overlay_modifier_state(struct overlay_pointer_state *state,
                                          unsigned int key, bool pressed)
{
    unsigned int mask = 0;

    switch (key)
    {
    case KEY_LEFTSHIFT:
    case KEY_RIGHTSHIFT: mask = ShiftMask; break;
    case KEY_LEFTCTRL:
    case KEY_RIGHTCTRL: mask = ControlMask; break;
    case KEY_LEFTALT:
    case KEY_RIGHTALT: mask = Mod1Mask; break;
    case KEY_LEFTMETA:
    case KEY_RIGHTMETA: mask = Mod4Mask; break;
    case KEY_CAPSLOCK:
        if (pressed) state->modifiers ^= LockMask;
        return;
    case KEY_NUMLOCK:
        if (pressed) state->modifiers ^= Mod2Mask;
        return;
    default: return;
    }

    if (pressed) state->modifiers |= mask;
    else state->modifiers &= ~mask;
}

static bool dispatch_overlay_input_event(const struct overlay_x11_funcs *funcs,
                                         Display *display, Window root, Window proxy,
                                         unsigned long *serial,
                                         struct overlay_pointer_state *state,
                                         const struct wine_wayland_external_input_event *input)
{
    XEvent event = {0};
    unsigned int button, count;
    long value;
    bool consumed, pressed;

    if (input->width > 0 && input->height > 0)
    {
        state->width = input->width;
        state->height = input->height;
    }
    if (input->flags & WINE_WAYLAND_EXTERNAL_INPUT_ABSOLUTE)
    {
        state->x = input->x;
        state->y = input->y;
        state->positioned = true;
    }
    if (input->type != WINE_WAYLAND_EXTERNAL_INPUT_KEY &&
        input->type != WINE_WAYLAND_EXTERNAL_INPUT_FOCUS &&
        (!state->width || !state->height)) return false;

    switch (input->type)
    {
    case WINE_WAYLAND_EXTERNAL_INPUT_POINTER_MOTION:
        if (!(input->flags & WINE_WAYLAND_EXTERNAL_INPUT_ABSOLUTE) && !state->positioned)
        {
            state->x = state->width / 2;
            state->y = state->height / 2;
            state->positioned = true;
        }
        if ((input->flags & WINE_WAYLAND_EXTERNAL_INPUT_RELATIVE) &&
            !(input->flags & WINE_WAYLAND_EXTERNAL_INPUT_ABSOLUTE))
        {
            state->x += input->dx;
            state->y += input->dy;
        }
        if (state->x < 0) state->x = 0;
        else if (state->x >= state->width) state->x = state->width - 1;
        if (state->y < 0) state->y = 0;
        else if (state->y >= state->height) state->y = state->height - 1;

        event.xmotion.type = MotionNotify;
        event.xmotion.root = root;
        event.xmotion.time = input->time;
        event.xmotion.x = event.xmotion.x_root = state->x;
        event.xmotion.y = event.xmotion.y_root = state->y;
        event.xmotion.state = state->modifiers | state->buttons;
        event.xmotion.same_screen = True;
        return send_overlay_event(funcs, display, proxy, &event, serial);

    case WINE_WAYLAND_EXTERNAL_INPUT_POINTER_BUTTON:
        if (!(button = overlay_button_from_linux(input->code))) return false;
        pressed = input->state != 0;
        event.xbutton.type = pressed ? ButtonPress : ButtonRelease;
        event.xbutton.root = root;
        event.xbutton.time = input->time;
        event.xbutton.x = event.xbutton.x_root = state->x;
        event.xbutton.y = event.xbutton.y_root = state->y;
        event.xbutton.state = state->modifiers | state->buttons;
        event.xbutton.button = button;
        event.xbutton.same_screen = True;
        consumed = send_overlay_event(funcs, display, proxy, &event, serial);
        if (pressed) state->buttons |= overlay_button_mask(button);
        else state->buttons &= ~overlay_button_mask(button);
        return consumed;

    case WINE_WAYLAND_EXTERNAL_INPUT_POINTER_AXIS:
        if (!input->value) return true;
        if (input->code == 0)
            button = input->value > 0 ? Button4 : Button5;
        else
            button = input->value > 0 ? 7 : 6;
        value = input->value;
        if (value < 0) value = -value;
        count = (value + 119) / 120;
        if (count > 32) count = 32;
        while (count--)
        {
            event.xbutton.type = ButtonPress;
            event.xbutton.root = root;
            event.xbutton.time = input->time;
            event.xbutton.x = event.xbutton.x_root = state->x;
            event.xbutton.y = event.xbutton.y_root = state->y;
            event.xbutton.state = state->modifiers | state->buttons;
            event.xbutton.button = button;
            event.xbutton.same_screen = True;
            if (!send_overlay_event(funcs, display, proxy, &event, serial)) return false;
            event.xbutton.type = ButtonRelease;
            if (!send_overlay_event(funcs, display, proxy, &event, serial)) return false;
        }
        return true;

    case WINE_WAYLAND_EXTERNAL_INPUT_KEY:
        pressed = input->state != 0;
        event.xkey.type = pressed ? KeyPress : KeyRelease;
        event.xkey.root = root;
        event.xkey.time = input->time;
        event.xkey.x = event.xkey.x_root = state->x;
        event.xkey.y = event.xkey.y_root = state->y;
        event.xkey.state = state->modifiers | state->buttons;
        event.xkey.keycode = input->code + 8;
        event.xkey.same_screen = True;
        consumed = send_overlay_event(funcs, display, proxy, &event, serial);
        update_overlay_modifier_state(state, input->code, pressed);
        return consumed;

    case WINE_WAYLAND_EXTERNAL_INPUT_FOCUS:
        if (!input->state) state->modifiers = state->buttons = 0;
        return send_overlay_focus_event(funcs, display, proxy, serial,
                                        input->state != 0);
    }

    return false;
}

static void sync_overlay_active(void)
{
    const struct wine_wayland_external_input_api *api;
    unsigned int serial;
    bool active, updated;

    pthread_mutex_lock(&overlay_input_mutex);
    if (!(api = overlay_input_api) || overlay_state_update_running ||
        overlay_applied_serial == overlay_state_serial)
    {
        pthread_mutex_unlock(&overlay_input_mutex);
        return;
    }
    overlay_state_update_running = true;

    for (;;)
    {
        serial = overlay_state_serial;
        active = overlay_active_locked();
        pthread_mutex_unlock(&overlay_input_mutex);

        updated = api->set_active(active);

        pthread_mutex_lock(&overlay_input_mutex);
        if (updated) overlay_applied_serial = serial;
        if (!updated || overlay_applied_serial == overlay_state_serial)
        {
            overlay_state_update_running = false;
            pthread_mutex_unlock(&overlay_input_mutex);
            return;
        }
    }
}

static bool event_updates_input_ownership(
        const struct wine_wayland_external_input_event *event)
{
    switch (event->type)
    {
    case WINE_WAYLAND_EXTERNAL_INPUT_POINTER_MOTION:
    case WINE_WAYLAND_EXTERNAL_INPUT_POINTER_BUTTON:
    case WINE_WAYLAND_EXTERNAL_INPUT_POINTER_AXIS:
        return true;
    case WINE_WAYLAND_EXTERNAL_INPUT_KEY:
        /* Releases following the activation shortcut are not consistently
         * retained by Steam and therefore cannot relinquish ownership. */
        return event->state != 0;
    default:
        return false;
    }
}

static int queue_overlay_input_event(void *context,
                                     const struct wine_wayland_external_input_event *event)
{
    struct overlay_input_completion completion = {0};
    struct overlay_input_queue_event *queued;
    unsigned int tail;
    bool active, wait;

    (void)context;
    if (event->size < sizeof(*event)) return 0;

    /* Apply ownership on WineWayland's event thread. set_handler() emits its
     * initial focus notification on the native worker, so skip it here. */
    if (event->type != WINE_WAYLAND_EXTERNAL_INPUT_FOCUS)
        sync_overlay_active();

    pthread_mutex_lock(&overlay_input_mutex);
    active = overlay_active_locked();
    wait = event->type == WINE_WAYLAND_EXTERNAL_INPUT_KEY && !active;

    if (event->type == WINE_WAYLAND_EXTERNAL_INPUT_FOCUS)
    {
        if (overlay_focus_valid && (int32_t)(event->code - overlay_focus_serial) <= 0)
        {
            pthread_mutex_unlock(&overlay_input_mutex);
            return 0;
        }
        overlay_focus_valid = true;
        overlay_focus_serial = event->code;
    }
    else if (event->type != WINE_WAYLAND_EXTERNAL_INPUT_KEY && !active)
    {
        pthread_mutex_unlock(&overlay_input_mutex);
        return 0;
    }

    if (event->type == WINE_WAYLAND_EXTERNAL_INPUT_POINTER_MOTION && overlay_input_count)
    {
        tail = (overlay_input_head + overlay_input_count - 1) % ARRAY_SIZE(overlay_input_events);
        queued = &overlay_input_events[tail];
        if (queued->event.type == event->type && !queued->completion)
        {
            if (event->flags & WINE_WAYLAND_EXTERNAL_INPUT_ABSOLUTE)
            {
                queued->event.flags |= WINE_WAYLAND_EXTERNAL_INPUT_ABSOLUTE;
                queued->event.x = event->x;
                queued->event.y = event->y;
            }
            if (event->flags & WINE_WAYLAND_EXTERNAL_INPUT_RELATIVE)
            {
                queued->event.flags |= WINE_WAYLAND_EXTERNAL_INPUT_RELATIVE;
                queued->event.dx += event->dx;
                queued->event.dy += event->dy;
            }
            queued->event.width = event->width;
            queued->event.height = event->height;
            pthread_cond_signal(&overlay_input_cond);
            pthread_mutex_unlock(&overlay_input_mutex);
            return active;
        }
    }

    if (overlay_input_count == ARRAY_SIZE(overlay_input_events))
    {
        pthread_mutex_unlock(&overlay_input_mutex);
        return active;
    }

    tail = (overlay_input_head + overlay_input_count++) % ARRAY_SIZE(overlay_input_events);
    overlay_input_events[tail].event = *event;
    overlay_input_events[tail].completion = wait ? &completion : NULL;
    pthread_cond_signal(&overlay_input_cond);

    /* Steam's hook must not wait for the WineWayland event thread. */
    while (wait && !completion.done && overlay_input_running)
        pthread_cond_wait(&overlay_input_cond, &overlay_input_mutex);
    if (wait && completion.done) active = completion.handled;
    pthread_mutex_unlock(&overlay_input_mutex);

    /* Apply ownership changed by the synchronous key probe. */
    if (wait) sync_overlay_active();
    return active;
}

static void *overlay_input_thread(void *arg)
{
    struct overlay_x11_funcs funcs = {0};
    struct wine_wayland_external_input_handler handler =
    {
        sizeof(handler), WINE_WAYLAND_EXTERNAL_INPUT_VERSION, NULL,
        queue_overlay_input_event,
    };
    struct overlay_pointer_state pointer_state = {0};
    const struct wine_wayland_external_input_api *input_api;
    Display *display;
    void *wayland_module = NULL;
    Window root, proxy;
    unsigned long serial = 0;
    bool handler_registered = false;

    (void)arg;
    pthread_mutex_lock(&overlay_input_mutex);
    display = overlay_display;
    proxy = overlay_window;
    pthread_mutex_unlock(&overlay_input_mutex);

    if (!(input_api = load_wayland_external_input(&wayland_module)))
    {
        TRACE("WineWayland external input interface is unavailable\n");
        goto failed;
    }
    if (!load_overlay_x11_funcs(&funcs)) goto failed;
    root = funcs.default_root_window(display);

    if (!input_api->set_handler(&handler))
    {
        TRACE("WineWayland external input listener is unavailable\n");
        goto failed;
    }
    handler_registered = true;

    pthread_mutex_lock(&overlay_input_mutex);
    overlay_input_api = input_api;
    overlay_state_serial++;
    overlay_input_initialized = true;
    pthread_cond_broadcast(&overlay_input_cond);
    pthread_mutex_unlock(&overlay_input_mutex);
    TRACE("registered for proxy window 0x%lx\n", (unsigned long)proxy);

    for (;;)
    {
        struct overlay_input_queue_event queued;
        bool active, consumed;

        pthread_mutex_lock(&overlay_input_mutex);
        while (!overlay_input_count)
            pthread_cond_wait(&overlay_input_cond, &overlay_input_mutex);
        queued = overlay_input_events[overlay_input_head];
        overlay_input_head = (overlay_input_head + 1) % ARRAY_SIZE(overlay_input_events);
        overlay_input_count--;
        pthread_mutex_unlock(&overlay_input_mutex);

        consumed = dispatch_overlay_input_event(&funcs, display, root, proxy, &serial,
                                                &pointer_state, &queued.event);
        pthread_mutex_lock(&overlay_input_mutex);
        if (event_updates_input_ownership(&queued.event) &&
            overlay_input_owned != consumed)
        {
            overlay_input_owned = consumed;
            overlay_state_serial++;
        }
        active = overlay_active_locked();
        pthread_mutex_unlock(&overlay_input_mutex);

        TRACE("event type %u code %u state %u consumed=%u active=%u\n",
              queued.event.type, queued.event.code, queued.event.state,
              consumed, active);
        if (queued.completion)
        {
            pthread_mutex_lock(&overlay_input_mutex);
            queued.completion->handled = consumed || active;
            queued.completion->done = true;
            pthread_cond_broadcast(&overlay_input_cond);
            pthread_mutex_unlock(&overlay_input_mutex);
        }
    }

failed:
    pthread_mutex_lock(&overlay_input_mutex);
    overlay_input_running = false;
    overlay_input_initialized = true;
    overlay_input_head = overlay_input_count = 0;
    overlay_focus_valid = false;
    overlay_input_api = NULL;
    pthread_cond_broadcast(&overlay_input_cond);
    pthread_mutex_unlock(&overlay_input_mutex);
    if (handler_registered) input_api->set_handler(NULL);
    if (funcs.x11_module) dlclose(funcs.x11_module);
    if (wayland_module) dlclose(wayland_module);
    return NULL;
}

void wineland_overlay_client_start(void *display, unsigned long window)
{
    Dl_info info = {0};
    pthread_t thread;

    if (!display || !window) return;

    pthread_mutex_lock(&overlay_input_mutex);
    if (overlay_input_running)
    {
        while (!overlay_input_initialized)
            pthread_cond_wait(&overlay_input_cond, &overlay_input_mutex);
        pthread_mutex_unlock(&overlay_input_mutex);
        return;
    }

    /* The detached event thread lives for the process lifetime, so retain the
     * layer containing it even if the originating Vulkan instance is freed. */
    if (!overlay_self_module &&
        (!dladdr((void *)wineland_overlay_client_start, &info) || !info.dli_fname ||
         !(overlay_self_module = dlopen(info.dli_fname,
                                       RTLD_NOW | RTLD_LOCAL | RTLD_NOLOAD))))
    {
        pthread_mutex_unlock(&overlay_input_mutex);
        TRACE("could not retain the translation layer\n");
        return;
    }

    overlay_display = display;
    overlay_window = window;
    overlay_input_running = true;
    overlay_input_initialized = false;
    if (pthread_create(&thread, NULL, overlay_input_thread, NULL))
    {
        overlay_input_running = false;
        pthread_mutex_unlock(&overlay_input_mutex);
        TRACE("could not create the input thread\n");
        return;
    }
    pthread_detach(thread);
    while (overlay_input_running && !overlay_input_initialized)
        pthread_cond_wait(&overlay_input_cond, &overlay_input_mutex);
    pthread_mutex_unlock(&overlay_input_mutex);
}

__attribute__((visibility("default")))
void __wineland_overlay_client_set_active_v1(int active)
{
    pthread_mutex_lock(&overlay_input_mutex);
    active = active != 0;
    if (active)
        overlay_authoritative_known = true;
    else if (!overlay_authoritative_known)
    {
        pthread_mutex_unlock(&overlay_input_mutex);
        return;
    }

    overlay_authoritative_active = active;
    if (!active) overlay_input_owned = false;
    overlay_state_serial++;
    pthread_cond_signal(&overlay_input_cond);
    pthread_mutex_unlock(&overlay_input_mutex);

    /* lsteamclient invokes this on a Wine callback thread. */
    sync_overlay_active();
    TRACE("authoritative state is %s\n", active ? "active" : "inactive");
}
