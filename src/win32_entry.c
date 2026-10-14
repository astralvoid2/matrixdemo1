/* GUI entry point; the console edition calls main() directly. UTF-8 argv
 * keeps WAV/export paths working for non-ASCII Windows user names. */
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include "platform.h"

int demo_main(int argc, char **argv);

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR command_line, int show)
{
    (void)instance; (void)previous; (void)command_line; (void)show;
    char log_path[DEMO_PATH_CAPACITY] = "";
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE *stream;
        freopen_s(&stream, "CONOUT$", "w", stdout);
        freopen_s(&stream, "CONOUT$", "w", stderr);
    } else if (platform_data_path(log_path, sizeof log_path, "matrixrain.log", 1) == 0) {
        wchar_t wide_log[DEMO_PATH_CAPACITY];
        if (MultiByteToWideChar(CP_UTF8, 0, log_path, -1, wide_log, DEMO_PATH_CAPACITY)) {
            _wfreopen(wide_log, L"w", stdout);
            _wfreopen(wide_log, L"a", stderr);
        }
    }
    int argc = 0;
    LPWSTR *wide_argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!wide_argv) return 1;
    char **argv = calloc((size_t)argc + 1, sizeof *argv);
    int result = 1;
    if (!argv) goto done;
    for (int i = 0; i < argc; i++) {
        int bytes = WideCharToMultiByte(CP_UTF8, 0, wide_argv[i], -1, NULL, 0, NULL, NULL);
        argv[i] = bytes ? malloc((size_t)bytes) : NULL;
        if (!argv[i] || !WideCharToMultiByte(CP_UTF8, 0, wide_argv[i], -1, argv[i], bytes, NULL, NULL)) goto done;
    }
    result = demo_main(argc, argv);
done:
    if (argv) { for (int i = 0; i < argc; i++) free(argv[i]); free(argv); }
    LocalFree(wide_argv);
    fflush(stdout);
    fflush(stderr);
    if (result && *log_path)
        MessageBoxW(NULL, L"Matrix Rain could not start. See %LOCALAPPDATA%\\MatrixRain\\matrixrain.log for details.",
                    L"Matrix Rain", MB_OK | MB_ICONERROR);
    return result;
}
