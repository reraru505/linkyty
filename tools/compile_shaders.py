#!/usr/bin/env python3
import os
import struct
import subprocess
import sys

def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    shader_dir = os.path.join(repo_root, "src/graphics/host_gpu/shaders")
    out_tiler = os.path.join(repo_root, "src/generated/gpu_tiler_shaders")
    out_blit = os.path.join(repo_root, "src/generated/gpu_blit_shaders")
    os.makedirs(out_tiler, exist_ok=True)
    os.makedirs(out_blit, exist_ok=True)

    glslang = os.environ.get("GLSLANG_VALIDATOR", "glslangValidator")

    def embed(spv_path, header_path, symbol):
        with open(spv_path, "rb") as f:
            data = f.read()
        words = [struct.unpack("<I", data[i:i+4])[0] for i in range(0, len(data), 4)]
        with open(header_path, "w") as f:
            f.write("#pragma once\n#include <cstdint>\ninline constexpr uint32_t " + symbol + "[] = {\n")
            for i in range(0, len(words), 8):
                chunk = words[i:i+8]
                f.write("  " + ", ".join(f"0x{w:08x}u" for w in chunk) + ",\n")
            f.write("};\n")

    tiler_names = [
        "standard256", "standard4", "standard4_3d", "standard64", "standard64_3d",
        "prt", "prt_3d", "render_target", "depth", "promote_d16", "demote_d16", "swap_bgra16"
    ]
    for name in tiler_names:
        src = os.path.join(shader_dir, f"gpu_tiler_{name}.comp")
        spv = os.path.join(out_tiler, f"gpu_tiler_{name}.spv")
        hdr = os.path.join(out_tiler, f"gpu_tiler_{name}_spv.h")
        subprocess.run([glslang, "-V", "--target-env", "vulkan1.3", "-Os", f"-I{shader_dir}", "-o", spv, src], check=True)
        embed(spv, hdr, f"GPU_TILER_{name.upper()}_SPV")

    # fault buffer
    spv = os.path.join(out_tiler, "fault_buffer_process.spv")
    hdr = os.path.join(out_tiler, "fault_buffer_process_spv.h")
    subprocess.run([glslang, "-V", "--target-env", "vulkan1.3", "-Os", "-DCACHING_PAGEBITS=14", "-DMAX_PAGE_FAULTS=1024", "-o", spv, os.path.join(shader_dir, "fault_buffer_process.comp")], check=True)
    embed(spv, hdr, "FAULT_BUFFER_PROCESS_SPV")

    # blit
    for name, ext in [("gpu_blit_fs_triangle", "vert"), ("gpu_blit_color_to_ms_depth", "frag")]:
        src = os.path.join(shader_dir, f"{name}.{ext}")
        spv = os.path.join(out_blit, f"{name}.spv")
        hdr = os.path.join(out_blit, f"{name}_spv.h")
        subprocess.run([glslang, "-V", "--target-env", "vulkan1.3", "-Os", f"-I{shader_dir}", "-o", spv, src], check=True)
        embed(spv, hdr, f"{name.upper()}_SPV")

    print("LinKyty shaders compiled and embedded successfully.")

if __name__ == "__main__":
    main()
