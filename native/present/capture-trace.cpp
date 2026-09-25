#include "capture-trace.h"
#include "capture/hooks.h"
#include <DirectXPackedVector.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <stdexcept>
#include <limits>

namespace dspaa {
namespace {
using Microsoft::WRL::ComPtr;
using namespace DirectX::PackedVector;
struct Budget { uint64_t bytes=0; };
struct Snapshot {
    std::shared_ptr<Budget> budget;
    uint64_t bytes=0,frame=0,generation=0,pass=0;
    unsigned width=0,height=0;
    DXGI_FORMAT view=DXGI_FORMAT_UNKNOWN;
    ComPtr<ID3D11Texture2D> texture;
    ~Snapshot(){if(budget)budget->bytes-=bytes;}
};
struct Pair {
    std::shared_ptr<Snapshot> before,after;
    ComPtr<ID3D11Query> ready;
};
struct Mapping {
    ID3D11DeviceContext* context;
    ID3D11Texture2D* texture;
    D3D11_MAPPED_SUBRESOURCE data{};
    Mapping(ID3D11DeviceContext* c,ID3D11Texture2D* t):context(c),texture(t){
        graphicsCheck(context->Map(texture,0,D3D11_MAP_READ,0,&data),"Map completed camera diagnostic");
    }
    ~Mapping(){context->Unmap(texture,0);}
};
bool half(DXGI_FORMAT format){return format==DXGI_FORMAT_R16G16B16A16_FLOAT;}
bool srgb(DXGI_FORMAT format){return format==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;}
bool supported(DXGI_FORMAT format){return half(format)||srgb(format)||format==DXGI_FORMAT_R8G8B8A8_UNORM;}
float decode(float value){return value<=.04045f?value/12.92f:std::pow((value+.055f)/1.055f,2.4f);}
float encode(float value){return value<=.0031308f?value*12.92f:1.055f*std::pow(value,1.f/2.4f)-.055f;}
std::array<float,4> pixel(const Snapshot& image,const Mapping& map,unsigned x,unsigned y,bool interpretSrgb=true){
    std::array<float,4> result{};
    const auto* at=static_cast<const unsigned char*>(map.data.pData)+y*map.data.RowPitch+x*(half(image.view)?8:4);
    for(unsigned c=0;c<4;++c){
        if(half(image.view)){uint16_t value;std::memcpy(&value,at+c*2,2);result[c]=XMConvertHalfToFloat(value);}
        else {result[c]=at[c]/255.f;if(c<3&&interpretSrgb&&srgb(image.view))result[c]=decode(result[c]);}
    }
    return result;
}
float quantized(float value,DXGI_FORMAT format,unsigned channel){
    if(half(format))return XMConvertHalfToFloat(XMConvertFloatToHalf(value));
    if(channel<3&&srgb(format))value=encode(value);
    value=std::round(std::clamp(value,0.f,1.f)*255.f)/255.f;
    return channel<3&&srgb(format)?decode(value):value;
}
struct Similarity {
    uint64_t equal=0,reverseEqual=0;
    double maximum=0,sum=0;
    void add(const std::array<float,4>& source,const std::array<float,4>& target,
             DXGI_FORMAT sourceFormat,DXGI_FORMAT targetFormat) {
        bool forward=true,reverse=true;
        for(unsigned c=0;c<3;++c) {
            if(!std::isfinite(source[c]) || !std::isfinite(target[c])){forward=reverse=false;continue;}
            const double delta=std::abs(static_cast<double>(source[c])-target[c]);
            maximum=std::max(maximum,delta);sum+=delta;
            forward=forward && target[c]==quantized(source[c],targetFormat,c);
            reverse=reverse && source[c]==quantized(target[c],sourceFormat,c);
        }
        equal+=forward;reverseEqual+=reverse;
    }
    std::string describe(const char* name,uint64_t pixels)const {
        char text[240];std::snprintf(text,sizeof(text)," %sEqual=%llu %sReverseEqual=%llu %sMax=%.9g %sMean=%.9g",
            name,static_cast<unsigned long long>(equal),name,static_cast<unsigned long long>(reverseEqual),
            name,maximum,name,sum/(pixels*3));return text;
    }
};
DXGI_FORMAT storageFormat(DXGI_FORMAT format){return srgb(format)?DXGI_FORMAT_R8G8B8A8_UNORM:format;}
}
struct CaptureTrace::Impl {
    std::shared_ptr<Dx11Dx12> graphics;
    std::function<void(const char*)> log;
    std::shared_ptr<Budget> budget=std::make_shared<Budget>();
    std::shared_ptr<Snapshot> previous;
    std::deque<Pair> pending;
    unsigned submitted=0;
    bool reported=false;
    Impl(std::shared_ptr<Dx11Dx12> g,std::function<void(const char*)> l):graphics(std::move(g)),log(std::move(l)){}
    void unavailable(const char* text){if(!reported&&log){reported=true;const auto message=std::string("capture.color-handoff unavailable: ")+text;log(message.c_str());}}
    std::shared_ptr<Snapshot> snapshot(uint64_t frame,uint64_t generation,uint64_t pass,ID3D11RenderTargetView* view){
        if(!view)return {};
        D3D11_RENDER_TARGET_VIEW_DESC v{};view->GetDesc(&v);
        if(v.ViewDimension!=D3D11_RTV_DIMENSION_TEXTURE2D||v.Texture2D.MipSlice||!supported(v.Format))return {};
        ComPtr<ID3D11Resource> resource;view->GetResource(&resource);ComPtr<ID3D11Texture2D> source;
        if(FAILED(resource.As(&source)))return {};
        D3D11_TEXTURE2D_DESC d{};source->GetDesc(&d);
        if(d.MipLevels!=1||d.ArraySize!=1||d.SampleDesc.Count!=1||!d.Width||!d.Height)return {};
        const uint64_t bytes=((static_cast<uint64_t>(d.Width)*(half(v.Format)?8:4)+1023)&~uint64_t(1023))*
            ((static_cast<uint64_t>(d.Height)+255)&~uint64_t(255));
        constexpr uint64_t maximum=128ull*1024*1024;
        if(bytes>maximum||budget->bytes>maximum-bytes){unavailable("bounded private staging budget reached");return {};}
        auto result=std::make_shared<Snapshot>();result->frame=frame;result->generation=generation;result->pass=pass;
        result->width=d.Width;result->height=d.Height;result->view=v.Format;
        d.Usage=D3D11_USAGE_STAGING;d.BindFlags=0;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;d.MiscFlags=0;
        graphicsCheck(graphics->device11()->CreateTexture2D(&d,nullptr,&result->texture),"Allocate private camera diagnostic");
        result->budget=budget;result->bytes=bytes;budget->bytes+=bytes;
        graphics->context11()->CopyResource(result->texture.Get(),source.Get());return result;
    }
    void compare(const Pair& pair){
        const auto& a=*pair.before;const auto& b=*pair.after;
        Mapping first(graphics->context11(),a.texture.Get()),second(graphics->context11(),b.texture.Get());
        uint64_t equalRgb=0,equalRgba=0,nonfinite=0;
        double maximumRgb=0,maximumAlpha=0,sum=0;
        std::array<float,3> sourceMinimum,sourceMaximum,targetMinimum,targetMaximum;
        sourceMinimum.fill(std::numeric_limits<float>::infinity());targetMinimum=sourceMinimum;
        sourceMaximum.fill(-std::numeric_limits<float>::infinity());targetMaximum=sourceMaximum;
        Similarity linearReference,flippedLinear,storageReference,flippedStorage;
        for(unsigned y=0;y<a.height;++y)for(unsigned x=0;x<a.width;++x){
            const auto source=pixel(a,first,x,y),actual=pixel(b,second,x,y);
            // Diagnostic candidates, not automatic production conversions. A
            // flipped render texture and a differently interpreted encoding can
            // otherwise look like an entirely unrelated camera color image.
            linearReference.add(source,actual,a.view,b.view);
            flippedLinear.add(pixel(a,first,x,a.height-1-y),actual,a.view,b.view);
            const auto rawTarget=pixel(b,second,x,y,false);
            storageReference.add(pixel(a,first,x,y,false),rawTarget,storageFormat(a.view),storageFormat(b.view));
            flippedStorage.add(pixel(a,first,x,a.height-1-y,false),rawTarget,storageFormat(a.view),storageFormat(b.view));
            bool sameRgb=true,sameAlpha=true;
            for(unsigned c=0;c<4;++c){
                if(!std::isfinite(source[c])||!std::isfinite(actual[c])){++nonfinite;if(c<3)sameRgb=false;else sameAlpha=false;continue;}
                const double delta=std::abs(static_cast<double>(actual[c])-source[c]);
                const bool same=actual[c]==quantized(source[c],b.view,c);
                if(c<3){
                    maximumRgb=std::max(maximumRgb,delta);sum+=delta;sameRgb=sameRgb&&same;
                    sourceMinimum[c]=std::min(sourceMinimum[c],source[c]);sourceMaximum[c]=std::max(sourceMaximum[c],source[c]);
                    targetMinimum[c]=std::min(targetMinimum[c],actual[c]);targetMaximum[c]=std::max(targetMaximum[c],actual[c]);
                }
                else {maximumAlpha=std::max(maximumAlpha,delta);sameAlpha=same;}
            }
            equalRgb+=sameRgb;equalRgba+=sameRgb&&sameAlpha;
        }
        const auto pixels=static_cast<uint64_t>(a.width)*a.height;
        char text[1024];std::snprintf(text,sizeof(text),
            "capture.color-handoff frame=%llu generation=%llu from=%llu to=%llu %ux%u views=%u/%u pixels=%llu rgbQuantizedEqual=%llu rgbaQuantizedEqual=%llu maxLinearRgb=%.9g meanLinearRgb=%.9g maxAlpha=%.9g nonfiniteComponents=%llu sourceMin=(%.9g,%.9g,%.9g) sourceMax=(%.9g,%.9g,%.9g) targetMin=(%.9g,%.9g,%.9g) targetMax=(%.9g,%.9g,%.9g); CPU standard-sRGB/FP16 reference, no equivalence assertion",
            static_cast<unsigned long long>(a.frame),static_cast<unsigned long long>(a.generation),
            static_cast<unsigned long long>(a.pass),static_cast<unsigned long long>(b.pass),a.width,a.height,a.view,b.view,
            static_cast<unsigned long long>(pixels),static_cast<unsigned long long>(equalRgb),static_cast<unsigned long long>(equalRgba),
            maximumRgb,sum/(pixels*3),maximumAlpha,static_cast<unsigned long long>(nonfinite),
            sourceMinimum[0],sourceMinimum[1],sourceMinimum[2],sourceMaximum[0],sourceMaximum[1],sourceMaximum[2],
            targetMinimum[0],targetMinimum[1],targetMinimum[2],targetMaximum[0],targetMaximum[1],targetMaximum[2]);
        if(log) {
            const auto message=std::string(text)+linearReference.describe("linear",pixels)+
                flippedLinear.describe("flipLinear",pixels)+storageReference.describe("storage",pixels)+
                flippedStorage.describe("flipStorage",pixels);
            log(message.c_str());
        }
    }
};
CaptureTrace::CaptureTrace(std::shared_ptr<Dx11Dx12> graphics,std::function<void(const char*)> log):impl_(std::make_unique<Impl>(std::move(graphics),std::move(log))){}
CaptureTrace::~CaptureTrace()=default;
void CaptureTrace::observe(uint64_t frame,uint64_t generation,uint64_t pass,ID3D11RenderTargetView* view) noexcept {
    try{
        auto& s=*impl_;if(s.submitted>=8)return;
        auto lock=s.graphics->lock();capture::Bypass bypass;
        if(pass==103||pass==203){s.previous=s.snapshot(frame,generation,pass,view);return;}
        if((pass!=200&&pass!=201&&pass!=300&&pass!=301)||!s.previous||s.pending.size()>=2||s.previous->frame!=frame||s.previous->generation!=generation)return;
        if((pass/100==2&&s.previous->pass!=103)||(pass/100==3&&s.previous->pass!=203))return;
        auto after=s.snapshot(frame,generation,pass,view);if(!after)return;
        if(after->width!=s.previous->width||after->height!=s.previous->height){s.unavailable("camera boundaries have different dimensions");return;}
        Pair pair;pair.before=s.previous;pair.after=std::move(after);
        D3D11_QUERY_DESC query{D3D11_QUERY_EVENT,0};graphicsCheck(s.graphics->device11()->CreateQuery(&query,&pair.ready),"Create camera diagnostic completion");
        s.graphics->context11()->End(pair.ready.Get());s.pending.push_back(std::move(pair));++s.submitted;
    }catch(const std::exception& error){impl_->unavailable(error.what());}catch(...){impl_->unavailable("unknown diagnostic error");}
}
void CaptureTrace::collect() noexcept {
    try{
        auto& s=*impl_;if(s.pending.empty())return;
        auto lock=s.graphics->lock();capture::Bypass bypass;
        while(!s.pending.empty()){
            BOOL ready=FALSE;const auto result=s.graphics->context11()->GetData(s.pending.front().ready.Get(),&ready,sizeof(ready),D3D11_ASYNC_GETDATA_DONOTFLUSH);
            if(result==S_FALSE||(result==S_OK&&!ready))return;
            graphicsCheck(result,"Query camera diagnostic completion");s.compare(s.pending.front());s.pending.pop_front();
        }
    }catch(const std::exception& error){impl_->unavailable(error.what());impl_->pending.clear();}catch(...){impl_->unavailable("unknown diagnostic collection error");impl_->pending.clear();}
}
} // namespace dspaa
