---
name: Compatibility Report
about: Game compatibility issues.

---

# Compatibility Report
- Name of the game/application with compatibility issues:
- Steam AppID / launcher / store:
- Game or launcher version, if known:

## System Information
- GPU: <!-- e.g. RX 580 or GTX 970 -->
- Video driver version: <!-- e.g. Mesa 18.2 or nvidia 396.54 -->
- Kernel version: <!-- e.g. 4.17 -->
- Session / compositor: <!-- e.g. native Wayland on KWin, X11, XWayland, gamescope -->
- Link to full system information report as [Gist](https://gist.github.com/):
- Proton-CachyOS / Wineland version:

## I confirm:
- [ ] that I haven't found an existing compatibility report for this game.
- [ ] that I have checked whether there are updates for my system available.
- [ ] that I tested vanilla Proton-CachyOS without Wineland, or explain below why I could not.

<!-- Please add `PROTON_LOG=1 %command%` to the game's launch options and
attach the generated $HOME/steam-$APPID.log to this issue report as a file.
For Wineland-specific visual, fullscreen, Wayland, or rendering issues, a more useful log is:
`PROTON_LOG=1 DXVK_LOG_LEVEL=info VKD3D_DEBUG=info WINEDEBUG=+timestamp,+pid,+tid,+system,+win,+event,+waylanddrv,+vulkan,+dxgi %command%`
(Proton logs compress well if needed.)-->

## Symptoms <!-- What's the problem? -->


## Comparison
- Result with Wineland:
- Result with vanilla Proton-CachyOS without Wineland:
- Result with official Proton / Proton Experimental, if tested:

## Reproduction


<!--
1. You can find the Steam AppID in the URL of the shop page of the game.
   e.g. for `The Witcher 3: Wild Hunt` the AppID is `292030`.
2. You can find your driver and Linux version, as well as your graphics
   processor's name in the system information report of Steam.
3. You can retrieve a full system information report by clicking
   `Help` > `System Information` in the Steam client on your machine.
4. Please copy it to your clipboard by pressing `Ctrl+A` and then `Ctrl+C`.
   Then paste it in a [Gist](https://gist.github.com/) and post the link in
   this issue.
5. Also, please copy the contents of `Help` > `Steam Runtime Diagnostics` to
   the gist.
6. Please search for open issues and pull requests by the name of the game and
   find out whether they are relevant and should be referenced above.
-->
