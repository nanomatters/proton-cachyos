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
    unsigned int source_serial;
};

struct overlay_pointer_state
{
    int x, y, width, height;
    unsigned int modifiers, buttons;
    bool positioned;
};

enum overlay_input_ownership
{
    OVERLAY_INPUT_INACTIVE,
    OVERLAY_INPUT_ACTIVATING,
    OVERLAY_INPUT_ACTIVE,
};

enum overlay_event_result
{
    OVERLAY_EVENT_UNDELIVERED = -1,
    OVERLAY_EVENT_DECLINED = 0,
    OVERLAY_EVENT_CONSUMED = 1,
};

static pthread_mutex_t overlay_input_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t overlay_input_cond = PTHREAD_COND_INITIALIZER;
static struct overlay_input_queue_event overlay_input_events[OVERLAY_INPUT_QUEUE_SIZE];
static unsigned int overlay_input_head, overlay_input_count;
static unsigned int overlay_focus_serial;
static bool overlay_focus_valid;
static bool overlay_input_running;
static bool overlay_input_initialized;
static _Thread_local bool overlay_input_worker;
static unsigned int overlay_input_sources, overlay_source_serial;
static enum overlay_input_ownership overlay_input_ownership;
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
    if (!overlay_input_sources) return false;
    if (overlay_authoritative_known) return overlay_authoritative_active;
    return overlay_input_ownership != OVERLAY_INPUT_INACTIVE;
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

static int dispatch_overlay_input_event(const struct overlay_x11_funcs *funcs,
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
        (!state->width || !state->height)) return OVERLAY_EVENT_UNDELIVERED;

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
        if (!(button = overlay_button_from_linux(input->code))) return OVERLAY_EVENT_UNDELIVERED;
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
        if (!input->value) return OVERLAY_EVENT_UNDELIVERED;
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

    return OVERLAY_EVENT_UNDELIVERED;
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

static void update_input_ownership_locked(
        const struct wine_wayland_external_input_event *event, int result)
{
    enum overlay_input_ownership previous = overlay_input_ownership;
    bool consumed = result == OVERLAY_EVENT_CONSUMED;

    if (!overlay_input_sources || overlay_authoritative_known ||
        result == OVERLAY_EVENT_UNDELIVERED) return;

    switch (event->type)
    {
    case WINE_WAYLAND_EXTERNAL_INPUT_POINTER_MOTION:
    case WINE_WAYLAND_EXTERNAL_INPUT_POINTER_AXIS:
        /* Steam may consume the activation shortcut before its pointer path
         * is ready. Do not let passive pointer traffic undo activation;
         * the first consumed pointer event completes the transition. */
        if (overlay_input_ownership == OVERLAY_INPUT_ACTIVATING)
        {
            if (consumed) overlay_input_ownership = OVERLAY_INPUT_ACTIVE;
        }
        else
        {
            overlay_input_ownership = consumed ? OVERLAY_INPUT_ACTIVE :
                                                 OVERLAY_INPUT_INACTIVE;
        }
        break;

    case WINE_WAYLAND_EXTERNAL_INPUT_POINTER_BUTTON:
        if (overlay_input_ownership == OVERLAY_INPUT_ACTIVATING)
        {
            if (consumed)
                overlay_input_ownership = OVERLAY_INPUT_ACTIVE;
            /* Only a delivered, declined press cancels pending ownership.
             * Missing geometry or an unsupported button says nothing about Steam. */
            else if (event->state)
                overlay_input_ownership = OVERLAY_INPUT_INACTIVE;
        }
        else
        {
            overlay_input_ownership = consumed ? OVERLAY_INPUT_ACTIVE :
                                                 OVERLAY_INPUT_INACTIVE;
        }
        break;

    case WINE_WAYLAND_EXTERNAL_INPUT_KEY:
        /* Releases following the activation shortcut are not consistently
         * retained by Steam and therefore cannot relinquish ownership. */
        if (!event->state) break;
        if (consumed)
        {
            if (overlay_input_ownership == OVERLAY_INPUT_INACTIVE)
                overlay_input_ownership = OVERLAY_INPUT_ACTIVATING;
        }
        else
        {
            overlay_input_ownership = OVERLAY_INPUT_INACTIVE;
        }
        break;

    case WINE_WAYLAND_EXTERNAL_INPUT_FOCUS:
        if (!event->state &&
            overlay_input_ownership == OVERLAY_INPUT_ACTIVATING)
            overlay_input_ownership = OVERLAY_INPUT_INACTIVE;
        break;

    default:
        break;
    }

    if ((previous != OVERLAY_INPUT_INACTIVE) !=
        (overlay_input_ownership != OVERLAY_INPUT_INACTIVE))
        overlay_state_serial++;
}

