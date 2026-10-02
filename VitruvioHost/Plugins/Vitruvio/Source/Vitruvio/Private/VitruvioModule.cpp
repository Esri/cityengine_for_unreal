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

#include "VitruvioModule.h"

#include "prt/API.h"

#include "PRTTypes.h"
#include "PRTUtils.h"
#include "TextureDecoding.h"
#include "UnrealCallbacks.h"

#include "Util/PolygonWindings.h"

#include "Async/Async.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Modules/ModuleManager.h"

#include "UObject/UObjectBaseUtility.h"
#include "Util/AttributeConversion.h"

#define LOCTEXT_NAMESPACE "VitruvioModule"

DEFINE_LOG_CATEGORY(LogUnrealPrt);

#define CHECK_PRT_INITIALIZED()                                                                                                                      \
    if (!Initialized)                                                                                                                                \
    {                                                                                                                                                \
        UE_LOG(LogUnrealPrt, Warning, TEXT("PRT not initialized"))                                                                                   \
        return {};                                                                                                                                   \
    }

#define CHECK_PRT_INITIALIZED_ASYNC(RESULT_CLASS, TOKEN_VAR)                                                                                         \
    if (!Initialized)                                                                                                                                \
    {                                                                                                                                                \
        UE_LOG(LogUnrealPrt, Warning, TEXT("PRT not initialized"))                                                                                   \
        TPromise<RESULT_CLASS::ResultType> Result;                                                                                                   \
        Result.SetValue({TOKEN_VAR, {}});                                                                                                            \
        return { Result.GetFuture(), TOKEN_VAR };                                                                                                    \
    }

namespace
{
constexpr const wchar_t* ATTRIBUTE_EVAL_ENCODER_ID = L"com.esri.prt.core.AttributeEvalEncoder";

struct FStartRuleInfo
{
	ResolveMapSPtr ResolveMap;
	FString RuleFile;
	FString StartRule;
	RuleFileInfoPtr RuleFileInfo;
};

class FLoadResolveMapTask
{
	TLazyObjectPtr<URulePackage> LazyRulePackagePtr;
	TPromise<ResolveMapSPtr> Promise;
	TMap<TLazyObjectPtr<URulePackage>, ResolveMapSPtr>& ResolveMapCache;
	FCriticalSection& LoadResolveMapLock;
	FString RpkFolder;

public:
	FLoadResolveMapTask(TPromise<ResolveMapSPtr>&& InPromise, const FString RpkFolder, const TLazyObjectPtr<URulePackage> LazyRulePackagePtr,
						TMap<TLazyObjectPtr<URulePackage>, ResolveMapSPtr>& ResolveMapCache, FCriticalSection& LoadResolveMapLock)
		: LazyRulePackagePtr(LazyRulePackagePtr), Promise(MoveTemp(InPromise)), ResolveMapCache(ResolveMapCache),
		  LoadResolveMapLock(LoadResolveMapLock), RpkFolder(RpkFolder)
	{
	}

	static const TCHAR* GetTaskName()
	{
		return TEXT("FLoadResolveMapTask");
	}
	FORCEINLINE static TStatId GetStatId()
	{
		RETURN_QUICK_DECLARE_CYCLE_STAT(FLoadResolveMapTask, STATGROUP_TaskGraphTasks);
	}

	static ENamedThreads::Type GetDesiredThread()
	{
		return ENamedThreads::AnyThread;
	}

	static ESubsequentsMode::Type GetSubsequentsMode()
	{
		return ESubsequentsMode::TrackSubsequents;
	}

