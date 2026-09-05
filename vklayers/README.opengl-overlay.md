# Experimental native OpenGL overlay

Build Proton normally, including Wine and the `wineland-vklayer` target.
The native OpenGL bridge is disabled by default. Enable it per game with
`PROTON_WAYLAND_OPENGL_OVERLAY=1`; when unset or `0`, Proton does not add the
OpenGL adapter preload. The launcher does not detect the game's graphics API.

The common `PROTON_WAYLAND_STEAM_OVERLAY` setting remains the master switch.
It is enabled by default for Steam games on WineWayland when the native Steam
overlay is available; setting it to `0` disables both bridges, even with the
OpenGL opt-in set. Vulkan overlay behavior is otherwise unchanged.

For a WineWayland OpenGL game, remove any forced Zink override and add:

```
PROTON_WAYLAND_OPENGL_OVERLAY=1 %command%
```

To also collect diagnostic logs:

```
PROTON_WAYLAND_OPENGL_OVERLAY=1 PROTON_LOG=1 WINELAND_VK_TRANSLATE_DEBUG=1 %command%
```

Steam's native `gameoverlayrenderer.so` must already be in
`LD_PRELOAD`; Proton appends the architecture-matched adapter after it.

The game keeps its EGL context and Wayland surface. The adapter gives Steam's
GLX renderer a scoped context identity and redirects its final swap to the
original EGL callback. No EGL object is passed to a real GLX function. The
existing InputOnly X11 proxy supplies window identity and dimensions; the
existing overlay input client is loaded locally, without enabling a Vulkan
layer for OpenGL. The proxy has no game framebuffer.

Look for `wineland-opengl-overlay: ready` followed by `presenting EGL context`
in the Proton log. With the debug option, the shared client also logs its
registration and forwarded input. `unavailable (...)` identifies an initialization
failure; `swap hook bypassed` disables the adapter. Both paths keep ordinary
EGL presentation. Disabling the GL adapter also releases its input ownership;
an independently started Vulkan input source remains available.
Successful initialization alone does not prove that Steam
rendered an overlay.

Test Shift+Tab, mouse clicks, scrolling, text entry, closing/reopening, window
resize and fullscreen transitions. Check that the game receives input again
after closing the overlay. Test both 32-bit and 64-bit games and both GPUs.

The isolated adapter and input-worker tests require no graphics driver, Steam or display:

```
cc -std=gnu11 -Wall -Wextra -Werror -Iwine/include vklayers/test_overlay_gl.c -pthread -ldl -o /tmp/test-overlay-gl
/tmp/test-overlay-gl
cc -std=gnu11 -Wall -Wextra -Werror -Iwine/include vklayers/test_overlay_input.c -pthread -ldl -o /tmp/test-overlay-input
/tmp/test-overlay-input
```

Repeat with `-m32` to check the 32-bit ABI when multilib is available.
