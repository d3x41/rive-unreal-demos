#include "pls_render_context_rhi_impl.hpp"

#include "CommonRenderResources.h"
#include "IImageWrapperModule.h"
#include "IImageWrapper.h"
#include "RenderGraphBuilder.h"
#include "RHIResourceUpdates.h"
#include "Containers/ResourceArray.h"
#include "RHIStaticStates.h"
#include "Modules/ModuleManager.h"

#include "RHICommandList.h"

#include "Shaders/ShaderPipelineManager.h"

THIRD_PARTY_INCLUDES_START
#include "rive/renderer/rive_render_image.hpp"
#include "rive/shaders/out/generated/shaders/constants.glsl.hpp"

#include "webp/decode.h"
#include "webp/demux.h"

THIRD_PARTY_INCLUDES_END
#include "RenderGraphUtils.h"
#include "Logs/RiveRendererLog.h"

template<typename VShaderType, typename PShaderType>
void BindShaders(FRHICommandList& CommandList, FGraphicsPipelineStateInitializer& GraphicsPSOInit,
    TShaderMapRef<VShaderType> VSShader, TShaderMapRef<PShaderType> PSShader, FRHIVertexDeclaration* VertexDeclaration)
{
    GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI = VertexDeclaration;
    GraphicsPSOInit.BoundShaderState.VertexShaderRHI = VSShader.GetVertexShader();
    GraphicsPSOInit.BoundShaderState.PixelShaderRHI = PSShader.GetPixelShader();
    SetGraphicsPipelineState(CommandList, GraphicsPSOInit, 0, EApplyRendertargetOption::CheckApply, true, EPSOPrecacheResult::NotSupported);
}

template<typename ShaderType>
void SetParameters(FRHICommandList& CommandList, FRHIBatchedShaderParameters& BatchedParameters,
    TShaderMapRef<ShaderType> Shader, typename ShaderType::FParameters& VParameters)
{
    ClearUnusedGraphResources(Shader, &VParameters);
    SetShaderParameters(BatchedParameters, Shader, VParameters);
    CommandList.SetBatchedShaderParameters(Shader.GetVertexShader(), BatchedParameters);
}

template<typename DataType, size_t size>
struct TStaticResourceData : public FResourceArrayInterface
{
    DataType Data[size];
public:
    TStaticResourceData() {}

    DataType* operator *()
    {return Data;}
    /**
     * @return A pointer to the resource data.
     */
    virtual const void* GetResourceData() const
    {return Data;}

    /**
     * @return size of resource data allocation (in bytes)
     */
    virtual uint32 GetResourceDataSize() const
    {return size*sizeof(DataType);};

    /** Do nothing on discard because this is static const CPU data */
    virtual void Discard() {};

    virtual bool IsStatic() const {return true;}

    /**
     * @return true if the resource keeps a copy of its resource data after the RHI resource has been created
     */
    virtual bool GetAllowCPUAccess() const
    {return true;}

    /** 
     * Sets whether the resource array will be accessed by CPU. 
     */
    virtual void SetAllowCPUAccess( bool bInNeedsCPUAccess ){}
};

template<typename DataType, size_t size>
struct TStaticExternalResourceData : public FResourceArrayInterface
{
    const DataType (&Data)[size];
public:
    TStaticExternalResourceData(const DataType (&Data)[size]) : Data(Data)
    {}
    /**
     * @return A pointer to the resource data.
     */
    virtual const void* GetResourceData() const
    {return Data;};

    /**
     * @return size of resource data allocation (in bytes)
     */
    virtual uint32 GetResourceDataSize() const
    {return size*sizeof(DataType);};

    /** Do nothing on discard because this is static const CPU data */
    virtual void Discard() {};

    virtual bool IsStatic() const {return true;}

    /**
     * @return true if the resource keeps a copy of its resource data after the RHI resource has been created
     */
    virtual bool GetAllowCPUAccess() const
    {return true;}

    /** 
     * Sets whether the resource array will be accessed by CPU. 
     */
    virtual void SetAllowCPUAccess( bool bInNeedsCPUAccess ){}
};

using namespace rive;
using namespace rive::gpu;

    TStaticExternalResourceData GImageRectIndices(kImageRectIndices);
    TStaticExternalResourceData GImageRectVertices(kImageRectVertices);
    TStaticExternalResourceData GTessSpanIndices(kTessSpanIndices);
    
    TStaticResourceData<PatchVertex, kPatchVertexBufferCount> GPatchVertices;
    TStaticResourceData<uint16_t, kPatchIndexBufferCount> GPatchIndices;

    void GetPermutationForFeatures(const rive::gpu::ShaderFeatures features,AtomicPixelPermutationDomain& PixelPermutationDomain, AtomicVertexPermutationDomain& VertexPermutationDomain)
    {
        VertexPermutationDomain.Set<FEnableClip>(features & ShaderFeatures::ENABLE_CLIPPING);
        VertexPermutationDomain.Set<FEnableClipRect>(features & ShaderFeatures::ENABLE_CLIP_RECT);
        VertexPermutationDomain.Set<FEnableAdvanceBlend>(features & ShaderFeatures::ENABLE_ADVANCED_BLEND);

        PixelPermutationDomain.Set<FEnableClip>(features & ShaderFeatures::ENABLE_CLIPPING);
        PixelPermutationDomain.Set<FEnableClipRect>(features & ShaderFeatures::ENABLE_CLIP_RECT);
        PixelPermutationDomain.Set<FEnableNestedClip>(features & ShaderFeatures::ENABLE_NESTED_CLIPPING);
        PixelPermutationDomain.Set<FEnableAdvanceBlend>(features & ShaderFeatures::ENABLE_ADVANCED_BLEND);
        PixelPermutationDomain.Set<FEnableFixedFunctionColorBlend>(!(features & ShaderFeatures::ENABLE_ADVANCED_BLEND));
        PixelPermutationDomain.Set<FEnableEvenOdd>(features & ShaderFeatures::ENABLE_EVEN_ODD);
        PixelPermutationDomain.Set<FEnableHSLBlendMode>(features & ShaderFeatures::ENABLE_HSL_BLEND_MODES);
    }
    
    template<typename DataType>
    FBufferRHIRef makeSimpleImmutableBuffer(FRHICommandList& RHICmdList, const TCHAR* DebugName, EBufferUsageFlags bindFlags, FResourceArrayInterface &ResourceArray)
    {
        const size_t size = ResourceArray.GetResourceDataSize();
        FRHIResourceCreateInfo Info(DebugName, &ResourceArray);
        auto buffer = RHICmdList.CreateBuffer(size,
            EBufferUsageFlags::Static | bindFlags,sizeof(DataType),
            ERHIAccess::VertexOrIndexBuffer, Info);
        return buffer;
    }
    
#define SYNC_BUFFER(buffer, command_list) if(buffer)buffer->Sync(command_list);
#define SYNC_BUFFER_WITH_OFFSET(buffer, command_list, offset)if(buffer)buffer->Sync(command_list, offset);
    
BufferRingRHIImpl::BufferRingRHIImpl(EBufferUsageFlags flags,
size_t in_sizeInBytes, size_t stride) : BufferRing(in_sizeInBytes), m_flags(flags)
{
    FRHIAsyncCommandList tmpCommandList;
    FRHIResourceCreateInfo Info(TEXT("BufferRingRHIImpl_"));
    m_buffer = tmpCommandList->CreateBuffer(in_sizeInBytes,
        /*EBufferUsageFlags::Volatile |*/ flags, stride, ERHIAccess::WriteOnlyMask, Info);
}

