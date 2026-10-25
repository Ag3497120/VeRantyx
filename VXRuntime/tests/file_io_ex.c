// ReadFileEx / WriteFileEx on a file opened for overlapped I/O (src/kernel32.c), as Microsoft documents them: the
// transfer at the OVERLAPPED's offset, the completion routine called with (error, bytes, OVERLAPPED) on the issuing
// thread only in an alertable wait (which then returns WAIT_IO_COMPLETION), hEvent left to the program, reading past
// the end failing with ERROR_HANDLE_EOF. Steam's content system imports WriteFileEx; it failed with
// ERROR_NOT_SUPPORTED here and a download of Steam's redistributables stopped with "Disk write failure".
#include <windows.h>
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { printf("file_io_ex: FAILED %s (line %d, last error %lu)\n", #c, __LINE__, GetLastError()); failures++; } } while (0)

static int calls;
static DWORD last_error, last_bytes;
static OVERLAPPED *last_ov;
static DWORD last_tid;
static VOID CALLBACK done(DWORD error, DWORD bytes, LPOVERLAPPED ov) {
    calls++; last_error = error; last_bytes = bytes; last_ov = ov; last_tid = GetCurrentThreadId();
}

int main(void) {
    char path[MAX_PATH + 32], tmp[MAX_PATH];
    GetTempPathA(sizeof tmp, tmp);
    snprintf(path, sizeof path, "%svxrt_file_io_ex.bin", tmp);
    HANDLE f = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_FLAG_OVERLAPPED, NULL);
    CHECK(f != INVALID_HANDLE_VALUE);
    LARGE_INTEGER size = { .QuadPart = 8990552 };                 // preallocated, then written in the middle
    CHECK(SetFilePointerEx(f, size, NULL, FILE_BEGIN) && SetEndOfFile(f));

    static char data[1 << 20], back[1 << 20];
    for (size_t i = 0; i < sizeof data; i++) data[i] = (char)(i * 7 + 3);
    OVERLAPPED ov = { 0 };
    ov.Offset = 4194304;
    ov.hEvent = (HANDLE)(intptr_t)0x1234;                          // the program's own field
    CHECK(WriteFileEx(f, data, sizeof data, &ov, done));
    CHECK(calls == 0);                                             // not before an alertable wait
    Sleep(10);
    CHECK(calls == 0);
    CHECK(SleepEx(1000, TRUE) == WAIT_IO_COMPLETION);
    CHECK(calls == 1 && last_error == 0 && last_bytes == sizeof data && last_ov == &ov && last_tid == GetCurrentThreadId());
    CHECK(ov.hEvent == (HANDLE)(intptr_t)0x1234);
    CHECK(ov.Internal == 0 && ov.InternalHigh == sizeof data);

    calls = 0;
    OVERLAPPED rov = { 0 };
    rov.Offset = 4194304;
    CHECK(ReadFileEx(f, back, sizeof back, &rov, done));
    CHECK(WaitForSingleObjectEx(GetCurrentProcess(), 1000, TRUE) == WAIT_IO_COMPLETION);
    CHECK(calls == 1 && last_bytes == sizeof back && last_ov == &rov);
    CHECK(!memcmp(data, back, sizeof data));
    LARGE_INTEGER now;
    CHECK(GetFileSizeEx(f, &now) && now.QuadPart == size.QuadPart);

    // Past the end: fails at once, nothing queued.
    calls = 0;
    OVERLAPPED eov = { 0 };
    eov.Offset = 9000000;
    SetLastError(0);
    CHECK(!ReadFileEx(f, back, 16, &eov, done));
    CHECK(GetLastError() == ERROR_HANDLE_EOF);
    CHECK(SleepEx(0, TRUE) == 0 && calls == 0);

    CloseHandle(f);
    DeleteFileA(path);
    if (!failures) printf("file_io_ex: ok\n");
    return failures != 0;
}
