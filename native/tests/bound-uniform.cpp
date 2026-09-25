#include "ui-proof/constants.h"
#include "ui-proof/default-ui.h"
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {
void require(bool value,const char* reason) {
    if (!value) {
        std::cerr << reason << '\n';
        std::exit(1);
    }
}
void defaultUiConstants() {
    using dspaa::proof::defaultUiFailure;
    const std::array<float, 4> tint{1, 2, 4, 0.5f}, add{0, 1, 2, 0};
    require(!defaultUiFailure(tint, add, 0.f), "Finite nonnegative HDR UI tint/sample-add was rejected");
    for (float alpha : {0.f, 1.f}) {
        auto color = tint;
        color[3] = alpha;
        require(!defaultUiFailure(color, add, 0.f), "Legal UI material alpha endpoint was rejected");
    }
    auto signedZero = add;
    signedZero[3] = -0.f;
    require(!defaultUiFailure(tint, signedZero, 0.f),
            "Arithmetic sample-add zero was confused with a raw shader boolean");
    const auto infinity = std::numeric_limits<float>::infinity();
    const auto nan = std::numeric_limits<float>::quiet_NaN();
    for (float alpha : {-0.01f, std::nextafter(1.f, infinity), infinity, -infinity, nan}) {
        auto color = tint;
        color[3] = alpha;
        require(defaultUiFailure(color, add, 0.f), "Out-of-range/nonfinite material alpha was certified");
    }
    for (float alpha : {-0.01f, 0.01f, infinity, -infinity, nan}) {
        auto sampleAdd = add;
        sampleAdd[3] = alpha;
        require(defaultUiFailure(tint, sampleAdd, 0.f), "Nonzero/nonfinite alpha sample-add was certified");
    }
    for (unsigned channel = 0; channel < 3; ++channel) {
        for (float invalid : {-0.01f, infinity, -infinity, nan}) {
            auto color = tint;
            color[channel] = invalid;
            color[3] = 0;
            require(defaultUiFailure(color, add, 0.f), "Zero alpha hid signed/nonfinite pre-premultiply RGB");
            auto sampleAdd = add;
            sampleAdd[channel] = invalid;
            color = tint;
            color[channel] = 0;
            require(defaultUiFailure(color, sampleAdd, 0.f), "Zero tint hid invalid sampled RGB arithmetic");
        }
    }
    const auto maximum = std::numeric_limits<float>::max();
    auto color = tint;
    color[0] = maximum;
    require(!defaultUiFailure(color, add, 0.f), "Finite maximum tint without RGB amplification was rejected");
    auto amplified = add;
    amplified[0] = 1;
    require(defaultUiFailure(color, amplified, 0.f),
            "Finite inputs with overflowing RGB product were certified");
    color[0] = maximum / 4;
    require(!defaultUiFailure(color, amplified, 0.f), "Safe large finite RGB product was rejected");
    // This exact real product fits FP32, but rounding (sampleAdd + 1) upward
    // before multiplication makes the actual shader multiply overflow.
    color[0] = std::bit_cast<float>(std::uint32_t{0x737ffffc});
    amplified[0] = std::bit_cast<float>(std::uint32_t{0x4b800001});
    require(defaultUiFailure(color, amplified, 0.f),
            "RGB proof ignored the intermediate FP32 addition rounding");

    // The gamma flag is an integer CB word, including when it looks like float
    // -0 or a denormal. Read its actual lane through a nonzero binding window;
    // neither the range reader nor the numerical admission may normalize bits.
    for (std::uint32_t word : {0u, 1u, 0x80000000u, 0x3f800000u, 0x7fc00000u, 0xffffffffu}) {
        unsigned observed = 0;
        auto cell = [&](unsigned offset, std::array<float, 4>& result) {
            observed = offset;
            result = {9, 9, std::bit_cast<float>(word), 9};
            return offset == 352;
        };
        std::array<float, 4> gamma{};
        require(dspaa::proof::readBoundFloats(512, 16, 16, 104, 1, cell, gamma) && observed == 352 &&
                    std::bit_cast<std::uint32_t>(gamma[0]) == word,
                "Gamma CB word changed bits or ignored its shader binding window");
        require((defaultUiFailure(tint, add, gamma[0]) == nullptr) == (word == 0),
                "Default UI movc condition was treated as floating-point zero instead of raw zero bits");
    }
}
}
int main() {
    std::vector<unsigned> reads;
    auto cells=[&](unsigned offset,std::array<float,4>& value) {
        reads.push_back(offset);
        for(unsigned i=0;i<4;++i)value[i]=static_cast<float>(offset/4+i);
        return true;
    };
    std::array<float,4> value{9,9,9,9};
    using dspaa::proof::readBoundFloats;
    using dspaa::proof::BoundReadFailure;
    constexpr unsigned small=65536,large=1024*1024;
    // A scalar at byte 108 is lane 3 of the seventh float4, not register 108.
    // The actual binding begins at byte 256 and must not read the pool prefix.
    require(readBoundFloats(small,16,16,108,1,cells,value),"Pinned scalar binding was rejected");
    require(value==std::array<float,4>{91,0,0,0} && reads==std::vector<unsigned>{352},"Scalar offset or slice origin was lost");
    reads.clear();
    require(readBoundFloats(small,16,16,108,4,cells,value),"Cross-cell vector was rejected");
    require(value==std::array<float,4>{91,92,93,94} && reads==std::vector<unsigned>{352,368},"Cross-cell vector read the wrong lanes");
    reads.clear();
    require(readBoundFloats(small,0,1,12,1,cells,value) && value[0]==3,"Last in-range scalar was rejected");
    require(readBoundFloats(small,4095,1,0,4,cells,value) && value[3]==16383,"Last legacy constant was rejected");
    auto failed=[&](unsigned offset,std::array<float,4>& result){return offset==352 && cells(offset,result);};
    value.fill(9);
    require(!readBoundFloats(small,16,16,108,4,failed,value) && value==std::array<float,4>{},"Failure exposed a partial vector");

    // A legitimate 64KiB window can start well beyond byte 65536 in a pool.
    reads.clear();
    require(readBoundFloats(large,4096,4096,108,4,cells,value),"Large allocation's remote window was rejected");
    require(value==std::array<float,4>{16411,16412,16413,16414} && reads==std::vector<unsigned>{65632,65648},
        "Remote window was truncated/wrapped or read the pool prefix");
    reads.clear();
    require(readBoundFloats(large,32768,4096,65520,4,cells,value),"Last shader-visible cell of a distant window was rejected");
    require(reads==std::vector<unsigned>{589808} && value[3]==147455,"Shader-window endpoint used an allocation-relative limit");

    // Get may expose a window larger than its physical allocation. Only the
    // touched cells need to exist, not every byte of the returned window.
    reads.clear();
    require(readBoundFloats(256,0,4096,108,4,cells,value),"Ordinary full-window binding to a small buffer was rejected");
    require(value==std::array<float,4>{27,28,29,30} && reads==std::vector<unsigned>{96,112},"Small allocation intersection was lost");
    reads.clear();
    require(readBoundFloats(65600,4096,4096,48,4,cells,value),"Physical tail inside a larger returned window was rejected");
    require(reads==std::vector<unsigned>{65584} && value[3]==16399,"Physical tail cell was read incorrectly");

    // Exercise the top of the UINT byte-address range without allocating a
    // multi-gigabyte buffer. Callback addresses, not rounded float indices,
    // distinguish the legal high cell from a silently wrapped low address.
    constexpr unsigned maximum=std::numeric_limits<unsigned>::max();
    reads.clear();
    auto highCell=[&](unsigned offset,std::array<float,4>& result) {
        reads.push_back(offset);result={1,2,3,4};return true;
    };
    require(readBoundFloats(maximum-15,0x0ffffff0u,4096,224,4,highCell,value),"High physical cell overflowed an intermediate address");
    require(reads==std::vector<unsigned>{0xffffffe0u} && value==std::array<float,4>{1,2,3,4},"High cell address narrowed too early");

    reads.clear();value.fill(9);
    BoundReadFailure failure=BoundReadFailure::None;
    auto remoteFailure=[&](unsigned offset,std::array<float,4>& result) {
        cells(offset,result);return offset==65632;
    };
    require(!readBoundFloats(large,4096,4096,108,4,remoteFailure,value,&failure),"Remote second-cell failure was accepted");
    require(reads==std::vector<unsigned>{65632,65648} && value==std::array<float,4>{} && failure==BoundReadFailure::Cell,
        "Remote failure published a partial/stale vector");
    reads.clear();value.fill(9);
    auto throwing=[&](unsigned offset,std::array<float,4>& result) {
        cells(offset,result);if(offset==65648)throw std::runtime_error("Synthetic cell failure");return true;
    };
    bool threw=false;
    try {readBoundFloats(large,4096,4096,108,4,throwing,value);}catch(const std::runtime_error&){threw=true;}
    require(threw && value==std::array<float,4>{},"Cell exception leaked a partially read vector");

    struct Invalid {unsigned allocation,first,count,offset,components;};
    const Invalid invalid[]={
        {small,0,1,12,2},{small,0,1,2,1},{small,4096,1,0,1},{small,0,0,0,1},
        {large,0,4097,0,1},{small,0,1,0,0},{small,0,1,0,5},{large,0,4096,65536,1},
        {maximum,maximum,1,0,1},{large,0,1,maximum,1},{0,0,4096,0,1},
        {65600,4096,4096,60,2}, // Requested bytes cross the physical tail.
        {65556,4096,4096,16,1}, // Scalar fits, but its sparse float4 cell does not.
        {large,32768,16,252,2},{large,32768,4096,65532,2}, // Window tails, not allocation tails.
        {maximum-15,0x10000000u,16,0,1}, // first*16 must not wrap to byte zero.
        {maximum-15,0x0ffffff0u,4096,256,1} // Origin fits UINT; adding the pin must not wrap.
    };
    for(const auto& item:invalid) {
        reads.clear();value.fill(9);failure=BoundReadFailure::None;
        require(!readBoundFloats(item.allocation,item.first,item.count,item.offset,item.components,cells,value,&failure),"Invalid range was accepted");
        require(reads.empty() && value==std::array<float,4>{} && failure!=BoundReadFailure::None,
            "Invalid range touched memory, exposed stale data or lost its failure category");
    }
    defaultUiConstants();
    std::cout<<"Bound uniform components and Default UI numerical admission: binding windows, raw gamma bits, alpha and finite nonnegative RGB passed\n";
}
