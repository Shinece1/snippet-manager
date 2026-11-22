# Third-party sources

This directory contains the dependencies needed for an offline build.  They are
unmodified upstream sources, trimmed of examples/tests and unrelated backends
where possible.

| Directory | Upstream | Version | License |
| --- | --- | --- | --- |
| `cimgui/` | https://github.com/cimgui/cimgui | commit `c4461a7eadbd17b6a6a8124a8abbf499aba6318d` | MIT (`LICENSE`) |
| `cimgui/imgui/` | https://github.com/ocornut/imgui | commit `6d32174508fb8c518e7a832cc1d571fadd5cba76` (1.93.0) | MIT (`LICENSE.txt`) |
| `sqlite/` | https://www.sqlite.org/ | amalgamation 3.46.1 | Public Domain |
| `SDL2/` | https://github.com/libsdl-org/SDL | 2.30.9 | zlib (`LICENSE.txt`) |

`cimgui` was generated against the exact Dear ImGui commit above.  Do not mix
these directories with a different ImGui version: the generated `cimgui.h`
layout and the core library must match.

To use system/vcpkg copies instead, configure with `-DSNIPGUI_CIMGUI_DIR=...`,
`-DSNIPGUI_IMGUI_DIR=...`, `-DSNIPGUI_SQLITE_DIR=...`, and/or
`-DSNIPGUI_SDL2_DIR=...`.
