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

#include "RuleAttributes.h"
#include "RulePackage.h"
#include "VitruvioModule.h"

#include "CoreMinimal.h"
#include "GeneratedModelHISMComponent.h"
#include "GeneratedModelStaticMeshComponent.h"
#include "InitialShape.h"
#include "InstanceReplacement.h"
#include "MaterialReplacement.h"
#include "VitruvioTypes.h"

#include "VitruvioComponent.generated.h"

DECLARE_LOG_CATEGORY_EXTERN(LogVitruvioComponent, Log, All);

class UGenerateCompletedCallbackProxy;

USTRUCT(BlueprintType)
struct FGenerateOptions
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Vitruvio")
	bool bIgnoreMaterialReplacements = false;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Vitruvio")
	bool bIgnoreInstanceReplacements = false;
};

struct FAttributesEvaluationQueueItem
{
	FAttributeMapPtr AttributeMap;
	bool bForceRegenerate;
	UGenerateCompletedCallbackProxy* CallbackProxy;
};

struct FGenerateQueueItem
{
	FGenerateResultDescription GenerateResultDescription;
	FGenerateOptions GenerateOptions;
	UGenerateCompletedCallbackProxy* CallbackProxy;
};

struct FInstance
{
	FString Name;
	TSharedPtr<FVitruvioMesh> InstanceMesh;
	TArray<UMaterialInstanceDynamic*> OverrideMaterials;
	TArray<FTransform> Transforms;

	friend FORCEINLINE uint32 GetTypeHash(const FInstance& Request)
	{
		return GetTypeHash(Request.InstanceMesh->GetIdentifier());
	}

	friend bool operator==(const FInstance& Lhs, const FInstance& Rhs)
	{
		return Lhs.InstanceMesh && Rhs.InstanceMesh ? Lhs.InstanceMesh->GetIdentifier() == Rhs.InstanceMesh->GetIdentifier() : false;
	}

	friend bool operator!=(const FInstance& Lhs, const FInstance& Rhs)
	{
		return !(Lhs == Rhs);
	}
};

struct FConvertedGenerateResult
{
	TSharedPtr<FVitruvioMesh> ShapeMesh;
	TArray<FInstance> Instances;
	TMap<FString, FReport> Reports;
};

FConvertedGenerateResult BuildGenerateResult(const FGenerateResultDescription& GenerateResult,
									 TMap<Vitruvio::FMaterialAttributeContainer, TObjectPtr<UMaterialInstanceDynamic>>& MaterialCache,
									 TMap<FString, Vitruvio::FTextureData>& TextureCache,
									 TMap<UMaterialInterface*, FString>& MaterialIdentifiers,
									 TMap<FString, int32>& UniqueMaterialIdentifiers,
									 UMaterial* OpaqueParent, UMaterial* MaskedParent, UMaterial* TranslucentParent,
									 UWorld* World);

FString UniqueComponentName(const FString& Name, TMap<FString, int32>& UsedNames);

void ApplyMaterialReplacements(UStaticMeshComponent* StaticMeshComponent, const TMap<UMaterialInterface*, FString>& MaterialIdentifiers,
							   UMaterialReplacementAsset* Replacement);

TSet<FInstance> ApplyInstanceReplacements(UGeneratedModelStaticMeshComponent* GeneratedModelComponent, 
											  const TArray<FInstance>& Instances, UInstanceReplacementAsset* Replacement, TMap<FString, int32>& NameMap);

void InitializeBodySetup(UBodySetup* BodySetup);

UCLASS(ClassGroup = (Custom), meta = (BlueprintSpawnableComponent))
class VITRUVIO_API UVitruvioComponent : public UActorComponent
{
	GENERATED_BODY()

	UPROPERTY()
	bool bValidRandomSeed = false;

	UPROPERTY()
	bool bAttributesReady = false;

	bool bIsGenerating = false;
	bool bHasGeneratedModel = false;

	bool bNotifyAttributeChange = false;

public:
	UVitruvioComponent();

