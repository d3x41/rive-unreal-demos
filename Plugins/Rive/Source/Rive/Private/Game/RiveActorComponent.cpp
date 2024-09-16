// Copyright Rive, Inc. All rights reserved.

#include "Game/RiveActorComponent.h"

#include "GameFramework/Actor.h"
#include "IRiveRenderer.h"
#include "IRiveRendererModule.h"
#include "Logs/RiveLog.h"
#include "Rive/RiveArtboard.h"
#include "Rive/RiveDescriptor.h"
#include "Rive/RiveFile.h"
#include "Rive/RiveTexture.h"
#include "Stats/RiveStats.h"

class FRiveStateMachine;

URiveActorComponent::URiveActorComponent(): Size(500, 500)
{
    // Set this component to be initialized when the game starts, and to be ticked every frame.  You can turn these features
    // off to improve performance if you don't need them.
    PrimaryComponentTick.bCanEverTick = true;
}

void URiveActorComponent::BeginPlay()
{
    Initialize();
    Super::BeginPlay();
}

void URiveActorComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
    Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

    if (!IsValidChecked(this))
    {
        return;
    }

    SCOPED_NAMED_EVENT_TEXT(TEXT("URiveActorComponent::TickComponent"), FColor::White);
    DECLARE_SCOPE_CYCLE_COUNTER(TEXT("URiveActorComponent::TickComponent"), STAT_RIVEACTORCOMPONENT_TICK, STATGROUP_Rive);

    if (RiveRenderTarget)
    {
        for (URiveArtboard* Artboard : Artboards)
        {
            RiveRenderTarget->Save();
            Artboard->Tick(DeltaTime);
            RiveRenderTarget->Restore();
        }


        RiveRenderTarget->SubmitAndClear();
    }
}

void URiveActorComponent::Initialize()
{
    IRiveRenderer* RiveRenderer = IRiveRendererModule::Get().GetRenderer();
    if (!RiveRenderer)
    {
        UE_LOG(LogRive, Error, TEXT("RiveRenderer is null, unable to initialize the RenderTarget for Rive file '%s'"), *GetFullNameSafe(this));
        return;
    }
    
    RiveRenderer->CallOrRegister_OnInitialized(IRiveRenderer::FOnRendererInitialized::FDelegate::CreateUObject(this, &URiveActorComponent::RiveReady));
}

void URiveActorComponent::ResizeRenderTarget(int32 InSizeX, int32 InSizeY)
{
    if (!RiveTexture)
    {
        return;
    }
	
    RiveTexture->ResizeRenderTargets(FIntPoint(InSizeX, InSizeY));
}

URiveArtboard* URiveActorComponent::AddArtboard(URiveFile* InRiveFile, const FString& InArtboardName, const FString& InStateMachineName)
{
    if (!IsValid(InRiveFile))
    {
        UE_LOG(LogRive, Error, TEXT("Can't instantiate an artboard without a valid RiveFile."));
        return nullptr;
    }
    if (!InRiveFile->IsInitialized())
    {
        UE_LOG(LogRive, Error, TEXT("Can't instantiate an artboard a RiveFile that is not initialized!"));
        return nullptr;
    }
	
    if (!IRiveRendererModule::IsAvailable())
    {
        UE_LOG(LogRive, Error, TEXT("Could not load rive file as the required Rive Renderer Module is either missing or not loaded properly."));
        return nullptr;
    }

    IRiveRenderer* RiveRenderer = IRiveRendererModule::Get().GetRenderer();

    if (!RiveRenderer)
    {
        UE_LOG(LogRive, Error, TEXT("Failed to instantiate the Artboard of Rive file '%s' as we do not have a valid renderer."), *GetFullNameSafe(InRiveFile));
        return nullptr;
    }

    if (!RiveRenderer->IsInitialized())
    {
        UE_LOG(LogRive, Error, TEXT("Could not load rive file as the required Rive Renderer is not initialized."));
        return nullptr;
    }
    
    URiveArtboard* Artboard = NewObject<URiveArtboard>();
    Artboard->Initialize(InRiveFile, RiveRenderTarget, InArtboardName, InStateMachineName);    
    Artboards.Add(Artboard);

    if (RiveAudioEngine != nullptr)
    {
        Artboard->SetAudioEngine(RiveAudioEngine);
    }
    
    return Artboard;
}

void URiveActorComponent::RemoveArtboard(URiveArtboard* InArtboard)
{
    Artboards.RemoveSingle(InArtboard);
}

URiveArtboard* URiveActorComponent::GetDefaultArtboard() const
{
    return GetArtboardAtIndex(0);
}

URiveArtboard* URiveActorComponent::GetArtboardAtIndex(int32 InIndex) const
{
    if (Artboards.IsEmpty())
    {
        return nullptr;
    }

    if (InIndex >= Artboards.Num())
    {
        UE_LOG(LogRive, Warning, TEXT("GetArtboardAtIndex with index %d is out of bounds"), InIndex);
        return nullptr;
    }

    return Artboards[InIndex];
}

int32 URiveActorComponent::GetArtboardCount() const
{
    return Artboards.Num();
}

void URiveActorComponent::SetAudioEngine(URiveAudioEngine* InRiveAudioEngine)
{
    RiveAudioEngine = InRiveAudioEngine;
    InitializeAudioEngine();
}

