/*
 * Wineland Vulkan surface translation layer.
 *
 * This library is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation; either version 2.1 of the License, or (at your option)
 * any later version.
 */

#define _GNU_SOURCE

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VK_USE_PLATFORM_XLIB_KHR
#define VK_USE_PLATFORM_WAYLAND_KHR
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#include "wine/wayland_vulkan_proxy.h"
#include "wineland_overlay_client.h"

struct physical_device_data
{
    VkPhysicalDevice physical_device;
    struct physical_device_data *next;
};

struct instance_data
{
    VkInstance instance;
    PFN_vkGetInstanceProcAddr next_gipa;
    PFN_vkCreateWaylandSurfaceKHR create_wayland_surface;
    PFN_vkGetPhysicalDeviceWaylandPresentationSupportKHR wayland_presentation_support;
    PFN_vkEnumeratePhysicalDevices enumerate_physical_devices;
    PFN_vkDestroyInstance destroy_instance;
    struct physical_device_data *physical_devices;

    Display *proxy_display;
    Window proxy_window;
    struct wl_display *wayland_display;
    struct wl_surface *registered_surface;
    int registration_active;

    struct instance_data *next;
};

static pthread_mutex_t instances_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct instance_data *instances;

static int trace_enabled(void)
{
    static int enabled = -1;

    if (enabled == -1)
    {
        const char *env = getenv("WINELAND_VK_TRANSLATE_DEBUG");
        enabled = env && *env != '0';
    }
    return enabled;
}

#define TRACE(fmt, ...) do { \
    if (trace_enabled()) fprintf(stderr, "wineland-vk-translate: " fmt, ##__VA_ARGS__); \
} while (0)

static struct instance_data *instance_from_handle_locked(VkInstance instance)
{
    struct instance_data *data;

    for (data = instances; data; data = data->next)
        if (data->instance == instance) return data;
    return NULL;
}

static struct instance_data *instance_from_physical_device_locked(VkPhysicalDevice physical_device)
{
    struct physical_device_data *physical;
    struct instance_data *data;

    for (data = instances; data; data = data->next)
        for (physical = data->physical_devices; physical; physical = physical->next)
            if (physical->physical_device == physical_device) return data;
    return NULL;
}

