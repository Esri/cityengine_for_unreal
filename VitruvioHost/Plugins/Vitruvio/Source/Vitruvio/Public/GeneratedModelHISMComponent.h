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

#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Interfaces/Interface_CollisionDataProvider.h"

#include "GeneratedModelHISMComponent.generated.h"

UCLASS(ClassGroup = (Custom), meta = (BlueprintSpawnableComponent))
class VITRUVIO_API UGeneratedModelHISMComponent : public UHierarchicalInstancedStaticMeshComponent
{
	GENERATED_BODY()
	
public:
	const FString& GetMeshIdentifier() const
	{
		return MeshIdentifier;
	}

	void SetMeshIdentifier(const FString& NewMeshIdentifier)
	{
		MeshIdentifier = NewMeshIdentifier;
	}

private:
	FString MeshIdentifier;
};