void BufferRingRHIImpl::Sync(FRHICommandList& commandList) const
{
    auto buffer = commandList.LockBuffer(m_buffer, 0, capacityInBytes(), RLM_WriteOnly_NoOverwrite);
    memcpy(buffer, shadowBuffer(), capacityInBytes());
    commandList.UnlockBuffer(m_buffer);
}

FBufferRHIRef BufferRingRHIImpl::contents()const
{
    return m_buffer;
}

void* BufferRingRHIImpl::onMapBuffer(int bufferIdx, size_t mapSizeInBytes)
{
    return shadowBuffer();
}

void BufferRingRHIImpl::onUnmapAndSubmitBuffer(int bufferIdx, size_t mapSizeInBytes)
{
}

StructuredBufferRingRHIImpl::StructuredBufferRingRHIImpl(EBufferUsageFlags flags,
    size_t in_sizeInBytes,
    size_t elementSize) : BufferRing(in_sizeInBytes),  m_flags(flags),
    m_elementSize(elementSize), m_lastMapSizeInBytes(in_sizeInBytes)
{
    FRHIAsyncCommandList commandList;
    FRHIResourceCreateInfo Info(TEXT("BufferRingRHIImpl_"));
    m_buffer = commandList->CreateStructuredBuffer(m_elementSize, capacityInBytes(),
         m_flags, ERHIAccess::WriteOnlyMask, Info);
    m_srv = commandList->CreateShaderResourceView(m_buffer);
}

FBufferRHIRef StructuredBufferRingRHIImpl::contents()const
{
    return m_buffer;
}

void* StructuredBufferRingRHIImpl::onMapBuffer(int bufferIdx, size_t mapSizeInBytes)
{
    m_lastMapSizeInBytes = mapSizeInBytes;
    return shadowBuffer();
}

void StructuredBufferRingRHIImpl::onUnmapAndSubmitBuffer(int bufferIdx, size_t mapSizeInBytes)
{
}

FShaderResourceViewRHIRef StructuredBufferRingRHIImpl::srv() const
{
    return m_srv;
}


RenderBufferRHIImpl::RenderBufferRHIImpl(RenderBufferType in_type,
                                         RenderBufferFlags in_flags, size_t in_sizeInBytes, size_t stride) :
    lite_rtti_override(in_type, in_flags, in_sizeInBytes),
    m_buffer(in_type == RenderBufferType::vertex ? EBufferUsageFlags::VertexBuffer : EBufferUsageFlags::IndexBuffer, in_sizeInBytes, stride),
    m_mappedBuffer(nullptr)
{
    if(in_flags & RenderBufferFlags::mappedOnceAtInitialization)
    {
        m_mappedBuffer = m_buffer.mapBuffer(in_sizeInBytes);
    }
}

void RenderBufferRHIImpl::Sync(FRHICommandList& commandList) const
{
    m_buffer.Sync(commandList);
}

FBufferRHIRef RenderBufferRHIImpl::contents()const
{
    return m_buffer.contents();
}

void* RenderBufferRHIImpl::onMap()
{
    if(flags() & RenderBufferFlags::mappedOnceAtInitialization)
    {
        check(m_mappedBuffer);
        return m_mappedBuffer;
    }
    return m_buffer.mapBuffer(sizeInBytes());
}

void RenderBufferRHIImpl::onUnmap()
{
    if(flags() & RenderBufferFlags::mappedOnceAtInitialization)
        return;
    
    m_buffer.unmapAndSubmitBuffer();
}

class PLSTextureRHIImpl : public Texture
{
public:
    PLSTextureRHIImpl(uint32_t width, uint32_t height, uint32_t mipLevelCount, const TArray<uint8>& imageDataRGBA, EPixelFormat PixelFormat = PF_B8G8R8A8) : 
        Texture(width, height)
    {
        FRHIAsyncCommandList commandList;
        auto Desc = FRHITextureCreateDesc::Create2D(TEXT("PLSTextureRHIImpl_"), m_width, m_height, PixelFormat);
        Desc.SetNumMips(mipLevelCount);
        m_texture = commandList->CreateTexture(Desc);
        commandList->UpdateTexture2D(m_texture, 0,
            FUpdateTextureRegion2D(0, 0, 0, 0, m_width, m_height), m_width * 4, imageDataRGBA.GetData());
        //commandList->Transition(FRHITransitionInfo(m_texture, ERHIAccess::Unknown, ERHIAccess::SRVGraphics));

    }
    virtual ~PLSTextureRHIImpl()override
    {
    }

    FTextureRHIRef contents()const
    {
        return m_texture;
    }

private:
    FTextureRHIRef  m_texture;
};

RenderTargetRHI::RenderTargetRHI(FRHICommandList& RHICmdList, const FTexture2DRHIRef& InTextureTarget) :
RenderTarget(InTextureTarget->GetSizeX(), InTextureTarget->GetSizeY()), m_textureTarget(InTextureTarget)
{
    FRHITextureCreateDesc coverageDesc = FRHITextureCreateDesc::Create2D(TEXT("RiveAtomicCoverage"), width(), height(), PF_R32_UINT);
    coverageDesc.SetNumMips(1);
    coverageDesc.AddFlags(ETextureCreateFlags::UAV | ETextureCreateFlags::Memoryless);
    m_atomicCoverageTexture = RHICmdList.CreateTexture(coverageDesc);

    FRHITextureCreateDesc scratchColorDesc = FRHITextureCreateDesc::Create2D(TEXT("RiveScratchColor"), width(), height(), PF_R8G8B8A8);
    scratchColorDesc.SetNumMips(1);
    scratchColorDesc.AddFlags(ETextureCreateFlags::UAV);
    m_scratchColorTexture = RHICmdList.CreateTexture(scratchColorDesc);

    FRHITextureCreateDesc clipDesc = FRHITextureCreateDesc::Create2D(TEXT("RiveClip"), width(), height(), PF_R32_UINT);
    clipDesc.SetNumMips(1);
    clipDesc.AddFlags(ETextureCreateFlags::UAV);
    m_clipTexture = RHICmdList.CreateTexture(clipDesc);
    
    RHICmdList.Transition(FRHITransitionInfo(m_coverageUAV, ERHIAccess::Unknown, ERHIAccess::UAVGraphics));
    RHICmdList.Transition(FRHITransitionInfo(m_scratchColorTexture, ERHIAccess::Unknown, ERHIAccess::UAVGraphics));
    RHICmdList.Transition(FRHITransitionInfo(m_clipTexture, ERHIAccess::Unknown, ERHIAccess::UAVGraphics));
    RHICmdList.Transition(FRHITransitionInfo(m_textureTarget, ERHIAccess::Unknown, ERHIAccess::UAVGraphics));

    m_coverageUAV = RHICmdList.CreateUnorderedAccessView(m_atomicCoverageTexture);
    m_clipUAV = RHICmdList.CreateUnorderedAccessView(m_clipTexture);
    m_scratchColorUAV = RHICmdList.CreateUnorderedAccessView(m_scratchColorTexture);
    m_targetUAV = RHICmdList.CreateUnorderedAccessView(m_textureTarget);
}

std::unique_ptr<RenderContext> RenderContextRHIImpl::MakeContext(FRHICommandListImmediate& CommandListImmediate)
{
    auto plsContextImpl = std::make_unique<RenderContextRHIImpl>(CommandListImmediate);
    return std::make_unique<RenderContext>(std::move(plsContextImpl));
}

