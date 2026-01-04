@echo off
echo Building HN4...

:: We explicitly list files to avoid compiling backups/garbage in the folders.
:: CORE ENGINE (src/)
set CORE=src\hn4_hal.c src\hn4_crc.c src\hn4_endians.c src\hn4_swizzle.c src\hn4_format.c src\hn4_mount.c src\hn4_unmount.c src\hn4_allocator.c src\hn4_repair.c src\hn4_read.c src\hn4_write.c src\hn4_anchor.c src\hn4_ecc.c

:: TESTS (tests/)
set TESTS=tests\main.c tests\hn4_test.c tests\test_crc.c src\hn4_namespace.c src/hn4_scavenger.c src\hn4_posix_shim.c src\hn4_api.c  tests\test_endians.c tests\hn4_swizzle_test.c tests\hn4_unmount_tests.c tests\hn4_format_tests.c tests\hn4_mount_tests.c tests\hn4_allocator_tests.c tests\hn4_repair_tests.c tests\hn4_write_tests.c tests\hn4_read_tests.c tests\hn4_paranoid_tests.c tests\hn4_chaos_tests.c tests\hn4_read_edge_tests.c tests\hn4_pico_tests.c tests\hn4_ludic_tests.c tests\hn4_tensor_tests.c tests\hn4_permission_tests.c tests\hn4_snapshot_tests.c tests\hn4_anchor_tests.c tests\hn4_hardware_tests.c tests\hn4_system_tests.c tests\hn4_edge_tests.c tests\hn4_hdd_tests.c  tests\hn4_namespace_tests.c   tests\hn4_api_tests.c  tests\hn4_arch_tests.c tests\hn4_scavenger_tests.c tests\hn4_vfs_tests.c tests\hn4_frag_tests.c

:: COMPILER COMMAND
:: -Isrc -Itests : Look for headers in these folders
:: Your Flags    : -dHN4_DEBUG_LOG -O3 -dHYDRA_DEBUG -march=native
gcc -o hn4.exe %CORE% %TESTS% -Isrc -Itests -dHN4_DEBUG_LOG -O3 -dHYDRA_DEBUG -march=native

if %errorlevel% neq 0 (
    echo Build failed.
    exit /b %errorlevel%
)

echo Build succeeded. Output: hn4.exe
echo Running...
hn4.exe