	/** Automatically generate after changing attributes or properties. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, DisplayName = "Generate Automatically", Category = "Vitruvio",
		meta = (EditCondition = "!bBatchGenerate", EditConditionHides))
	bool GenerateAutomatically = true;

	/** Automatically hide initial shape after generation. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, DisplayName = "Hide Initial Shape after Generation", Category = "Vitruvio",
		meta = (EditCondition = "!bBatchGenerate", EditConditionHides))
	bool HideAfterGeneration = false;

	/** Default parent material for opaque geometry. */
	UPROPERTY(EditAnywhere, DisplayName = "Opaque Parent", Category = "Vitruvio Default Materials",
		meta = (EditCondition = "!bBatchGenerate", EditConditionHides))
	UMaterial* OpaqueParent;

	/** Default parent material for masked geometry. */
	UPROPERTY(EditAnywhere, DisplayName = "Masked Parent", Category = "Vitruvio Default Materials",
		meta = (EditCondition = "!bBatchGenerate", EditConditionHides))
	UMaterial* MaskedParent;

	/** Default parent material for translucent geometry. */
	UPROPERTY(EditAnywhere, DisplayName = "Translucent Parent", Category = "Vitruvio Default Materials",
		meta = (EditCondition = "!bBatchGenerate", EditConditionHides))
	UMaterial* TranslucentParent;

	UPROPERTY(VisibleAnywhere, Instanced, Category = "Vitruvio")
	UInitialShape* InitialShape = nullptr;

	UPROPERTY(Transient, TextExportTransient, DuplicateTransient)
	USceneComponent* InitialShapeSceneComponent;

	/** The material replacement asset which defines how materials are replaced after generating a model. */
	UPROPERTY(EditAnywhere, Category = "Vitruvio Replacmeents", Setter = SetMaterialReplacementAsset,
		meta = (EditCondition = "!bBatchGenerate", EditConditionHides))
	UMaterialReplacementAsset* MaterialReplacement;

	/** The instance replacement asset which defines how instances are replaced after generating a model. */
	UPROPERTY(EditAnywhere, Category = "Vitruvio Replacmeents", Setter = SetInstanceReplacementAsset,
		meta = (EditCondition = "!bBatchGenerate", EditConditionHides))
	UInstanceReplacementAsset* InstanceReplacement;

	/**
	 * Enables or disables batch generation.
	 */
	UFUNCTION(BlueprintCallable, Category = "Vitruvio")
	void SetBatchGenerated(bool bBatchGeneration);

	/**
	 * Returns whether batch generation is enabled.
	 */
	UFUNCTION(BlueprintCallable, Category = "Vitruvio")
	bool IsBatchGenerated() const;
	
	/**
	 * Sets the material replacement Asset and regenerates the model.
	 */
	UFUNCTION(BlueprintCallable, Category = "Vitruvio Replacmeents")
	void SetMaterialReplacementAsset(UMaterialReplacementAsset* MaterialReplacementAsset);

	/**
	 * Sets the instance replacement Asset and regenerates the model.
	 */
	UFUNCTION(BlueprintCallable, Category = "Vitruvio Replacmeents")
	void SetInstanceReplacementAsset(UInstanceReplacementAsset* InstanceReplacementAsset);

	/**
	 * Generates a model using the current Rule Package and initial shape. If the attributes are not yet available, they will first be evaluated. If
	 * no Initial Shape or Rule Package is set, this method will do nothing.
	 */
	void Generate(UGenerateCompletedCallbackProxy* CallbackProxy = nullptr, const FGenerateOptions& GenerateOptions = {});

	/**
	 * Sets the given Rule Package. This will reevaluate the attributes and if bGenerateModel is set to true, also generates the model.
	 */
	void SetRpk(URulePackage* RulePackage, bool bEvaluateAttributes = true, bool bGenerateModel = true, UGenerateCompletedCallbackProxy* CallbackProxy = nullptr);

