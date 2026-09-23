/*
 * Build the OpenGL 3 renderer backend on top of libepoxy instead of its
 * bundled loader, so that it shares QEMU's GL dispatch.  The backend detects
 * GLES 3 contexts at runtime, so this works for both desktop GL and GLES.
 */
#include <epoxy/gl.h>
#define IMGUI_IMPL_OPENGL_LOADER_CUSTOM
#include "backends/imgui_impl_opengl3.cpp"