static VkResult remember_physical_devices(struct instance_data *data, uint32_t count,
                                          const VkPhysicalDevice *physical_devices)
{
    struct physical_device_data *physical, **tail;
    uint32_t i;

    pthread_mutex_lock(&instances_mutex);
    tail = &data->physical_devices;
    while (*tail) tail = &(*tail)->next;
    for (i = 0; i < count; i++)
    {
        for (physical = data->physical_devices; physical; physical = physical->next)
            if (physical->physical_device == physical_devices[i]) break;
        if (physical) continue;
        if (!(physical = malloc(sizeof(*physical))))
        {
            pthread_mutex_unlock(&instances_mutex);
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        physical->physical_device = physical_devices[i];
        physical->next = NULL;
        *tail = physical;
        tail = &physical->next;
    }
    pthread_mutex_unlock(&instances_mutex);
    return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL wineland_RegisterWaylandSurface(
        VkInstance instance, void *x11_display, uint64_t x11_window,
        void *wl_display, void *wl_surface)
{
    struct instance_data *data;
    VkResult result = VK_SUCCESS;

    pthread_mutex_lock(&instances_mutex);
    if (!(data = instance_from_handle_locked(instance)))
        result = VK_ERROR_INITIALIZATION_FAILED;
    else if (!wl_display && !wl_surface)
    {
        data->registration_active = 0;
        data->registered_surface = NULL;
    }
    else if (!x11_display || !x11_window || !wl_display || !wl_surface ||
             data->registration_active)
        result = VK_ERROR_INITIALIZATION_FAILED;
    else
    {
        data->proxy_display = x11_display;
        data->proxy_window = x11_window;
        data->wayland_display = wl_display;
        data->registered_surface = wl_surface;
        data->registration_active = 1;
    }
    pthread_mutex_unlock(&instances_mutex);
    return result;
}

static VKAPI_ATTR VkResult VKAPI_CALL wineland_CreateXlibSurfaceKHR(
        VkInstance instance, const VkXlibSurfaceCreateInfoKHR *create_info,
        const VkAllocationCallbacks *allocator, VkSurfaceKHR *surface)
{
    PFN_vkCreateWaylandSurfaceKHR create_wayland_surface = NULL;
    VkWaylandSurfaceCreateInfoKHR wayland_info =
    {
        .sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR,
    };
    struct instance_data *data;
    VkResult result;

    pthread_mutex_lock(&instances_mutex);
    if ((data = instance_from_handle_locked(instance)) && data->registration_active &&
        data->proxy_display == create_info->dpy && data->proxy_window == create_info->window)
    {
        create_wayland_surface = data->create_wayland_surface;
        wayland_info.display = data->wayland_display;
        wayland_info.surface = data->registered_surface;
    }
    pthread_mutex_unlock(&instances_mutex);

    if (!create_wayland_surface || !wayland_info.display || !wayland_info.surface)
    {
        TRACE("rejecting an unregistered Xlib surface\n");
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    result = create_wayland_surface(instance, &wayland_info, allocator, surface);
    TRACE("translated proxy window 0x%lx to Wayland surface %p, result=%d\n",
          (unsigned long)create_info->window, (void *)wayland_info.surface, result);
    if (result == VK_SUCCESS)
        wineland_overlay_client_start(create_info->dpy, create_info->window);
    return result;
}

static VKAPI_ATTR VkResult VKAPI_CALL wineland_EnumeratePhysicalDevices(
        VkInstance instance, uint32_t *count, VkPhysicalDevice *physical_devices)
{
    PFN_vkEnumeratePhysicalDevices enumerate = NULL;
    struct instance_data *data;
    VkResult result;

    pthread_mutex_lock(&instances_mutex);
    if ((data = instance_from_handle_locked(instance)))
        enumerate = data->enumerate_physical_devices;
    pthread_mutex_unlock(&instances_mutex);
    if (!enumerate) return VK_ERROR_INITIALIZATION_FAILED;

    result = enumerate(instance, count, physical_devices);
    if (physical_devices && (result == VK_SUCCESS || result == VK_INCOMPLETE))
    {
        VkResult remember_result = remember_physical_devices(data, *count, physical_devices);
        if (remember_result != VK_SUCCESS) return remember_result;
    }
    return result;
}

static VKAPI_ATTR VkBool32 VKAPI_CALL wineland_GetPhysicalDeviceXlibPresentationSupportKHR(
        VkPhysicalDevice physical_device, uint32_t queue_family, Display *display,
        VisualID visual_id)
{
    PFN_vkGetPhysicalDeviceWaylandPresentationSupportKHR presentation_support = NULL;
    struct wl_display *wayland_display = NULL;
    struct instance_data *data;

    (void)visual_id;
    pthread_mutex_lock(&instances_mutex);
    if ((data = instance_from_physical_device_locked(physical_device)) &&
        data->proxy_display == display)
    {
        presentation_support = data->wayland_presentation_support;
        wayland_display = data->wayland_display;
    }
    pthread_mutex_unlock(&instances_mutex);

    if (!presentation_support || !wayland_display)
    {
        TRACE("no Wayland target registered for Xlib presentation support\n");
        return VK_FALSE;
    }
    return presentation_support(physical_device, queue_family, wayland_display);
}

static void free_instance_data(struct instance_data *data)
{
    struct physical_device_data *physical;

    while ((physical = data->physical_devices))
    {
        data->physical_devices = physical->next;
        free(physical);
    }
    free(data);
}

static VKAPI_ATTR void VKAPI_CALL wineland_DestroyInstance(
        VkInstance instance, const VkAllocationCallbacks *allocator)
{
    PFN_vkDestroyInstance destroy_instance = NULL;
    struct instance_data **entry, *data = NULL;

    pthread_mutex_lock(&instances_mutex);
    for (entry = &instances; *entry; entry = &(*entry)->next)
    {
        if ((*entry)->instance != instance) continue;
        data = *entry;
        *entry = data->next;
        destroy_instance = data->destroy_instance;
        break;
    }
    pthread_mutex_unlock(&instances_mutex);

    if (!data)
    {
        TRACE("destroy requested for an unknown instance %p\n", (void *)instance);
        return;
    }
    free_instance_data(data);
    destroy_instance(instance, allocator);
}

static VKAPI_ATTR VkResult VKAPI_CALL wineland_CreateInstance(
        const VkInstanceCreateInfo *create_info, const VkAllocationCallbacks *allocator,
        VkInstance *instance)
{
    VkLayerInstanceCreateInfo *link = (VkLayerInstanceCreateInfo *)create_info->pNext;
    VkInstanceCreateInfo next_info = *create_info;
    PFN_vkGetInstanceProcAddr next_gipa;
    PFN_vkCreateInstance next_create;
    const char **extensions = NULL;
    struct instance_data *data;
    uint32_t i, count = 0;
    VkResult result;

    while (link && !(link->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO &&
                     link->function == VK_LAYER_LINK_INFO))
        link = (VkLayerInstanceCreateInfo *)link->pNext;
    if (!link) return VK_ERROR_INITIALIZATION_FAILED;

    next_gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;
    if (!(next_create = (PFN_vkCreateInstance)next_gipa(NULL, "vkCreateInstance")))
        return VK_ERROR_INITIALIZATION_FAILED;

    if (create_info->enabledExtensionCount)
    {
        if (!(extensions = malloc(create_info->enabledExtensionCount * sizeof(*extensions))))
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        for (i = 0; i < create_info->enabledExtensionCount; i++)
        {
            if (!strcmp(create_info->ppEnabledExtensionNames[i], VK_KHR_XLIB_SURFACE_EXTENSION_NAME))
                continue;
            extensions[count++] = create_info->ppEnabledExtensionNames[i];
        }
        next_info.enabledExtensionCount = count;
        next_info.ppEnabledExtensionNames = extensions;
    }

    result = next_create(&next_info, allocator, instance);
    free(extensions);
    if (result != VK_SUCCESS) return result;

    if (!(data = calloc(1, sizeof(*data))))
    {
        PFN_vkDestroyInstance destroy = (PFN_vkDestroyInstance)
                next_gipa(*instance, "vkDestroyInstance");
        if (destroy) destroy(*instance, allocator);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }

    data->instance = *instance;
    data->next_gipa = next_gipa;
    data->create_wayland_surface = (PFN_vkCreateWaylandSurfaceKHR)
            next_gipa(*instance, "vkCreateWaylandSurfaceKHR");
    data->wayland_presentation_support =
            (PFN_vkGetPhysicalDeviceWaylandPresentationSupportKHR)
            next_gipa(*instance, "vkGetPhysicalDeviceWaylandPresentationSupportKHR");
    data->enumerate_physical_devices = (PFN_vkEnumeratePhysicalDevices)
            next_gipa(*instance, "vkEnumeratePhysicalDevices");
    data->destroy_instance = (PFN_vkDestroyInstance)
            next_gipa(*instance, "vkDestroyInstance");
    if (!data->create_wayland_surface || !data->wayland_presentation_support ||
        !data->enumerate_physical_devices || !data->destroy_instance)
    {
        if (data->destroy_instance) data->destroy_instance(*instance, allocator);
        free(data);
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    pthread_mutex_lock(&instances_mutex);
    data->next = instances;
    instances = data;
    pthread_mutex_unlock(&instances_mutex);

    TRACE("instance %p ready\n", (void *)*instance);
    return VK_SUCCESS;
}

__attribute__((visibility("default")))
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance,
                                                               const char *name)
{
    PFN_vkGetInstanceProcAddr next_gipa = NULL;
    struct instance_data *data;

    if (!strcmp(name, "vkGetInstanceProcAddr")) return (PFN_vkVoidFunction)vkGetInstanceProcAddr;
    if (!strcmp(name, "vkCreateInstance")) return (PFN_vkVoidFunction)wineland_CreateInstance;
    if (!strcmp(name, "vkDestroyInstance")) return (PFN_vkVoidFunction)wineland_DestroyInstance;
    if (!strcmp(name, "vkEnumeratePhysicalDevices"))
        return (PFN_vkVoidFunction)wineland_EnumeratePhysicalDevices;
    if (!strcmp(name, "vkCreateXlibSurfaceKHR"))
        return (PFN_vkVoidFunction)wineland_CreateXlibSurfaceKHR;
    if (!strcmp(name, "vkGetPhysicalDeviceXlibPresentationSupportKHR"))
        return (PFN_vkVoidFunction)wineland_GetPhysicalDeviceXlibPresentationSupportKHR;
    pthread_mutex_lock(&instances_mutex);
    if ((data = instance_from_handle_locked(instance))) next_gipa = data->next_gipa;
    pthread_mutex_unlock(&instances_mutex);
    return next_gipa ? next_gipa(instance, name) : NULL;
}

__attribute__((visibility("default")))
int32_t __wine_wayland_vulkan_register_surface_v1(
        void *instance, void *x11_display, uint64_t x11_window,
        void *wl_display, void *wl_surface)
{
    return wineland_RegisterWaylandSurface((VkInstance)instance, x11_display, x11_window,
                                           wl_display, wl_surface);
}

__attribute__((visibility("default")))
VKAPI_ATTR VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(
        VkNegotiateLayerInterface *interface)
{
    if (interface->loaderLayerInterfaceVersion > 2)
        interface->loaderLayerInterfaceVersion = 2;
    interface->pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
    interface->pfnGetDeviceProcAddr = NULL;
    interface->pfnGetPhysicalDeviceProcAddr = NULL;
    return VK_SUCCESS;
}