	/** Returns true if the component has valid input data (initial shape and Rule Package). */
	UFUNCTION(BlueprintCallable, Category = "Vitruvio")
	bool HasValidInputData() const;

	/** Returns true if the component is ready to generate, meaning it HasValidInputData and the attributes are loaded. */
	UFUNCTION(BlueprintCallable, Category = "Vitruvio")
	bool IsReadyToGenerate() const;

	/**
	 * Sets the string attribute with the given Name to the given value. Regenerates the model if GenerateAutomatically is set to true.
	 *
	 * @param Name The name of the attribute to set.
	 * @param Value The new value for the attribute.
	 * @param bGenerateModel Whether a model should be generated after the attribute has been set.
	 * @param CallbackProxy The callback proxy used to register for completion events.
	 */
	void SetStringAttribute(const FString& Name, const FString& Value, bool bGenerateModel = true,
							UGenerateCompletedCallbackProxy* CallbackProxy = nullptr);

	/**
	 * Access the string attribute with the given Name. The OutValue is default initialized if no attribute with the given Name is found.
	 *
	 * @param Name The name of the string attribute.
	 * @param OutValue Set to the attributes value if it exists or to an empty String otherwise.
	 * @returns True if the String attribute with the given Name exists or false otherwise.
	 */
	UFUNCTION(BlueprintCallable, Category = "Vitruvio")
	bool GetStringAttribute(const FString& Name, FString& OutValue) const;

	/**
	 * Sets the string array attribute with the given Name to the given value. Regenerates the model if bGenerateModel is set to true.
	 *
	 * @param Name The name of the attribute.
	 * @param Values The new values for the attribute.
	 * @param bGenerateModel Whether a model should be generated after the attribute has been set.
	 * @param CallbackProxy The optional callback proxy used for generate completed notifications.
	 * @returns true if the attribute has been set to the new value or false otherwise.
	 */
	void SetStringArrayAttribute(const FString& Name, const TArray<FString>& Values, bool bGenerateModel = true,
								 UGenerateCompletedCallbackProxy* CallbackProxy = nullptr);

	/**
	 * Access the string array attribute with the given Name. The OutValue is default initialized if no attribute with the given Name is found.
	 *
	 * @param Name The name of the string attribute.
	 * @param OutValue Set to the array attribute value if it exists or an empty array otherwise.
	 * @returns true if the string array attribute with the given Name exists or false otherwise.
	 */
	UFUNCTION(BlueprintCallable, Category = "Vitruvio")
	bool GetStringArrayAttribute(const FString& Name, TArray<FString>& OutValue) const;

	/**
	 * Sets the bool attribute with the given Name to the given value. Regenerates the model if bGenerateModel is set to true.
	 *
	 * @param Name The name of the attribute.
	 * @param Value The new value for the attribute.
	 * @param bGenerateModel Whether a model should be generated after the attribute has been set.
	 * @param CallbackProxy The optional callback proxy used for generate completed notifications.
	 * @returns true if the attribute has been set to the new value or false otherwise.
	 */
	void SetBoolAttribute(const FString& Name, bool Value, bool bGenerateModel = true, UGenerateCompletedCallbackProxy* CallbackProxy = nullptr);

	/**
	 * Access the bool attribute with the given Name. The OutValue is default initialized if no attribute with the given Name is found.
	 *
	 * @param Name The name of the bool attribute.
	 * @param OutValue Set to the attributes value if it exists or to false otherwise.
	 * @returns true if the bool attribute with the given Name exists or false otherwise.
	 */
	UFUNCTION(BlueprintCallable, Category = "Vitruvio")
	bool GetBoolAttribute(const FString& Name, bool& OutValue) const;

	/**
	 * Sets the bool array attribute with the given Name to the given value. Regenerates the model if bGenerateModel is set to true.
	 *
	 * @param Name The name of the attribute.
	 * @param Values The new values for the attribute.
	 * @param bGenerateModel Whether a model should be generated after the attribute has been set.
	 * @param CallbackProxy The optional callback proxy used for generate completed notifications.
	 * @returns true if the attribute has been set to the new value or false otherwise.
	 */
	void SetBoolArrayAttribute(const FString& Name, const TArray<bool>& Values, bool bGenerateModel = true,
							   UGenerateCompletedCallbackProxy* CallbackProxy = nullptr);

