"""Resolve a desktop's monitor preference for Wine at launch."""

import json
import shlex
import shutil
import subprocess


def _plasma_monitors(text):
    config = json.loads(text)
    if not isinstance(config, dict) or not isinstance(config.get("outputs"), list):
        return []
    return [output.get("name") for output in config["outputs"]
            if isinstance(output, dict) and output.get("connected") and output.get("enabled")
            and output.get("priority") == 1 and output.get("name")]


def _gnome_monitors(text):
    # GetResources exposes output flags; GetCurrentState's logical-monitor flags
    # can remain stale after changing the primary monitor in Mutter.
    config = json.loads(text)
    data = config.get("data") if isinstance(config, dict) else None
    if not isinstance(data, list) or len(data) != 6 or not isinstance(data[2], list):
        return []
    monitors = []
    for output in data[2]:
        if (not isinstance(output, list) or len(output) != 8
                or type(output[2]) is not int or not isinstance(output[7], dict)):
            return []
        primary = output[7].get("primary", {})
        if not isinstance(primary, dict):
            return []
        if output[2] >= 0 and primary.get("type") == "b" and primary.get("data") is True:
            monitors.append(output[4])
    return monitors


def _niri_monitors(text):
    # niri supplies the focused output, not a configured desktop primary.
    output = json.loads(text)
    if not isinstance(output, dict) or not isinstance(output.get("logical"), dict):
        return []
    return [output.get("name")]


def _cosmic_monitors(text):
    # Read cosmic-randr list --kdl's emitted layout, not arbitrary KDL.
    # Only output headers, direct primary flags and block boundaries matter.
    monitors = []
    depth = 0
    for line in text.splitlines():
        line = line.strip()
        if not line:
            continue
        fields = shlex.split(line)
        if not depth:
            if (len(fields) != 4 or fields[0] != "output" or not fields[1]
                    or fields[2] not in ("enabled=#true", "enabled=#false") or fields[3] != "{"):
                return []
            name, enabled = fields[1], fields[2] == "enabled=#true"
            primary = None
            depth = 1
        elif line == "}":
            depth -= 1
            if not depth and enabled and primary:
                monitors.append(name)
        elif line == "modes {" and depth == 1:
            depth = 2
        elif depth == 2:
            if not line.startswith("mode "):
                return []
        elif fields[0] == "xwayland_primary":
            if primary is not None or line not in ("xwayland_primary #true", "xwayland_primary #false"):
                return []
            primary = line == "xwayland_primary #true"
        elif line.endswith(("{", "}")) or line.startswith("output "):
            return []
    return [] if depth else monitors


# Desktop token -> log description, command, parser. Order preserves precedence
# for composite desktop names and KDE_FULL_SESSION.
_DESKTOPS = {
    "KDE": ("Plasma primary", ("kscreen-doctor", "-j"), _plasma_monitors),
    "GNOME": ("GNOME primary", (
        "busctl", "--user", "--json=short", "--timeout=2",
        "--auto-start=no", "--allow-interactive-authorization=no",
        "call", "org.gnome.Mutter.DisplayConfig", "/org/gnome/Mutter/DisplayConfig",
        "org.gnome.Mutter.DisplayConfig", "GetResources"), _gnome_monitors),
    "NIRI": ("niri focused", ("niri", "msg", "--json", "focused-output"), _niri_monitors),
    "COSMIC": ("COSMIC Xwayland-primary", ("cosmic-randr", "list", "--kdl"), _cosmic_monitors),
}


def setup_primary_monitor(env, log):
    """Set Wine's initial monitor in env, without changing desktop settings.

    Explicit overrides (including empty ones) win. Make at most one query;
    missing tools, failed queries and ambiguous answers leave env unchanged.
    """
    if "WAYLANDDRV_PRIMARY_MONITOR" in env:
        return

    desktops = {name.upper() for variable in ("XDG_CURRENT_DESKTOP", "XDG_SESSION_DESKTOP")
                for name in env.get(variable, "").split(":") if name}
    if env.get("KDE_FULL_SESSION", "0") not in ("", "0"):
        desktops.add("KDE")
    for desktop, (description, command, parse) in _DESKTOPS.items():
        if desktop in desktops:
            break
    else:
        return

    launcher = shutil.which("steam-runtime-launch-client", path=env.get("PATH"))
    if launcher:
        command = [launcher, "--alongside-steam", "--", "/usr/bin/" + command[0], *command[1:]]
    else:
        executable = shutil.which(command[0], path=env.get("PATH"))
        if not executable:
            return
        command = [executable, *command[1:]]

    try:
        result = subprocess.run(command, env=env, stdout=subprocess.PIPE,
                                stderr=subprocess.DEVNULL, text=True, timeout=2, check=True)
        monitors = parse(result.stdout)
    except (OSError, subprocess.SubprocessError, ValueError):
        return

    if len(monitors) == 1 and isinstance(monitors[0], str) and monitors[0]:
        env["WAYLANDDRV_PRIMARY_MONITOR"] = monitors[0]
        log("Using " + description + " monitor " + monitors[0])
