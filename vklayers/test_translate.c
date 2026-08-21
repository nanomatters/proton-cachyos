#define _GNU_SOURCE
#define VK_USE_PLATFORM_XLIB_KHR
#define VK_USE_PLATFORM_WAYLAND_KHR
#include <dlfcn.h>
#include <limits.h>
#include <link.h>
#include <stdio.h>
#include <string.h>
#include <vulkan/vulkan.h>
#include <wayland-client.h>
#include <X11/Xlib.h>
#include "wine/wayland_vulkan_proxy.h"

static struct wl_compositor *compositor;
static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver)
{
    (void)d;
    (void)ver;
    if (!strcmp(iface, "wl_compositor"))
        compositor = wl_registry_bind(r, name, &wl_compositor_interface, 1);
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t name)
{
    (void)d;
    (void)r;
    (void)name;
}
static const struct wl_registry_listener reg_listener = { reg_global, reg_remove };

struct module_search
{
    const char *name;
    char path[PATH_MAX];
};

static int find_module(struct dl_phdr_info *info, size_t size, void *arg)
{
    struct module_search *search = arg;
    const char *name;

    (void)size;
    if (!(name = strrchr(info->dlpi_name, '/'))) name = info->dlpi_name;
    else name++;
    if (strcmp(name, search->name)) return 0;
    snprintf(search->path, sizeof(search->path), "%s", info->dlpi_name);
    return 1;
}

static void *get_register_surface(void)
{
#if defined(__x86_64__)
    struct module_search search = { .name = "libVkLayer_WINELAND_translate_x86_64.so" };
#else
    struct module_search search = { .name = "libVkLayer_WINELAND_translate_i386.so" };
#endif
    void *module;

    if (!dl_iterate_phdr(find_module, &search) ||
        !(module = dlopen(search.path, RTLD_NOW | RTLD_LOCAL | RTLD_NOLOAD)))
        return NULL;
    return dlsym(module, WINE_WAYLAND_VK_REGISTER_SURFACE_SYMBOL);
}

int main(void)
{
    struct wl_display *wl_dpy; struct wl_surface *wl_surf; struct wl_registry *reg;
    Display *xdpy; Window xwin; XSetWindowAttributes attrs = {0};
    VkInstance inst; VkSurfaceKHR surf; VkResult r;
    const char *exts[] = { "VK_KHR_surface", "VK_KHR_xlib_surface", "VK_KHR_wayland_surface" };
#if defined(__x86_64__)
    const char *layers[] = { "VK_LAYER_WINELAND_translate_x86_64" };
#else
    const char *layers[] = { "VK_LAYER_WINELAND_translate_i386" };
#endif
    VkApplicationInfo ai = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3 };
    VkInstanceCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai,
                                .enabledLayerCount = 1, .ppEnabledLayerNames = layers,
                                .enabledExtensionCount = 3, .ppEnabledExtensionNames = exts };

    if (!(wl_dpy = wl_display_connect(NULL))) { printf("FAIL: no wayland display\n"); return 1; }
    reg = wl_display_get_registry(wl_dpy);
    wl_registry_add_listener(reg, &reg_listener, NULL);
    wl_display_roundtrip(wl_dpy);
    if (!compositor) { printf("FAIL: no wl_compositor\n"); return 1; }
    wl_surf = wl_compositor_create_surface(compositor);
    printf("wayland: display=%p surface=%p\n", (void *)wl_dpy, (void *)wl_surf);

    if (!(xdpy = XOpenDisplay(NULL))) { printf("FAIL: no X display (XWayland running?)\n"); return 1; }
    attrs.override_redirect = True;
    xwin = XCreateWindow(xdpy, XDefaultRootWindow(xdpy), -10000, -10000, 32, 32, 0,
                         CopyFromParent, InputOutput, CopyFromParent, CWOverrideRedirect, &attrs);
    XMapWindow(xdpy, xwin); XSync(xdpy, False);
    printf("x11: proxy window 0x%lx\n", (unsigned long)xwin);

    if ((r = vkCreateInstance(&ci, NULL, &inst)) != VK_SUCCESS) { printf("FAIL: vkCreateInstance=%d\n", r); return 1; }

    {
        wine_wayland_vk_register_surface_func register_surface =
                (wine_wayland_vk_register_surface_func)get_register_surface();
        VkXlibSurfaceCreateInfoKHR xi = {
            .sType = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR,
            .dpy = xdpy, .window = xwin };
        PFN_vkCreateXlibSurfaceKHR create = (PFN_vkCreateXlibSurfaceKHR)
                vkGetInstanceProcAddr(inst, "vkCreateXlibSurfaceKHR");
        if (!register_surface || !create) { printf("FAIL: translator entry points unavailable\n"); return 1; }
        if (register_surface(inst, xdpy, xwin, wl_dpy, wl_surf) != VK_SUCCESS)
        { printf("FAIL: registration failed\n"); return 1; }
        r = create(inst, &xi, NULL, &surf);
        register_surface(inst, xdpy, xwin, NULL, NULL);
        printf("RESULT: vkCreateXlibSurfaceKHR = %d %s\n", r, r == VK_SUCCESS ? "OK" : "FAILED");
        if (r != VK_SUCCESS) return 1;
    }

    /* If translation worked, the surface must behave like a Wayland surface. */
    {
        uint32_t n = 0; VkPhysicalDevice pd[8]; VkBool32 supported = VK_FALSE;
        vkEnumeratePhysicalDevices(inst, &n, NULL); if (n > 8) n = 8;
        vkEnumeratePhysicalDevices(inst, &n, pd);
        if (n) {
            vkGetPhysicalDeviceSurfaceSupportKHR(pd[0], 0, surf, &supported);
            printf("RESULT: surface support on queue 0 = %s\n", supported ? "YES" : "no");
            VkSurfaceCapabilitiesKHR caps;
            if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd[0], surf, &caps) == VK_SUCCESS)
                printf("RESULT: surface caps currentExtent=%ux%u  <-- surface is live\n",
                       caps.currentExtent.width, caps.currentExtent.height);
        }
    }
    vkDestroySurfaceKHR(inst, surf, NULL);
    vkDestroyInstance(inst, NULL);
    printf("RESULT: completed cleanly\n");
    return 0;
}
