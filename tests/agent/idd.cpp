#include <array>
#include <cassert>
#include <vector>

#include "platform/windows/idd/idd_shared.protocol.h"
#include "platform/windows/idd_topology.transport.h"

using namespace ultraterimal::agent;

int main() {
    // 1. Adapter classification tests
    assert(isUltraterimalIddAdapter(L"Ultraterimal Virtual Display"));
    assert(isUltraterimalIddAdapter(L"ultraterimal virtual display (idd)"));
    assert(!isUltraterimalIddAdapter(L"NVIDIA GeForce RTX 4090"));

    assert(isSoftwareDisplayAdapter(L"Red Hat VirtIO GPU DOD controller", 0x1AF4));
    assert(isSoftwareDisplayAdapter(L"Microsoft Basic Display Adapter", 0x1414));
    assert(isSoftwareDisplayAdapter(L"Microsoft Hyper-V Video", 0));
    assert(isSoftwareDisplayAdapter(L"Red Hat QXL controller", 0x1B36));
    assert(isSoftwareDisplayAdapter(L"VMware SVGA 3D", 0x15AD));
    assert(!isSoftwareDisplayAdapter(L"Ultraterimal Virtual Display", 0));
    assert(!isSoftwareDisplayAdapter(L"NVIDIA GeForce RTX 4090", 0x10DE));
    assert(!isSoftwareDisplayAdapter(L"AMD Radeon RX 7900 XTX", 0x1002));
    assert(!isSoftwareDisplayAdapter(L"Intel(R) UHD Graphics 770", 0x8086));

    // 2. Dirty band computation tests
    {
        const auto empty = idd::computeDirtyBand(1920, 1080, {});
        assert(empty.fullFrame);
        assert(empty.rowBegin == 0 && empty.rowEnd == 1080);
        assert(empty.changedPixels == 1920ull * 1080ull);
    }
    {
        const std::array<idd::DirtyRect, 2> rects{{{10, 101, 210, 201}, {300, 150, 500, 305}}};
        const auto band = idd::computeDirtyBand(1920, 1080, rects);
        assert(!band.fullFrame);
        assert(band.rowBegin == 100);
        assert(band.rowEnd == 306);
        assert(band.changedPixels == 200ull * 100ull + 200ull * 155ull);
    }
    {
        // Area > 60% of frame triggers full-frame mode.
        const std::array<idd::DirtyRect, 1> huge{{{0, 0, 1920, 800}}};
        const auto band = idd::computeDirtyBand(1920, 1080, huge);
        assert(band.fullFrame);
        assert(band.rowBegin == 0 && band.rowEnd == 1080);
    }
    {
        // Out-of-bounds clamping
        const std::array<idd::DirtyRect, 1> oob{{{-50, -10, 100, 99}}};
        const auto band = idd::computeDirtyBand(1920, 1080, oob);
        assert(!band.fullFrame);
        assert(band.rowBegin == 0 && band.rowEnd == 100);
        assert(band.changedPixels == 100ull * 99ull);
    }
    {
        // Too many rects (>64) falls back to full frame.
        std::vector<idd::DirtyRect> many(65, {0, 0, 10, 10});
        const auto band = idd::computeDirtyBand(1920, 1080, many);
        assert(band.fullFrame);
    }
    return 0;
}
