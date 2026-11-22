// Compile the OpenGL3 backend as a C-linkage translation unit (see
// imgui_backend_sdl2.cpp for the rationale).
#define IMGUI_IMPL_API extern "C"
#include "imgui_impl_opengl3.cpp"
