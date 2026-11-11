// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>

namespace Shader {
struct Profile;
namespace IR {
struct Program;
}
} // namespace Shader

namespace Vulkan::NirHandoff {

// A platform whose Vulkan driver is linked into the app (the PS5 port links RADV) can hand a
// program's NIR straight to the driver, skipping SPIR-V: the driver takes a shader module that
// carries NIR as its own built-in shaders do. Set by that platform at startup; unset elsewhere.
//
// Attach: builds the program's NIR (compute programs; descriptors bound as the SPIR-V backend
// binds them, from binding 0) and attaches it to `module`, a VkShaderModule created from any SPIR-V.
// False when the program uses something the NIR backend does not translate: keep the SPIR-V.
using AttachFn = bool (*)(const Shader::Profile& profile, Shader::IR::Program& program, void* module);
// Detach: once the pipeline is created (the driver has its own copy), frees the NIR.
using DetachFn = void (*)(void* module);

inline std::atomic<AttachFn> attach{nullptr};
inline std::atomic<DetachFn> detach{nullptr};

} // namespace Vulkan::NirHandoff
