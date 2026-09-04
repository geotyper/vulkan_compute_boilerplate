# vulkan_compute_boilerplate

A modular C++20 starting point for Vulkan compute experiments, using GLFW,
GLM, and Dear ImGui for optional visualization. It builds on the
`vulkan_boilerplate` template and adds reusable compute resources rather than
application-specific simulation code. The stable compute baseline is tagged
`v0.2.0`.

## What is included

- Vulkan 1.3 instance, device, swapchain, synchronization, and presentation;
- a module lifecycle and a small composition root;
- RAII wrappers for Vulkan handles, buffers, images, samplers, and shaders;
- synchronous staging uploads and GPU readback through `ImmediateContext`;
- a compute pipeline builder and descriptor allocator/writer;
- `PingPongBuffer` and `PingPongImage` with explicit read/write swapping;
- synchronization2 buffer/image barrier helpers and dispatch-size calculation;
- off-screen graphics rendering displayed in an ImGui viewport;
- a button-triggered compute blur example;
- a headless Game of Life GPU smoke test suitable for software Vulkan;
- CPU/GPU profiling with rolling statistics and synchronization breakdowns;
- CMake debug/release presets, tests, and Linux CI.

The build is split into reusable targets:

- `vkexp_core`: window, Vulkan context, frame loop, module lifecycle, and
  reusable Vulkan resources;
- `vkexp_compute`: pipelines, descriptors, staging/readback, barriers,
  dispatch helpers, and ping-pong resources;
- `vkexp_profiling`: registered CPU/GPU timing metrics;
- `vkexp_imgui`: the generic GLFW/Vulkan ImGui backend and profiler panel;
- `vkexp_demo`: the triangle, compute blur, presets, and demo UI;
- `vulkan_compute_boilerplate`: the composition root in `src/main.cpp`.

`Application` does not select modules. A derived project creates them in its
composition root and adds them with `Application::addModule()`. Shared
experiment data lives in `DemoState`, outside the core lifecycle API.

## Demo

The scene renders into an off-screen texture shown in the ImGui **Viewport**
window. The **Start** button dispatches a compute shader that applies a
configurable box blur to the current triangle texture. The result remains in
the independent **Blur Output** window until the next dispatch.

The separate `game_of_life.comp` shader and `vkexp_compute_smoke` executable
exercise a complete buffer-based cellular-automaton step without GLFW or a
surface. They are infrastructure examples; a full interactive cellular
automaton belongs in a derived project.

ImGui persists the **Controls**, **Viewport**, **Blur Output**, and
**Profiler** layouts in the active build directory's `imgui.ini`.

## Reusable compute API

Include `vkexp/compute/ComputeResources.hpp` and link `vkexp_compute`. A
typical buffer workflow is:

```cpp
vkexp::PingPongBuffer state;
state.create(physicalDevice, device, {
    byteSize,
    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
        VK_BUFFER_USAGE_TRANSFER_DST_BIT,
});

vkexp::ImmediateContext transfers{physicalDevice, device, queueFamily, queue};
transfers.uploadBuffer(state.read(), initialData, byteSize);

vkexp::DescriptorSetWriter{}
    .writeBuffer(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, state.read().buffer())
    .writeBuffer(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, state.write().buffer())
    .update(device, descriptorSet);

const std::array<VkDeviceSize, 2> ranges{
    state.read().size(), state.write().size()};
const vkexp::DispatchSize groups = vkexp::checkedDispatchSize(
    physicalDevice, {{width, height, 1}, {8, 8, 1}, sizeof(Settings), ranges});
vkCmdDispatch(commands, groups.x, groups.y, groups.z);
state.swap();
```

`ImmediateContext` waits for each submitted transfer and is intended for
initialization, tools, tests, and occasional readback. Per-frame streaming
should use frame-owned staging allocations and asynchronous synchronization.
`checkedDispatchSize` validates group counts, local workgroup dimensions and
invocations, push-constant bytes, and storage-buffer descriptor ranges against
the selected physical device before commands are recorded.

## Profiler

The persistent **Profiler** window separates total frame wall time from actual
process CPU time and Vulkan synchronization waits. `Frame wall` includes the
complete loop, while `CPU work` is process CPU time. `Fence wait`,
`Acquire wait`, `Queue submit`, and `Present` show where the main thread
was blocked instead of executing code. Current and rolling CPU load are
derived from CPU time divided by wall time.

The panel reports the refresh rate of the monitor containing most of the
window and estimates missed VSync intervals from frame wall time. This is an
estimate rather than a display-timing-extension measurement. Every metric
keeps the latest 120 samples and exposes current, average, minimum, maximum,
and p95 values.

GPU measurements use Vulkan timestamp queries and are resolved without
stalling the command stream. The panel reports when timestamps are unsupported
and hides metrics that have no samples for the selected CPU or GPU backend.

## Requirements

- CMake 3.24 or newer;
- a C++20 compiler;
- Ninja;
- Vulkan 1.3 development files and a compatible driver;
- GLFW 3.3 or newer;
- GLM;
- `glslangValidator`.

The selected Vulkan device must support dynamic rendering, synchronization2,
graphics, compute, and presentation. GPU timestamp profiling is optional.

On Ubuntu/Debian, install the build dependencies with:

```bash
sudo apt-get update
sudo apt-get install --yes \
  build-essential cmake ninja-build \
  libvulkan-dev vulkan-tools vulkan-validationlayers \
  libglfw3-dev libglm-dev glslang-tools
```

Dear ImGui is pinned to `v1.91.8` and downloaded by CMake through
`FetchContent`, so the first configure requires internet access.

## Build and run