RenderContextRHIImpl::RenderContextRHIImpl(FRHICommandListImmediate& CommandListImmediate)
{
    m_platformFeatures.supportsFragmentShaderAtomics = true;
    m_platformFeatures.supportsClipPlanes = true;
    m_platformFeatures.supportsRasterOrdering = false;
    m_platformFeatures.invertOffscreenY = true;
    
    auto ShaderMap =  GetGlobalShaderMap(GMaxRHIFeatureLevel);

    VertexDeclarations[static_cast<int32>(EVertexDeclarations::Resolve)] = GEmptyVertexDeclaration.VertexDeclarationRHI;
    
    FVertexDeclarationElementList pathElementList;
    pathElementList.Add(FVertexElement(FVertexElement(0, 0, VET_Float4, 0, sizeof(PathData), false)));
    pathElementList.Add(FVertexElement(FVertexElement(0, sizeof(float4), VET_Float4, 1, sizeof(PathData), false)));
    auto PathVertexDeclaration = PipelineStateCache::GetOrCreateVertexDeclaration(pathElementList);
    VertexDeclarations[static_cast<int32>(EVertexDeclarations::Paths)] = PathVertexDeclaration;
    
    FVertexDeclarationElementList trianglesElementList;
    trianglesElementList.Add(FVertexElement(0, 0, VET_Float3, 0, sizeof(TriangleVertex), false));
    auto TrianglesVertexDeclaration = PipelineStateCache::GetOrCreateVertexDeclaration(trianglesElementList);
    VertexDeclarations[static_cast<int32>(EVertexDeclarations::InteriorTriangles)] = TrianglesVertexDeclaration;
    
    FVertexDeclarationElementList ImageMeshElementList;
    ImageMeshElementList.Add(FVertexElement(0, 0, VET_Float2, 0, sizeof(Vec2D), false));
    ImageMeshElementList.Add(FVertexElement(1, 0, VET_Float2, 1, sizeof(Vec2D), false));
    auto ImageMeshVertexDeclaration = PipelineStateCache::GetOrCreateVertexDeclaration(ImageMeshElementList);
    VertexDeclarations[static_cast<int32>(EVertexDeclarations::ImageMesh)] = ImageMeshVertexDeclaration;
    
    FVertexDeclarationElementList SpanElementList;
    SpanElementList.Add(FVertexElement(0, 0, VET_UInt, 0, sizeof(GradientSpan), true));
    SpanElementList.Add(FVertexElement(0, 4, VET_UInt, 1, sizeof(GradientSpan), true));
    SpanElementList.Add(FVertexElement(0, 8, VET_UInt, 2, sizeof(GradientSpan), true));
    SpanElementList.Add(FVertexElement(0, 12, VET_UInt, 3, sizeof(GradientSpan), true));
    auto SpanVertexDeclaration = PipelineStateCache::GetOrCreateVertexDeclaration(SpanElementList);
    VertexDeclarations[static_cast<int32>(EVertexDeclarations::Gradient)] = SpanVertexDeclaration;
    
    FVertexDeclarationElementList TessElementList;
    size_t tessOffset = 0;
    size_t tessStride = sizeof(TessVertexSpan);
    TessElementList.Add(FVertexElement(0, tessOffset, VET_Float4, 0, tessStride, true));
    tessOffset += 4*sizeof(float);
    TessElementList.Add(FVertexElement(0, tessOffset, VET_Float4, 1, tessStride, true));
    tessOffset += 4*sizeof(float);
    TessElementList.Add(FVertexElement(0, tessOffset, VET_Float4, 2, tessStride, true));
    tessOffset += 4*sizeof(float);
    TessElementList.Add(FVertexElement(0, tessOffset, VET_UInt,3, tessStride, true));
    tessOffset += 4;
    TessElementList.Add(FVertexElement(0, tessOffset, VET_UInt,4, tessStride, true));
    tessOffset += 4;
    TessElementList.Add(FVertexElement(0, tessOffset, VET_UInt,5, tessStride, true));
    tessOffset += 4;
    TessElementList.Add(FVertexElement(0, tessOffset, VET_UInt,6, tessStride, true));
    check(tessOffset+4 == sizeof(TessVertexSpan));
    
    auto TessVertexDeclaration = PipelineStateCache::GetOrCreateVertexDeclaration(TessElementList);
    VertexDeclarations[static_cast<int32>(EVertexDeclarations::Tessellation)] = TessVertexDeclaration;
    
    FVertexDeclarationElementList ImageRectVertexElementList;
    ImageRectVertexElementList.Add(
        FVertexElement(0, 0, VET_Float4, 0, sizeof(ImageRectVertex), false));
    auto ImageRectDecleration = PipelineStateCache::GetOrCreateVertexDeclaration(ImageRectVertexElementList);
    VertexDeclarations[static_cast<int32>(EVertexDeclarations::ImageRect)] = ImageRectDecleration;
    
    GeneratePatchBufferData(*GPatchVertices, *GPatchIndices);
    
    m_patchVertexBuffer = makeSimpleImmutableBuffer<PatchVertex>(CommandListImmediate,
            TEXT("RivePatchVertexBuffer"),
            EBufferUsageFlags::VertexBuffer, GPatchVertices);
    m_patchIndexBuffer = makeSimpleImmutableBuffer<uint16_t>(CommandListImmediate,
            TEXT("RivePatchIndexBuffer"),
            EBufferUsageFlags::IndexBuffer, GPatchIndices);
    
    m_tessSpanIndexBuffer = makeSimpleImmutableBuffer<uint16_t>(CommandListImmediate,
        TEXT("RiveTessIndexBuffer"),
        EBufferUsageFlags::IndexBuffer,
        GTessSpanIndices);
    
    m_imageRectVertexBuffer = makeSimpleImmutableBuffer<ImageRectVertex>(CommandListImmediate,
        TEXT("ImageRectVertexBuffer"),
        EBufferUsageFlags::VertexBuffer,
        GImageRectVertices);
    
    m_imageRectIndexBuffer = makeSimpleImmutableBuffer<uint16>(CommandListImmediate,
        TEXT("ImageRectIndexBuffer"),
        EBufferUsageFlags::IndexBuffer,
        GImageRectIndices);
    
    m_mipmapSampler = TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp, AM_Clamp, 0, 1, 0, SCF_Never>::GetRHI();
    m_linearSampler = TStaticSamplerState<SF_AnisotropicLinear, AM_Clamp, AM_Clamp, AM_Clamp, 0, 1, 0, SCF_Never>::GetRHI();
}

rcp<RenderTargetRHI> RenderContextRHIImpl::makeRenderTarget(FRHICommandListImmediate& RHICmdList,const FTexture2DRHIRef& InTargetTexture)
{
    return make_rcp<RenderTargetRHI>(RHICmdList, InTargetTexture);
}

