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

#include "DetailCategoryBuilder.h"
#include "DetailLayoutBuilder.h"
#include "DetailWidgetRow.h"
#include "IDetailCustomization.h"
#include "IDetailPropertyRow.h"
#include "VitruvioComponent.h"

class FVitruvioComponentDetails final : public IDetailCustomization
{
public:
	FVitruvioComponentDetails();
	~FVitruvioComponentDetails();

	static TSharedRef<IDetailCustomization> MakeInstance();

	void AddSwitchInitialShapeCombobox(IDetailCategoryBuilder& RootCategory, const TSharedPtr<FString>& CurrentInitialShapeType,
									   UVitruvioComponent* VitruvioComponent);

	void CustomizeDetails(IDetailLayoutBuilder& DetailBuilder) override;
	void CustomizeDetails(const TSharedPtr<IDetailLayoutBuilder>& DetailBuilder) override;

	void OnAttributesChanged(UObject* Object, struct FPropertyChangedEvent& Event);
	void OnGenerateAutomaticallyChanged();

	void OnVitruvioComponentHierarchyChanged(UVitruvioComponent* Component);

private:
	void BuildAttributeEditor(IDetailLayoutBuilder& DetailBuilder, IDetailCategoryBuilder& RootCategory, UVitruvioComponent* VitruvioActor);

	TArray<TWeakObjectPtr<UObject>> ObjectsBeingCustomized;
	TArray<UVitruvioComponent*> SelectedVitruvioComponents;
	TWeakPtr<IDetailLayoutBuilder> CachedDetailBuilder;
	TSharedPtr<SWidget> ColorPickerParentWidget;
	TSharedPtr<STextComboBox> ChangeInitialShapeCombo;

	TArray<TSharedPtr<FString>> InitialShapeTypes;
	TMap<TSharedPtr<FString>, UClass*> InitialShapeTypeMap;
	TMap<UClass*, TSharedPtr<FString>> InitialShapeClassMap;

	TArray<TSharedPtr<IPropertyRowGenerator>> Generators;
};

template <typename T>
class SPropertyComboBox final : public SComboBox<TSharedPtr<T>>
{
	using SelectionChanged = typename SComboBox<TSharedPtr<T>>::FOnSelectionChanged;

public:
	SLATE_BEGIN_ARGS(SPropertyComboBox) {}
	SLATE_ATTRIBUTE(TArray<TSharedPtr<T>>, ComboItemList)
	SLATE_EVENT(SelectionChanged, OnSelectionChanged)
	SLATE_ATTRIBUTE(TSharedPtr<T>, InitialValue)
	SLATE_ATTRIBUTE(bool, HasMultipleValues)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

private:
	TArray<TSharedPtr<T>> ComboItemList;
	bool HasMultipleValues;

	TSharedRef<SWidget> OnGenerateComboWidget(TSharedPtr<T> InValue) const;
};