	/**
	 * Access the bool array attribute with the given Name. The OutValue is default initialized if no attribute with the given Name is found.
	 *
	 * @param Name The name of the bool attribute.
	 * @param OutValue Set to the array attribute value if it exists or an empty array otherwise.
	 * @returns true if the bool array attribute with the given Name exists or false otherwise.
	 */
	UFUNCTION(BlueprintCallable, Category = "Vitruvio")
	bool GetBoolArrayAttribute(const FString& Name, TArray<bool>& OutValue) const;

	/**
	 * Sets the float attribute with the given Name to the given value. Regenerates the model if bGenerateModel is set to true.
	 *
	 * @param Name The name of the attribute.
	 * @param Value The new value for the attribute.
	 * @param bGenerateModel Whether a model should be generated after the attribute has been set.
	 * @param CallbackProxy The optional callback proxy used for generate completed notifications.
	 * @returns true if the attribute has been set to the new value or false otherwise.
	 */
	void SetFloatAttribute(const FString& Name, double Value, bool bGenerateModel = true, UGenerateCompletedCallbackProxy* CallbackProxy = nullptr);

	/**
	 * Access the float attribute with the given Name. The OutValue is default initialized if no attribute with the given Name is found.
	 *
	 * @param Name The name of the float attribute.
	 * @param OutValue Set to the attributes value if it exists or to 0.0f otherwise.
	 * @returns true if the float attribute with the given Name exists or false otherwise.
	 */
	UFUNCTION(BlueprintCallable, Category = "Vitruvio")
	bool GetFloatAttribute(const FString& Name, double& OutValue) const;

	/**
	 * Sets the float array attribute with the given Name to the given value. Regenerates the model if bGenerateModel is set to true.
	 *
	 * @param Name The name of the attribute.
	 * @param Values The new values for the attribute.
	 * @param bGenerateModel Whether a model should be generated after the attribute has been set.
	 * @param CallbackProxy The optional callback proxy used for generate completed notifications.
	 * @returns true if the attribute has been set to the new value or false otherwise.
	 */
	void SetFloatArrayAttribute(const FString& Name, const TArray<double>& Values, bool bGenerateModel = true,
								UGenerateCompletedCallbackProxy* CallbackProxy = nullptr);

	/**
	 * Access the float array attribute with the given Name. The OutValue is default initialized if no attribute with the given Name is found.
	 *
	 * @param Name The name of the float attribute.
	 * @param OutValue Set to the array attribute value if it exists or an empty array otherwise.
	 * @returns true if the float array attribute with the given Name exists or false otherwise.
	 */
	UFUNCTION(BlueprintCallable, Category = "Vitruvio")
	bool GetFloatArrayAttribute(const FString& Name, TArray<double>& OutValue) const;

	/**
	 * Sets a given attribute. If a key with the provided name is not found in the current attributes, the key-value pair will be ignored.
	 * Regenerates the model if bGenerateModel is set to true. The type of the attribute will be deduced from its
	 * string representation: <br> "1.0" for the float 1.0 <br> "hello" for the string "hello" and <br> "true" for the bool true <br>
	 * array values are separated via a comma eg: "1.3,4.5,0" for a float array with the values 1.3, 4.5 and 0.
	 *
	 * @param Key The name of the attribute to be changed.
	 * @param Value The updated/changed value for the attribute.
	 * @param bGenerateModel Whether a model should be generated after the attribute has been set.
	 * @param CallbackProxy The optional callback proxy used for generate completed notifications.
	 */
	void SetAttribute(const FString& Key, const FString& Value, bool bGenerateModel = true,
					   UGenerateCompletedCallbackProxy* CallbackProxy = nullptr);

