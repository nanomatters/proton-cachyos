/*
 * Isolated input-client tests with the real queue and worker, mocked Wine/X11.
 * cc -std=gnu11 -Iwine/include vklayers/test_overlay_input.c -pthread -ldl -o test-overlay-input
 */
#define dlopen mock_dlopen
#define dlclose mock_dlclose
#define dlsym mock_dlsym
#define dladdr mock_dladdr
#include "wineland_overlay_client.c"
#undef dlopen
#undef dlclose
#undef dlsym
#undef dladdr
#include <assert.h>

static pthread_mutex_t mock_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t mock_cond = PTHREAD_COND_INITIALIZER;
static XEvent sent_event;
static bool steam_consumes, block_dispatch, dispatch_waiting;
static unsigned int events_sent, focus_serial;
static int wine_active;
static char module_token, display_token;

static int mock_set_active(int active)
{
    /* No Wine cursor calls from the native worker, including initial focus. */
    assert(!overlay_input_worker);
    wine_active = active;
    return 1;
}

static int mock_set_handler(const struct wine_wayland_external_input_handler *handler)
{
    const struct wine_wayland_external_input_event initial_focus =
    {
        .size = sizeof(initial_focus), .type = WINE_WAYLAND_EXTERNAL_INPUT_FOCUS,
        .state = 1,
    };
    assert(overlay_input_worker);
    assert(handler);
    handler->event(handler->context, &initial_focus);
    return 1;
}

static const struct wine_wayland_external_input_api mock_api =
{
    sizeof(mock_api), WINE_WAYLAND_EXTERNAL_INPUT_VERSION, mock_set_handler, mock_set_active,
};

static void mock_lock_display(Display *display) { (void)display; }
static Window mock_root(Display *display) { (void)display; return 1; }
static int mock_put_back(Display *display, XEvent *event)
{
    (void)display;
    sent_event = *event;
    return 0;
}

static Bool mock_check_event(Display *display, XEvent *event,
                            Bool (*predicate)(Display *, XEvent *, XPointer), XPointer arg)
{
    bool consumed;

    assert(overlay_input_worker);
    assert(predicate(display, &sent_event, arg));
    pthread_mutex_lock(&mock_mutex);
    events_sent++;
    dispatch_waiting = true;
    pthread_cond_broadcast(&mock_cond);
    while (block_dispatch) pthread_cond_wait(&mock_cond, &mock_mutex);
    consumed = steam_consumes;
    dispatch_waiting = false;
    *event = sent_event;
    pthread_mutex_unlock(&mock_mutex);
    return !consumed;
}

static void set_consumed(bool consumed)
{
    pthread_mutex_lock(&mock_mutex);
    steam_consumes = consumed;
    pthread_mutex_unlock(&mock_mutex);
}

void *mock_dlopen(const char *name, int flags)
{
    (void)name;
    (void)flags;
    return &module_token;
}
int mock_dlclose(void *module) { (void)module; return 0; }
int mock_dladdr(const void *addr, Dl_info *info)
{
    (void)addr;
    info->dli_fname = "/mock/gameoverlayrenderer.so";
    return 1;
}
void *mock_dlsym(void *module, const char *name)
{
    (void)module;
    if (!strcmp(name, WINE_WAYLAND_EXTERNAL_INPUT_SYMBOL)) return (void *)&mock_api;
    if (!strcmp(name, "XLockDisplay") || !strcmp(name, "XUnlockDisplay")) return mock_lock_display;
    if (!strcmp(name, "XDefaultRootWindow")) return mock_root;
    if (!strcmp(name, "_XPutBackEvent")) return mock_put_back;
    if (!strcmp(name, "XCheckIfEvent")) return mock_check_event;
    assert(!"unexpected symbol");
    return NULL;
}

static struct wine_wayland_external_input_event key =
{
    .size = sizeof(key), .type = WINE_WAYLAND_EXTERNAL_INPUT_KEY,
    .code = KEY_TAB, .state = 1,
};
static struct wine_wayland_external_input_event click =
{
    .size = sizeof(click), .type = WINE_WAYLAND_EXTERNAL_INPUT_POINTER_BUTTON,
    .flags = WINE_WAYLAND_EXTERNAL_INPUT_ABSOLUTE,
    .x = 120, .y = 80, .width = 800, .height = 600, .code = BTN_LEFT, .state = 1,
};
static struct wine_wayland_external_input_event motion =
{
    .size = sizeof(motion), .type = WINE_WAYLAND_EXTERNAL_INPUT_POINTER_MOTION,
    .flags = WINE_WAYLAND_EXTERNAL_INPUT_ABSOLUTE,
    .x = 120, .y = 80, .width = 800, .height = 600,
};

static void focus(bool active)
{
    const struct wine_wayland_external_input_event event =
    {
        .size = sizeof(event), .type = WINE_WAYLAND_EXTERNAL_INPUT_FOCUS,
        .code = ++focus_serial, .state = active,
    };
    queue_overlay_input_event(NULL, &event);
}

static void pending(void)
{
    focus(false);
    set_consumed(false);
    queue_overlay_input_event(NULL, &key);
    assert(!wine_active);
    focus(true);
    set_consumed(true);
    assert(queue_overlay_input_event(NULL, &key));
    assert(wine_active && overlay_input_ownership == OVERLAY_INPUT_ACTIVATING);
}

