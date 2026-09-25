const std = @import("std");
const sources = @import("tools/sources.zig");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    // -------------------------------------------------------------------------
    // Step: Compile Shaders
    // -------------------------------------------------------------------------
    const shader_step = b.step("shaders", "Compile and embed Vulkan SPIR-V shaders");
    const compile_shaders_cmd = b.addSystemCommand(&.{
        "python3",
        "tools/compile_shaders.py",
    });
    shader_step.dependOn(&compile_shaders_cmd.step);

    // -------------------------------------------------------------------------
    // Common Compilation Flags & Includes
    // -------------------------------------------------------------------------
    const cpp_flags = [_][]const u8{
        "-std=c++20",
        "-D_GNU_SOURCE",

        "-DKYTY_ENDIAN=2",   // KYTY_ENDIAN_LITTLE
        "-D_TIMESPEC_DEFINED",
        "-DIMGUI_IMPL_VULKAN_NO_PROTOTYPES",
        "-DTRACY_ENABLE",
        "-DTRACY_ON_DEMAND",
        "-DTRACY_DELAYED_INIT",
        "-DTRACY_MANUAL_LIFETIME",
        "-Wno-pragma-pack",
        "-Wno-deprecated-declarations",
        "-Wno-unused-function",
        "-Wno-unused-variable",
        "-Wno-date-time",
    };

    const c_flags = [_][]const u8{
        "-std=c11",
        "-D_GNU_SOURCE",
        "-Wno-unused-function",
        "-Wno-unused-variable",
    };

    const include_paths = [_][]const u8{
        "src",
        "src/generated",
        "3rdparty/Vulkan-Headers/include",
        "3rdparty/VulkanMemoryAllocator/include",
        "3rdparty/fmt/include",
        "3rdparty/spdlog/include",
        "3rdparty/nlohmann_json/include",
        "3rdparty/magic_enum/include",
        "3rdparty/magic_enum/include/magic_enum",
        "3rdparty/SPIRV-Headers/include",
        "3rdparty/SPIRV-Tools/include",
        "3rdparty/stb",
        "3rdparty/renderdoc",
        "3rdparty/xxHash",
        "3rdparty/tracy/public",
        "3rdparty/cpuinfo/include",
        "3rdparty/cpuinfo/src",
        "3rdparty/cpuinfo/deps/clog/include",
        "3rdparty/LibAtrac9/C/src",
        "3rdparty/imgui",
        "3rdparty/imgui/backends",
        "/usr/include/SDL3",
    };

    // Helper to configure common includes, system libs, and flags on a Module
    const configureModule = struct {
        fn apply(
            builder: *std.Build,
            mod: *std.Build.Module,
            incs: []const []const u8,
        ) void {
            mod.link_libc = true;
            mod.linkSystemLibrary("stdc++", .{});

            for (incs) |inc| {
                if (std.mem.startsWith(u8, inc, "/")) {
                    mod.addIncludePath(.{ .cwd_relative = inc });
                } else {
                    mod.addIncludePath(builder.path(inc));
                }
            }

            // System libraries
            mod.linkSystemLibrary("vulkan", .{});
            mod.linkSystemLibrary("SDL3", .{});
            mod.linkSystemLibrary("avcodec", .{});
            mod.linkSystemLibrary("avformat", .{});
            mod.linkSystemLibrary("avutil", .{});
            mod.linkSystemLibrary("swresample", .{});
            mod.linkSystemLibrary("swscale", .{});
            mod.linkSystemLibrary("Zydis", .{});
            mod.linkSystemLibrary("SPIRV-Tools-opt", .{});
            mod.linkSystemLibrary("SPIRV-Tools", .{});
            mod.linkSystemLibrary("SPIRV-Tools-link", .{});
            mod.linkSystemLibrary("pthread", .{});
            mod.linkSystemLibrary("dl", .{});
            mod.linkSystemLibrary("m", .{});
        }
    }.apply;

    // Helper to add core emulator sources
    const addEmulatorSources = struct {
        fn add(
            mod: *std.Build.Module,
            cxx_flags: []const []const u8,
            c_flgs: []const []const u8,
        ) void {
            mod.addCSourceFiles(.{ .files = &sources.common, .flags = cxx_flags });
            mod.addCSourceFiles(.{ .files = &sources.libs, .flags = cxx_flags });
            mod.addCSourceFiles(.{ .files = &sources.graphics, .flags = cxx_flags });
            mod.addCSourceFiles(.{ .files = &sources.kernel, .flags = cxx_flags });
            mod.addCSourceFiles(.{ .files = &sources.loader, .flags = cxx_flags });
            mod.addCSourceFiles(.{ .files = &sources.imgui, .flags = cxx_flags });
            mod.addCSourceFiles(.{ .files = &sources.atrac9, .flags = c_flgs });
            mod.addCSourceFiles(.{ .files = &sources.cpuinfo, .flags = c_flgs });
            mod.addCSourceFiles(.{ .files = &sources.fmt, .flags = cxx_flags });

            mod.addCSourceFiles(.{
                .files = &.{
                    "src/emulator.cpp",
                    "3rdparty/tracy/public/TracyClient.cpp",
                },
                .flags = cxx_flags,
            });

            mod.addCSourceFiles(.{
                .files = &.{
                    "3rdparty/xxHash/xxhash.c",
                },
                .flags = c_flgs,
            });
        }
    }.add;

    // -------------------------------------------------------------------------
    // Target 1: linkyty (Main Executable)
    // -------------------------------------------------------------------------
    const emulator_mod = b.createModule(.{
        .target = target,
        .optimize = optimize,
    });
    configureModule(b, emulator_mod, &include_paths);
    addEmulatorSources(emulator_mod, &cpp_flags, &c_flags);
    emulator_mod.addCSourceFiles(.{
        .files = &.{"src/main.cpp"},
        .flags = &cpp_flags,
    });

    const emulator_exe = b.addExecutable(.{
        .name = "linkyty",
        .root_module = emulator_mod,
    });

    emulator_exe.step.dependOn(&compile_shaders_cmd.step);
    b.installArtifact(emulator_exe);

    // Run emulator step
    const run_emulator_cmd = b.addRunArtifact(emulator_exe);
    run_emulator_cmd.step.dependOn(b.getInstallStep());
    if (b.args) |args| {
        run_emulator_cmd.addArgs(args);
    }
    const run_step = b.step("run", "Run the linkyty executable");
    run_step.dependOn(&run_emulator_cmd.step);

    // -------------------------------------------------------------------------
    // Target 2: sync_benchmark (Benchmark Harness)
    // -------------------------------------------------------------------------
    const bench_mod = b.createModule(.{
        .target = target,
        .optimize = optimize,
    });
    configureModule(b, bench_mod, &include_paths);
    addEmulatorSources(bench_mod, &cpp_flags, &c_flags);
    bench_mod.addCSourceFiles(.{
        .files = &.{"tests/SyncBenchmark.cpp"},
        .flags = &cpp_flags,
    });

    const bench_exe = b.addExecutable(.{
        .name = "sync_benchmark",
        .root_module = bench_mod,
    });

    bench_exe.step.dependOn(&compile_shaders_cmd.step);
    b.installArtifact(bench_exe);

    const run_bench_cmd = b.addRunArtifact(bench_exe);
    const bench_step = b.step("bench", "Run the synchronization benchmark suite");
    bench_step.dependOn(&run_bench_cmd.step);

    // -------------------------------------------------------------------------
    // Target 3: sync_on_address_tests (Unit Test)
    // -------------------------------------------------------------------------
    const test_mod = b.createModule(.{
        .target = target,
        .optimize = optimize,
    });
    test_mod.link_libc = true;
    test_mod.linkSystemLibrary("stdc++", .{});
    for (include_paths) |inc| {
        if (std.mem.startsWith(u8, inc, "/")) {
            test_mod.addIncludePath(.{ .cwd_relative = inc });
        } else {
            test_mod.addIncludePath(b.path(inc));
        }
    }
    test_mod.linkSystemLibrary("pthread", .{});
    test_mod.linkSystemLibrary("dl", .{});
    test_mod.linkSystemLibrary("m", .{});

    test_mod.addCSourceFiles(.{ .files = &sources.common, .flags = &cpp_flags });
    test_mod.addCSourceFiles(.{ .files = &sources.cpuinfo, .flags = &c_flags });
    test_mod.addCSourceFiles(.{ .files = &sources.fmt, .flags = &cpp_flags });
    test_mod.addCSourceFiles(.{
        .files = &.{
            "src/kernel/syncOnAddress.cpp",
            "tests/SyncOnAddressTests.cpp",
            "3rdparty/tracy/public/TracyClient.cpp",
        },
        .flags = &cpp_flags,
    });

    const test_exe = b.addExecutable(.{
        .name = "sync_on_address_tests",
        .root_module = test_mod,
    });

    b.installArtifact(test_exe);

    const run_test_cmd = b.addRunArtifact(test_exe);
    const test_step = b.step("test", "Run synchronization unit tests");
    test_step.dependOn(&run_test_cmd.step);

    // -------------------------------------------------------------------------
    // Target 4: vm_allocation_tests (Guest virtual-memory allocation suite)
    // -------------------------------------------------------------------------
    // The suite exercises the guest address space and direct-memory backing store
    // (KernelAllocateDirectMemory / KernelMapDirectMemory / munmap / placeholder reuse),
    // so its instrumentation hooks are enabled for every translation unit in this target.
    const vm_test_flags = cpp_flags ++ [_][]const u8{"-DKYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS"};

    const vm_test_mod = b.createModule(.{
        .target = target,
        .optimize = optimize,
    });
    configureModule(b, vm_test_mod, &include_paths);
    // The suite links against kernel/memory.cpp's GPU-fault and backing-store paths, which
    // reference Libs::Graphics, so it needs the same source closure as the emulator itself
    // (minus src/main.cpp) - exactly how sync_benchmark is built. Narrowing this to
    // sources.common + sources.kernel + sources.loader leaves unresolved Graphics symbols.
    addEmulatorSources(vm_test_mod, &vm_test_flags, &c_flags);
    vm_test_mod.addCSourceFiles(.{
        .files = &.{"tests/VirtualMemoryAllocationTests.cpp"},
        .flags = &vm_test_flags,
    });

    const vm_test_exe = b.addExecutable(.{
        .name = "vm_allocation_tests",
        .root_module = vm_test_mod,
    });

    vm_test_exe.step.dependOn(&compile_shaders_cmd.step);
    b.installArtifact(vm_test_exe);

    const run_vm_test_cmd = b.addRunArtifact(vm_test_exe);
    const vm_test_step = b.step("test-vm", "Run guest virtual-memory allocation unit tests");
    vm_test_step.dependOn(&run_vm_test_cmd.step);
    // Deliberately a separate step rather than folded into `test`: this suite needs the full
    // emulator source closure (kernel/memory.cpp references Libs::Graphics), so it is an
    // order of magnitude more expensive to build than the synchronization tests.

    // -------------------------------------------------------------------------
    // Target 5: waterfall descriptor tests (shader recompiler regression suite)
    // -------------------------------------------------------------------------
    // Focused harness that links only the typed IR passes, so it stays free of the Vulkan/SDL
    // toolchain and runs in well under a second.
    const shader_test_flags = [_][]const u8{
        "-std=c++20",
        "-D_GNU_SOURCE",

        "-DKYTY_ENDIAN=2",
        "-D_TIMESPEC_DEFINED",
        "-Wno-pragma-pack",
        "-Wno-deprecated-declarations",
        "-Wno-unused-function",
        "-Wno-unused-variable",
    };
    const shader_test_mod = b.createModule(.{
        .target = target,
        .optimize = optimize,
    });
    shader_test_mod.link_libc = true;
    shader_test_mod.linkSystemLibrary("stdc++", .{});
    for (include_paths) |inc| {
        if (std.mem.startsWith(u8, inc, "/")) {
            shader_test_mod.addIncludePath(.{ .cwd_relative = inc });
        } else {
            shader_test_mod.addIncludePath(b.path(inc));
        }
    }
    shader_test_mod.linkSystemLibrary("pthread", .{});
    shader_test_mod.linkSystemLibrary("dl", .{});
    shader_test_mod.linkSystemLibrary("m", .{});
    shader_test_mod.addCSourceFiles(.{ .files = &sources.fmt, .flags = &shader_test_flags });
    shader_test_mod.addCSourceFiles(.{
        .files = &.{
            // The typed-IR implementation set is amalgamated into the harness itself.
            "tests/WaterfallDescriptorTests.cpp",
            "src/common/dateTime.cpp",
            "src/common/emulatorConfig.cpp",
            "src/common/file.cpp",
            "src/common/logging/log.cpp",
            "src/common/platform/sysLinuxFileIO.cpp",
            "src/graphics/guest_gpu/gpu_format.cpp",
            "src/graphics/shader/shaderPixelParameter.cpp",
            "src/graphics/shader/recompiler/ir/passes/BindingLayout.cpp",
            "src/graphics/shader/recompiler/ir/passes/ConstantPropagation.cpp",
            "src/graphics/shader/recompiler/ir/passes/DeadCodeElimination.cpp",
            "src/graphics/shader/recompiler/ir/passes/ReadLaneElimination.cpp",
            "src/graphics/shader/recompiler/ir/passes/ResourceMaterialization.cpp",
            "src/graphics/shader/recompiler/ir/passes/ResourceTracking.cpp",
            "src/graphics/shader/recompiler/ir/passes/ShaderInfoCollection.cpp",
            "src/graphics/shader/recompiler/ir/passes/SrtWalker.cpp",
            "src/graphics/shader/recompiler/ir/passes/SsaRewrite.cpp",
            "src/graphics/shader/recompiler/ir/passes/WaterfallDescriptor.cpp",
        },
        .flags = &shader_test_flags,
    });

    // Not installed: `zig build` stays lean, the suite is built on demand by `test-shader`.
    const shader_test_exe = b.addExecutable(.{
        .name = "waterfall_descriptor_tests",
        .root_module = shader_test_mod,
    });

    const run_shader_test_cmd = b.addRunArtifact(shader_test_exe);
    const shader_test_step = b.step("test-shader", "Run shader recompiler regression tests");
    shader_test_step.dependOn(&run_shader_test_cmd.step);

    // -------------------------------------------------------------------------
    // Target 6: spin-lock contention suite
    // -------------------------------------------------------------------------
    // Measures the emulator's own synchronization primitives under the access pattern the guest's
    // job system produces, so lock layout changes can be evaluated in a second instead of by
    // booting the game. Links only the common logging/assert closure plus the lock header.
    const lock_test_flags = [_][]const u8{
        "-std=c++20",
        "-D_GNU_SOURCE",

        "-DKYTY_ENDIAN=2",
        "-D_TIMESPEC_DEFINED",
        "-Wno-pragma-pack",
        "-Wno-deprecated-declarations",
        "-Wno-unused-function",
        "-Wno-unused-variable",
    };
    const lock_test_mod = b.createModule(.{
        .target = target,
        .optimize = optimize,
    });
    lock_test_mod.link_libc = true;
    lock_test_mod.linkSystemLibrary("stdc++", .{});
    for (include_paths) |inc| {
        if (std.mem.startsWith(u8, inc, "/")) {
            lock_test_mod.addIncludePath(.{ .cwd_relative = inc });
        } else {
            lock_test_mod.addIncludePath(b.path(inc));
        }
    }
    lock_test_mod.linkSystemLibrary("pthread", .{});
    lock_test_mod.linkSystemLibrary("dl", .{});
    lock_test_mod.linkSystemLibrary("m", .{});
    lock_test_mod.addCSourceFiles(.{ .files = &sources.fmt, .flags = &lock_test_flags });
    lock_test_mod.addCSourceFiles(.{
        .files = &.{
            "tests/LockContentionTests.cpp",
            "src/common/assert.cpp",
            "src/common/dateTime.cpp",
            "src/common/emulatorConfig.cpp",
            "src/common/file.cpp",
            "src/common/logging/log.cpp",
            "src/common/platform/sysLinuxDbg.cpp",
            "src/common/platform/sysLinuxFileIO.cpp",
            "src/common/subsystems.cpp",
        },
        .flags = &lock_test_flags,
    });

    const lock_test_exe = b.addExecutable(.{
        .name = "lock_contention_tests",
        .root_module = lock_test_mod,
    });

    const run_lock_test_cmd = b.addRunArtifact(lock_test_exe);
    const lock_test_step = b.step("test-locks", "Run spin-lock contention benchmarks");
    lock_test_step.dependOn(&run_lock_test_cmd.step);

    // -------------------------------------------------------------------------
    // Target 7: synchronization primitive benchmarks
    // -------------------------------------------------------------------------
    const sync_test_mod = b.createModule(.{
        .target = target,
        .optimize = optimize,
    });
    sync_test_mod.link_libc = true;
    sync_test_mod.linkSystemLibrary("stdc++", .{});
    for (include_paths) |inc| {
        if (std.mem.startsWith(u8, inc, "/")) {
            sync_test_mod.addIncludePath(.{ .cwd_relative = inc });
        } else {
            sync_test_mod.addIncludePath(b.path(inc));
        }
    }
    sync_test_mod.linkSystemLibrary("pthread", .{});
    sync_test_mod.linkSystemLibrary("dl", .{});
    sync_test_mod.linkSystemLibrary("m", .{});
    sync_test_mod.addCSourceFiles(.{ .files = &sources.fmt, .flags = &lock_test_flags });
    sync_test_mod.addCSourceFiles(.{
        .files = &.{
            "tests/SyncPrimitiveTests.cpp",
            "src/common/assert.cpp",
            "src/common/dateTime.cpp",
            "src/common/emulatorConfig.cpp",
            "src/common/file.cpp",
            "src/common/logging/log.cpp",
            "src/common/platform/sysLinuxDbg.cpp",
            "src/common/platform/sysLinuxFileIO.cpp",
            "src/common/subsystems.cpp",
            "src/common/threads.cpp",
            "src/common/timer.cpp",
        },
        .flags = &lock_test_flags,
    });

    const sync_test_exe = b.addExecutable(.{
        .name = "sync_primitive_tests",
        .root_module = sync_test_mod,
    });

    const run_sync_test_cmd = b.addRunArtifact(sync_test_exe);
    const sync_test_step = b.step("bench-sync", "Run synchronization primitive benchmarks");
    sync_test_step.dependOn(&run_sync_test_cmd.step);
}
