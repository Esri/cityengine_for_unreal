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

#pragma once

#include "VitruvioBatchActor.h"
#include "Runtime/Engine/Public/Subsystems/WorldSubsystem.h"

#include "VitruvioBatchSubsystem.generated.h"

class UGenerateCompletedCallbackProxy;
class UVitruvioComponent;

UCLASS()
class VITRUVIO_API UVitruvioBatchSubsystem final : public UWorldSubsystem
{
	GENERATED_BODY()
	
public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	
	void RegisterVitruvioComponent(UVitruvioComponent* VitruvioComponent, bool bGenerateModel = true);
	void UnregisterVitruvioComponent(UVitruvioComponent* VitruvioComponent);

	void EvaluateAttributes(UVitruvioComponent* VitruvioComponent, UGenerateCompletedCallbackProxy* CallbackProxy = nullptr);
	void EvaluateAllAttributes(UGenerateCompletedCallbackProxy* CallbackProxy = nullptr);
	
	void Generate(UVitruvioComponent* VitruvioComponent, UGenerateCompletedCallbackProxy* CallbackProxy = nullptr);
	void GenerateAll(UGenerateCompletedCallbackProxy* CallbackProxy);

	void AddBatchedInitialShape(const FInitialShapeData& InitialShape);

	AVitruvioBatchActor* GetBatchActor();
	bool HasRegisteredVitruvioComponents() const;

	DECLARE_MULTICAST_DELEGATE(FOnComponentRegistered);
	FOnComponentRegistered OnComponentRegistered;

	DECLARE_MULTICAST_DELEGATE(FOnComponentDeregistered);
	FOnComponentDeregistered OnComponentDeregistered;
	
private:

	UPROPERTY()
	AVitruvioBatchActor* VitruvioBatchActor = nullptr;

	UPROPERTY()
	TSet<UVitruvioComponent*> RegisteredComponents;

#if WITH_EDITORONLY_DATA
	FDelegateHandle OnActorMoved;
	FDelegateHandle OnActorsMoved;
	FDelegateHandle OnActorDeleted;
#endif
	
};