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
        "-DKYTY_PLATFORM=5", // KYTY_PLATFORM_LINUX
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
}
