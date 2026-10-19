# Multistream for OBS

The Multistream dock in OBS Studio: it checks your OBS settings, your platforms
and where OBS is sending before you go live, then goes live on all of them with
one button, and shows each platform's live time while you stream.

Download it at **https://multistream.gg/download/obs** (Windows, OBS 31 or newer).

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