rcp<Texture> RenderContextRHIImpl::decodeImageTexture(Span<const uint8_t> encodedBytes)
{

    constexpr uint8_t PNG[4] =  {0x89, 0x50, 0x4E, 0x47};
    constexpr uint8_t JPEG[3] =  {0xFF, 0xD8, 0xFF};
    constexpr uint8_t WEBP[3] = {0x52, 0x49, 0x46};

    EImageFormat format = EImageFormat::Invalid;
    
    if(memcmp(PNG, encodedBytes.data(), sizeof(PNG)) == 0)
    {
        format = EImageFormat::PNG;
    }
    else if (memcmp(JPEG, encodedBytes.data(), sizeof(JPEG)) == 0)
    {
        format = EImageFormat::JPEG;
    }
    else if(memcmp(WEBP, encodedBytes.data(), sizeof(WEBP)) == 0)
    {
        format = EImageFormat::Invalid;
    }
    else
    {
        RIVE_DEBUG_VERBOSE("Invalid Decode Image header");
        return nullptr;
    }

    if(format != EImageFormat::Invalid)
    {
        // Use Unreal for PNG and JPEG
        IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(FName("ImageWrapper"));
        TSharedPtr<IImageWrapper> ImageWrapper = ImageWrapperModule.CreateImageWrapper(format);
        if(!ImageWrapper.IsValid() || !ImageWrapper->SetCompressed(encodedBytes.data(), encodedBytes.size()))
        {
            return nullptr;
        }

        TArray<uint8> UncompressedBGRA;
        if (!ImageWrapper->GetRaw(ERGBFormat::BGRA, 8, UncompressedBGRA))
        {
            return nullptr;
        }
    
        return make_rcp<PLSTextureRHIImpl>(ImageWrapper->GetWidth(), ImageWrapper->GetHeight(), 1, UncompressedBGRA);
    }
    else
    {
        // WEBP Decoding
        WebPDecoderConfig config;
        if (!WebPInitDecoderConfig(&config))
        {
            fprintf(stderr, "DecodeWebP - Library version mismatch!\n");
            return nullptr;
        }
        config.options.dithering_strength = 50;
        config.options.alpha_dithering_strength = 100;

        if (!WebPGetInfo(encodedBytes.data(), encodedBytes.size(), nullptr, nullptr))
        {
            fprintf(stderr, "DecodeWebP - Input file doesn't appear to be WebP format.\n");
        }

        WebPData data = {encodedBytes.data(), encodedBytes.size()};
        WebPDemuxer* demuxer = WebPDemux(&data);
        if (demuxer == nullptr)
        {
            RIVE_DEBUG_VERBOSE("DecodeWebP - Could not create demuxer.");
            return nullptr;
        }

        WebPIterator currentFrame;
        if (!WebPDemuxGetFrame(demuxer, 1, &currentFrame))
        {
            RIVE_DEBUG_VERBOSE("DecodeWebP - WebPDemuxGetFrame couldn't get frame.");
            WebPDemuxDelete(demuxer);
            return nullptr;
        }
        config.output.colorspace = MODE_RGBA;

        uint32_t width = WebPDemuxGetI(demuxer, WEBP_FF_CANVAS_WIDTH);
        uint32_t height = WebPDemuxGetI(demuxer, WEBP_FF_CANVAS_HEIGHT);

        size_t pixelBufferSize =
            static_cast<size_t>(width) * static_cast<size_t>(height) * static_cast<size_t>(4);
        TArray<uint8> pixelBuffer;
        pixelBuffer.AddUninitialized(pixelBufferSize);

        config.output.u.RGBA.rgba = (uint8_t*)pixelBuffer.GetData();
        config.output.u.RGBA.stride = static_cast<int>(width * 4);
        config.output.u.RGBA.size = pixelBufferSize;
        config.output.is_external_memory = 1;

        if (WebPDecode(currentFrame.fragment.bytes, currentFrame.fragment.size, &config) !=
            VP8_STATUS_OK)
        {
            RIVE_DEBUG_VERBOSE("DecodeWebP - WebPDemuxGetFrame couldn't decode.");
            WebPDemuxReleaseIterator(&currentFrame);
            WebPDemuxDelete(demuxer);
            return nullptr;
        }

        WebPDemuxReleaseIterator(&currentFrame);
        WebPDemuxDelete(demuxer);

        return make_rcp<PLSTextureRHIImpl>(width, height, 1, std::move(pixelBuffer), EPixelFormat::PF_R8G8B8A8);
    }
}

void RenderContextRHIImpl::resizeFlushUniformBuffer(size_t sizeInBytes)
{
    m_flushUniformBuffer.reset();
    if(sizeInBytes != 0)
    {
        m_flushUniformBuffer = std::make_unique<UniformBufferRHIImpl<FFlushUniforms>>(sizeInBytes);
    }
}

void RenderContextRHIImpl::resizeImageDrawUniformBuffer(size_t sizeInBytes)
{
    m_imageDrawUniformBuffer.reset();
    if(sizeInBytes != 0)
    {
        m_imageDrawUniformBuffer = std::make_unique<UniformBufferRHIImpl<FImageDrawUniforms>>(sizeInBytes);
    }
}

void RenderContextRHIImpl::resizePathBuffer(size_t sizeInBytes, StorageBufferStructure structure)
{
    m_pathBuffer.reset();
    if(sizeInBytes != 0)
    {
        m_pathBuffer = std::make_unique<StructuredBufferRingRHIImpl>(EBufferUsageFlags::StructuredBuffer | EBufferUsageFlags::ShaderResource, sizeInBytes,
            StorageBufferElementSizeInBytes(structure));
    }
}

void RenderContextRHIImpl::resizePaintBuffer(size_t sizeInBytes, StorageBufferStructure structure)
{
    m_paintBuffer.reset();
    if(sizeInBytes != 0)
    {
        m_paintBuffer = std::make_unique<StructuredBufferRingRHIImpl>(EBufferUsageFlags::StructuredBuffer | EBufferUsageFlags::ShaderResource, sizeInBytes, StorageBufferElementSizeInBytes(structure));
    }
}

void RenderContextRHIImpl::resizePaintAuxBuffer(size_t sizeInBytes, StorageBufferStructure structure)
{
    m_paintAuxBuffer.reset();
    if(sizeInBytes != 0)
    {
        m_paintAuxBuffer = std::make_unique<StructuredBufferRingRHIImpl>(EBufferUsageFlags::StructuredBuffer | EBufferUsageFlags::ShaderResource, sizeInBytes, StorageBufferElementSizeInBytes(structure));
    }
}

void RenderContextRHIImpl::resizeContourBuffer(size_t sizeInBytes, StorageBufferStructure structure)
{
    m_contourBuffer.reset();
    if(sizeInBytes != 0)
    {
        m_contourBuffer = std::make_unique<StructuredBufferRingRHIImpl>(EBufferUsageFlags::StructuredBuffer | EBufferUsageFlags::ShaderResource, sizeInBytes, StorageBufferElementSizeInBytes(structure));
    }
}

void RenderContextRHIImpl::resizeSimpleColorRampsBuffer(size_t sizeInBytes)
{
    m_simpleColorRampsBuffer.reset();
    if(sizeInBytes != 0)
    {
        m_simpleColorRampsBuffer = std::make_unique<HeapBufferRing>(sizeInBytes);
    }
}

void RenderContextRHIImpl::resizeGradSpanBuffer(size_t sizeInBytes)
{
    m_gradSpanBuffer.reset();
    if(sizeInBytes != 0)
    {
        m_gradSpanBuffer = std::make_unique<BufferRingRHIImpl>(EBufferUsageFlags::VertexBuffer, sizeInBytes, sizeof(GradientSpan));
    }
}

void RenderContextRHIImpl::resizeTessVertexSpanBuffer(size_t sizeInBytes)
{
    m_tessSpanBuffer.reset();
    if(sizeInBytes != 0)
    {
        m_tessSpanBuffer = std::make_unique<BufferRingRHIImpl>(EBufferUsageFlags::VertexBuffer, sizeInBytes, sizeof(TessVertexSpan));
    }
}

void RenderContextRHIImpl::resizeTriangleVertexBuffer(size_t sizeInBytes)
{
    m_triangleBuffer.reset();
    if(sizeInBytes != 0)
    {
        m_triangleBuffer = std::make_unique<BufferRingRHIImpl>(EBufferUsageFlags::VertexBuffer, sizeInBytes, sizeof(TriangleVertex));
    }
}

void* RenderContextRHIImpl::mapFlushUniformBuffer(size_t mapSizeInBytes)
{
    return m_flushUniformBuffer->mapBuffer(mapSizeInBytes);
}

void* RenderContextRHIImpl::mapImageDrawUniformBuffer(size_t mapSizeInBytes)
{
    return m_imageDrawUniformBuffer->mapBuffer(mapSizeInBytes);
}

