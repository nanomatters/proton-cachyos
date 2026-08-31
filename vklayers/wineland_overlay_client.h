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

void wineland_overlay_client_start(void *display, unsigned long window);

/* Process local callback state supplied by lsteamclient when the application
 * receives GameOverlayActivated_t. */
void __wineland_overlay_client_set_active_v1(int active);

#endif /* __WINELAND_OVERLAY_CLIENT_H */
