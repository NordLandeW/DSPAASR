#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <smmintrin.h>
#include <windows.h>

namespace dspaa::proof {
inline bool streamingUploadReadsAvailable() noexcept {
    static const bool available = IsProcessorFeaturePresent(PF_SSE4_1_INSTRUCTIONS_AVAILABLE) != FALSE;
    return available;
}
// Copy only an already-observed CPU interval. The upload hint changes HOW bytes
// are loaded, never which bytes are valid or any resource/GPU retirement rule.
// Other CPU inputs and machines without SSE4.1 retain the ordinary copy path.
inline void copyObservedBytes(void* destination, const void* source, size_t bytes,
                              bool uploadMemory) noexcept {
    if (!bytes)
        return;
    constexpr size_t vectorBytes = sizeof(__m128i), blockBytes = 4 * vectorBytes;
    auto* to = static_cast<unsigned char*>(destination);
    auto* from = static_cast<const unsigned char*>(source);
    if (!uploadMemory || !streamingUploadReadsAvailable() ||
        reinterpret_cast<uintptr_t>(from) % vectorBytes || bytes < vectorBytes) {
        std::memcpy(to, from, bytes);
        return;
    }
    // MOVNTDQA is a WC streaming load. MFENCE orders it against the application's
    // preceding mapped writes and later ordinary accesses. A mutex/locked op is
    // not a substitute. These are CPU barriers, NOT a D3D Flush or GPU fence.
    _mm_mfence();
    while (bytes >= blockBytes) {
        const auto* input = reinterpret_cast<const __m128i*>(from);
        const auto a = _mm_stream_load_si128(input), b = _mm_stream_load_si128(input + 1);
        const auto c = _mm_stream_load_si128(input + 2), d = _mm_stream_load_si128(input + 3);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(to), a);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(to + vectorBytes), b);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(to + 2 * vectorBytes), c);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(to + 3 * vectorBytes), d);
        from += blockBytes;
        to += blockBytes;
        bytes -= blockBytes;
    }
    while (bytes >= vectorBytes) {
        const auto value = _mm_stream_load_si128(reinterpret_cast<const __m128i*>(from));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(to), value);
        from += vectorBytes;
        to += vectorBytes;
        bytes -= vectorBytes;
    }
    _mm_mfence();
    // Never round a short physical tail up to a SIMD read.
    if (bytes)
        std::memcpy(to, from, bytes);
}
} // namespace dspaa::proof