	/**
	 * Sets the given attributes. If a key from the NewAttributes is not found in the current attributes, the key-value pair will be ignored.
	 * Regenerates the model if bGenerateModel is set to true. The type of the attribute will be deduced from its
	 * string representation: <br> "1.0" for the float 1.0 <br> "hello" for the string "hello" and <br> "true" for the bool true <br>
	 * array values are separated via a comma eg: "1.3,4.5,0" for a float array with the values 1.3, 4.5 and 0.
	 *
	 * @param NewAttributes The attributes to be set.
	 * @param bGenerateModel Whether a model should be generated after the attribute has been set.
	 * @param CallbackProxy The optional callback proxy used for generate completed notifications.
	 */
	void SetAttributes(const TMap<FString, FString>& NewAttributes, bool bGenerateModel = true,
					   UGenerateCompletedCallbackProxy* CallbackProxy = nullptr);

	/**
	 * Sets the given static mesh as initial shape. Regenerates the model if bGenerateModel is set to true.
	 *
	 * @param StaticMesh the new initial shape static mesh.
	 * @param bGenerateModel Whether a model should be generated after the attribute has been set.
	 * @param CallbackProxy The optional callback proxy used for generate completed notifications.
	 */
	void SetMeshInitialShape(UStaticMesh* StaticMesh, bool bGenerateModel = true, UGenerateCompletedCallbackProxy* CallbackProxy = nullptr);

	/**
	 * Sets the given spline points as initial shape. Regenerates the model if bGenerateModel is set to true.
	 *
	 * @param SplinePoints the new initial shape spline points.
	 * @param bGenerateModel Whether a model should be generated after the attribute has been set.
	 * @param CallbackProxy The optional callback proxy used for generate completed notifications.
	 */
	void SetSplineInitialShape(const TArray<FSplinePoint>& SplinePoints, bool bGenerateModel = true,
							   UGenerateCompletedCallbackProxy* CallbackProxy = nullptr);

	/** Returns the attributes used for generation. */
	UFUNCTION(BlueprintCallable, Category = "Vitruvio")
	const TMap<FString, URuleAttribute*>& GetAttributes() const;

	/** Returns the rule package used for generation. */
	UFUNCTION(BlueprintCallable, Category = "Vitruvio")
	URulePackage* GetRpk() const;

	/** Returns the reports created during generation. */
	UFUNCTION(BlueprintCallable, Category = "Vitruvio")
	const TMap<FString, FReport>& GetReports() const;

	/** Sets the visibility of the initial shape component. */
	UFUNCTION(BlueprintCallable, Category = "Vitruvio")
	void SetInitialShapeVisible(bool bVisible);

	/**
	 * Sets the random seed used for generation. This will reevaluate the attributes and if bGenerateModel is set to true, also generates the model.
	 */
	void SetRandomSeed(int32 NewRandomSeed, bool bGenerateModel = true, UGenerateCompletedCallbackProxy* CallbackProxy = nullptr);

	UFUNCTION(BlueprintCallable, Category = "Vitruvio")
	/** Returns the random seed used for generation. */
	int32 GetRandomSeed();
	
	/* Initialize the VitruvioComponent. Only needs to be called if the Component is natively attached. */
	void Initialize();

	/* Removes the generated meshes from this VitruvioComponent. */
	void RemoveGeneratedMeshes();

	/* Returns whether the attributes are ready. */
	bool GetAttributesReady() const;

	/* Returns whether this component has a generated model */
	bool HasGeneratedModel() const;

	/* Returns the generated model component */
	UGeneratedModelStaticMeshComponent* GetGeneratedModelComponent() const;

	/* Returns the generated model HISM components */
	TArray<UGeneratedModelHISMComponent*> GetGeneratedModelHISMComponents() const;

	/* Returns the material identifier of the given material for replacements. */
	FString GetMaterialIdentifier(const UMaterialInterface* SourceMaterial) const;

	/**
	 * Evaluate rule attributes.
	 *
	 * @param ForceRegenerate Whether to force regenerate even if generate automatically is set to false
	 */
	void EvaluateRuleAttributes(bool ForceRegenerate = false, UGenerateCompletedCallbackProxy* CallbackProxy = nullptr);