static void *send_click(void *arg)
{
    int *handled = arg;
    *handled = queue_overlay_input_event(NULL, &click);
    return NULL;
}

int main(void)
{
    struct wine_wayland_external_input_event event;
    struct overlay_pointer_state pointer = {0};
    struct overlay_x11_funcs funcs = {0};
    unsigned long serial = 0;
    pthread_t caller;
    unsigned int count;
    int handled, result;

    assert(__wineland_overlay_client_start_v1(&display_token, 2, WINELAND_OVERLAY_INPUT_OPENGL));
    focus(true); /* Also drains the native worker's initial focus notification. */
    assert(!wine_active);

    /* Opening and immediately clicking needs no preceding external motion. */
    pending();
    assert(queue_overlay_input_event(NULL, &click));
    assert(wine_active && overlay_input_ownership == OVERLAY_INPUT_ACTIVE);
    assert(sent_event.xbutton.x == 120 && sent_event.xbutton.y == 80);

    /* Motion during activation must not cancel it; a declined click must fall
     * through and restore Wine's cursor before the callback returns. */
    pending();
    set_consumed(false);
    event = key;
    event.state = 0;
    assert(queue_overlay_input_event(NULL, &event));
    assert(wine_active && overlay_input_ownership == OVERLAY_INPUT_ACTIVATING);
    assert(queue_overlay_input_event(NULL, &motion));
    focus(true); /* FIFO barrier. */
    assert(wine_active && overlay_input_ownership == OVERLAY_INPUT_ACTIVATING);
    assert(!queue_overlay_input_event(NULL, &click));
    assert(!wine_active && overlay_input_ownership == OVERLAY_INPUT_INACTIVE);
    event = click;
    event.state = 0;
    assert(!queue_overlay_input_event(NULL, &event));

    pending();
    focus(false);
    assert(!wine_active && overlay_input_ownership == OVERLAY_INPUT_INACTIVE);
    focus(true);
    assert(!wine_active);

    /* Keyboard-only close has no authoritative callback here. Its recovery key
     * must reach Wine and apply the state change without another event. */
    pending();
    assert(queue_overlay_input_event(NULL, &key));
    set_consumed(false);
    assert(!queue_overlay_input_event(NULL, &key));
    assert(!wine_active);

    /* Local delivery failures are not evidence that the overlay has closed. */
    pending();
    event = click;
    event.width = event.height = 0;
    result = dispatch_overlay_input_event(&funcs, NULL, 0, 0, &serial, &pointer, &event);
    assert(result == OVERLAY_EVENT_UNDELIVERED && !serial);
    pthread_mutex_lock(&overlay_input_mutex);
    update_input_ownership_locked(&event, result);
    pthread_mutex_unlock(&overlay_input_mutex);
    assert(wine_active && overlay_input_ownership == OVERLAY_INPUT_ACTIVATING);
    event = click;
    event.code = BTN_TASK;
    assert(!queue_overlay_input_event(NULL, &event));
    assert(wine_active && overlay_input_ownership == OVERLAY_INPUT_ACTIVATING);

    /* A failed GL renderer releases input and invalidates a consumed event
     * already inside Steam. Even a newly available renderer cannot revive it. */
    pthread_mutex_lock(&mock_mutex);
    block_dispatch = true;
    dispatch_waiting = false;
    pthread_mutex_unlock(&mock_mutex);
    assert(!pthread_create(&caller, NULL, send_click, &handled));
    pthread_mutex_lock(&mock_mutex);
    while (!dispatch_waiting) pthread_cond_wait(&mock_cond, &mock_mutex);
    pthread_mutex_unlock(&mock_mutex);
    __wineland_overlay_client_stop_v1(WINELAND_OVERLAY_INPUT_OPENGL);
    assert(!wine_active);
    count = events_sent;
    assert(!queue_overlay_input_event(NULL, &key));
    assert(events_sent == count);
    assert(__wineland_overlay_client_start_v1(&display_token, 2, WINELAND_OVERLAY_INPUT_VULKAN));
    pthread_mutex_lock(&mock_mutex);
    block_dispatch = false;
    pthread_cond_broadcast(&mock_cond);
    pthread_mutex_unlock(&mock_mutex);
    pthread_join(caller, NULL);
    assert(!handled && !wine_active);

    /* GL failure must not disable a working Vulkan consumer of the same client. */
    assert(__wineland_overlay_client_start_v1(&display_token, 2, WINELAND_OVERLAY_INPUT_OPENGL));
    pending();
    __wineland_overlay_client_stop_v1(WINELAND_OVERLAY_INPUT_OPENGL);
    assert(wine_active && overlay_input_sources == WINELAND_OVERLAY_INPUT_VULKAN);
    assert(queue_overlay_input_event(NULL, &click));
    assert(wine_active && overlay_input_ownership == OVERLAY_INPUT_ACTIVE);

    /* Authoritative Steamworks state must override the consumption heuristic. */
    __wineland_overlay_client_set_active_v1(1);
    set_consumed(false);
    assert(queue_overlay_input_event(NULL, &click));
    assert(wine_active);
    __wineland_overlay_client_set_active_v1(0);
    assert(!wine_active);
    __wineland_overlay_client_stop_v1(WINELAND_OVERLAY_INPUT_VULKAN);
    assert(!wine_active);

    puts("Overlay input client tests passed");
    return 0;
}