Debug builds enable tests and Vulkan validation when the validation layer is
installed:

```bash
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
./build/debug/vulkan_compute_boilerplate --preset mixed
```

Build an optimized executable without tests with:

```bash
cmake --preset release
cmake --build --preset release
./build/release/vulkan_compute_boilerplate --preset mixed
```

## Command-line options

| Option | Description |
| --- | --- |
| `--preset graphics` | Enable graphics and disable compute. |
| `--preset compute` | Enable compute and disable graphics. |
| `--preset mixed` | Enable both pipelines; this is the default. |
| `--list-presets` | Print all available startup presets. |
| `--no-validation` | Disable Vulkan validation for this run. |
| `--help`, `-h` | Print command-line help. |

```bash
./build/debug/vulkan_compute_boilerplate --list-presets
./build/debug/vulkan_compute_boilerplate --preset graphics --no-validation
```

## Configuration

Edit `config/window.preset` to change the initial native-window size. CMake
copies it into the active build directory and reconfigures when the source
preset changes.

Delete `build/debug/imgui.ini` or `build/release/imgui.ini` to reset
persisted ImGui window positions and sizes.

## Create an independent project from the template

This is the recommended workflow when the new project does not need to track
future boilerplate changes.

1. Open the repository on GitHub.
2. Select **Use this template**, then **Create a new repository**.
3. Enter the new repository name and leave **Include all branches** disabled.
4. Create the repository and clone the new repository, not this template:

```bash
git clone git@github.com:geotyper/my_new_project.git
cd my_new_project
git remote -v
```

The generated repository starts with a fresh history and its `origin` points
only to the new project.

### Rename checklist

Find template-specific names:

```bash
rg -n "vulkan_compute_boilerplate|Vulkan compute boilerplate"
```

Then:

1. rename `project(vulkan_compute_boilerplate ...)` in `CMakeLists.txt`;
2. rename the `vulkan_compute_boilerplate` executable target and its test command;
3. update the application title in `src/main.cpp`;
4. update executable paths and descriptions in this README;
5. replace or remove `vkexp_demo` modules while retaining reusable targets;
6. run debug and release builds before creating the new baseline tag.

The `vkexp` namespace and target prefix may remain unchanged when they
identify the embedded framework rather than the product.

## Alternative: clone while keeping an upstream link

Use this workflow when the derived project should inspect or cherry-pick future
boilerplate fixes. First create an empty GitHub repository without an initial
README or license, then run:

```bash
git clone --branch main --single-branch \
  git@github.com:geotyper/vulkan_compute_boilerplate.git my_new_project
cd my_new_project
git remote rename origin boilerplate
git remote add origin git@github.com:geotyper/my_new_project.git
git push -u origin main
```

Future boilerplate changes can be inspected and integrated explicitly:

```bash
git fetch boilerplate
git log --oneline main..boilerplate/main
git cherry-pick <commit>
```

## Add a custom module

A module implements only the lifecycle hooks it needs:

```cpp
#include "vkexp/core/Module.hpp"

class MyModule final : public vkexp::Module {
public:
    void onAttach(vkexp::AppContext& context) override {
        // Create long-lived resources.
    }

    void onUpdate(vkexp::AppContext& context,
                  const vkexp::FrameInfo& frame) override {
        // Update CPU-side state.
    }

    void onRender(vkexp::AppContext& context,
                  const vkexp::FrameInfo& frame) override {
        // Record Vulkan commands.
    }

    void onDetach(vkexp::AppContext& context) override {
        // Release resources before the Vulkan context is destroyed.
    }
};
```

Register it in the composition root:

```cpp
app.addModule(std::make_unique<MyModule>());
```

Modules attach and execute in registration order and detach in reverse order.
See `include/vkexp/core/Module.hpp` and `src/main.cpp` for the complete API
and composition example.

## Tests and CI

The debug preset builds CPU-only unit tests, a CLI test, and a headless Vulkan
compute smoke test:

```bash
ctest --preset debug --output-on-failure
```

GitHub Actions configures, builds, and tests the project on Linux. It does not
launch the graphical application or compare rendered images. The headless test
returns CTest's skip code when no compatible Vulkan ICD exists; with a software
or hardware Vulkan 1.3 device it validates upload, dispatch, ping-pong, and
readback.

## Known limitations

- only Linux is covered by CI;
- the libraries are internal CMake targets and are not exported by
  `cmake --install`;
- the renderer intentionally uses one frame in flight;
- `DescriptorAllocator` uses a fixed-capacity pool rather than automatic
  pool growth;
- `ImmediateContext` is synchronous and serializes its queue;
- shader hot reload is not implemented;
- automated rendering/image-comparison tests are not implemented.

## Troubleshooting

- **`glslangValidator` not found:** install `glslang-tools` and reconfigure.
- **No suitable Vulkan 1.3 device:** run `vulkaninfo --summary` and update the
  Vulkan driver or select compatible hardware.
- **Validation output is missing:** install `vulkan-validationlayers`; use
  `--no-validation` when validation is intentionally unavailable.
- **The window layout is unusable:** remove the active build directory's
  `imgui.ini` and restart the application.
- **A dependency remains cached incorrectly:** remove the affected build
  directory and run the corresponding configure preset again.

## License

This project is released under the zero-clause BSD license
([SPDX: 0BSD](https://spdx.org/licenses/0BSD.html)). It permits use, copying,
modification, and distribution for any purpose, with or without fee and
without attribution requirements. See [LICENSE](LICENSE).
The license covers the original code in this repository. Third-party
dependencies retain their respective licenses.

See [PLAN.md](PLAN.md) for the architecture and remaining roadmap.
