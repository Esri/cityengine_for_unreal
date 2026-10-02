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

#include "AttributeMap.h"
#include "InitialShape.h"
#include "MeshCache.h"
#include "PRTTypes.h"
#include "Report.h"
#include "RulePackage.h"

#include "prt/Object.h"

#include "Engine/StaticMesh.h"
#include "HAL/ThreadSafeCounter.h"
#include "HAL/ThreadSafeBool.h"
#include "Modules/ModuleManager.h"
#include "UObject/GCObject.h"

#include "UnrealLogHandler.h"
#include "VitruvioTypes.h"

#include <memory>

class UWorld;

DECLARE_LOG_CATEGORY_EXTERN(LogUnrealPrt, Log, All);

struct FGenerateResultDescription
{
	TSharedPtr<FVitruvioMesh> GeneratedModel;
	
	Vitruvio::FInstanceMap Instances;
	TMap<FString, TSharedPtr<FVitruvioMesh>> InstanceMeshes;
	TMap<FString, FString> InstanceNames;
	
	TMap<FString, FReport> Reports;

	TArray<FAttributeMapPtr> EvaluatedAttributes;
};

class FInvalidationToken
{
public:
	mutable FCriticalSection Lock;

	void Invalidate()
	{
		FScopeLock InvalidationLock(&Lock);
		bIsInvalid = true;
	}

	bool IsInvalid() const
	{
		return bIsInvalid;
	}

private:
	FThreadSafeBool bIsInvalid = false;
};

class FEvalAttributesToken : public FInvalidationToken
{
};

class FGenerateToken : public FInvalidationToken
{
};

template <typename R, typename T>
class TResult
{
public:
	using FTokenConstPtr = TSharedPtr<const T>;
	using FTokenPtr = TSharedPtr<T>;

	struct ResultType
	{
		FTokenConstPtr Token;
		R Value;
	};
	using FFutureType = TFuture<ResultType>;

	FFutureType Result;
	FTokenPtr Token;
};

struct FInitialShape
{
	int64 InitialShapeIndex;
	FVector Position;
	FInitialShapePolygon Polygon;
	TMap<FString, TWeakObjectPtr<URuleAttribute>> Attributes;
	int32 RandomSeed = 0;
	URulePackage* RulePackage = nullptr;
	bool bOccluderOnly = false;
};

using FGenerateResult = TResult<FGenerateResultDescription, FGenerateToken>;
using FBatchGenerateResult = TResult<FGenerateResultDescription, FGenerateToken>;
using FAttributeMapResult = TResult<FAttributeMapPtr, FEvalAttributesToken>;
using FAttributeMapsResult = TResult<TArray<FAttributeMapPtr>, FEvalAttributesToken>;

class VitruvioModule final : public IModuleInterface, public FGCObject
{
	friend class VitruvioEditorModule;

public:
	void StartupModule() override;
	void ShutdownModule() override;

	/**
	 * \brief Decodes the given texture.
	 */
	VITRUVIO_API Vitruvio::FTextureData DecodeTexture(UObject* Outer, const FString& Path, const FString& Key) const;

	/**
	 * \brief Asynchronously evaluates the attributes and generates the models for all given InitialShapes.
	 *
	 * \param InitialShapes
	 * \param bEnableOcclusionQueries
	 * \param OccluderOnlyShapes
	 * \return the generated UStaticMesh.
	 */
	VITRUVIO_API FBatchGenerateResult BatchGenerateAsync(TArray<FInitialShape> InitialShapes, bool bEnableOcclusionQueries, TArray<FInitialShape> OccluderOnlyShapes) const;

	/**
	 * \brief Generate the models with the given InitialShapes.
	 *
	 * \param InitialShapes
	 * \param bEnableOcclusionQueries
	 * \param OccluderOnlyShapes
	 * \return the generated UStaticMesh.
	 */
	VITRUVIO_API FGenerateResultDescription BatchGenerate(TArray<FInitialShape> InitialShapes, bool bEnableOcclusionQueries, TArray<FInitialShape> OccluderOnlyShapes) const;

	/**
	 * \brief Asynchronously Evaluates attributes for the given initial shapes and rule packages.
	 *
	 * \param InitialShapes
	 */
	VITRUVIO_API FAttributeMapsResult BatchEvaluateRuleAttributesAsync(TArray<FInitialShape> InitialShapes) const;
	
	/**
	 * \brief Evaluates attributes for the given initial shapes and rule package.
	 *
	 * \param InitialShapes
	 */
	VITRUVIO_API TArray<FAttributeMapPtr> BatchEvaluateRuleAttributes(TArray<FInitialShape> InitialShapes) const;

	/**
	 * \brief Asynchronously generate the models with the given InitialShape, RulePackage and Attributes.
	 *
	 * \param InitialShapes The initial shapes to generate the models for.
	 *						Initial shapes after the first one are considered occlusion shapes and will not be generated as models,
	 *						but only used for occlusion queries.
	 * \return the generated UStaticMesh.
	 */
	VITRUVIO_API FGenerateResult GenerateAsync(TArray<FInitialShape> InitialShapes) const;

	/**
	 * \brief Generate the models with the given InitialShape, RulePackage and Attributes.
	 *
	 * \param InitialShapes The initial shapes to generate the models for.
	 *						Initial shapes after the first one are considered occlusion shapes and will not be generated as models,
	 *						but only used for occlusion queries.
	 * \return the generated UStaticMesh.
	 */
	VITRUVIO_API FGenerateResultDescription Generate(TArray<FInitialShape> InitialShapes) const;

