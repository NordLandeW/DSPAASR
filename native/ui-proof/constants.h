#pragma once
#include <array>
#include <algorithm>
#include <cstdint>

namespace dspaa::proof {
enum class BoundReadFailure { None, Components, Alignment, Window, WindowRange, AllocationRange, Cell };
// Get*ConstantBuffers1 reports 16-byte constants; reflection offsets are bytes
// relative to that window. The allocation may exceed 64KiB, but the shader's
// window never does. Only the requested bytes/cells must be physically present;
// the rest of a valid binding window may extend beyond the allocation.
// Out-of-allocation/window shader zero-fill is deliberately NOT emulated here.
// Read only touched, already-observed float4 cells, publishing nothing on failure.
template<class ReadCell>
bool readBoundFloats(unsigned allocationBytes, unsigned firstConstant, unsigned constantCount, unsigned byteOffset,
                     unsigned components, ReadCell&& readCell, std::array<float,4>& output,
                     BoundReadFailure* failure = nullptr) {
    output = {};
    if (failure) *failure = BoundReadFailure::None;
    const auto fail = [&](BoundReadFailure why) { if (failure) *failure = why; return false; };
    if (!components || components > 4) return fail(BoundReadFailure::Components);
    if (byteOffset % 4) return fail(BoundReadFailure::Alignment);
    if (!constantCount || constantCount > 4096) return fail(BoundReadFailure::Window);
    const std::uint64_t bytes = components * 4;
    const std::uint64_t windowBytes = static_cast<std::uint64_t>(constantCount) * 16;
    if (byteOffset > windowBytes || bytes > windowBytes - byteOffset) return fail(BoundReadFailure::WindowRange);
    // All inputs are 32-bit D3D UINTs. These products/sums fit in uint64_t;
    // narrow only after proving the complete last cell fits the UINT allocation.
    const std::uint64_t absolute = static_cast<std::uint64_t>(firstConstant) * 16 + byteOffset;
    const std::uint64_t end = absolute + bytes;
    const std::uint64_t cellEnd = ((end - 1) / 16 + 1) * 16;
    if (end > allocationBytes || cellEnd > allocationBytes) return fail(BoundReadFailure::AllocationRange);
    std::array<float,4> result{};
    unsigned copied = 0;
    while (copied < components) {
        const unsigned position = static_cast<unsigned>(absolute + copied * 4);
        const unsigned within = (position % 16) / 4;
        const unsigned take = std::min(components - copied, 4 - within);
        std::array<float,4> cell{};
        if (!readCell(position - position % 16, cell)) return fail(BoundReadFailure::Cell);
        std::copy_n(cell.begin() + within, take, result.begin() + copied);
        copied += take;
    }
    output = result;
    return true;
}
} // namespace dspaa::proof
