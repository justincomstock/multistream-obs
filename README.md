# Multistream for OBS

The Multistream dock in OBS Studio: it checks your OBS settings, your platforms
and where OBS is sending before you go live, then goes live on all of them with
one button, and shows each platform's live time while you stream.

Download it at **https://multistream.gg/download/obs** (Windows, OBS 31.1 or newer).

## What's in this plugin

- **The dock.** An OBS dock (`src/dock.cpp`) showing the Multistream dock page
  in OBS's browser. The page itself is served by multistream.gg; this plugin
  keeps a copy so the dock opens offline.
- **A local bridge** (`src/bridge.cpp`) on `127.0.0.1:14590` that serves that
  page and lets it read OBS's streaming state and press Start/Stop streaming,
  point OBS at Multistream and set the bitrate and keyframes. Only the page it
  served can use it (a per-run secret), and it only answers on 127.0.0.1.
- **The Twitch output** (`src/relay-output.cpp`): when OBS streams straight to
  Twitch, a second output sends a copy to Multistream for your other platforms.
- **Vertical** (`src/vertical/`): a 9:16 canvas (OBS 31.1 canvases) with its
  own docks, managed like OBS's own scenes:
  - *Vertical Scenes*: add, rename, duplicate, reorder, remove; "from a main
    scene" lays one out for 9:16; each can follow main scenes (linked, or by
    name) and has a "Switch to vertical scene" hotkey. Switches use OBS's
    current transition.
  - *Vertical Sources*: show/hide, lock, order, add existing or new sources,
    properties, filters.
  - *Vertical Preview*: drag, resize, crop (Alt), rotate, zoom (Ctrl+wheel),
    safe-area guides, undo; record, replay buffer and Save clip below it.
  - *Vertical Mixer*: the vertical stream's own sound (`audio-mix.cpp` mixes it
    from the sources in the vertical scene on air), per source "same as main"
    or its own volume and mute. The main stream never hears vertical-only
    sources.
  - Its own output to Multistream (feed `vertical`), recording and replay
    buffer (following OBS's buttons by default), Twitch Dual Format, and
    importing Aitum Vertical's scenes or the Vertical tool script's framing.
  - obs-websocket vendor `multistream-vertical` (`websocket.cpp`) for Stream
    Deck and the Multistream app: scenes, items, transforms, audio, outputs,
    recording, replay; it sends a `scene_changed` event.
- **The installer** (`installer/`): NSIS, installs into
  `C:\ProgramData\obs-studio\plugins\multistream`.

Your Multistream sign-in stays in the plugin (`plugin_config/multistream/`),
never in the page.

## Building

Built with OBS's [plugin template](https://github.com/obsproject/obs-plugintemplate):

```
.github/scripts/Build-Windows.ps1 -Target x64 -Configuration RelWithDebInfo
```

The output is in `release/RelWithDebInfo/multistream`. GitHub Actions
(`.github/workflows/build.yaml`) builds it and the installer on every push.

## License

GPL-2.0, like OBS Studio (see `LICENSE`). Each released version is tagged here
(`v1.0.0`, ...) with the exact source of that installer.