	void DoTask(ENamedThreads::Type CurrentThread, const FGraphEventRef& MyCompletionGraphEvent)
	{
		const FString UriPath = LazyRulePackagePtr->GetPathName();

		// Create rpk on disk for PRT
		IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();

		const FString RpkFile = FPaths::GetBaseFilename(UriPath, false) + TEXT(".rpk");
		const FString RpkFilePath = FPaths::Combine(RpkFolder, RpkFile);
		const FString RpkFolderPath = FPaths::GetPath(RpkFilePath);

		IFileManager::Get().Delete(*RpkFilePath);
		PlatformFile.CreateDirectoryTree(*RpkFolderPath);

		IFileHandle* RpkHandle = PlatformFile.OpenWrite(*RpkFilePath);
		if (RpkHandle)
		{
			// Write file to disk
			RpkHandle->Write(LazyRulePackagePtr->Data.GetData(), LazyRulePackagePtr->Data.Num());
			RpkHandle->Flush();
			delete RpkHandle;

			// Create rpk
			const std::wstring AbsoluteRpkPath(TCHAR_TO_WCHAR(*FPaths::ConvertRelativePathToFull(RpkFilePath)));

			const std::wstring RpkFileUri = prtu::toFileURI(AbsoluteRpkPath);
			prt::Status Status;
			const ResolveMapSPtr ResolveMapPtr(prt::createResolveMap(RpkFileUri.c_str(), nullptr, &Status), PRTDestroyer());
			{
				FScopeLock Lock(&LoadResolveMapLock);
				ResolveMapCache.Add(LazyRulePackagePtr, ResolveMapPtr);
				Promise.SetValue(ResolveMapPtr);
			}
		}
		else
		{
			Promise.SetValue(nullptr);
		}
	}
};

void SetInitialShapeGeometry(const InitialShapeBuilderUPtr& InitialShapeBuilder, const FInitialShape& InitialShape)
{
	std::vector<double> vertexCoords;
	std::vector<uint32_t> indices;
	std::vector<uint32_t> faceCounts;
	std::vector<uint32_t> holes;
	
	for (int VertexIndex = 0; VertexIndex < InitialShape.Polygon.Vertices.Num(); ++ VertexIndex)
	{
		
		const FVector Vertex = InitialShape.Position + InitialShape.Polygon.Vertices[VertexIndex];
		const FVector CEVertex = FVector(Vertex.X, Vertex.Z, Vertex.Y) / 100.0;
		vertexCoords.push_back(CEVertex.X);
		vertexCoords.push_back(CEVertex.Y);
		vertexCoords.push_back(CEVertex.Z);
	}

	for (const FInitialShapeFace& Face : InitialShape.Polygon.Faces)
	{
		faceCounts.push_back(Face.Indices.Num());
		for (const int32& Index : Face.Indices)
		{
			indices.push_back(Index);
		}

		if (Face.Holes.Num() > 0)
		{
			holes.push_back(faceCounts.size() - 1);

			for (const FInitialShapeHole& Hole : Face.Holes)
			{
				faceCounts.push_back(Hole.Indices.Num());
				for (const int32& Index : Hole.Indices)
				{
					indices.push_back(Index);
				}
				holes.push_back(faceCounts.size() - 1);
			}

			holes.push_back(std::numeric_limits<uint32_t>::max());
		}
	}

	const prt::Status SetGeometryStatus = InitialShapeBuilder->setGeometry(vertexCoords.data(), vertexCoords.size(), indices.data(), indices.size(),
																		   faceCounts.data(), faceCounts.size(), holes.data(), holes.size());

	if (SetGeometryStatus != prt::STATUS_OK)
	{
		UE_LOG(LogUnrealPrt, Error, TEXT("InitialShapeBuilder setGeometry failed status = %hs"), prt::getStatusDescription(SetGeometryStatus))
	}

	for (int32 UVSet = 0; UVSet < 8; ++UVSet)
	{
		std::vector<double> uvCoords;
		std::vector<uint32_t> uvIndices;

		uint32_t CurrentUVIndex = 0;
		if (UVSet >= InitialShape.Polygon.TextureCoordinateSets.Num())
		{
			continue;
		}

		for (const auto& UV : InitialShape.Polygon.TextureCoordinateSets[UVSet].TextureCoordinates)
		{
			uvIndices.push_back(CurrentUVIndex++);
			uvCoords.push_back(UV.X);
			uvCoords.push_back(-UV.Y);
		}

		if (uvCoords.empty())
		{
			continue;
		}

		InitialShapeBuilder->setUVs(uvCoords.data(), uvCoords.size(), uvIndices.data(), uvIndices.size(), faceCounts.data(), faceCounts.size(),
									UVSet);
	}
}

AttributeMapUPtr EvaluateRuleAttributes(const std::wstring& RuleFile, const std::wstring& StartRule, 
										const ResolveMapSPtr& ResolveMapPtr, const FInitialShape& InitialShape, prt::Cache* Cache)
{
	TArray<AttributeMapBuilderUPtr> AttributeMapBuilders;
	AttributeMapBuilders.Add(AttributeMapBuilderUPtr(prt::AttributeMapBuilder::create()));
	UnrealCallbacks UnrealCallbacks(AttributeMapBuilders);

	InitialShapeBuilderUPtr InitialShapeBuilder(prt::InitialShapeBuilder::create());

	SetInitialShapeGeometry(InitialShapeBuilder, InitialShape);

	AttributeMapUPtr Attributes = Vitruvio::CreateAttributeMap(InitialShape.Attributes);
	InitialShapeBuilder->setAttributes(RuleFile.c_str(), StartRule.c_str(), InitialShape.RandomSeed, L"", Attributes.get(), ResolveMapPtr.get());

	const InitialShapeUPtr Shape(InitialShapeBuilder->createInitialShapeAndReset());
	const InitialShapeNOPtrVector InitialShapes = {Shape.get()};

	const std::vector<const wchar_t*> EncoderIds = {ATTRIBUTE_EVAL_ENCODER_ID};
	const AttributeMapUPtr AttributeEncodeOptions = prtu::createValidatedOptions(ATTRIBUTE_EVAL_ENCODER_ID);
	const AttributeMapNOPtrVector EncoderOptions = {AttributeEncodeOptions.get()};

	generate(InitialShapes.data(), InitialShapes.size(), nullptr, EncoderIds.data(), EncoderIds.size(), EncoderOptions.data(), &UnrealCallbacks,
				  Cache, nullptr);

	return AttributeMapUPtr(AttributeMapBuilders[0]->createAttributeMap());
}

TArray<int64> GetInitialShapeIndices(const TArray<FInitialShape>& InitialShapes)
{
	TArray<int64> Indices;
	for (const FInitialShape& InitialShape : InitialShapes)
	{
		Indices.Add(InitialShape.InitialShapeIndex);
	}
	return Indices;
}

void CleanupTempRpkFolder()
{
	FString TempDir(WCHAR_TO_TCHAR(prtu::temp_directory_path().c_str()));
	const FString RpkUnpackFolder = FPaths::Combine(TempDir, TEXT("PRT"), TEXT("UnrealGeometryEncoder"));
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	PlatformFile.DeleteDirectoryRecursively(*RpkUnpackFolder);
}

FString GetPlatformName()
{
#if PLATFORM_64BITS && PLATFORM_WINDOWS
	return "Win64";
#elif PLATFORM_MAC
	return "Mac";
#else
	return "Unknown";
#endif
}

FString GetPrtThirdPartyPath()
{
	const FString BaseDir = FPaths::ConvertRelativePathToFull(IPluginManager::Get().FindPlugin("Vitruvio")->GetBaseDir());
	const FString BinariesPath = FPaths::Combine(*BaseDir, TEXT("Source"), TEXT("ThirdParty"), TEXT("PRT"));
	return BinariesPath;
}

FString GetEncoderExtensionPath()
{
	const FString BaseDir = FPaths::ConvertRelativePathToFull(IPluginManager::Get().FindPlugin("Vitruvio")->GetBaseDir());
	const FString BinariesPath = FPaths::Combine(*BaseDir, TEXT("Source"), TEXT("ThirdParty"), TEXT("UnrealGeometryEncoderLib"), TEXT("lib"),
												 GetPlatformName(), TEXT("Release"));
	return BinariesPath;
}

FString GetPrtLibDir()
{
	const FString BaseDir = GetPrtThirdPartyPath();
	const FString LibDir = FPaths::Combine(*BaseDir, TEXT("lib"), GetPlatformName(), TEXT("Release"));
	return LibDir;
}

FString GetPrtBinDir()
{
	const FString BaseDir = GetPrtThirdPartyPath();
	const FString BinDir = FPaths::Combine(*BaseDir, TEXT("bin"), GetPlatformName(), TEXT("Release"));
	return BinDir;
}

FString GetPrtDllPath()
{
	const FString BaseDir = GetPrtBinDir();
	return FPaths::Combine(*BaseDir, TEXT("com.esri.prt.core.dll"));
}

} // namespace

void VitruvioModule::InitializePrt()
{
	const FString PrtLibPath = GetPrtDllPath();
	const FString PrtBinDir = GetPrtBinDir();
	const FString PrtLibDir = GetPrtLibDir();

	FPlatformProcess::AddDllDirectory(*PrtBinDir);
	FPlatformProcess::AddDllDirectory(*PrtLibDir);
	PrtDllHandle = FPlatformProcess::GetDllHandle(*PrtLibPath);

	TArray<wchar_t*> PRTPluginsPaths;
	const FString EncoderExtensionPath = GetEncoderExtensionPath();
	const FString PrtExtensionPaths = GetPrtLibDir();
	PRTPluginsPaths.Add(const_cast<wchar_t*>(TCHAR_TO_WCHAR(*EncoderExtensionPath)));
	PRTPluginsPaths.Add(const_cast<wchar_t*>(TCHAR_TO_WCHAR(*PrtExtensionPaths)));

	LogHandler = MakeUnique<UnrealLogHandler>();
	prt::addLogHandler(LogHandler.Get());

	prt::Status Status;
	PrtLibrary = prt::init(PRTPluginsPaths.GetData(), PRTPluginsPaths.Num(), prt::LogLevel::LOG_TRACE, &Status);
	Initialized = Status == prt::STATUS_OK;

	PrtCache.reset(prt::CacheObject::create(prt::CacheObject::CACHE_TYPE_DEFAULT));

	const FString TempDir(WCHAR_TO_TCHAR(prtu::temp_directory_path().c_str()));
	RpkFolder = FPaths::CreateTempFilename(*TempDir, TEXT("Vitruvio_"), TEXT(""));

	OcclusionSet.reset(prt::OcclusionSet::create());
}

