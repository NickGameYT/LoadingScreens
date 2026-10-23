# Loading Screens

SKSE plugin for Skyrim Special Edition / Anniversary Edition. Shows custom images and video on loading screens and optionally on the main menu.

## Requirements

- Visual Studio 2022 with C++ desktop workload
- CMake 3.24+
- Skyrim SE/AE Address Library
- SKSE64

## Build

```bat
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Optional CMake options:

- `CommonLibSSE_ROOT` — path to a local [CommonLibSSE-NG](https://github.com/CharmedBaryon/CommonLibSSE-NG) checkout (otherwise it is fetched)
- `SKSE_PLUGIN_DIR` — folder where `LoadingScreens.dll` is written after build

Example:

```bat
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 ^
  -DCommonLibSSE_ROOT=C:/path/to/CommonLibSSE-NG ^
  -DSKSE_PLUGIN_DIR=C:/Mods/LoadingScreens/SKSE/Plugins
```

## Install

Copy `LoadingScreens.dll` to `Data/SKSE/Plugins/`.

Media goes in `Data/SKSE/Plugins/LoadingScreens/`. Emergency clips go in `LoadingScreens/Emergency/`. Optional loading soundtrack files go in `LoadingScreens/Audio/` when folder audio is enabled in the ini.

`LoadingScreens.ini` is created next to the DLL on first run.

## License

MIT. `extern/stb_image.h` is public domain (stb). Other dependencies keep their own licenses.