	/**
	 * \brief Asynchronously evaluates attributes for the given initial shape and rule package.
	 *
	 * \param InitialShape
	 * \return
	 */
	VITRUVIO_API FAttributeMapResult EvaluateRuleAttributesAsync(FInitialShape InitialShape) const;

	/**
	 * \return whether PRT is initialized meaning installed and ready to use. Before initialization generation is not possible and will
	 * immediately return without results.
	 */
	VITRUVIO_API bool IsInitialized() const
	{
		return Initialized;
	}

	/**
	 * \return true if currently at least one generate call ongoing.
	 */
	VITRUVIO_API bool IsGenerating() const
	{
		return GenerateCallsCounter.GetValue() > 0;
	}

	/**
	 * \return the number of active generate calls.
	 */
	VITRUVIO_API int32 GetNumGenerateCalls() const
	{
		return GenerateCallsCounter.GetValue();
	}

	/**
	 * \return true if currently at least one RPK is being loaded.
	 */
	VITRUVIO_API bool IsLoadingRpks() const
	{
		return RpkLoadingTasksCounter.GetValue() > 0;
	}

	/**
	 * \returns the cache used for materials generated by PRT.
	 */
	VITRUVIO_API TMap<Vitruvio::FMaterialAttributeContainer, TObjectPtr<UMaterialInstanceDynamic>>& GetMaterialCache()
	{
		return MaterialCache;
	}

	/**
	 * \returns the cache used for instanced meshes by PRT.
	 */
	VITRUVIO_API FMeshCache& GetMeshCache()
	{
		return MeshCache;
	}

	/**
	 * \returns the cache used for materials generated by PRT.
	 */
	VITRUVIO_API TMap<FString, Vitruvio::FTextureData>& GetTextureCache()
	{
		return TextureCache;
	}

	/**
	 * Registers a generated mesh to keep it from being garbage collected.
	 */
	VITRUVIO_API void RegisterMesh(UStaticMesh* StaticMesh);

	/**
	 * Unregisters a generated mesh and therefore allows the garbage collector to delete it if it not referenced anywhere else.
	 */
	VITRUVIO_API void UnregisterMesh(UStaticMesh* StaticMesh);

	/**
	 * Invalidates the occlusion handle of the given initial shape index.
	 * 
	 * @param InitialShapeIndex 
	 */
	VITRUVIO_API void InvalidateOcclusionHandle(int64 InitialShapeIndex);

	/**
	 * Invalidates the occlusion handles of the given initial shape indices.
	 * 
	 * @param InitialShapeIndices 
	 */
	VITRUVIO_API void InvalidateOcclusionHandles(const TArray<int64>& InitialShapeIndices) const;

	/**
	 * Invalidates all occlusion handles.
	 */
	VITRUVIO_API void InvalidateAllOcclusionHandles();

	DECLARE_MULTICAST_DELEGATE_OneParam(FOnGenerateCompleted, int);

	DECLARE_MULTICAST_DELEGATE_TwoParams(FOnAllGenerateCompleted, int, int);

	/**
	 * Delegate which is called after a generate call has completed.
	 */
	FOnGenerateCompleted OnGenerateCompleted;

	/**
	 * Delegate which is called after all generate calls have completed.
	 */
	FOnAllGenerateCompleted OnAllGenerateCompleted;

	void AddReferencedObjects(FReferenceCollector& Collector) override
	{
		Collector.AddReferencedObjects(MaterialCache);
		Collector.AddReferencedObjects(RegisteredMeshes);
	}

	FString GetReferencerName() const override
	{
		return TEXT("Vitruvio");
	}

	static VitruvioModule& Get()
	{
		return FModuleManager::LoadModuleChecked<VitruvioModule>("Vitruvio");
	}

	static VitruvioModule* GetUnchecked()
	{
		return FModuleManager::LoadModulePtr<VitruvioModule>("Vitruvio");
	}

private:
	void* PrtDllHandle = nullptr;
	prt::Object const* PrtLibrary = nullptr;
	CacheObjectUPtr PrtCache;
	
	TUniquePtr<UnrealLogHandler> LogHandler;

	TAtomic<bool> Initialized = false;

	mutable TMap<TLazyObjectPtr<URulePackage>, ResolveMapSPtr> ResolveMapCache;
	mutable TMap<TLazyObjectPtr<URulePackage>, FGraphEventRef> ResolveMapEventGraphRefCache;

	mutable FCriticalSection LoadResolveMapLock;

	mutable FThreadSafeCounter GenerateCallsCounter;
	mutable FThreadSafeCounter RpkLoadingTasksCounter;
	mutable FThreadSafeCounter LoadAttributesCounter;

	FString RpkFolder;

	TMap<Vitruvio::FMaterialAttributeContainer, TObjectPtr<UMaterialInstanceDynamic>> MaterialCache;
	TMap<FString, Vitruvio::FTextureData> TextureCache;
	FMeshCache MeshCache;

	mutable FCriticalSection OcclusionLock;
	mutable TMap<int64, prt::OcclusionSet::Handle> OcclusionHandleCache;

	mutable OcclusionSetUPtr OcclusionSet;

	FCriticalSection RegisterMeshLock;
	TSet<TObjectPtr<UStaticMesh>> RegisteredMeshes;

	FDelegateHandle OnWorldCleanupHandle;
	void OnWorldCleanup(UWorld* World, bool bSessionEnded, bool bCleanupResources);

	void NotifyGenerateCompleted() const;

	TFuture<ResolveMapSPtr> LoadResolveMapAsync(URulePackage* RulePackage) const;
	void InitializePrt();

	VITRUVIO_API void EvictFromResolveMapCache(URulePackage* RulePackage);
};