void VitruvioModule::StartupModule()
{
	// During cooking we do not start Vitruvio
	if (IsRunningCommandlet())
	{
		return;
	}

	InitializePrt();

	OnWorldCleanupHandle = FWorldDelegates::OnWorldCleanup.AddRaw(this, &VitruvioModule::OnWorldCleanup);
}

void VitruvioModule::ShutdownModule()
{
	FWorldDelegates::OnWorldCleanup.Remove(OnWorldCleanupHandle);

	if (!Initialized)
	{
		return;
	}

	Initialized = false;

	UE_LOG(LogUnrealPrt, Display,
		   TEXT("Shutting down Vitruvio. Waiting for ongoing generate calls (%d), RPK loading tasks (%d) and attribute loading tasks (%d)"),
		   GenerateCallsCounter.GetValue(), RpkLoadingTasksCounter.GetValue(), LoadAttributesCounter.GetValue())

	// Wait until no more PRT calls are ongoing
	FGenericPlatformProcess::ConditionalSleep(
		[this]() { return GenerateCallsCounter.GetValue() == 0 && RpkLoadingTasksCounter.GetValue() == 0 && LoadAttributesCounter.GetValue() == 0; },
		0); // Yield to other threads

	UE_LOG(LogUnrealPrt, Display, TEXT("PRT calls finished. Shutting down."))

	if (PrtDllHandle)
	{
		FPlatformProcess::FreeDllHandle(PrtDllHandle);
		PrtDllHandle = nullptr;
	}
	if (PrtLibrary)
	{
		PrtLibrary->destroy();
	}

	CleanupTempRpkFolder();

	

	UE_LOG(LogUnrealPrt, Display, TEXT("Shutdown complete"))
}

Vitruvio::FTextureData VitruvioModule::DecodeTexture(UObject* Outer, const FString& Path, const FString& Key) const
{
	const prt::AttributeMap* TextureMetadataAttributeMap = prt::createTextureMetadata(*Path, PrtCache.get());
	Vitruvio::FTextureMetadata TextureMetadata = Vitruvio::ParseTextureMetadata(TextureMetadataAttributeMap);

	size_t BufferSize = TextureMetadata.Width * TextureMetadata.Height * TextureMetadata.Bands * TextureMetadata.BytesPerBand;
	auto Buffer = std::make_unique<uint8_t[]>(BufferSize);

	prt::getTexturePixeldata(*Path, Buffer.get(), BufferSize, PrtCache.get());

	return Vitruvio::DecodeTexture(Outer, Key, Path, TextureMetadata, std::move(Buffer), BufferSize);
}

FBatchGenerateResult VitruvioModule::BatchGenerateAsync(TArray<FInitialShape> InitialShapes, bool bEnableOcclusionQueries, TArray<FInitialShape> OccluderOnlyShapes) const
{
    const FBatchGenerateResult::FTokenPtr Token = MakeShared<FGenerateToken>();
    	
	CHECK_PRT_INITIALIZED_ASYNC(FBatchGenerateResult, Token)

	FBatchGenerateResult::FFutureType ResultFuture = Async(EAsyncExecution::Thread, [this, Token, bEnableOcclusionQueries, InitialShapes = MoveTemp(InitialShapes), OccluderOnlyShapes = MoveTemp(OccluderOnlyShapes)]() mutable {
		FGenerateResultDescription Result = BatchGenerate(MoveTemp(InitialShapes), bEnableOcclusionQueries, MoveTemp(OccluderOnlyShapes));
		return FBatchGenerateResult::ResultType { Token, MoveTemp(Result) };
	});

	return FBatchGenerateResult { MoveTemp(ResultFuture), Token };
}

