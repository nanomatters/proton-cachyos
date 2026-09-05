/*
 * Wineland renderer local Steam overlay input client.
 * Copyright 2026 Erhan Bilgili
 *
 * This library is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation; either version 2.1 of the License, or (at your option)
 * any later version.
 */

#ifndef __WINELAND_OVERLAY_CLIENT_H
#define __WINELAND_OVERLAY_CLIENT_H

/* Renderer availability, not overlay visibility. Vulkan is process-lifetime;
 * the OpenGL bridge can withdraw its source if its swap hook is bypassed. */
#define WINELAND_OVERLAY_INPUT_VULKAN 0x1u
#define WINELAND_OVERLAY_INPUT_OPENGL 0x2u

__attribute__((visibility("default")))
int __wineland_overlay_client_start_v1(void *display, unsigned long window, unsigned int source);
__attribute__((visibility("default")))
void __wineland_overlay_client_stop_v1(unsigned int source);

/* Process local callback state supplied by lsteamclient when the application
 * receives GameOverlayActivated_t. */
void __wineland_overlay_client_set_active_v1(int active);

#endif /* __WINELAND_OVERLAY_CLIENT_H */
