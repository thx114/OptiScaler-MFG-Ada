#include "../OptiScaler/with_dx12/PresentCapture.h"
#include <cassert>
#include <iostream>
#include <thread>

struct Image { int color = 1; int shared = 0; int copies = 0; bool fail = false; };
static bool Copy(void* context)
{
    auto& image = *static_cast<Image*>(context);
    ++image.copies;
    if (PresentCapture::current)
        PresentCapture::current->TryCopy(&image, false);
    if (image.fail)
        return false;
    image.shared = image.color;
    return true;
}

int main()
{
    Image image;
    Copy(&image); // Baseline fallback, before addon.
    PresentCapture::Request request {&image, &image, Copy};
    {
        PresentCapture::Scope scope(request);
        request.TryCopy(&image, true);
        request.TryCopy(&request, false);
        assert(!request.attempted);
        std::thread worker([] { assert(PresentCapture::current == nullptr); });
        worker.join();
        image.color = 9; // ReShade present-mode NR.
        request.TryCopy(&image, false); // Before native flip.
        image.color = -1; // Discard/rotation.
        request.TryCopy(&image, false);
        assert(image.shared == 9 && image.copies == 2 && request.succeeded);
        Image nested;
        PresentCapture::Request other {&nested, &nested, Copy};
        {
            PresentCapture::Scope nestedScope(other);
            assert(PresentCapture::current == &other);
            other.TryCopy(&nested, false);
            assert(other.succeeded);
        }
        assert(PresentCapture::current == &request);
    }
    assert(PresentCapture::current == nullptr);
    Image failure {2, 2, 0, true};
    PresentCapture::Request failed {&failure, &failure, Copy};
    {
        PresentCapture::Scope scope(failed);
        failed.TryCopy(&failure, false);
        failed.TryCopy(&failure, false);
    }
    assert(failed.attempted && !failed.succeeded && failure.shared == 2 && failure.copies == 1);
    Image skipped {3, 3};
    PresentCapture::Request notReached {&skipped, &skipped, Copy};
    { PresentCapture::Scope scope(notReached); }
    assert(!notReached.attempted && skipped.shared == 3);
    try
    {
        PresentCapture::Scope scope(notReached);
        throw 1;
    }
    catch (...) {}
    assert(PresentCapture::current == nullptr);
    std::cout << "PASS pre-flip capture: post-addon color, once-only, TEST/foreign gates, thread isolation, nesting, failure/fallback, scope cleanup\n";
}