FGenerateResultDescription VitruvioModule::BatchGenerate(TArray<FInitialShape> InitialShapes, bool bEnableOcclusionQueries, TArray<FInitialShape> OccluderOnlyShapes) const
{
	if (InitialShapes.IsEmpty())
	{
		return {};
	}
	
	CHECK_PRT_INITIALIZED()

	GenerateCallsCounter.Add(InitialShapes.Num());

	const int NumInitialShapes = InitialShapes.Num();
	TArray<int64> InitialShapeIndices = GetInitialShapeIndices(InitialShapes);
	TArray<int64> OccluderShapeIndices = GetInitialShapeIndices(OccluderOnlyShapes);
	
	TMap<URulePackage*, TArray<FInitialShape>> RulePackages;

	auto ExtractRulePackage = [&RulePackages](TArray<FInitialShape> InitialShapes)
	{
		for (FInitialShape& InitialShape : InitialShapes)
		{
			RulePackages.FindOrAdd(InitialShape.RulePackage).Add(MoveTemp(InitialShape));
		}
	};

	ExtractRulePackage(MoveTemp(InitialShapes));
	ExtractRulePackage(MoveTemp(OccluderOnlyShapes));

	TArray<TTuple<TFuture<ResolveMapSPtr>, TArray<FInitialShape>>> ResolveMapFutures;
	for (auto& [RulePackage, InitialShapesByRpk] : RulePackages)
	{
		ResolveMapFutures.Add(MakeTuple(LoadResolveMapAsync(RulePackage), MoveTemp(InitialShapesByRpk)));
	}

	TArray<TTuple<FStartRuleInfo, TArray<FInitialShape>>> RuleInfoInitialShapes;
	for (auto& [ResolveMapFuture, InitialShapesByRpk] : ResolveMapFutures)
	{
		const ResolveMapSPtr ResolveMap = ResolveMapFuture.Get();

		const std::wstring RuleFile = ResolveMap->findCGBKey();
		const wchar_t* RuleFileUri = ResolveMap->getString(RuleFile.c_str());

		const RuleFileInfoPtr RuleFileInfo = prt_make_shared<const prt::RuleFileInfo>(prt::createRuleFileInfo(RuleFileUri));
		const std::wstring StartRule = prtu::detectStartRule(RuleFileInfo);
		
		FStartRuleInfo StartRuleInfo { ResolveMap, RuleFile.c_str(), StartRule.c_str(), RuleFileInfo };

		RuleInfoInitialShapes.Add(MakeTuple(StartRuleInfo, MoveTemp(InitialShapesByRpk)));
	}
	
	auto ForeachInitialShape = [&RuleInfoInitialShapes](bool bOccluders, bool bNonOccluders, auto Fun)
	{
		int InitialShapeIndex = 0;
		for (auto& [StartRuleInfo, InitialShapesByRpk] : RuleInfoInitialShapes)
		{
			for (const FInitialShape& InitialShape : InitialShapesByRpk)
			{
				if ((bOccluders && InitialShape.bOccluderOnly) || (bNonOccluders && !InitialShape.bOccluderOnly))
				{
					Fun(InitialShapeIndex, InitialShape, StartRuleInfo);

					InitialShapeIndex++;
				}
			}
		}
	};
	
	InitialShapeUPtrVector InitialShapeUPtrs;
	AttributeMapVector AttributeMaps;
	
	TMap<int64, const prt::InitialShape*> InitialShapeByIndex;
	TMap<const prt::InitialShape*, int64> IndexByInitialShape;

	ForeachInitialShape(false, true, [&](int32, const FInitialShape& InitialShape, const FStartRuleInfo& StartRuleInfo)
	{
		InitialShapeBuilderUPtr InitialShapeBuilder(prt::InitialShapeBuilder::create());
		SetInitialShapeGeometry(InitialShapeBuilder, InitialShape);

		AttributeMapUPtr Attributes = Vitruvio::CreateAttributeMap(InitialShape.Attributes);
		InitialShapeBuilder->setAttributes(*StartRuleInfo.RuleFile, *StartRuleInfo.StartRule, InitialShape.RandomSeed, L"",
			Attributes.get(), StartRuleInfo.ResolveMap.get());
		InitialShapeUPtr Shape(InitialShapeBuilder->createInitialShape());

		InitialShapeByIndex.Add(InitialShape.InitialShapeIndex, Shape.get());
		IndexByInitialShape.Add(Shape.get(), InitialShape.InitialShapeIndex);
		InitialShapeUPtrs.push_back(std::move(Shape));
		AttributeMaps.push_back(std::move(Attributes));
	});

	TArray<FAttributeMapPtr> EvaluatedAttributes;
	
	// Evaluate attributes
	{
		TArray<AttributeMapBuilderUPtr> EvaluateAttributeMapBuilders;
		for (int32 InitialShapeIndex = 0; InitialShapeIndex < NumInitialShapes; ++InitialShapeIndex)
		{
			EvaluateAttributeMapBuilders.Add(AttributeMapBuilderUPtr(prt::AttributeMapBuilder::create()));
		}
		TSharedPtr<UnrealCallbacks> OutputHandler(new UnrealCallbacks(EvaluateAttributeMapBuilders));
		
		const std::vector EncoderIds = { ATTRIBUTE_EVAL_ENCODER_ID };
		const AttributeMapUPtr AttributeEncodeOptions = prtu::createValidatedOptions(ATTRIBUTE_EVAL_ENCODER_ID);
		const AttributeMapNOPtrVector EncoderOptions = {AttributeEncodeOptions.get()};

		AttributeMapBuilderUPtr GenerateOptionsBuilder(prt::AttributeMapBuilder::create());
		GenerateOptionsBuilder->setInt(L"numberWorkerThreads", FPlatformMisc::NumberOfCores());
		const AttributeMapUPtr GenerateOptions(GenerateOptionsBuilder->createAttributeMapAndReset());

		TArray<const prt::InitialShape*> InitialShapesPtrs;
		InitialShapeByIndex.GenerateValueArray(InitialShapesPtrs);
		prt::Status GenerateStatus = generate(InitialShapesPtrs.GetData(), InitialShapesPtrs.Num(), nullptr, EncoderIds.data(),
			EncoderIds.size(), EncoderOptions.data(), OutputHandler.Get(),
					  PrtCache.get(), nullptr, GenerateOptions.get());

		if (GenerateStatus != prt::STATUS_OK)
		{
			GenerateCallsCounter.Subtract(InitialShapes.Num());
			UE_LOG(LogUnrealPrt, Error, TEXT("PRT generate failed: %hs"), prt::getStatusDescription(GenerateStatus))
			return {};
		}
		
		ForeachInitialShape(false, true, [&](int32 Index, const FInitialShape&, const FStartRuleInfo& StartRuleInfo)
		{
			const FAttributeMapPtr AttributeMap = MakeShared<FAttributeMap>(
				AttributeMapUPtr(EvaluateAttributeMapBuilders[Index]->createAttributeMapAndReset()),
				StartRuleInfo.RuleFileInfo);
			EvaluatedAttributes.Add(AttributeMap);
		});
	}

	// Generate Occluders
	TArray<AttributeMapBuilderUPtr> GenerateAttributeMapBuilders;
	TSharedPtr<UnrealCallbacks> GenerateOutputHandler(new UnrealCallbacks(GenerateAttributeMapBuilders));
	
	auto UnlockOcclusionLock = [bEnableOcclusionQueries](FCriticalSection& OcclusionLock)
	{
		if (bEnableOcclusionQueries)
		{
			OcclusionLock.Unlock();
		}
	};

	if (bEnableOcclusionQueries)
	{
		OcclusionLock.Lock();
					
		InvalidateOcclusionHandles(InitialShapeIndices);

		ForeachInitialShape(true, false, [&]
			(int32, const FInitialShape& InitialShape, const FStartRuleInfo& StartRuleInfo)
		{
			InitialShapeBuilderUPtr InitialShapeBuilder(prt::InitialShapeBuilder::create());
			AttributeMapBuilderUPtr AttributeMapBuilder(prt::AttributeMapBuilder::create());
			SetInitialShapeGeometry(InitialShapeBuilder, InitialShape);
			
			AttributeMapUPtr Attributes = Vitruvio::CreateAttributeMap(InitialShape.Attributes);
			InitialShapeBuilder->setAttributes(*StartRuleInfo.RuleFile, *StartRuleInfo.StartRule, InitialShape.RandomSeed, L"",
				Attributes.get(), StartRuleInfo.ResolveMap.get());
			
			InitialShapeUPtr Shape(InitialShapeBuilder->createInitialShape());

			InitialShapeByIndex.Add(InitialShape.InitialShapeIndex, Shape.get());
			IndexByInitialShape.Add(Shape.get(), InitialShape.InitialShapeIndex);
			InitialShapeUPtrs.push_back(std::move(Shape));
			AttributeMaps.push_back(std::move(Attributes));
		});
			
		if (!InitialShapeByIndex.IsEmpty())
		{
			TArray<prt::OcclusionSet::Handle> NewOcclusionHandles;
			TArray<const prt::InitialShape*> OcclusionShapesArray;

			for (const auto& [InitialShape, InitialShapeIndex] : IndexByInitialShape)
			{
				if (!OcclusionHandleCache.Contains(InitialShapeIndex))
				{
					OcclusionShapesArray.Add(InitialShape);
				}
			}
			
			NewOcclusionHandles.SetNum(OcclusionShapesArray.Num());
	
			const prt::Status GenerateOccludersStatus =  generateOccluders(OcclusionShapesArray.GetData(), OcclusionShapesArray.Num(), NewOcclusionHandles.GetData(), nullptr, 0,
	nullptr, GenerateOutputHandler.Get(), PrtCache.get(), OcclusionSet.get());

			if (GenerateOccludersStatus != prt::STATUS_OK)
			{
				GenerateCallsCounter.Decrement();

				UnlockOcclusionLock(OcclusionLock);
				
				UE_LOG(LogUnrealPrt, Error, TEXT("PRT generateOccluders failed: %hs"), prt::getStatusDescription(GenerateOccludersStatus))
				return {};
			}

			for (int32 OcclusionShapeIndex = 0; OcclusionShapeIndex < OcclusionShapesArray.Num(); ++OcclusionShapeIndex)
			{
				const prt::InitialShape* InitialShape = OcclusionShapesArray[OcclusionShapeIndex];
				prt::OcclusionSet::Handle OcclusionHandle = NewOcclusionHandles[OcclusionShapeIndex];
				int64 OcclusionInitialShapeIndex = IndexByInitialShape[InitialShape];

				OcclusionHandleCache.Add(OcclusionInitialShapeIndex, OcclusionHandle);
			}
		}
	}

	// Generate
	AttributeMapBuilderUPtr AttributeMapBuilder(prt::AttributeMapBuilder::create());

	const std::vector UnrealEncoderIds = { UNREAL_GEOMETRY_ENCODER_ID };
	const AttributeMapUPtr UnrealEncoderOptions(prtu::createValidatedOptions(UNREAL_GEOMETRY_ENCODER_ID));
	const AttributeMapNOPtrVector GenerateEncoderOptions = {UnrealEncoderOptions.get()};

	AttributeMapBuilderUPtr GenerateOptionsBuilder(prt::AttributeMapBuilder::create());
	GenerateOptionsBuilder->setInt(L"numberWorkerThreads", FPlatformMisc::NumberOfCores());
	const AttributeMapUPtr GenerateOptions(GenerateOptionsBuilder->createAttributeMapAndReset());

	prt::OcclusionSet* OcclusionSetPtr = bEnableOcclusionQueries ? OcclusionSet.get() : nullptr;
	TArray<const prt::InitialShape*> InitialShapePtrs;
	TArray<prt::OcclusionSet::Handle> OcclusionHandles;

	ForeachInitialShape(false,  true, [&](int32, const FInitialShape& InitialShape, const FStartRuleInfo& StartRuleInfo)
	{
		const prt::InitialShape* InitialShapePtr = InitialShapeByIndex[InitialShape.InitialShapeIndex];
		InitialShapePtrs.Add(InitialShapePtr);

		if (bEnableOcclusionQueries)
		{
			OcclusionHandles.Add(OcclusionHandleCache[InitialShape.InitialShapeIndex]);
		}
	});

	if (bEnableOcclusionQueries)
	{
		ForeachInitialShape(true, false, [&](int32, const FInitialShape& InitialShape, const FStartRuleInfo& StartRuleInfo)
		{
			OcclusionHandles.Add(OcclusionHandleCache[InitialShape.InitialShapeIndex]);
		});
	}

	prt::OcclusionSet::Handle* OcclusionHandlesPtr = bEnableOcclusionQueries ? OcclusionHandles.GetData() : nullptr;
	
	prt::Status GenerateStatus = generate(InitialShapePtrs.GetData(), InitialShapePtrs.Num(), OcclusionHandlesPtr,
		UnrealEncoderIds.data(), UnrealEncoderIds.size(), GenerateEncoderOptions.data(), GenerateOutputHandler.Get(),
		PrtCache.get(), OcclusionSetPtr, GenerateOptions.get());

	if (GenerateStatus != prt::STATUS_OK)
	{
		GenerateCallsCounter.Subtract(NumInitialShapes);
		UnlockOcclusionLock(OcclusionLock);
		
		UE_LOG(LogUnrealPrt, Error, TEXT("PRT generate failed: %hs"), prt::getStatusDescription(GenerateStatus))
		return {};
	}

	CHECK_PRT_INITIALIZED()

	GenerateCallsCounter.Subtract(NumInitialShapes);
	UnlockOcclusionLock(OcclusionLock);

	NotifyGenerateCompleted();
    
    return FGenerateResultDescription { GenerateOutputHandler->GetGeneratedModel(), GenerateOutputHandler->GetInstances(),
    	GenerateOutputHandler->GetInstanceMeshes(), GenerateOutputHandler->GetInstanceNames(), {}, EvaluatedAttributes };
}