void* RenderContextRHIImpl::mapPathBuffer(size_t mapSizeInBytes)
{
    return m_pathBuffer->mapBuffer(mapSizeInBytes);
}

void* RenderContextRHIImpl::mapPaintBuffer(size_t mapSizeInBytes)
{
    return m_paintBuffer->mapBuffer(mapSizeInBytes);
}

void* RenderContextRHIImpl::mapPaintAuxBuffer(size_t mapSizeInBytes)
{
    return m_paintAuxBuffer->mapBuffer(mapSizeInBytes);
}

void* RenderContextRHIImpl::mapContourBuffer(size_t mapSizeInBytes)
{
    return m_contourBuffer->mapBuffer(mapSizeInBytes);
}

void* RenderContextRHIImpl::mapSimpleColorRampsBuffer(size_t mapSizeInBytes)
{
    return m_simpleColorRampsBuffer->mapBuffer(mapSizeInBytes);
}

void* RenderContextRHIImpl::mapGradSpanBuffer(size_t mapSizeInBytes)
{
    return m_gradSpanBuffer->mapBuffer(mapSizeInBytes);
}

void* RenderContextRHIImpl::mapTessVertexSpanBuffer(size_t mapSizeInBytes)
{
    return m_tessSpanBuffer->mapBuffer(mapSizeInBytes);
}

void* RenderContextRHIImpl::mapTriangleVertexBuffer(size_t mapSizeInBytes)
{
    return m_triangleBuffer->mapBuffer(mapSizeInBytes);
}

void RenderContextRHIImpl::unmapFlushUniformBuffer()
{
    m_flushUniformBuffer->unmapAndSubmitBuffer();
}

void RenderContextRHIImpl::unmapImageDrawUniformBuffer()
{
    m_imageDrawUniformBuffer->unmapAndSubmitBuffer();
}

void RenderContextRHIImpl::unmapPathBuffer()
{
    m_pathBuffer->unmapAndSubmitBuffer();
}

void RenderContextRHIImpl::unmapPaintBuffer()
{
    m_paintBuffer->unmapAndSubmitBuffer();
}

void RenderContextRHIImpl::unmapPaintAuxBuffer()
{
    m_paintAuxBuffer->unmapAndSubmitBuffer();
}

void RenderContextRHIImpl::unmapContourBuffer()
{
    m_contourBuffer->unmapAndSubmitBuffer();
}

void RenderContextRHIImpl::unmapSimpleColorRampsBuffer()
{
    m_simpleColorRampsBuffer->unmapAndSubmitBuffer();
}

void RenderContextRHIImpl::unmapGradSpanBuffer()
{
    m_gradSpanBuffer->unmapAndSubmitBuffer();
}

void RenderContextRHIImpl::unmapTessVertexSpanBuffer()
{
    m_tessSpanBuffer->unmapAndSubmitBuffer();
}

void RenderContextRHIImpl::unmapTriangleVertexBuffer()
{
    m_triangleBuffer->unmapAndSubmitBuffer();
}

rcp<RenderBuffer> RenderContextRHIImpl::makeRenderBuffer(RenderBufferType type,
                                                            RenderBufferFlags flags,
                                                            size_t sizeInBytes)
{
    if(sizeInBytes == 0)
        return nullptr;

    return make_rcp<RenderBufferRHIImpl>(type, flags, sizeInBytes, type == RenderBufferType::index ? sizeof(uint16_t) : 0);
}

void RenderContextRHIImpl::resizeGradientTexture(uint32_t width, uint32_t height)
{
    check(IsInRenderingThread());
    if(width == 0 && height == 0)
    {
        m_gradiantTexture = nullptr;
        return;
    }
    
    width = std::max(width, 1u);
    height = std::max(height, 1u);
    
    auto& commandList = GRHICommandList.GetImmediateCommandList();
    FRHITextureCreateDesc Desc = FRHITextureCreateDesc::Create2D(TEXT("riveGradientTexture"),
        {static_cast<int32_t>(width), static_cast<int32_t>(height)}, PF_R8G8B8A8);
    Desc.AddFlags(ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::ShaderResource);
    Desc.SetClearValue(FClearValueBinding(FLinearColor::Red));
    Desc.DetermineInititialState();
    m_gradiantTexture = commandList.CreateTexture(Desc);

    commandList.Transition(FRHITransitionInfo(m_gradiantTexture, ERHIAccess::Unknown, ERHIAccess::SRVGraphics));

}

void RenderContextRHIImpl::resizeTessellationTexture(uint32_t width, uint32_t height)
{
    check(IsInRenderingThread());
    if(width == 0 && height == 0)
    {
        m_tesselationTexture = nullptr;
        return;
    }
    
    width = std::max(width, 1u);
    height = std::max(height, 1u);
    
    auto& commandList = GRHICommandList.GetImmediateCommandList();
    FRHITextureCreateDesc Desc = FRHITextureCreateDesc::Create2D(TEXT("riveTessTexture"),
        {static_cast<int32_t>(width), static_cast<int32_t>(height)}, PF_R32G32B32A32_UINT);
    Desc.AddFlags(ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::ShaderResource );
    Desc.DetermineInititialState();
    m_tesselationTexture = commandList.CreateTexture(Desc);

    commandList.Transition(FRHITransitionInfo(m_tesselationTexture, ERHIAccess::Unknown, ERHIAccess::SRVGraphics));

    
    FRHITextureSRVCreateInfo Info(0, 1, 0, 1, EPixelFormat::PF_R32G32B32A32_UINT);
    m_tessSRV = commandList.CreateShaderResourceView(m_tesselationTexture, Info);
}


