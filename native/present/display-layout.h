#pragma once
#include <climits>
#include <cmath>
#include <initializer_list>
#include <stdexcept>
#include <windef.h>

namespace dspaa {
inline bool emptyRect(const RECT& r) {
    return !r.left && !r.top && !r.right && !r.bottom;
}
inline bool sameRect(const RECT& a, const RECT& b) {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}
inline RECT displayRect(unsigned width, unsigned height, RECT rect = {}) {
    if (!width || !height || width > static_cast<unsigned>(LONG_MAX) ||
        height > static_cast<unsigned>(LONG_MAX))
        throw std::invalid_argument("Invalid display dimensions");
    if (emptyRect(rect))
        return {0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    if (rect.left < 0 || rect.top < 0 || rect.right <= rect.left || rect.bottom <= rect.top ||
        static_cast<unsigned>(rect.right) > width || static_cast<unsigned>(rect.bottom) > height)
        throw std::invalid_argument("Content rectangle is outside the display");
    return rect;
}
// Unity's final blit quantizes its viewport independently of L/D aspect-fit
// arithmetic. Consume the observed display-pixel extent without recentering it.
inline RECT displayViewport(unsigned displayWidth, unsigned displayHeight, float left, float top, float width,
                            float height) {
    (void)displayRect(displayWidth, displayHeight);
    for (const auto value : {left, top, width, height})
        if (!std::isfinite(value) || value != std::floor(value))
            throw std::invalid_argument("Final display viewport must have finite integer pixel bounds");
    if (left < 0 || top < 0 || width <= 0 || height <= 0 || double(left) + width > displayWidth ||
        double(top) + height > displayHeight)
        throw std::invalid_argument("Final display viewport is outside the display");
    return {static_cast<LONG>(left), static_cast<LONG>(top), static_cast<LONG>(double(left) + width),
            static_cast<LONG>(double(top) + height)};
}
} // namespace dspaa