FAttributeMapsResult VitruvioModule::BatchEvaluateRuleAttributesAsync(TArray<FInitialShape> InitialShapes) const
{
	FAttributeMapsResult::FTokenPtr InvalidationToken = MakeShared<FEvalAttributesToken>();

	CHECK_PRT_INITIALIZED_ASYNC(FAttributeMapsResult, InvalidationToken)

	FAttributeMapsResult::FFutureType AttributeMapPtrFuture = Async(EAsyncExecution::Thread, [this, InvalidationToken, InitialShapes = MoveTemp(InitialShapes)]() mutable {
		TArray<FAttributeMapPtr> Result = BatchEvaluateRuleAttributes(MoveTemp(InitialShapes));
		return FAttributeMapsResult::ResultType { InvalidationToken, MoveTemp(Result) };
	});

	return {MoveTemp(AttributeMapPtrFuture), InvalidationToken};
}

FGenerateResult VitruvioModule::GenerateAsync(TArray<FInitialShape> InitialShapes) const
{
	const FGenerateResult::FTokenPtr Token = MakeShared<FGenerateToken>();

	CHECK_PRT_INITIALIZED_ASYNC(FGenerateResult, Token)

	FGenerateResult::FFutureType ResultFuture = Async(EAsyncExecution::Thread, [this, Token, InitialShapes = MoveTemp(InitialShapes)]() mutable {
		FGenerateResultDescription Result = Generate(MoveTemp(InitialShapes));
		return FGenerateResult::ResultType{Token, MoveTemp(Result)};
	});

	return FGenerateResult{MoveTemp(ResultFuture), Token};
}

