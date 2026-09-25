#include "ui-proof/upload.h"
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
void require(bool condition, const char* why) {
    if (!condition)
        throw std::runtime_error(why);
}
struct GuardedSource {
    unsigned char* bytes = nullptr;
    size_t page = 0;
    GuardedSource() {
        SYSTEM_INFO system{};
        GetSystemInfo(&system);
        page = system.dwPageSize;
        bytes = static_cast<unsigned char*>(
            VirtualAlloc(nullptr, 2 * page, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        require(bytes != nullptr, "Allocate private CPU copy fixture");
        DWORD before = 0;
        if (!VirtualProtect(bytes + page, page, PAGE_NOACCESS, &before)) {
            VirtualFree(bytes, 0, MEM_RELEASE);
            bytes = nullptr;
            throw std::runtime_error("Protect source boundary");
        }
    }
    ~GuardedSource() {
        if (bytes)
            VirtualFree(bytes, 0, MEM_RELEASE);
    }
};
} // namespace
int main() {
    try {
        GuardedSource source;
        const std::vector<size_t> lengths{
            0, 1, 7, 15, 16, 17, 31, 32, 63, 64, 65, 255, 256, 257, 1023, source.page - 1, source.page};
        unsigned cases = 0;
        for (const bool upload : {false, true})
            for (unsigned revision = 0; revision < 3; ++revision) {
                for (size_t i = 0; i < source.page; ++i)
                    source.bytes[i] = static_cast<unsigned char>(i ^ (i >> 8) ^ (revision * 71));
                for (const auto length : lengths)
                    for (unsigned skew : {0u, 1u}) {
                        // One placement ends exactly at an inaccessible page. The other
                        // starts aligned and exercises SIMD plus a short scalar tail.
                        for (const bool atEnd : {false, true}) {
                            const auto* input = atEnd ? source.bytes + source.page - length : source.bytes;
                            constexpr size_t margin = 32;
                            std::vector<unsigned char> output(length + 2 * margin + skew, 0xCD);
                            auto* destination = output.data() + margin + skew;
                            dspaa::proof::copyObservedBytes(destination, input, length, upload);
                            require(std::equal(input, input + length, destination),
                                    "Copy changed bytes or reused an earlier revision");
                            require(std::all_of(output.begin(), output.begin() + margin + skew,
                                                [](auto value) { return value == 0xCD; }),
                                    "Copy wrote before destination");
                            require(std::all_of(output.begin() + margin + skew + length, output.end(),
                                                [](auto value) { return value == 0xCD; }),
                                    "Copy rounded destination past the requested range");
                            ++cases;
                        }
                    }
            }
        dspaa::proof::copyObservedBytes(nullptr, nullptr, 0, true);
        std::cout << "upload_copy_cases=" << cases
                  << " streaming_available=" << dspaa::proof::streamingUploadReadsAvailable()
                  << " gpu_created=false window_created=false\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
