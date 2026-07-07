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