FGenerateResultDescription VitruvioModule::Generate(TArray<FInitialShape> InitialShapes) const
{
	CHECK_PRT_INITIALIZED()

	if (InitialShapes.Num() == 0)
	{
		return {};
	}

	GenerateCallsCounter.Increment();

	const FInitialShape& FirstInitialShape = InitialShapes[0];
	const ResolveMapSPtr ResolveMap = LoadResolveMapAsync(FirstInitialShape.RulePackage).Get();

	const std::wstring RuleFile = ResolveMap->findCGBKey();
	const wchar_t* RuleFileUri = ResolveMap->getString(RuleFile.c_str());

	const RuleFileInfoPtr StartRuleInfo = prt_make_shared<const prt::RuleFileInfo>(prt::createRuleFileInfo(RuleFileUri));
	const std::wstring StartRule = prtu::detectStartRule(StartRuleInfo);

	TArray<AttributeMapBuilderUPtr> AttributeMapBuilders;
	AttributeMapBuilders.Add(AttributeMapBuilderUPtr(prt::AttributeMapBuilder::create()));
	const TSharedPtr<UnrealCallbacks> OutputHandler(new UnrealCallbacks(AttributeMapBuilders, FirstInitialShape.Position));

	const std::vector<const wchar_t*> EncoderIds = {UNREAL_GEOMETRY_ENCODER_ID};
	const AttributeMapUPtr UnrealEncoderOptions(prtu::createValidatedOptions(UNREAL_GEOMETRY_ENCODER_ID));
	const AttributeMapNOPtrVector EncoderOptions = {UnrealEncoderOptions.get()};
	
	AttributeMapVector AttributeMaps;
	InitialShapeNOPtrVector Shapes = {};
	TArray<InitialShapeUPtr> ShapesPointers;

	InitialShapeBuilderUPtr InitialShapeBuilder(prt::InitialShapeBuilder::create());
	for (const FInitialShape& InitialShape : InitialShapes)
	{
		SetInitialShapeGeometry(InitialShapeBuilder, InitialShape);

		AttributeMapUPtr Attributes = Vitruvio::CreateAttributeMap(InitialShape.Attributes);
		InitialShapeBuilder->setAttributes(RuleFile.c_str(), StartRule.c_str(), InitialShape.RandomSeed, L"", Attributes.get(), ResolveMap.get());

		InitialShapeUPtr InitialShapePtr(InitialShapeBuilder->createInitialShapeAndReset());
		Shapes.push_back(InitialShapePtr.get());
		ShapesPointers.Add(MoveTemp(InitialShapePtr));
		AttributeMaps.push_back(MoveTemp(Attributes));
	}

	bool bInterOcclusion = InitialShapes.Num() > 1;
	TArray<prt::OcclusionSet::Handle> OcclusionHandles;
	
	if (bInterOcclusion)
	{
		OcclusionLock.Lock();
	
		TMap<const prt::InitialShape*, int64> OcclusionInitialShapeIndexMap;
		for (int ShapeIndex = 0; ShapeIndex < InitialShapes.Num(); ++ShapeIndex)
		{
			const FInitialShape& InitialShape = InitialShapes[ShapeIndex];

			if (!OcclusionHandleCache.Contains(InitialShape.InitialShapeIndex))
			{
				OcclusionInitialShapeIndexMap.Add(Shapes[ShapeIndex], InitialShape.InitialShapeIndex);
			}
		}

		if (!OcclusionInitialShapeIndexMap.IsEmpty())
		{
			TArray<prt::OcclusionSet::Handle> NewOcclusionHandles;
			NewOcclusionHandles.SetNum(OcclusionInitialShapeIndexMap.Num());

			TArray<const prt::InitialShape*> OcclusionShapesArray;
			OcclusionInitialShapeIndexMap.GenerateKeyArray(OcclusionShapesArray);
	
			const prt::Status GenerateOccludersStatus =  generateOccluders(OcclusionShapesArray.GetData(), OcclusionShapesArray.Num(), NewOcclusionHandles.GetData(), nullptr, 0,
	nullptr, OutputHandler.Get(), PrtCache.get(), OcclusionSet.get());

			if (GenerateOccludersStatus != prt::STATUS_OK)
			{
				GenerateCallsCounter.Decrement();

				OcclusionLock.Unlock();
				UE_LOG(LogUnrealPrt, Error, TEXT("PRT generateOccluders failed: %hs"), prt::getStatusDescription(GenerateOccludersStatus))
				return {};
			}

			for (int32 OcclusionShapeIndex = 0; OcclusionShapeIndex < OcclusionShapesArray.Num(); ++OcclusionShapeIndex)
			{
				const prt::InitialShape* InitialShape = OcclusionShapesArray[OcclusionShapeIndex];
				prt::OcclusionSet::Handle OcclusionHandle = NewOcclusionHandles[OcclusionShapeIndex];
				int64 OcclusionInitialShapeIndex = OcclusionInitialShapeIndexMap[InitialShape];

				OcclusionHandleCache.Add(OcclusionInitialShapeIndex, OcclusionHandle);
			}
		}

		for (const FInitialShape& InitialShape : InitialShapes)
		{
			if (OcclusionHandleCache.Contains(InitialShape.InitialShapeIndex))
			{
				OcclusionHandles.Add(OcclusionHandleCache[InitialShape.InitialShapeIndex]);
			}
		}
	}

	const prt::Status GenerateStatus = generate(Shapes.data(), 1, bInterOcclusion ? OcclusionHandles.GetData() : nullptr, EncoderIds.data(), EncoderIds.size(),
													 EncoderOptions.data(), OutputHandler.Get(), PrtCache.get(), bInterOcclusion ? OcclusionSet.get() : nullptr);

	if (bInterOcclusion)
	{
		OcclusionLock.Unlock();
	}
	
	GenerateCallsCounter.Decrement();
	if (GenerateStatus != prt::STATUS_OK)
	{
		UE_LOG(LogUnrealPrt, Error, TEXT("PRT generate failed: %hs"), prt::getStatusDescription(GenerateStatus))
		return {};
	}

	CHECK_PRT_INITIALIZED()
	
	NotifyGenerateCompleted();

	return FGenerateResultDescription{ OutputHandler->GetGeneratedModel(), OutputHandler->GetInstances(), OutputHandler->GetInstanceMeshes(),
									  OutputHandler->GetInstanceNames(), OutputHandler->GetReports() };
}