	/* Returns whether the initial shape type can be changed */
	bool CanChangeInitialShapeType() const
	{
		return !InitialShapeSceneComponent || InitialShapeSceneComponent->CreationMethod == EComponentCreationMethod::Instance;
	}

	void InitializeInitialShapeComponent();

	/* Destroys the current initial shape component */
	void DestroyInitialShapeComponent();

	DECLARE_DYNAMIC_MULTICAST_DELEGATE(FGenerateCompletedDelegate);
	DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnAttributesEvaluatedDelegate);

	/** Called after attributes have been evaluated. */
	UPROPERTY(BlueprintAssignable, Category = "Vitruvio")
	FOnAttributesEvaluatedDelegate OnAttributesEvaluated;

	/** Called after a model generation has completed. */
	UPROPERTY(BlueprintAssignable, Category = "Vitruvio")
	FGenerateCompletedDelegate OnGenerateCompleted;

	DECLARE_MULTICAST_DELEGATE_OneParam(FOnHierarchyChanged, UVitruvioComponent*);
	static FOnHierarchyChanged OnHierarchyChanged;

	DECLARE_MULTICAST_DELEGATE_TwoParams(FOnAttributesChanged, UObject*, struct FPropertyChangedEvent&);
	static FOnAttributesChanged OnAttributesChanged;

	void LoadInitialShape();

	virtual void OnComponentDestroyed(bool bDestroyingHierarchy) override;

	void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

#if WITH_EDITOR
	virtual void PreEditUndo() override;
	void PostUndoRedo();
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;

	void OnPropertyChanged(UObject* Object, FPropertyChangedEvent& PropertyChangedEvent);
	void SetInitialShapeType(const TSubclassOf<UInitialShape>& Type);
#endif

	static TArray<TSubclassOf<UInitialShape>> GetInitialShapesClasses();

private:
	/** CityEngine Rule Package used for generation. */
	UPROPERTY(EditAnywhere, DisplayName = "Rule Package", Category = "Vitruvio", meta = (AllowPrivateAccess = "true"))
	URulePackage* Rpk;

	/** Rule attributes used for generation. */
	UPROPERTY(EditAnywhere, DisplayName = "Attributes", Category = "Vitruvio")
	TMap<FString, URuleAttribute*> Attributes;

	/** Random seed used for generation. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, DisplayName = "Random Seed", Category = "Vitruvio", meta = (AllowPrivateAccess = "true"))
	int32 RandomSeed;

	/** CGA Reports from generation. */
	UPROPERTY(VisibleAnywhere, DisplayName = "Reports", Category = "Vitruvio")
	TMap<FString, FReport> Reports;

	/** Generate several VitruvioComponents together in batches which can improve generate as well as rendering performance. */
	UPROPERTY(EditAnywhere, DisplayName = "Batch Generate", Category = "Vitruvio")
	bool bBatchGenerate = false;

	UPROPERTY(Transient)
	bool bInitialized = false;

	bool bInGenerateCallback = false;

	TQueue<FGenerateQueueItem> GenerateQueue;
	TQueue<FAttributesEvaluationQueueItem> AttributesEvaluationQueue;

	FGenerateResult::FTokenPtr GenerateToken;
	FAttributeMapResult::FTokenPtr EvalAttributesInvalidationToken;

	bool HasGeneratedMesh = false;

	// Note that these are only unique per VitruvioComponent
	UPROPERTY()
	TMap<UMaterialInterface*, FString> MaterialIdentifiers;
	TMap<FString, int32> UniqueMaterialIdentifiers;

	void CalculateRandomSeed();

	void NotifyAttributesChanged();

	void ProcessGenerateQueue();
	void ProcessAttributesEvaluationQueue();

#if WITH_EDITOR
	FDelegateHandle PropertyChangeDelegate;
#endif

	friend class AVitruvioBatchActor;
};