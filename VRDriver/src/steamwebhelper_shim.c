#include <windows.h>
#include <stdlib.h>
#include <wchar.h>

static const wchar_t *arguments_after_executable(const wchar_t *command_line) {
    int inside_quotes = 0;
    const wchar_t *cursor = command_line;

    while (*cursor) {
        if (*cursor == L'"') {
            inside_quotes = !inside_quotes;
        } else if (*cursor == L' ' && !inside_quotes) {
            while (*cursor == L' ') cursor++;
            return cursor;
        }
        cursor++;
    }
    return cursor;
}

int wmain(void) {
    wchar_t module_path[MAX_PATH];
    DWORD module_length = GetModuleFileNameW(NULL, module_path, MAX_PATH);
    if (module_length == 0 || module_length >= MAX_PATH) return 1;

    wchar_t *separator = wcsrchr(module_path, L'\\');
    if (!separator) return 1;
    separator[1] = L'\0';

    const wchar_t *real_name = L"steamwebhelper_real.exe";
    size_t real_path_length = wcslen(module_path) + wcslen(real_name) + 1;
    wchar_t *real_path = (wchar_t *)calloc(real_path_length, sizeof(wchar_t));
    if (!real_path) return 1;
    wcscpy(real_path, module_path);
    wcscat(real_path, real_name);

    const wchar_t *tail = arguments_after_executable(GetCommandLineW());
    const wchar_t *injected_flags = L" --disable-gpu --single-process";
    size_t command_length = wcslen(real_path) + wcslen(injected_flags) + wcslen(tail) + 8;
    wchar_t *command_line = (wchar_t *)calloc(command_length, sizeof(wchar_t));
    if (!command_line) {
        free(real_path);
        return 1;
    }

    wcscpy(command_line, L"\"");
    wcscat(command_line, real_path);
    wcscat(command_line, L"\"");
    wcscat(command_line, injected_flags);
    if (*tail) {
        wcscat(command_line, L" ");
        wcscat(command_line, tail);
    }

    STARTUPINFOW startup = {0};
    PROCESS_INFORMATION process = {0};
    startup.cb = sizeof(startup);
    BOOL started = CreateProcessW(
        real_path,
        command_line,
        NULL,
        NULL,
        TRUE,
        0,
        NULL,
        NULL,
        &startup,
        &process
    );
    if (!started) {
        free(command_line);
        free(real_path);
        return 1;
    }

    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exit_code = 1;
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    free(command_line);
    free(real_path);
    return (int)exit_code;
}