static int queue_overlay_input_event(void *context,
                                     const struct wine_wayland_external_input_event *event)
{
    struct overlay_input_completion completion = {0};
    struct overlay_input_queue_event *queued;
    unsigned int tail, source_serial;
    bool active, wait;

    (void)context;
    if (event->size < sizeof(*event)) return 0;

    /* set_handler() emits initial focus on our native worker. It must neither
     * wait for itself nor invoke Wine's cursor APIs. Live focus runs in Wine. */
    if (!overlay_input_worker) sync_overlay_active();

    pthread_mutex_lock(&overlay_input_mutex);
    if (!overlay_input_sources || !overlay_input_running)
    {
        pthread_mutex_unlock(&overlay_input_mutex);
        return 0;
    }
    active = overlay_active_locked();
    source_serial = overlay_source_serial;
    wait = !overlay_input_worker &&
           (event->type == WINE_WAYLAND_EXTERNAL_INPUT_KEY ||
            event->type == WINE_WAYLAND_EXTERNAL_INPUT_POINTER_BUTTON ||
            event->type == WINE_WAYLAND_EXTERNAL_INPUT_FOCUS);

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
        if (queued->event.type == event->type && !queued->completion &&
            queued->source_serial == overlay_source_serial)
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

    /* Preserve discrete events even if coalesced pointer traffic fills the queue. */
    while (wait && overlay_input_count == ARRAY_SIZE(overlay_input_events) && overlay_input_running)
        pthread_cond_wait(&overlay_input_cond, &overlay_input_mutex);
    if (!overlay_input_running || !overlay_input_sources || source_serial != overlay_source_serial)
    {
        pthread_mutex_unlock(&overlay_input_mutex);
        return 0;
    }
    if (overlay_input_count == ARRAY_SIZE(overlay_input_events))
    {
        pthread_mutex_unlock(&overlay_input_mutex);
        return active;
    }

    tail = (overlay_input_head + overlay_input_count++) % ARRAY_SIZE(overlay_input_events);
    overlay_input_events[tail].event = *event;
    overlay_input_events[tail].completion = wait ? &completion : NULL;
    overlay_input_events[tail].source_serial = overlay_source_serial;
    pthread_cond_signal(&overlay_input_cond);

    /* Steam's hook must not wait for the WineWayland event thread. */
    while (wait && !completion.done && overlay_input_running)
        pthread_cond_wait(&overlay_input_cond, &overlay_input_mutex);
    if (wait && completion.done) active = completion.handled;
    pthread_mutex_unlock(&overlay_input_mutex);

    /* Restore cursor/clipping before a declined press falls through to Wine. */
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
    unsigned int pointer_source_serial = 0;
    bool handler_registered = false;

    (void)arg;
    overlay_input_worker = true;
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
        bool active, pending;
        int result;

        pthread_mutex_lock(&overlay_input_mutex);
        while (!overlay_input_count)
            pthread_cond_wait(&overlay_input_cond, &overlay_input_mutex);
        queued = overlay_input_events[overlay_input_head];
        overlay_input_head = (overlay_input_head + 1) % ARRAY_SIZE(overlay_input_events);
        overlay_input_count--;
        if (overlay_input_count == ARRAY_SIZE(overlay_input_events) - 1)
            pthread_cond_broadcast(&overlay_input_cond);
        if (!overlay_input_sources || queued.source_serial != overlay_source_serial)
        {
            if (queued.completion)
            {
                queued.completion->handled = false;
                queued.completion->done = true;
                pthread_cond_broadcast(&overlay_input_cond);
            }
            pthread_mutex_unlock(&overlay_input_mutex);
            continue;
        }
        pthread_mutex_unlock(&overlay_input_mutex);

        if (pointer_source_serial != queued.source_serial)
        {
            memset(&pointer_state, 0, sizeof(pointer_state));
            pointer_source_serial = queued.source_serial;
        }
        result = dispatch_overlay_input_event(&funcs, display, root, proxy, &serial,
                                              &pointer_state, &queued.event);
        pthread_mutex_lock(&overlay_input_mutex);
        if (!overlay_input_sources || queued.source_serial != overlay_source_serial)
            result = OVERLAY_EVENT_UNDELIVERED;
        update_input_ownership_locked(&queued.event, result);
        active = overlay_active_locked();
        pending = overlay_input_ownership == OVERLAY_INPUT_ACTIVATING;
        if (queued.completion)
        {
            queued.completion->handled = result != OVERLAY_EVENT_UNDELIVERED &&
                                          (result == OVERLAY_EVENT_CONSUMED || active);
            queued.completion->done = true;
            pthread_cond_broadcast(&overlay_input_cond);
        }
        pthread_mutex_unlock(&overlay_input_mutex);

        TRACE("event type %u code %u state %u delivered=%u consumed=%u active=%u pending=%u\n",
              queued.event.type, queued.event.code, queued.event.state,
              result != OVERLAY_EVENT_UNDELIVERED, result == OVERLAY_EVENT_CONSUMED,
              active, pending);
    }

