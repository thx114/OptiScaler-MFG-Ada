#include "../OptiScaler/dlssnr/DlssNr_DlaaPassthrough.h"
#include <cassert>
#include <cstdio>

int main()
{
    using namespace DlssNr;
    for (unsigned bits = 0; bits < 128; ++bits)
    {
        const bool bridge = bits & 1, dlss = bits & 2, enabled = bits & 4, finished = bits & 8;
        const bool deferred = bits & 16, rr = bits & 32, sharpeningHandled = bits & 64;
        const bool expected = bridge && dlss && enabled && finished && !deferred && !rr && sharpeningHandled;
        assert(UseDlaaPassthrough(bridge, dlss, enabled, finished, deferred, rr, sharpeningHandled,
                                 2560, 1600, 2560, 1600) == expected);
    }
    assert(!UseDlaaPassthrough(true, true, true, true, false, false, true, 1280, 800, 2560, 1600));
    assert(!UseDlaaPassthrough(true, true, true, true, false, false, true, 2560, 800, 2560, 1600));
    assert(!UseDlaaPassthrough(true, true, true, true, false, false, true, 0, 0, 0, 0));
    D3D12_RESOURCE_DESC s {};
    s.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    s.Width = 2560; s.Height = 1600; s.DepthOrArraySize = s.MipLevels = s.SampleDesc.Count = 1;
    s.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    auto d = s;
    const auto valid = [&] { return CanCopyDlaaColor(s, d, 2560, 1600, 0, 0, 0, 0); };
    assert(valid());
    d.Width = 3000; d.Height = 1800; assert(valid()); // Active box, not a whole-resource copy.
    d = s; d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; assert(!valid());
    d = s; d.Width = 1280; assert(!valid());
    d = s; d.Height = 800; assert(!valid());
    d = s; d.SampleDesc.Count = 2; assert(!valid());
    d = s; d.MipLevels = 2; assert(!valid());
    d = s; d.DepthOrArraySize = 2; assert(!valid());
    d = s; d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; assert(!valid());
    d = s;
    assert(!CanCopyDlaaColor(s, d, 2560, 1600, 1, 0, 0, 0));
    assert(!CanCopyDlaaColor(s, d, 2560, 1600, 0, 1, 0, 0));
    assert(!CanCopyDlaaColor(s, d, 2560, 1600, 0, 0, 1, 0));
    assert(!CanCopyDlaaColor(s, d, 2560, 1600, 0, 0, 0, 1));
    assert(!CanCopyDlaaColor(s, d, 0, 1600, 0, 0, 0, 0));
    puts("PASS NR DLAA bypass: 128 scope gates, 1:1, sharpness handling, format/extent/subrect copy guards");
}