FAttributeMapResult VitruvioModule::EvaluateRuleAttributesAsync(FInitialShape InitialShape) const
{
	FAttributeMapResult::FTokenPtr InvalidationToken = MakeShared<FEvalAttributesToken>();

	CHECK_PRT_INITIALIZED_ASYNC(FAttributeMapResult, InvalidationToken)

	LoadAttributesCounter.Increment();

	FAttributeMapResult::FFutureType AttributeMapPtrFuture = Async(EAsyncExecution::Thread, [this, InvalidationToken, InitialShape = MoveTemp(InitialShape)]() mutable {
		const ResolveMapSPtr ResolveMap = LoadResolveMapAsync(InitialShape.RulePackage).Get();

		const std::wstring RuleFile = ResolveMap->findCGBKey();
		const wchar_t* RuleFileUri = ResolveMap->getString(RuleFile.c_str());

		const RuleFileInfoPtr StartRuleInfo = prt_make_shared<const prt::RuleFileInfo>(prt::createRuleFileInfo(RuleFileUri));
		const std::wstring StartRule = prtu::detectStartRule(StartRuleInfo);

		prt::Status InfoStatus;
		RuleFileInfoUPtr RuleInfo(prt::createRuleFileInfo(RuleFileUri, PrtCache.get(), &InfoStatus));
		if (!RuleInfo || InfoStatus != prt::STATUS_OK)
		{
			UE_LOG(LogUnrealPrt, Error, TEXT("could not get rule file info from rule file %s"), RuleFileUri)
			return FAttributeMapResult::ResultType{
				InvalidationToken,
				nullptr,
			};
		}

		AttributeMapUPtr DefaultAttributeMap(EvaluateRuleAttributes(RuleFile.c_str(),
			StartRule.c_str(), ResolveMap, InitialShape, PrtCache.get()));

		LoadAttributesCounter.Decrement();

		if (!Initialized)
		{
			return FAttributeMapResult::ResultType{InvalidationToken, nullptr};
		}

		const TSharedPtr<FAttributeMap> AttributeMap = MakeShared<FAttributeMap>(std::move(DefaultAttributeMap), std::move(RuleInfo));
		return FAttributeMapResult::ResultType{InvalidationToken, AttributeMap};
	});

	return {MoveTemp(AttributeMapPtrFuture), InvalidationToken};
}

TArray<FAttributeMapPtr> VitruvioModule::BatchEvaluateRuleAttributes(TArray<FInitialShape> InitialShapes) const
{
	CHECK_PRT_INITIALIZED()
	
	LoadAttributesCounter.Add(InitialShapes.Num());

	TMap<URulePackage*, TArray<FInitialShape>> RulePackages;
	for (FInitialShape& InitialShape : InitialShapes)
	{
		RulePackages.FindOrAdd(InitialShape.RulePackage).Add(MoveTemp(InitialShape));
	}

	TArray<TTuple<TFuture<ResolveMapSPtr>, TArray<FInitialShape>>> ResolveMapFutures;
	for (auto& [RulePackage, InitialShapesByRpk] : RulePackages)
	{
		ResolveMapFutures.Add(MakeTuple(LoadResolveMapAsync(RulePackage), MoveTemp(InitialShapesByRpk)));
	}

	TArray<TTuple<FStartRuleInfo, TArray<FInitialShape>>> RuleInfoInitialShapes;
	for (auto& [ResolveMapFuture, InitialShapesByRpk] : ResolveMapFutures)
	{
		const ResolveMapSPtr ResolveMap = ResolveMapFuture.Get();

		const std::wstring RuleFile = ResolveMap->findCGBKey();
		const wchar_t* RuleFileUri = ResolveMap->getString(RuleFile.c_str());

		const RuleFileInfoPtr RuleFileInfo = prt_make_shared<const prt::RuleFileInfo>(prt::createRuleFileInfo(RuleFileUri));
		const std::wstring StartRule = prtu::detectStartRule(RuleFileInfo);
		
		FStartRuleInfo StartRuleInfo { ResolveMap, RuleFile.c_str(), StartRule.c_str(), RuleFileInfo };

		RuleInfoInitialShapes.Add(MakeTuple(StartRuleInfo, MoveTemp(InitialShapesByRpk)));
	}
	
	auto ForeachInitialShape = [&](auto Fun)
	{
		int InitialShapeIndex = 0;
		for (auto& [StartRuleInfo, InitialShapesByRpk] : RuleInfoInitialShapes)
		{
			for (const FInitialShape& InitialShape : InitialShapesByRpk)
			{
				Fun(InitialShapeIndex, InitialShape, StartRuleInfo);

				InitialShapeIndex++;
			}
		}
	};
	
	TArray<InitialShapeBuilderUPtr> InitialShapeBuilders;
	InitialShapeUPtrVector InitialShapeUPtrs;
	InitialShapeNOPtrVector InitialShapePtrs;
	AttributeMapVector AttributeMaps;
	
	ForeachInitialShape([&](int32 InitialShapeIndex, const FInitialShape& InitialShape, const FStartRuleInfo& StartRuleInfo)
	{
		InitialShapeBuilderUPtr InitialShapeBuilder(prt::InitialShapeBuilder::create());
		SetInitialShapeGeometry(InitialShapeBuilder, InitialShape);

		AttributeMapUPtr Attributes = Vitruvio::CreateAttributeMap(InitialShape.Attributes);
		InitialShapeBuilder->setAttributes(*StartRuleInfo.RuleFile, *StartRuleInfo.StartRule, InitialShape.RandomSeed, L"",
			Attributes.get(), StartRuleInfo.ResolveMap.get());
		InitialShapeUPtr Shape(InitialShapeBuilder->createInitialShape());
		InitialShapePtrs.push_back(Shape.get());
		InitialShapeUPtrs.push_back(std::move(Shape));
		AttributeMaps.push_back(std::move(Attributes));
		InitialShapeBuilders.Add(MoveTemp(InitialShapeBuilder));
	});

	TArray<FAttributeMapPtr> EvaluatedAttributes;
	
	// Evaluate attributes
	{
		TArray<AttributeMapBuilderUPtr> EvaluateAttributeMapBuilders;
		for (int32 InitialShapeIndex = 0; InitialShapeIndex < InitialShapes.Num(); ++InitialShapeIndex)
		{
			EvaluateAttributeMapBuilders.Add(AttributeMapBuilderUPtr(prt::AttributeMapBuilder::create()));
		}
		TSharedPtr<UnrealCallbacks> OutputHandler(new UnrealCallbacks(EvaluateAttributeMapBuilders));
		
		const std::vector EncoderIds = { ATTRIBUTE_EVAL_ENCODER_ID };
		const AttributeMapUPtr AttributeEncodeOptions = prtu::createValidatedOptions(ATTRIBUTE_EVAL_ENCODER_ID);
		const AttributeMapNOPtrVector EncoderOptions = {AttributeEncodeOptions.get()};

		AttributeMapBuilderUPtr GenerateOptionsBuilder(prt::AttributeMapBuilder::create());
		GenerateOptionsBuilder->setInt(L"numberWorkerThreads", FPlatformMisc::NumberOfCores());
		const AttributeMapUPtr GenerateOptions(GenerateOptionsBuilder->createAttributeMapAndReset());

		prt::Status GenerateStatus = generate(InitialShapePtrs.data(), InitialShapePtrs.size(), nullptr, EncoderIds.data(),
			EncoderIds.size(), EncoderOptions.data(), OutputHandler.Get(),
					  PrtCache.get(), nullptr, GenerateOptions.get());

		if (GenerateStatus != prt::STATUS_OK)
		{
			LoadAttributesCounter.Subtract(InitialShapes.Num());
			UE_LOG(LogUnrealPrt, Error, TEXT("PRT generate failed: %hs"), prt::getStatusDescription(GenerateStatus))
			return {};
		}
		
		ForeachInitialShape([&](int32 InitialShapeIndex, const FInitialShape& InitialShape, const FStartRuleInfo& StartRuleInfo)
		{
			const FAttributeMapPtr AttributeMap = MakeShared<FAttributeMap>(
				AttributeMapUPtr(EvaluateAttributeMapBuilders[InitialShapeIndex]->createAttributeMapAndReset()),
				StartRuleInfo.RuleFileInfo);
			EvaluatedAttributes.Add(AttributeMap);
		});
	}

	LoadAttributesCounter.Subtract(InitialShapes.Num());

	return EvaluatedAttributes;
}


