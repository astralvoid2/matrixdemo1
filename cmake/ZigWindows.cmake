# Zig bundles Clang, the Windows C runtime/headers and the archiver. Keep all
# generated toolchain state local to the repository; no SDK install needed.
if(NOT MATRIXRAIN_ZIG_EXECUTABLE)
    message(FATAL_ERROR "Set MATRIXRAIN_ZIG_EXECUTABLE to the downloaded zig.exe")
endif()
set(CMAKE_C_COMPILER "${MATRIXRAIN_ZIG_EXECUTABLE}" CACHE FILEPATH "" FORCE)
set(CMAKE_C_COMPILER_ARG1 cc CACHE STRING "" FORCE)
set(CMAKE_AR "${MATRIXRAIN_ZIG_EXECUTABLE}" CACHE FILEPATH "" FORCE)
set(CMAKE_RANLIB "${MATRIXRAIN_ZIG_EXECUTABLE}" CACHE FILEPATH "" FORCE)
# try_compile uses a separate CMake project and must receive this path too.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES MATRIXRAIN_ZIG_EXECUTABLE)
