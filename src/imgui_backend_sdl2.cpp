// Compile the SDL2 backend as a C-linkage translation unit.
//
// Dear ImGui's backend headers default IMGUI_IMPL_API to nothing, which gives
// the functions C++ linkage.  The application is written in C and calls these
// entry points directly, so the macro is defined before the backend header is
// seen.  Everything else is the unmodified upstream implementation.
#define IMGUI_IMPL_API extern "C"
#include "imgui_impl_sdl2.cpp"