void VitruvioModule::EvictFromResolveMapCache(URulePackage* RulePackage)
{
	const TLazyObjectPtr<URulePackage> LazyRulePackagePtr(RulePackage);
	FScopeLock Lock(&LoadResolveMapLock);
	ResolveMapCache.Remove(LazyRulePackagePtr);
	PrtCache->flushAll();
}

void VitruvioModule::RegisterMesh(UStaticMesh* StaticMesh)
{
	FScopeLock Lock(&RegisterMeshLock);
	RegisteredMeshes.Add(StaticMesh);
}

void VitruvioModule::UnregisterMesh(UStaticMesh* StaticMesh)
{
	FScopeLock Lock(&RegisterMeshLock);
	RegisteredMeshes.Remove(StaticMesh);
}

void VitruvioModule::InvalidateOcclusionHandle(int64 InitialShapeIndex)
{
	FScopeLock Lock(&OcclusionLock);

	if (OcclusionHandleCache.Contains(InitialShapeIndex))
	{
		TArray InvalidateHandles = { OcclusionHandleCache[InitialShapeIndex] };
		OcclusionSet->dispose(InvalidateHandles.GetData(), InvalidateHandles.Num());
		OcclusionHandleCache.Remove(InitialShapeIndex);
	}
}

void VitruvioModule::InvalidateOcclusionHandles(const TArray<int64>& InitialShapeIndices) const
{
	FScopeLock Lock(&OcclusionLock);

	TArray<prt::OcclusionSet::Handle> InvalidateHandles;
	for (int32 Index = 0; Index < InitialShapeIndices.Num(); ++Index)
	{
		int64 InitialShapeIndex = InitialShapeIndices[Index];
		if (OcclusionHandleCache.Contains(InitialShapeIndex))
		{
			InvalidateHandles.Add(OcclusionHandleCache[InitialShapeIndex]);
		}

		OcclusionHandleCache.Remove(InitialShapeIndex);
	}

	OcclusionSet->dispose(InvalidateHandles.GetData(), InvalidateHandles.Num());
}

void VitruvioModule::OnWorldCleanup(UWorld* World, bool bSessionEnded, bool bCleanupResources)
{
	if (!Initialized || !World || !World->IsGameWorld())
	{
		return;
	}

	// Cached meshes keep collision objects in the old world and must be rebuilt after it is destroyed.
	MeshCache.Empty();
	InvalidateAllOcclusionHandles();
}

void VitruvioModule::InvalidateAllOcclusionHandles()
{
	FScopeLock Lock(&OcclusionLock);

	OcclusionHandleCache.Empty();
	OcclusionSet.reset(prt::OcclusionSet::create());
}

void VitruvioModule::NotifyGenerateCompleted() const
{
	const int GenerateCalls = GenerateCallsCounter.GetValue();
	
	AsyncTask(ENamedThreads::GameThread, [this, GenerateCalls]() {
		if (!Initialized)
		{
			return;
		}

		OnGenerateCompleted.Broadcast(GenerateCalls);

		if (GenerateCalls == 0)
		{
			TArray<FLogMessage> Messages = LogHandler->PopMessages();

			int Warnings = 0;
			int Errors = 0;

			for (const FLogMessage& Message : Messages)
			{
				if (Message.Level == prt::LOG_WARNING)
				{
					Warnings++;
				}
				else if (Message.Level == prt::LOG_ERROR || Message.Level == prt::LOG_FATAL)
				{
					Errors++;
				}
			}

			OnAllGenerateCompleted.Broadcast(Warnings, Errors);
		}
	});
}

TFuture<ResolveMapSPtr> VitruvioModule::LoadResolveMapAsync(URulePackage* const RulePackage) const
{
	TPromise<ResolveMapSPtr> Promise;
	TFuture<ResolveMapSPtr> Future = Promise.GetFuture();

	if (!Initialized)
	{
		Promise.SetValue({});
		return Future;
	}

	const TLazyObjectPtr<URulePackage> LazyRulePackagePtr(RulePackage);

	// Check if has already been cached
	{
		FScopeLock Lock(&LoadResolveMapLock);
		const auto CachedResolveMap = ResolveMapCache.Find(LazyRulePackagePtr);
		if (CachedResolveMap)
		{
			Promise.SetValue(*CachedResolveMap);
			return Future;
		}
	}

	// Check if a task is already running for loading the specified resolve map
	FGraphEventRef* ScheduledTaskEvent;
	{
		FScopeLock Lock(&LoadResolveMapLock);
		ScheduledTaskEvent = ResolveMapEventGraphRefCache.Find(LazyRulePackagePtr);
	}
	if (ScheduledTaskEvent)
	{
		// Add task which only fetches the result from the cache once the actual loading has finished
		FGraphEventArray Prerequisites;
		Prerequisites.Add(*ScheduledTaskEvent);
		TGraphTask<TAsyncGraphTask<ResolveMapSPtr>>::CreateTask(&Prerequisites)
			.ConstructAndDispatchWhenReady(
				[this, LazyRulePackagePtr]() {
					FScopeLock Lock(&LoadResolveMapLock);
					return ResolveMapCache[LazyRulePackagePtr];
				},
				MoveTemp(Promise), ENamedThreads::AnyThread);
	}
	else
	{
		RpkLoadingTasksCounter.Increment();

		FGraphEventRef LoadTask;
		{
			FScopeLock Lock(&LoadResolveMapLock);
			// Task which does the actual resolve map loading which might take a long time
			LoadTask = TGraphTask<FLoadResolveMapTask>::CreateTask().ConstructAndDispatchWhenReady(MoveTemp(Promise), RpkFolder, LazyRulePackagePtr,
																								   ResolveMapCache, LoadResolveMapLock);
			ResolveMapEventGraphRefCache.Add(LazyRulePackagePtr, LoadTask);
		}

		// Task which removes the event from the cache once finished
		FFunctionGraphTask::CreateAndDispatchWhenReady(
			[this, LazyRulePackagePtr]() {
				FScopeLock Lock(&LoadResolveMapLock);
				RpkLoadingTasksCounter.Decrement();
				ResolveMapEventGraphRefCache.Remove(LazyRulePackagePtr);
			},
			TStatId(), LoadTask, ENamedThreads::AnyThread);
	}

	return Future;
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(VitruvioModule, Vitruvio)