failed:
    pthread_mutex_lock(&overlay_input_mutex);
    overlay_input_running = false;
    overlay_input_sources = 0;
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

int __wineland_overlay_client_start_v1(void *display, unsigned long window, unsigned int source)
{
    Dl_info info = {0};
    pthread_t thread;
    bool ready;

    if (!display || !window ||
        (source != WINELAND_OVERLAY_INPUT_OPENGL && source != WINELAND_OVERLAY_INPUT_VULKAN))
        return 0;

    pthread_mutex_lock(&overlay_input_mutex);
    if (overlay_input_running)
    {
        while (!overlay_input_initialized)
            pthread_cond_wait(&overlay_input_cond, &overlay_input_mutex);
        ready = overlay_input_api != NULL;
        if (ready) overlay_input_sources |= source;
        pthread_mutex_unlock(&overlay_input_mutex);
        return ready;
    }

    /* The detached event thread lives for the process lifetime, so retain the
     * layer containing it even if the originating Vulkan instance is freed. */
    if (!overlay_self_module &&
        (!dladdr((void *)__wineland_overlay_client_start_v1, &info) || !info.dli_fname ||
         !(overlay_self_module = dlopen(info.dli_fname,
                                       RTLD_NOW | RTLD_LOCAL | RTLD_NOLOAD))))
    {
        pthread_mutex_unlock(&overlay_input_mutex);
        TRACE("could not retain the translation layer\n");
        return 0;
    }

    overlay_display = display;
    overlay_window = window;
    overlay_input_running = true;
    overlay_input_initialized = false;
    overlay_input_sources = source;
    if (pthread_create(&thread, NULL, overlay_input_thread, NULL))
    {
        overlay_input_running = false;
        overlay_input_sources = 0;
        pthread_mutex_unlock(&overlay_input_mutex);
        TRACE("could not create the input thread\n");
        return 0;
    }
    pthread_detach(thread);
    while (overlay_input_running && !overlay_input_initialized)
        pthread_cond_wait(&overlay_input_cond, &overlay_input_mutex);
    ready = overlay_input_api != NULL;
    pthread_mutex_unlock(&overlay_input_mutex);
    return ready;
}

void __wineland_overlay_client_stop_v1(unsigned int source)
{
    pthread_mutex_lock(&overlay_input_mutex);
    overlay_input_sources &= ~source;
    if (!overlay_input_sources)
    {
        /* In-flight events from the failed renderer must not reacquire input. */
        overlay_source_serial++;
        overlay_input_ownership = OVERLAY_INPUT_INACTIVE;
        overlay_authoritative_active = false;
        overlay_state_serial++;
    }
    pthread_mutex_unlock(&overlay_input_mutex);

    /* The GL bridge calls this from Wine's swap thread, never the native worker. */
    sync_overlay_active();
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
    overlay_input_ownership = active ? OVERLAY_INPUT_ACTIVE : OVERLAY_INPUT_INACTIVE;
    overlay_state_serial++;
    pthread_cond_signal(&overlay_input_cond);
    pthread_mutex_unlock(&overlay_input_mutex);

    /* lsteamclient invokes this on a Wine callback thread. */
    sync_overlay_active();
    TRACE("authoritative state is %s\n", active ? "active" : "inactive");
}
