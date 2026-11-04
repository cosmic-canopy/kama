// The compiler's own stack (KR-304).
//
// Every walk over the AST recurses, so the stack a compile needs grows with how deep a program's expressions
// go, and the stack it was handed is the host's to choose: 8 MiB from most shells, less under a tighter `ulimit -s`,
// whatever the linker's default put in a PE. A deep expression was therefore a SIGSEGV whose depth changed
// with the machine and with the build (past 1,500 terms in a release compiler, under 200 with ASan's frames). The
// language's limit lives in the parser, and a stated limit is only true if no host can run out below it, so the
// compile runs on a stack of its own.
//
// A thread, because it is the one mechanism all three hosts have: macOS caps `setrlimit` at 64 MiB, Linux has no
// link-time setting, and a PE's reserve is fixed at link. The size is a RESERVATION: address space, with pages
// committed only as they are touched, so a small compile costs what it always did.
//
// A TU of its own because <windows.h> and <pthread.h> cannot share one with kama.parser.hpp's token enum.
#ifdef _WIN32
  #include <windows.h>
#else
  #include <pthread.h>
#endif
#include <cstddef>

namespace {

struct MainCall {
    int (*body)(int, char**);
    int argc;
    char** argv;
    int rc;
};

#ifdef _WIN32
DWORD WINAPI entry(LPVOID p)
{
    MainCall* c = static_cast<MainCall*>(p);
    c->rc = c->body(c->argc, c->argv);
    return 0;
}

bool runOnStack(size_t bytes, MainCall& c)
{
    HANDLE t = CreateThread(nullptr, bytes, entry, &c, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (!t) return false;
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
    return true;
}
#else
void* entry(void* p)
{
    MainCall* c = static_cast<MainCall*>(p);
    c->rc = c->body(c->argc, c->argv);
    return nullptr;
}

bool runOnStack(size_t bytes, MainCall& c)
{
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0) return false;
    pthread_t t;
    const bool started = pthread_attr_setstacksize(&attr, bytes) == 0 && pthread_create(&t, &attr, entry, &c) == 0;
    pthread_attr_destroy(&attr);
    if (started) pthread_join(t, nullptr);
    return started;
}
#endif

}  // namespace

// `body` is the whole compiler; a call to `exit` from inside it ends the process as it always did. A host that
// will not reserve 1 GiB (a strict `vm.overcommit_memory`, a `ulimit -v`) gets less rather than a refusal, and one
// that will not start a thread at all gets the stack it already gave.
int kama_main_on_own_stack(int (*body)(int, char**), int argc, char** argv)
{
    MainCall c = { body, argc, argv, 0 };
    for (size_t bytes = size_t(1) << 30; bytes >= (size_t(64) << 20); bytes /= 2)
        if (runOnStack(bytes, c)) return c.rc;
    return body(argc, argv);
}
