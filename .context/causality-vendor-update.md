# Causality Vendor Update

## 2026-07-07 Main Pull

- `vendors/causality` was fast-forwarded from `aa74b8a` to `4e8a4dc`
  (`api for viewport index`).
- The update splits native Vulkan integration out of `<causality.h>` into
  `<ca_gpu.h>`. `src/ui/force_graph.c` is the only ctx source that needs this
  header because it renders the custom graph viewport with Vulkan handles from
  Causality.
- `Ca_ViewportDesc.clear_color` is now a plain `{ r, g, b, a }` float array,
  not a Vulkan `VkClearColorValue` union.
- Keep ordinary Causality UI code on `<causality.h>` through `src/pch.h`; do
  not expose `<ca_gpu.h>` globally unless another source records native GPU
  commands directly.
- The previous Apple Objective-C integration requirement still applies:
  `CMakeLists.txt` enables `OBJC` on Apple before adding the vendor tree, so
  `src/platform/mouse_state_mac.m` builds correctly.

## Verification

- Rebuild with `cmake --build build --parallel` after any Causality pull.
- Run `git diff --check` in both the main repo and `vendors/causality` before
  handing off changes.

## 2026-09-29 Causality `346626c` Compatibility

- `<ca_gpu.h>` now includes `<vk_mem_alloc.h>` and exposes `VmaAllocator`,
  while Causality links Vulkan/VMA PRIVATE ("consumers of ca_gpu.h provide
  them"). `CMakeLists.txt` links `Vulkan::Vulkan` on macOS/Windows and adds
  VMA's interface include dir as a SYSTEM include (suppresses VMA nullability
  warnings in ctx).
- `ca_shader_compile()` now takes `Ca_Instance *` (for the shader cache), not
  `VkDevice`.
- Causality's top-level CMake does `include(CTest)`; ctx forces
  `BUILD_TESTING OFF` before `add_subdirectory` so unbuilt Causality tests are
  not registered in ctx's ctest.
- Viewports now render with `CA_FRAMES_IN_FLIGHT` (2) pipelined slots.
  `force_graph.c` keeps one host-visible vertex buffer per slot
  (`GraphVertexSlot`, indexed by `ca_viewport_frame_index()`); the slot's
  fence is already waited when `on_render` runs, so resizing that slot's buffer
  is safe. Pipeline recreation and `ctx_force_graph_destroy()` call
  `vkDeviceWaitIdle` first.
- Local Causality fix (uncommitted in submodule): `ca_viewport_gpu_destroy()`
  waited only on the viewport's render fences, but `render_done` and
  `desc_set` are also used by the swapchain composite submit. Viewport resize
  destroyed in-use objects -> GPU address fault -> device lost -> SIGSEGV in
  `vkQueuePresentKHR`. Now idles `gfx_queue` (same pattern as
  `ca_image_destroy_impl`).
- GUI mode now honors SIGINT/SIGTERM via async-signal-safe
  `ctx_ui_request_close()` polled by the `ctx_ui_run()` tick loop.
- Verified: clean build (no ctx warnings), ctest 3/3, GUI run with live graph
  updates shows no Vulkan validation errors and exits 0 on SIGINT.
