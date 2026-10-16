// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// The Windows half of the host shim
//
#include <windows.h>

#include <atomic>

#include <fcntl.h>
#include <io.h>

#include "plat.h"
#include "win_util.h"

namespace {

// Set from the console control handler, which Windows runs on a thread of its
// own, so it is read and written as an atomic rather than a plain flag.
std::atomic<bool> g_interrupted{false};

BOOL WINAPI onConsoleEvent(DWORD event)
{
    switch (event) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
            // A second one, with the loop still not unwinding because a call
            // is wedged, falls through to the default handler, which ends
            // the process -- what SA_RESETHAND gives the POSIX side.
            return g_interrupted.exchange(true) ? FALSE : TRUE;

        case CTRL_CLOSE_EVENT:
            // Closing the console window ends the process as soon as this
            // handler returns, so it does not return: it asks the loop to
            // unwind and waits here. main() returning ends the process, this
            // thread with it; should that take longer than Windows waits for
            // (a few seconds), Windows ends it anyway.
            g_interrupted = true;
            Sleep(INFINITE);
            return TRUE;

        default:
            // Logoff and shutdown only ever reach a console program that runs
            // as a service, and the default handling is the right one there.
            return FALSE;
    }
}

}

void Plat::installInterruptHandler()
{
    SetConsoleCtrlHandler(onConsoleEvent, TRUE);
}

bool Plat::interruptRequested()
{
    return g_interrupted;
}

std::vector<std::string> Plat::arguments(int, char*[])
{
    // main()'s arguments are deliberately ignored; see plat.h.
    return Win::commandLineArguments();
}

std::filesystem::path Plat::path(const std::string& utf8)
{
    return std::filesystem::path(Win::widen(utf8));
}

Plat::StdioSetup::StdioSetup()
{
    // meta_save reads a blob from stdin and meta_load writes one to stdout,
    // byte for byte. In the C runtime's default text mode CR LF would become
    // LF on the way in and LF would become CR LF on the way out, and a 0x1A
    // would end the input early. stderr follows suit, so that every stream
    // ends its lines the same way, as the POSIX side's do.
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);

    // Everything the tool prints is UTF-8, which a console only shows as such
    // when told to. The setting belongs to the console, not to this process,
    // so the one it had is put back on the way out.
    m_codePage = GetConsoleOutputCP();
    if (m_codePage != 0 && m_codePage != CP_UTF8) {
        SetConsoleOutputCP(CP_UTF8);
    } else {
        m_codePage = 0;
    }
}

Plat::StdioSetup::~StdioSetup()
{
    if (m_codePage != 0) {
        SetConsoleOutputCP(m_codePage);
    }
}
