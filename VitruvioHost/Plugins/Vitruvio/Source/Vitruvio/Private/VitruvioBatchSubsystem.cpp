/* Copyright 2024 Esri
*
 * Licensed under the Apache License Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "VitruvioBatchSubsystem.h"

#include "EngineUtils.h"

void UVitruvioBatchSubsystem::RegisterVitruvioComponent(UVitruvioComponent* VitruvioComponent, bool bGenerateModel)
{
	GetBatchActor()->RegisterVitruvioComponent(VitruvioComponent, bGenerateModel);
	
	RegisteredComponents.Add(VitruvioComponent);
	OnComponentRegistered.Broadcast();
}

void UVitruvioBatchSubsystem::UnregisterVitruvioComponent(UVitruvioComponent* VitruvioComponent)
{
	RegisteredComponents.Remove(VitruvioComponent);
	GetBatchActor()->UnregisterVitruvioComponent(VitruvioComponent);

	OnComponentDeregistered.Broadcast();
}

void UVitruvioBatchSubsystem::EvaluateAttributes(UVitruvioComponent* VitruvioComponent, UGenerateCompletedCallbackProxy* CallbackProxy)
{
	GetBatchActor()->EvaluateAttributes(VitruvioComponent, CallbackProxy);
}

void UVitruvioBatchSubsystem::EvaluateAllAttributes(UGenerateCompletedCallbackProxy* CallbackProxy)
{
	GetBatchActor()->EvaluateAllAttributes(CallbackProxy);
}

void UVitruvioBatchSubsystem::Generate(UVitruvioComponent* VitruvioComponent, UGenerateCompletedCallbackProxy* CallbackProxy)
{
	GetBatchActor()->Generate(VitruvioComponent, CallbackProxy);
}

void UVitruvioBatchSubsystem::GenerateAll(UGenerateCompletedCallbackProxy* CallbackProxy)
{
	GetBatchActor()->GenerateAll(CallbackProxy);
}

AVitruvioBatchActor* UVitruvioBatchSubsystem::GetBatchActor()
{
	if (!VitruvioBatchActor)
	{
		const TActorIterator<AVitruvioBatchActor> ActorIterator(GetWorld());

		if (ActorIterator)
		{
			VitruvioBatchActor = *ActorIterator;
		}
		else
		{
			FActorSpawnParameters ActorSpawnParameters;
			ActorSpawnParameters.Name = FName(TEXT("VitruvioBatchActor"));
			VitruvioBatchActor = GetWorld()->SpawnActor<AVitruvioBatchActor>(ActorSpawnParameters);
			
			for (UVitruvioComponent* VitruvioComponent : RegisteredComponents)
			{
				VitruvioBatchActor->RegisterVitruvioComponent(VitruvioComponent);
			}
		}
	}

	return VitruvioBatchActor;
}

bool UVitruvioBatchSubsystem::HasRegisteredVitruvioComponents() const
{
	return RegisteredComponents.Num() > 0;
}

void UVitruvioBatchSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	UWorldSubsystem::Initialize(Collection);

#if WITH_EDITOR
	auto ActorMoved = [this](const AActor* Actor)
	{
		if (UVitruvioComponent* VitruvioComponent = Actor->FindComponentByClass<UVitruvioComponent>())
		{
			if (VitruvioComponent->IsBatchGenerated())
			{
				UnregisterVitruvioComponent(VitruvioComponent);
				RegisterVitruvioComponent(VitruvioComponent);
			}
		}
	};
	
	OnActorMoved = GEngine->OnActorMoved().AddLambda([ActorMoved](AActor* Actor)
	{
		ActorMoved(Actor);
	});

	OnActorsMoved = GEngine->OnActorsMoved().AddLambda([ActorMoved](const TArray<AActor*>& Actors)
	{
		for (const AActor* Actor : Actors)
		{
			ActorMoved(Actor);
		}
	});

	OnActorDeleted = GEngine->OnLevelActorDeleted().AddLambda([this](AActor* Actor)
	{
		if (UVitruvioComponent* VitruvioComponent = Actor->FindComponentByClass<UVitruvioComponent>())
		{
			if (VitruvioComponent->IsBatchGenerated())
			{
				UnregisterVitruvioComponent(VitruvioComponent);
			}
		}
	});
#endif

	for (TActorIterator<AActor> It(GetWorld()); It; ++It)
	{
		AActor* Actor = *It;

		if (UVitruvioComponent* VitruvioComponent = Actor->FindComponentByClass<UVitruvioComponent>())
		{
			if (VitruvioComponent->IsBatchGenerated())
			{
				RegisterVitruvioComponent(VitruvioComponent);
			}
		}
	}
}

void UVitruvioBatchSubsystem::Deinitialize()
{
#if WITH_EDITOR
	GEngine->OnActorMoved().Remove(OnActorMoved);
	GEngine->OnActorsMoved().Remove(OnActorsMoved);
	GEngine->OnLevelActorDeleted().Remove(OnActorDeleted);
#endif
	
	UWorldSubsystem::Deinitialize();
}
