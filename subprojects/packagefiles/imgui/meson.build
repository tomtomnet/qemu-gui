project('imgui', 'cpp',
  version: '1.92.9',
  license: 'MIT',
  meson_version: '>=1.6.0',
  default_options: ['cpp_std=c++17', 'warning_level=0'])

sdl2_dep = dependency('sdl2')
epoxy_dep = dependency('epoxy', required: get_option('opengl'))

imgui_args = ['-DIMGUI_DISABLE_OBSOLETE_FUNCTIONS', '-DIMGUI_DISABLE_DEMO_WINDOWS']

imgui_sources = files(
  'imgui.cpp',
  'imgui_demo.cpp',
  'imgui_draw.cpp',
  'imgui_tables.cpp',
  'imgui_widgets.cpp',
  'backends/imgui_impl_sdl2.cpp',
  'backends/imgui_impl_sdlrenderer2.cpp',
)
imgui_deps = [sdl2_dep]
if epoxy_dep.found()
  imgui_sources += files('imgui_impl_opengl3_epoxy.cpp')
  imgui_deps += epoxy_dep
endif

# pic: the SDL display may be built as a loadable module
imgui_lib = static_library('imgui', imgui_sources,
  cpp_args: imgui_args,
  include_directories: include_directories('.', 'backends'),
  dependencies: imgui_deps,
  pic: true)

imgui_dep = declare_dependency(
  link_with: imgui_lib,
  compile_args: imgui_args,
  include_directories: include_directories('.', 'backends', is_system: true),
  dependencies: imgui_deps)
