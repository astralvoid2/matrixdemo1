cmake_minimum_required(VERSION 3.24)
project(MatrixRain VERSION 1.0 LANGUAGES C)

if(MATRIXRAIN_ZIG_EXECUTABLE)
    # Windows/Clang's platform rules are loaded during project(), so override
    # the archive commands here, after those rules have been initialized.
    set(CMAKE_C_ARCHIVE_CREATE "<CMAKE_AR> ar qc <TARGET> <LINK_FLAGS> <OBJECTS>")
    set(CMAKE_C_ARCHIVE_FINISH "<CMAKE_RANLIB> ranlib <TARGET>")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
if(MSVC)
    # Match GLFW and the application, and avoid a VC++ redistributable install
    # for release builds by linking the MSVC runtime statically.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# Build GLFW statically. Its pinned source also includes a generated GLAD 3.3
# loader, so neither a separate loader install nor Python is required.
include(FetchContent)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(glfw
    URL https://github.com/glfw/glfw/archive/refs/tags/3.4.zip
    URL_HASH SHA256=a133ddc3d3c66143eba9035621db8e0bcf34dba1ee9514a9e23e96afd39fd57a
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(glfw)

set(generated_dir "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(embedded_header "${generated_dir}/embedded_shaders.h")
add_custom_command(OUTPUT "${embedded_header}"
    COMMAND "${CMAKE_COMMAND}"
        "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/src"
        "-DOUTPUT_FILE=${embedded_header}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedShaders.cmake"
    DEPENDS src/shader.frag src/post.frag cmake/EmbedShaders.cmake
    VERBATIM)

set(audio_backend src/audio.c)
if(WIN32)
    set(audio_backend src/audio_win32.c)
endif()
add_library(matrixrain_core OBJECT
    src/main.c src/hud.c src/synth.c src/audio_common.c src/platform.c
    src/gl_loader.c ${audio_backend} "${embedded_header}")
target_include_directories(matrixrain_core PRIVATE "${generated_dir}" "${glfw_SOURCE_DIR}/deps")
target_compile_definitions(matrixrain_core PRIVATE DEMO_USE_GLAD DEMO_EMBEDDED_C)
target_link_libraries(matrixrain_core PUBLIC glfw)

if(MSVC)
    target_compile_definitions(matrixrain_core PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(matrixrain_core PRIVATE /W4 /utf-8)
else()
    target_compile_options(matrixrain_core PRIVATE -Wall -Wextra)
endif()
if(WIN32)
    target_compile_definitions(matrixrain_core PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0a00)
    target_link_libraries(matrixrain_core PUBLIC ole32 uuid avrt shell32 psapi)
    add_executable(matrixrain WIN32 src/win32_entry.c src/windows.manifest)
    # The console edition preserves --help, --export and diagnostic output.
    add_executable(matrixrain-console src/console_entry.c src/windows.manifest)
    foreach(target matrixrain matrixrain-console)
        target_link_libraries(${target} PRIVATE matrixrain_core)
        if(MSVC)
            target_compile_options(${target} PRIVATE /utf-8)
        endif()
    endforeach()
    target_include_directories(matrixrain PRIVATE src)
    target_compile_definitions(matrixrain PRIVATE _CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN NOMINMAX)
    if(NOT MSVC)
        target_link_options(matrixrain PRIVATE -municode)
        if(MATRIXRAIN_ZIG_EXECUTABLE)
            # Zig cc accepts the explicit linker subsystem option; CMake's
            # usual -mwindows driver option alone leaves a console subsystem.
            target_link_options(matrixrain PRIVATE -Wl,--subsystem,windows)
            find_program(POWERSHELL_EXECUTABLE NAMES pwsh powershell REQUIRED)
            set(windows_resource "${CMAKE_CURRENT_BINARY_DIR}/windows.res")
            add_custom_command(OUTPUT "${windows_resource}"
                COMMAND "${POWERSHELL_EXECUTABLE}" -NoProfile -NonInteractive -ExecutionPolicy Bypass
                    -File "${CMAKE_CURRENT_SOURCE_DIR}/cmake/WriteWindowsResource.ps1"
                    -Manifest "${CMAKE_CURRENT_SOURCE_DIR}/src/windows.manifest" -Output "${windows_resource}"
                DEPENDS src/windows.manifest cmake/WriteWindowsResource.ps1 VERBATIM)
            set_source_files_properties("${windows_resource}" PROPERTIES EXTERNAL_OBJECT TRUE)
            target_sources(matrixrain PRIVATE "${windows_resource}")
            target_sources(matrixrain-console PRIVATE "${windows_resource}")
        else()
            enable_language(RC)
            target_sources(matrixrain PRIVATE src/windows.rc)
            target_sources(matrixrain-console PRIVATE src/windows.rc)
        endif()
    endif()
else()
    find_package(ALSA REQUIRED)
    find_package(Threads REQUIRED)
    target_link_libraries(matrixrain_core PUBLIC ALSA::ALSA Threads::Threads m)
    add_executable(matrixrain src/console_entry.c)
    target_link_libraries(matrixrain PRIVATE matrixrain_core)
endif()

set_target_properties(matrixrain PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/bin")
if(WIN32)
    set_target_properties(matrixrain-console PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/bin")
endif()
include(CTest)
if(BUILD_TESTING)
    # A headless check of synthesis, WAV output and the native executable.
    if(WIN32)
        set(test_program matrixrain-console)
    else()
        set(test_program matrixrain)
    endif()
    add_test(NAME command_line_help COMMAND ${test_program} --help)
    add_test(NAME export_music COMMAND ${test_program} --export "${CMAKE_CURRENT_BINARY_DIR}/test-track.wav")
    add_executable(audio-data-tests tests/audio_data_test.c src/audio_common.c src/synth.c src/platform.c)
    target_include_directories(audio-data-tests PRIVATE src)
    if(WIN32)
        find_program(POWERSHELL_EXECUTABLE NAMES pwsh powershell REQUIRED)
        add_test(NAME windows_executables
            COMMAND "${POWERSHELL_EXECUTABLE}" -NoProfile -NonInteractive -ExecutionPolicy Bypass
                -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/WindowsExecutableTest.ps1"
                -GuiExe $<TARGET_FILE:matrixrain> -ConsoleExe $<TARGET_FILE:matrixrain-console>)
        target_link_libraries(audio-data-tests PRIVATE ole32 shell32 uuid psapi)
        target_compile_definitions(audio-data-tests PRIVATE _CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN NOMINMAX)
        add_executable(audio-clock-test tests/audio_clock_test.c src/audio_win32.c src/audio_common.c src/synth.c src/platform.c)
        target_include_directories(audio-clock-test PRIVATE src)
        target_compile_definitions(audio-clock-test PRIVATE _CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0a00)
        target_link_libraries(audio-clock-test PRIVATE ole32 shell32 uuid psapi avrt)
        add_test(NAME wasapi_clock COMMAND audio-clock-test)
        set_tests_properties(wasapi_clock PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30)
    else()
        target_link_libraries(audio-data-tests PRIVATE m)
    endif()
    add_test(NAME audio_data COMMAND audio-data-tests)
    option(MATRIXRAIN_TEST_RENDERING "Run the hidden-window OpenGL integration test" OFF)
    if(MATRIXRAIN_TEST_RENDERING)
        add_test(NAME render_scene COMMAND ${test_program} --smoke-test --hud)
        set_tests_properties(render_scene PROPERTIES TIMEOUT 30)
    endif()
endif()
