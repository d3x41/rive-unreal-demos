﻿// Copyright Rive, Inc. All rights reserved.

#include "Rive/Assets/RiveImageAsset.h"

#include "IRiveRenderer.h"
#include "IRiveRendererModule.h"
#include "Logs/RiveLog.h"
#include "Engine/Texture2D.h"
#include "TextureResource.h"

THIRD_PARTY_INCLUDES_START
#include "rive/factory.hpp"
#include "rive/renderer/render_context.hpp"
THIRD_PARTY_INCLUDES_END


URiveImageAsset::URiveImageAsset()
{
	Type = ERiveAssetType::Image;
}

void URiveImageAsset::LoadTexture(UTexture2D* InTexture)
{
	if (!InTexture) return;

	UE_LOG(LogRive, Warning, TEXT("LoadTexture NYI"));
	return;

#pragma warning (push)
#pragma warning (disable: 4702)
	IRiveRenderer* RiveRenderer = IRiveRendererModule::Get().GetRenderer();

	RiveRenderer->CallOrRegister_OnInitialized(IRiveRenderer::FOnRendererInitialized::FDelegate::CreateLambda(
		[this, InTexture](IRiveRenderer* RiveRenderer)
		{
			rive::gpu::RenderContext* RenderContext;
			{
				FScopeLock Lock(&RiveRenderer->GetThreadDataCS());
				RenderContext = RiveRenderer->GetRenderContext();
			}

			if (ensure(RenderContext))
			{
				InTexture->SetForceMipLevelsToBeResident(30.f);
				InTexture->WaitForStreaming();

				EPixelFormat PixelFormat = InTexture->GetPixelFormat();
				if (PixelFormat != PF_R8G8B8A8)
				{
					UE_LOG(LogRive, Error, TEXT("Error loading Texture '%s': Rive only supports RGBA pixel formats. This texture is of format"), *InTexture->GetName())
					return;
				}

				FTexture2DMipMap& Mip = InTexture->GetPlatformData()->Mips[0];
				uint8* MipData = reinterpret_cast<uint8*>(Mip.BulkData.Lock(LOCK_READ_ONLY));
				int32 BitmapDataSize = Mip.SizeX * Mip.SizeY * sizeof(FColor);

				TArray<uint8> BitmapData;
				BitmapData.AddUninitialized(BitmapDataSize);
				FMemory::Memcpy(BitmapData.GetData(), MipData, BitmapDataSize);
				Mip.BulkData.Unlock();

				if (MipData == nullptr)
				{
					UE_LOG(LogRive, Error, TEXT("Unable to load Mip data for %s"), *InTexture->GetName());
					return;
				}


				// decodeImage() here requires encoded bytes and returns a rive::RenderImage
				// it will call:
				//   PLSRenderContextHelperImpl::decodeImageTexture() ->
				//     Bitmap::decode()
				//       // here, Bitmap only decodes webp, jpg, png and discards otherwise
				rive::rcp<rive::RenderImage> DecodedImage = RenderContext->decodeImage(rive::make_span(BitmapData.GetData(), BitmapDataSize));

				// This is what we need, to make a RenderImage and supply raw bitmap bytes that aren't already encoded:
				// makeImage, createImage, or any other descriptive name could be used
				// rive::rcp<rive::RenderImage> RenderImage = PLSRenderContext->makeImage(rive::make_span(BitmapData.GetData(), BitmapDataSize));

				if (DecodedImage == nullptr)
				{
					UE_LOG(LogRive, Error, TEXT("Could not decode image asset: %s"), *InTexture->GetName());
					return;
				}

				NativeAsset->as<rive::ImageAsset>()->renderImage(DecodedImage);
			}
		}
	));
#pragma warning (pop)
}

void URiveImageAsset::LoadImageBytes(const TArray<uint8>& InBytes)
{
	IRiveRenderer* RiveRenderer = IRiveRendererModule::Get().GetRenderer();

	// We'll copy InBytes into the lambda because there's no guarantee they'll exist by the time it's hit
	RiveRenderer->CallOrRegister_OnInitialized(IRiveRenderer::FOnRendererInitialized::FDelegate::CreateLambda(
		[this, InBytes](IRiveRenderer* RiveRenderer)
		{
			rive::gpu::RenderContext* RenderContext;
			{
				FScopeLock Lock(&RiveRenderer->GetThreadDataCS());
				RenderContext = RiveRenderer->GetRenderContext();
			}
	
			if (ensure(RenderContext))
			{
				auto DecodedImage = RenderContext->decodeImage(rive::make_span(InBytes.GetData(), InBytes.Num()));
			
				if (DecodedImage == nullptr)
				{
					UE_LOG(LogRive, Error, TEXT("LoadImageBytes: Could not decode image bytes"));
					return;
				}
									
				rive::ImageAsset* ImageAsset = NativeAsset->as<rive::ImageAsset>();
				ImageAsset->renderImage(DecodedImage);
			}
		}
	));
}

bool URiveImageAsset::LoadNativeAssetBytes(rive::FileAsset& InAsset, rive::Factory* InRiveFactory, const rive::Span<const uint8>& AssetBytes)
{
	rive::rcp<rive::RenderImage> DecodedImage = InRiveFactory->decodeImage(AssetBytes);

	if (DecodedImage == nullptr)
	{
		UE_LOG(LogRive, Error, TEXT("Could not decode image asset: %s"), *Name);
		return false;
	}

	rive::ImageAsset* ImageAsset = InAsset.as<rive::ImageAsset>();
	ImageAsset->renderImage(DecodedImage);
	NativeAsset = ImageAsset;
	return true;
}
