#pragma once

// One synchronous, thread-local capture at the native Present boundary. The caller keeps
// both the target COM object and the copy context alive for the whole scope.
namespace PresentCapture
{
struct Request
{
    const void* target;
    void* context;
    bool (*copy)(void*);
    bool attempted = false;
    bool succeeded = false;

    void TryCopy(const void* presenting, bool test)
    {
        if (test || presenting != target || attempted)
            return;
        attempted = true; // Set before calling out: nested Present must not copy twice.
        succeeded = copy(context);
    }
};

inline thread_local Request* current = nullptr;

class Scope
{
    Request* previous;

  public:
    explicit Scope(Request& request) : previous(current) { current = &request; }
    ~Scope() { current = previous; }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
};
}