#if WITH_EDITOR
void URiveActorComponent::PostEditChangeChainProperty(FPropertyChangedChainEvent& PropertyChangedEvent)
{
    Super::PostEditChangeProperty(PropertyChangedEvent);
	
    const FName PropertyName = PropertyChangedEvent.GetPropertyName();
    const FName ActiveMemberNodeName = *PropertyChangedEvent.PropertyChain.GetActiveMemberNode()->GetValue()->GetName();
	
    if (PropertyName == GET_MEMBER_NAME_CHECKED(FRiveDescriptor, RiveFile) ||
        PropertyName == GET_MEMBER_NAME_CHECKED(FRiveDescriptor, ArtboardIndex) ||
        PropertyName == GET_MEMBER_NAME_CHECKED(FRiveDescriptor, ArtboardName))
    {
        TArray<FString> ArtboardNames = GetArtboardNamesForDropdown();
        if (ArtboardNames.Num() > 0 && DefaultRiveDescriptor.ArtboardIndex == 0 && (DefaultRiveDescriptor.ArtboardName.IsEmpty() || !ArtboardNames.Contains(DefaultRiveDescriptor.ArtboardName)))
        {
            DefaultRiveDescriptor.ArtboardName = ArtboardNames[0];
        }
        
        TArray<FString> StateMachineNames = GetStateMachineNamesForDropdown();
        if (StateMachineNames.Num() == 1)
        {
            DefaultRiveDescriptor.StateMachineName = StateMachineNames[0]; // No state machine, use blank
        } else if (DefaultRiveDescriptor.StateMachineName.IsEmpty() || !StateMachineNames.Contains(DefaultRiveDescriptor.StateMachineName))
        {
            DefaultRiveDescriptor.StateMachineName = StateMachineNames[1];
        }
    }
}
#endif

void URiveActorComponent::OnResourceInitialized_RenderThread(FRHICommandListImmediate& RHICmdList, FTextureRHIRef& NewResource)
{
    // When the resource change, we need to tell the Render Target otherwise we will keep on drawing on an outdated RT
    if (const TSharedPtr<IRiveRenderTarget> RTarget = RiveRenderTarget) //todo: might need a lock
    {
        RTarget->CacheTextureTarget_RenderThread(RHICmdList, NewResource);
    }
}

void URiveActorComponent::OnDefaultArtboardTickRender(float DeltaTime, URiveArtboard* InArtboard)
{
    InArtboard->Align(DefaultRiveDescriptor.FitType, DefaultRiveDescriptor.Alignment);
    InArtboard->Draw();
}

TArray<FString> URiveActorComponent::GetArtboardNamesForDropdown() const
{
    TArray<FString> Output;
    if (DefaultRiveDescriptor.RiveFile)
    {
        for (URiveArtboard* Artboard : DefaultRiveDescriptor.RiveFile->Artboards)
        {
            Output.Add(Artboard->GetArtboardName());
        }
    }
    return Output;
}

TArray<FString> URiveActorComponent::GetStateMachineNamesForDropdown() const
{
    TArray<FString> Output {""};
    if (DefaultRiveDescriptor.RiveFile)
    {
        for (URiveArtboard* Artboard : DefaultRiveDescriptor.RiveFile->Artboards)
        {
            if (Artboard->GetArtboardName().Equals(DefaultRiveDescriptor.ArtboardName))
            {
                Output.Append(Artboard->GetStateMachineNames());
                break;
            }
        }
    }
    return Output;
}

void URiveActorComponent::InitializeAudioEngine()
{
    if (RiveAudioEngine == nullptr)
    {
        if (URiveAudioEngine* AudioEngine = GetOwner()->GetComponentByClass<URiveAudioEngine>())
        {
            RiveAudioEngine = AudioEngine;
        }
    }
    
    if (RiveAudioEngine != nullptr)
    {
        if (RiveAudioEngine->GetNativeAudioEngine() == nullptr)
        {
            if (AudioEngineLambdaHandle.IsValid())
            {
                RiveAudioEngine->OnRiveAudioReady.Remove(AudioEngineLambdaHandle);
                AudioEngineLambdaHandle.Reset();
            }

            TFunction<void()> AudioLambda = [this]()
            {
                for (URiveArtboard* Artboard : Artboards)
                {
                    Artboard->SetAudioEngine(RiveAudioEngine);
                }
                RiveAudioEngine->OnRiveAudioReady.Remove(AudioEngineLambdaHandle);
            };
            AudioEngineLambdaHandle = RiveAudioEngine->OnRiveAudioReady.AddLambda(AudioLambda);
        }
        else
        {
            for (URiveArtboard* Artboard : Artboards)
            {
                Artboard->SetAudioEngine(RiveAudioEngine);
            }
        }
    }
}

void URiveActorComponent::RiveReady(IRiveRenderer* InRiveRenderer)
{
    RiveTexture = NewObject<URiveTexture>();
    // Initialize Rive Render Target Only after we resize the texture
    RiveRenderTarget = InRiveRenderer->CreateTextureTarget_GameThread(GetFName(), RiveTexture);
    RiveRenderTarget->SetClearColor(FLinearColor::Transparent);
    RiveTexture->ResizeRenderTargets(FIntPoint(Size.X, Size.Y));
    RiveRenderTarget->Initialize();

    RiveTexture->OnResourceInitializedOnRenderThread.AddUObject(this, &URiveActorComponent::OnResourceInitialized_RenderThread);
    
    if (DefaultRiveDescriptor.RiveFile)
    {
        URiveArtboard* Artboard = AddArtboard(DefaultRiveDescriptor.RiveFile, DefaultRiveDescriptor.ArtboardName, DefaultRiveDescriptor.StateMachineName);
        Artboard->OnArtboardTick_Render.BindDynamic(this, &URiveActorComponent::OnDefaultArtboardTickRender);
    }
    
    InitializeAudioEngine();
    
    OnRiveReady.Broadcast();
}
