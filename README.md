![Proton Wineland](wineland-banner.png)

What is Proton Wineland?
-------------------------

Proton Wineland is an effort to solve Linux gaming problems through complete,
high-quality solutions rather than accumulating game-specific hacks and
workarounds. Its goal is to address issues that are genuinely solvable when the
necessary time and care are invested.

The project began with the lack of robust Wayland support. Since then, it has
grown beyond launchers to address broader compatibility, rendering, input, and
media issues across games and applications.

While upstream compatibility was an early consideration, it should not limit
what the project can achieve. Proton Wineland's focus is the quality of the
solution and the experience it delivers to Linux gamers.


What does Proton Wineland offer over other Proton versions?
-----------------------------------------------------------

Proton Wineland is not intended to change how every game runs. Its advantages
are most noticeable when a game or launcher runs into Wayland-specific problems
that other Proton versions may work around only partially.

It can provide:

- More reliable Windows game launchers, including Chromium and CEF-based
  applications such as Battle.net, Ubisoft Connect, Rockstar Games Launcher,
  and the native Windows Steam client.

- Better fullscreen, borderless-window, minimise, maximise, restore, alt-tab,
  and monitor-switching behaviour, particularly in multi-monitor and
  mixed-scaling setups.

- Correct rendering for overlays, popups, login windows, and other content
  created by a separate Windows process from the visible game window.

- Improved video and media playback compatibility in applications that use
  Windows Media Foundation.

- More accurate mouse and keyboard behaviour when a game is fullscreen, scaled,
  or moved between monitors.

- Better integration with Wayland desktops, including Windows tray icons,
  application menus, and compatible HDR and colour-management paths.

These improvements work automatically where they are applicable. They are
intended to solve underlying compatibility problems rather than require
per-game environment variables or launch-option workarounds.

These benefits depend on the game, GPU driver, compositor, and desktop setup.
A game that already works well with another Proton version may not show a
visible difference.


What does it improve?
----------------------

Proton Wineland aims to make Windows games and their supporting applications
feel at home on a Wayland desktop. It improves launcher compatibility,
fullscreen behaviour, and common window actions such as minimising, maximising,
restoring, and moving games between monitors. It also works to improve video
and media playback, and the rendering of overlays or companion windows,
including cases where those windows are created by a separate process.


How do I activate the Wayland features?
----------------------------------------

You do not need to enable anything manually. Proton Wineland enables Wayland by
default by setting `PROTON_ENABLE_WAYLAND=1`, so games and supporting
applications run through Wayland automatically.


What does it not promise?
-------------------------

No Proton version can guarantee that every game will work perfectly. Not every
problem originates in Proton or can be solved within Wine. Issues may also come
from the game itself, graphics drivers, the desktop compositor, or another part
of the Linux graphics stack.


Is it experimental?
-------------------

Yes, but "experimental" does not mean inherently unstable. Proton Wineland
takes a different approach from the usual pattern of relying on environment
variables, command-line parameters, and game-specific Wine workarounds to make
individual titles run.

Where possible, it aims to solve the underlying problem in a durable way. This
does not mean the project is bug-free, including in newly added areas, but its
experimental nature does not come with an intended trade-off of reduced
stability. In some situations, it may even be more stable than other Proton
versions.


How is artificial intelligence used?
------------------------------------

AI tools are a regular part of Proton Wineland's development process. I use
them to analyse logs, investigate problems, review changes, and help fix or
generate code when I consider that appropriate.

Before delivery, every commit goes through an AI-assisted review. I examine
the findings and decide whether the suggested changes are technically sound
and suitable for the project. When they are, I may let the AI modify the code
and then review the result again.

AI does not replace engineering judgement, testing, or responsibility for the
code that is delivered. Used thoughtfully, however, it is a valuable software
development tool that can accelerate investigation and help identify problems
that might otherwise be missed.


What is the relationship to CachyOS Proton?
-------------------------------------------

Proton Wineland is currently based on CachyOS Proton. Proton is made up of many
components, with Wine being only one of them. Most Proton Wineland changes are
made to Wine, while CachyOS Proton provides the surrounding build, integration,
and runtime framework.

Early on, Proton Wineland was regularly rebased onto the latest CachyOS
branches. As its Wayland work expanded, rebasing the Wine component became
increasingly difficult. Repeatedly resolving the growing number of conflicts
began to destabilise the codebase, so Proton Wineland now maintains its Wine
work independently and selectively cherry-picks compatible upstream and
bleeding-edge changes.

An early possibility was contributing the Wineland changes directly to CachyOS.
As the project grew in scope and followed its own technical direction,
independent development became the better fit. This gives Proton Wineland room
to pursue broader solutions when they can improve the Proton experience.

CachyOS has its own development priorities and established relationships with
the Wine development process. Proton Wineland complements that work with an
independent focus. I have great respect for the CachyOS maintainers and
developers, and value their tremendous work.


Can Proton Wineland be merged into other larger Proton projects?
---------------------------------------------------------------

I cannot speak for the plans of other projects. Proton Wineland is open source,
and anyone is free to use or adapt parts of it. For example, GE-Proton has
already adopted its Status Notifier Item support.

Adopting the entire project would be more difficult because the changes span a
broad part of Wine's Wayland, rendering, and compatibility stack. The
cross-process rendering framework is already relatively mature, but taking it
wholesale could make it harder for another project to integrate future upstream
Wine changes.


What about upstream Wine and Valve's Wine work?
------------------------------------------------

A full merge into upstream Wine or Valve's Wine work is currently unlikely. My
understanding is that Wine is pursuing its own approach to Wayland
cross-process rendering, which differs from Proton Wineland's design. I have
not investigated the details of that work myself, so I do not want to speculate
beyond that.

The same is likely true for Valve's Wine work. Valve and upstream Wine have
closely connected development efforts, with contributors working across both
projects. Proton Wineland is therefore best understood as an independent
project that can share ideas and individual improvements where appropriate,
rather than something expected to be merged wholesale.


Original Proton documentation
-----------------------------

The original Proton documentation, including build instructions, is available
in [README_PROTON.md](README_PROTON.md).