void RenderContextRHIImpl::flush(const FlushDescriptor& desc)
{
    check(IsInRenderingThread());

    auto renderTarget = static_cast<RenderTargetRHI*>(desc.renderTarget);
    FTextureRHIRef DestTexture = renderTarget->texture();
    
    FRHICommandList& CommandList = GRHICommandList.GetImmediateCommandList();
    auto ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);

    SYNC_BUFFER_WITH_OFFSET(m_flushUniformBuffer, CommandList, desc.flushUniformDataOffsetInBytes);
    if( desc.pathCount > 0)
    {
        check(m_pathBuffer);
        check(m_paintBuffer);
        check(m_paintAuxBuffer);
        
        m_pathBuffer->Sync<PathData>(CommandList, desc.firstPath, desc.pathCount);
        m_paintBuffer->Sync<PaintData>(CommandList, desc.firstPaint, desc.pathCount);
        m_paintAuxBuffer->Sync<PaintAuxData>(CommandList, desc.firstPaintAux, desc.pathCount);
    }
    
    if(desc.contourCount > 0)
    {
        check(m_contourBuffer);
        m_contourBuffer->Sync<ContourData>(CommandList,  desc.firstContour, desc.contourCount);
    }
    
    SYNC_BUFFER(m_gradSpanBuffer, CommandList);
    SYNC_BUFFER(m_tessSpanBuffer, CommandList);
    SYNC_BUFFER(m_triangleBuffer, CommandList);

    FGraphicsPipelineStateInitializer GraphicsPSOInit;
    GraphicsPSOInit.BlendState = TStaticBlendState<>::GetRHI();
    GraphicsPSOInit.RasterizerState = TStaticRasterizerState<FM_Solid, CM_None, ERasterizerDepthClipMode::DepthClamp, false>::GetRHI();
    GraphicsPSOInit.DepthStencilState = TStaticDepthStencilState<false, ECompareFunction::CF_Always>::GetRHI();
    FRHIBatchedShaderParameters& BatchedShaderParameters = CommandList.GetScratchShaderParameters();

    CommandList.ClearUAVUint(renderTarget->coverageUAV(), FUintVector4(desc.coverageClearValue,desc.coverageClearValue,desc.coverageClearValue,desc.coverageClearValue ));
    if (desc.combinedShaderFeatures & gpu::ShaderFeatures::ENABLE_CLIPPING)
    {
        CommandList.ClearUAVUint(renderTarget->clipUAV(), FUintVector4(0));
    }
    
    if (desc.complexGradSpanCount > 0)
    {
        check(m_gradiantTexture);
        CommandList.Transition(FRHITransitionInfo(m_gradiantTexture, ERHIAccess::SRVGraphics, ERHIAccess::RTV));
        GraphicsPSOInit.PrimitiveType = PT_TriangleStrip;
        
        FRHIRenderPassInfo Info(m_gradiantTexture, ERenderTargetActions::Clear_Store);
        CommandList.BeginRenderPass(Info, TEXT("Rive_Render_Gradient"));
        CommandList.SetViewport(0, desc.complexGradRowsTop, 0,
            kGradTextureWidth, desc.complexGradRowsTop + desc.complexGradRowsHeight, 1.0);
        CommandList.ApplyCachedRenderTargets(GraphicsPSOInit);

        TShaderMapRef<FRiveGradientVertexShader> VertexShader(ShaderMap);
        TShaderMapRef<FRiveGradientPixelShader> PixelShader(ShaderMap);
        
        BindShaders(CommandList, GraphicsPSOInit, VertexShader,
            PixelShader, VertexDeclarations[static_cast<int32>(EVertexDeclarations::Gradient)]);
        
        FRiveGradientVertexShader::FParameters VertexParameters;
        FRiveGradientPixelShader::FParameters PixelParameters;
        
        VertexParameters.FlushUniforms = m_flushUniformBuffer->contents();
        PixelParameters.FlushUniforms = m_flushUniformBuffer->contents();
        
        SetParameters(CommandList, BatchedShaderParameters, VertexShader,VertexParameters);
        SetParameters(CommandList, BatchedShaderParameters, PixelShader,PixelParameters);
        
        CommandList.SetStreamSource(0, m_gradSpanBuffer->contents(), desc.firstComplexGradSpan * sizeof(GradientSpan));
        
        CommandList.DrawPrimitive(0, 2, desc.complexGradSpanCount);
        
        CommandList.EndRenderPass();
        CommandList.Transition(FRHITransitionInfo(m_gradiantTexture, ERHIAccess::RTV, ERHIAccess::SRVGraphics));
    }
    
    if (desc.simpleGradTexelsHeight > 0)
    {
        assert(desc.simpleGradTexelsHeight * desc.simpleGradTexelsWidth * 4 <=
               simpleColorRampsBufferRing()->capacityInBytes());
        
        CommandList.Transition(FRHITransitionInfo(m_gradiantTexture, ERHIAccess::SRVGraphics, ERHIAccess::CopyDest));
        CommandList.UpdateTexture2D(m_gradiantTexture, 0,
            {0, 0, 0, 0, desc.simpleGradTexelsWidth, desc.simpleGradTexelsHeight},
            kGradTextureWidth * 4, m_simpleColorRampsBuffer->contents() + desc.simpleGradDataOffsetInBytes);
        CommandList.Transition(FRHITransitionInfo(m_gradiantTexture, ERHIAccess::CopyDest, ERHIAccess::SRVGraphics));
    }

    if (desc.tessVertexSpanCount > 0)
    {
        check(m_tesselationTexture)
        CommandList.Transition(FRHITransitionInfo(m_tesselationTexture, ERHIAccess::SRVGraphics, ERHIAccess::RTV));
        FRHIRenderPassInfo Info(m_tesselationTexture, ERenderTargetActions::DontLoad_Store);
        CommandList.BeginRenderPass(Info, TEXT("RiveTessUpdate"));
        CommandList.ApplyCachedRenderTargets(GraphicsPSOInit);
        
        GraphicsPSOInit.RasterizerState = TStaticRasterizerState<FM_Solid, CM_CCW, ERasterizerDepthClipMode::DepthClip, false>::GetRHI();
        GraphicsPSOInit.PrimitiveType = PT_TriangleList;
        
        TShaderMapRef<FRiveTessVertexShader> VertexShader(ShaderMap);
        TShaderMapRef<FRiveTessPixelShader> PixelShader(ShaderMap);
        
        BindShaders(CommandList, GraphicsPSOInit, VertexShader,
            PixelShader, VertexDeclarations[static_cast<int32>(EVertexDeclarations::Tessellation)]);
        
        CommandList.SetStreamSource(0, m_tessSpanBuffer->contents(), desc.firstTessVertexSpan * sizeof(TessVertexSpan));
        
        FRiveTessPixelShader::FParameters PixelParameters;
        FRiveTessVertexShader::FParameters VertexParameters;
        
        PixelParameters.FlushUniforms = m_flushUniformBuffer->contents();
        VertexParameters.FlushUniforms = m_flushUniformBuffer->contents();
        VertexParameters.GLSL_pathBuffer_raw = m_pathBuffer->srv();
        VertexParameters.GLSL_contourBuffer_raw = m_contourBuffer->srv();

        SetParameters(CommandList, BatchedShaderParameters, VertexShader,VertexParameters);
        SetParameters(CommandList, BatchedShaderParameters, PixelShader,PixelParameters);

        CommandList.SetViewport(0, 0, 0,
            static_cast<float>(kTessTextureWidth), static_cast<float>(desc.tessDataHeight), 1);

        const size_t numTessVerts = (m_tessSpanBuffer->capacityInBytes() / sizeof(TessVertexSpan)) - desc.firstTessVertexSpan;
        CommandList.DrawIndexedPrimitive(m_tessSpanIndexBuffer, 0, desc.firstTessVertexSpan,
            numTessVerts, 0, std::size(kTessSpanIndices)/3,
            desc.tessVertexSpanCount);
        CommandList.EndRenderPass();
        CommandList.Transition(FRHITransitionInfo(m_tesselationTexture, ERHIAccess::RTV, ERHIAccess::SRVGraphics));
    }

    ERenderTargetActions loadAction = ERenderTargetActions::Load_Store;
    switch (desc.colorLoadAction)
    {
    case LoadAction::clear:
            {
                float clearColor4f[4];
                UnpackColorToRGBA32F(desc.clearColor, clearColor4f);
                CommandList.ClearUAVFloat(renderTarget->targetUAV(),
                    FVector4f(clearColor4f[0], clearColor4f[1], clearColor4f[2], clearColor4f[3]));
            }
        loadAction = ERenderTargetActions::Load_Store;
        break;
    case LoadAction::preserveRenderTarget:
        loadAction = ERenderTargetActions::Load_Store;
        break;
    case LoadAction::dontCare:
        loadAction = ERenderTargetActions::DontLoad_Store;
        break;
    }
    
    FRHIRenderPassInfo Info;
    if(!(desc.combinedShaderFeatures & ShaderFeatures::ENABLE_ADVANCED_BLEND))
    {
        Info.ColorRenderTargets[0].RenderTarget = DestTexture;
        Info.ColorRenderTargets[0].Action = loadAction;
        CommandList.Transition(FRHITransitionInfo(DestTexture, ERHIAccess::UAVGraphics, ERHIAccess::RTV));
    }
    else
    {
        Info.ResolveRect = FResolveRect(0, 0, renderTarget->width(), renderTarget->height());
    }
    
     CommandList.BeginRenderPass(Info, TEXT("Rive_Render_Flush"));
     CommandList.SetViewport(0, 0, 0, renderTarget->width(), renderTarget->height(), 1.0);

     // FIXED_FUNCTION_BLEND
    if(!(desc.combinedShaderFeatures & ShaderFeatures::ENABLE_ADVANCED_BLEND))
        GraphicsPSOInit.BlendState = TStaticBlendState<CW_RGBA, BO_Add, BF_One, BF_InverseSourceAlpha,BO_Add, BF_One, BF_InverseSourceAlpha>::GetRHI();
     // otherwise no blend
     else
        GraphicsPSOInit.BlendState = TStaticBlendState<CW_NONE>::CreateRHI();

     GraphicsPSOInit.RasterizerState = GetStaticRasterizerState<false>(FM_Solid, CM_CCW);
     CommandList.ApplyCachedRenderTargets(GraphicsPSOInit);

     for (const DrawBatch& batch : *desc.drawList)
     {
         if (batch.elementCount == 0)
         {
             continue;
         }

         AtomicPixelPermutationDomain PixelPermutationDomain;
         AtomicVertexPermutationDomain VertexPermutationDomain;

         GetPermutationForFeatures(desc.combinedShaderFeatures, PixelPermutationDomain, VertexPermutationDomain);
         
         CommandList.Transition(FRHITransitionInfo(renderTarget->coverageUAV(), ERHIAccess::UAVGraphics, ERHIAccess::UAVGraphics));
         if(desc.combinedShaderFeatures & ShaderFeatures::ENABLE_CLIPPING)
            CommandList.Transition(FRHITransitionInfo(renderTarget->clipUAV(), ERHIAccess::UAVGraphics, ERHIAccess::UAVGraphics));
         if(desc.combinedShaderFeatures & ShaderFeatures::ENABLE_ADVANCED_BLEND)
            CommandList.Transition(FRHITransitionInfo(renderTarget->targetUAV(), ERHIAccess::UAVGraphics, ERHIAccess::UAVGraphics));

         switch (batch.drawType)
         {
             case DrawType::midpointFanPatches:
             case DrawType::outerCurvePatches:
             {
                 GraphicsPSOInit.RasterizerState = GetStaticRasterizerState<false>(FM_Solid, CM_CCW);
                 GraphicsPSOInit.PrimitiveType = EPrimitiveType::PT_TriangleList;

                 TShaderMapRef<FRivePathVertexShader> VertexShader(ShaderMap, VertexPermutationDomain);
                 TShaderMapRef<FRivePathPixelShader> PixelShader(ShaderMap, PixelPermutationDomain);

                 BindShaders(CommandList, GraphicsPSOInit, VertexShader,
                    PixelShader, VertexDeclarations[static_cast<int32>(EVertexDeclarations::Paths)]);

                 FRivePathPixelShader::FParameters PixelParameters;
                 FRivePathVertexShader::FParameters VertexParameters;

                 PixelParameters.FlushUniforms = m_flushUniformBuffer->contents();
                 VertexParameters.FlushUniforms = m_flushUniformBuffer->contents();

                 PixelParameters.gradSampler = m_linearSampler;
                 PixelParameters.GLSL_gradTexture_raw = m_gradiantTexture;
                 PixelParameters.GLSL_paintAuxBuffer_raw = m_paintAuxBuffer->srv();
                 PixelParameters.GLSL_paintBuffer_raw = m_paintBuffer->srv();
                 PixelParameters.coverageCountBuffer = renderTarget->coverageUAV();
                 PixelParameters.clipBuffer = renderTarget->clipUAV();
                 PixelParameters.colorBuffer = renderTarget->targetUAV();
                 VertexParameters.GLSL_tessVertexTexture_raw= m_tessSRV;
                 VertexParameters.GLSL_pathBuffer_raw= m_pathBuffer->srv();
                 VertexParameters.GLSL_contourBuffer_raw= m_contourBuffer->srv();
                 VertexParameters.baseInstance = batch.baseElement;
                     
                 SetParameters(CommandList, BatchedShaderParameters, VertexShader,VertexParameters);
                 SetParameters(CommandList, BatchedShaderParameters, PixelShader,PixelParameters);

                 CommandList.SetStreamSource(0, m_patchVertexBuffer, 0);
                 CommandList.DrawIndexedPrimitive(m_patchIndexBuffer, 0,
                     0, kPatchVertexBufferCount,
                     PatchBaseIndex(batch.drawType), 
                     PatchIndexCount(batch.drawType) / 3,
                     batch.elementCount);
             }
                 break;
             case DrawType::interiorTriangulation:
             {
                 GraphicsPSOInit.RasterizerState = GetStaticRasterizerState<false>(FM_Solid, CM_CCW);
                 GraphicsPSOInit.PrimitiveType = EPrimitiveType::PT_TriangleList;
                 
                 TShaderMapRef<FRiveInteriorTrianglesVertexShader> VertexShader(ShaderMap, VertexPermutationDomain);
                 TShaderMapRef<FRiveInteriorTrianglesPixelShader> PixelShader(ShaderMap, PixelPermutationDomain);

                 BindShaders(CommandList, GraphicsPSOInit, VertexShader,
                    PixelShader, VertexDeclarations[static_cast<int32>(EVertexDeclarations::InteriorTriangles)]);
                 
                 FRiveInteriorTrianglesVertexShader::FParameters VertexParameters;
                 FRiveInteriorTrianglesPixelShader::FParameters PixelParameters;
                 
                 PixelParameters.FlushUniforms = m_flushUniformBuffer->contents();
                 VertexParameters.FlushUniforms = m_flushUniformBuffer->contents();
                 
                 PixelParameters.gradSampler = m_linearSampler;
                 PixelParameters.GLSL_gradTexture_raw = m_gradiantTexture;
                 PixelParameters.GLSL_paintAuxBuffer_raw = m_paintAuxBuffer->srv();
                 PixelParameters.GLSL_paintBuffer_raw = m_paintBuffer->srv();
                 PixelParameters.coverageCountBuffer = renderTarget->coverageUAV();
                 PixelParameters.clipBuffer = renderTarget->clipUAV();
                 PixelParameters.colorBuffer = renderTarget->targetUAV();
                 VertexParameters.GLSL_pathBuffer_raw= m_pathBuffer->srv();
                     
                 SetParameters(CommandList, BatchedShaderParameters, VertexShader,VertexParameters);
                 SetParameters(CommandList, BatchedShaderParameters, PixelShader,PixelParameters);
                 
                 CommandList.SetStreamSource(0, m_triangleBuffer->contents(), 0);
                 CommandList.DrawPrimitive(batch.baseElement,
                     batch.elementCount / 3, 1);
             }
                 break;
             case DrawType::imageRect:
                 SYNC_BUFFER_WITH_OFFSET(m_imageDrawUniformBuffer, CommandList, batch.imageDrawDataOffset);
             {
                 GraphicsPSOInit.RasterizerState = TStaticRasterizerState<FM_Solid, CM_None, ERasterizerDepthClipMode::DepthClip, false>::GetRHI();;
                 GraphicsPSOInit.PrimitiveType = EPrimitiveType::PT_TriangleList;
                 
                 TShaderMapRef<FRiveImageRectVertexShader> VertexShader(ShaderMap, VertexPermutationDomain);
                 TShaderMapRef<FRiveImageRectPixelShader> PixelShader(ShaderMap, PixelPermutationDomain);

                 BindShaders(CommandList, GraphicsPSOInit, VertexShader,
                    PixelShader, VertexDeclarations[static_cast<int32>(EVertexDeclarations::ImageRect)]);

                 auto imageTexture = static_cast<const PLSTextureRHIImpl*>(batch.imageTexture);
                 
                 FRiveImageRectVertexShader::FParameters VertexParameters;
                 FRiveImageRectPixelShader::FParameters PixelParameters;
             
                 VertexParameters.FlushUniforms = m_flushUniformBuffer->contents();
                 VertexParameters.ImageDrawUniforms = m_imageDrawUniformBuffer->contents();
             
                 PixelParameters.FlushUniforms = m_flushUniformBuffer->contents();
                 PixelParameters.ImageDrawUniforms = m_imageDrawUniformBuffer->contents();
                 
                 PixelParameters.GLSL_gradTexture_raw = m_gradiantTexture;
                 PixelParameters.GLSL_imageTexture_raw = imageTexture->contents();
                 PixelParameters.gradSampler = m_linearSampler;
                 PixelParameters.imageSampler = m_mipmapSampler;
                 PixelParameters.GLSL_paintAuxBuffer_raw = m_paintAuxBuffer->srv();
                 PixelParameters.GLSL_paintBuffer_raw = m_paintBuffer->srv();
                 PixelParameters.coverageCountBuffer = renderTarget->coverageUAV();
                 PixelParameters.clipBuffer = renderTarget->clipUAV();
                 PixelParameters.colorBuffer = renderTarget->targetUAV();

                 SetParameters(CommandList, BatchedShaderParameters, VertexShader,VertexParameters);
                 SetParameters(CommandList, BatchedShaderParameters, PixelShader,PixelParameters);
                 
                 CommandList.SetStreamSource(0, m_imageRectVertexBuffer, 0);
                 CommandList.DrawIndexedPrimitive(m_imageRectIndexBuffer, 0, 0, std::size(kImageRectVertices), 0, std::size(kImageRectIndices) / 3, 1);
             }
                 break;
             case DrawType::imageMesh:
             {
                 SYNC_BUFFER_WITH_OFFSET(m_imageDrawUniformBuffer, CommandList, batch.imageDrawDataOffset);
                 GraphicsPSOInit.RasterizerState = TStaticRasterizerState<FM_Solid, CM_None, ERasterizerDepthClipMode::DepthClip, false>::GetRHI();
                 GraphicsPSOInit.PrimitiveType = PT_TriangleList;
                     
                 LITE_RTTI_CAST_OR_RETURN(IndexBuffer,const RenderBufferRHIImpl*, batch.indexBuffer);
                 LITE_RTTI_CAST_OR_RETURN(VertexBuffer,const RenderBufferRHIImpl*, batch.vertexBuffer);
                 LITE_RTTI_CAST_OR_RETURN(UVBuffer,const RenderBufferRHIImpl*, batch.uvBuffer);
             
                 auto imageTexture = static_cast<const PLSTextureRHIImpl*>(batch.imageTexture);
             
                 SYNC_BUFFER(IndexBuffer, CommandList)
                 SYNC_BUFFER(VertexBuffer, CommandList)
                 SYNC_BUFFER(UVBuffer, CommandList)
                 
                 TShaderMapRef<FRiveImageMeshVertexShader> VertexShader(ShaderMap, VertexPermutationDomain);
                 TShaderMapRef<FRiveImageMeshPixelShader> PixelShader(ShaderMap, PixelPermutationDomain);

                 BindShaders(CommandList, GraphicsPSOInit, VertexShader,
                    PixelShader, VertexDeclarations[static_cast<int32>(EVertexDeclarations::ImageMesh)]);
             
                 CommandList.SetStreamSource(0, VertexBuffer->contents(), 0);
                 CommandList.SetStreamSource(1, UVBuffer->contents(), 0);
             
                 FRiveImageMeshVertexShader::FParameters VertexParameters;
                 FRiveImageMeshPixelShader::FParameters PixelParameters;
             
                 VertexParameters.FlushUniforms = m_flushUniformBuffer->contents();
                 VertexParameters.ImageDrawUniforms = m_imageDrawUniformBuffer->contents();
             
                 PixelParameters.FlushUniforms = m_flushUniformBuffer->contents();
                 PixelParameters.ImageDrawUniforms = m_imageDrawUniformBuffer->contents();
                 
                 PixelParameters.GLSL_gradTexture_raw = m_gradiantTexture;
                 PixelParameters.GLSL_imageTexture_raw = imageTexture->contents();
                 PixelParameters.gradSampler = m_linearSampler;
                 PixelParameters.imageSampler = m_mipmapSampler;
                 PixelParameters.GLSL_paintAuxBuffer_raw = m_paintAuxBuffer->srv();
                 PixelParameters.GLSL_paintBuffer_raw = m_paintBuffer->srv();
                 PixelParameters.coverageCountBuffer = renderTarget->coverageUAV();
                 PixelParameters.clipBuffer = renderTarget->clipUAV();
                 PixelParameters.colorBuffer = renderTarget->targetUAV();

                 SetParameters(CommandList, BatchedShaderParameters, VertexShader,VertexParameters);
                 SetParameters(CommandList, BatchedShaderParameters, PixelShader,PixelParameters);
                     
                 CommandList.DrawIndexedPrimitive(IndexBuffer->contents(), 0, 0,
                     VertexBuffer->sizeInBytes() / sizeof(Vec2D), 0, batch.elementCount/3,
                     1);
             }
                break;
         case DrawType::gpuAtomicResolve:
             {
                 GraphicsPSOInit.RasterizerState = TStaticRasterizerState<FM_Solid, CM_None, ERasterizerDepthClipMode::DepthClip, false>::GetRHI();
                 GraphicsPSOInit.PrimitiveType = PT_TriangleStrip;

                 TShaderMapRef<FRiveAtomiResolveVertexShader> VertexShader(ShaderMap, VertexPermutationDomain);
                 TShaderMapRef<FRiveAtomiResolvePixelShader> PixelShader(ShaderMap, PixelPermutationDomain);

                 BindShaders(CommandList, GraphicsPSOInit, VertexShader,
                    PixelShader, VertexDeclarations[static_cast<int32>(EVertexDeclarations::Resolve)]);

                 FRiveAtomiResolveVertexShader::FParameters VertexParameters;
                 FRiveAtomiResolvePixelShader::FParameters PixelParameters;
                 
                 PixelParameters.GLSL_gradTexture_raw = m_gradiantTexture;
                 PixelParameters.gradSampler = m_linearSampler;
                 PixelParameters.GLSL_paintAuxBuffer_raw = m_paintAuxBuffer->srv();
                 PixelParameters.GLSL_paintBuffer_raw = m_paintBuffer->srv();
                 PixelParameters.coverageCountBuffer = renderTarget->coverageUAV();
                 PixelParameters.clipBuffer = renderTarget->clipUAV();
                 PixelParameters.colorBuffer = renderTarget->targetUAV();
                 
                 VertexParameters.FlushUniforms = m_flushUniformBuffer->contents();
                 
                 SetParameters(CommandList, BatchedShaderParameters, VertexShader,VertexParameters);
                 SetParameters(CommandList, BatchedShaderParameters, PixelShader,PixelParameters);
                 
                 CommandList.DrawPrimitive(0, 2, 1);
             }
                 break;
             case DrawType::gpuAtomicInitialize:
             case DrawType::stencilClipReset:
                 RIVE_UNREACHABLE();
         }
     }
    
    CommandList.EndRenderPass();
    if(desc.combinedShaderFeatures & ShaderFeatures::ENABLE_ADVANCED_BLEND)
    {
        CommandList.Transition(FRHITransitionInfo(DestTexture, ERHIAccess::UAVGraphics, ERHIAccess::UAVGraphics));
    }
    else
    {
        // needed for fixed function blend mode
        CommandList.Transition(FRHITransitionInfo(DestTexture, ERHIAccess::RTV, ERHIAccess::UAVGraphics));
    }
}
