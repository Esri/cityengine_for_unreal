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

#include "VitruvioComponentDetails.h"

#include "GenerateCompletedCallbackProxy.h"
#include "VitruvioComponent.h"

#include "Algo/Transform.h"
#include "Brushes/SlateColorBrush.h"
#include "HAL/PlatformApplicationMisc.h"
#include "IDetailGroup.h"
#include "IDetailTreeNode.h"
#include "IPropertyRowGenerator.h"
#include "ISinglePropertyView.h"
#include "InstanceReplacementDialog.h"
#include "LevelEditor.h"
#include "MaterialReplacementDialog.h"
#include "Misc/ScopedSlowTask.h"
#include "VitruvioEditorModule.h"
#include "Widgets/Colors/SColorBlock.h"
#include "Widgets/Colors/SColorPicker.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SSpinBox.h"
#include "Widgets/Input/STextComboBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "VitruvioComponentDetails"

namespace
{

bool ReplacementDialogOpen = false;

FString ValueToString(const TSharedPtr<FString>& In)
{
	return *In;
}

FString ValueToString(const TSharedPtr<double>& In)
{
	return FString::SanitizeFloat(*In);
}

FString ValueToString(const TSharedPtr<bool>& In)
{
	return *In ? TEXT("True") : TEXT("False");
}

bool IsDefault(double Value)
{
	return Value == 0.0;
}

bool IsDefault(const FString& Value)
{
	return Value.IsEmpty();
}

template <typename A, typename V>
void UpdateAttributeValue(UVitruvioComponent* VitruvioActor, A* Attribute, const V& Value)
{
	Attribute->Value = Value;
	Attribute->bUserSet = true;
	VitruvioActor->EvaluateRuleAttributes(VitruvioActor->GenerateAutomatically);
}

TArray<UVitruvioComponent*> GetAllVitruvioComponents(const TArray<TWeakObjectPtr<UObject>>& ObjectsBeingCustomized)
{
	TArray<UVitruvioComponent*> OutVitruvioComponents;
	for (const TWeakObjectPtr<UObject>& CurrentObject : ObjectsBeingCustomized)
	{
		if (CurrentObject.IsValid())
		{
			UVitruvioComponent* VitruvioComponent = Cast<UVitruvioComponent>(CurrentObject.Get());
			if (VitruvioComponent)
			{
				OutVitruvioComponents.Add(VitruvioComponent);
			}
		}
	}

	return OutVitruvioComponents;
}

template <typename V, typename A>
TSharedPtr<SPropertyComboBox<V>> CreateEnumWidget(A* Annotation, const TSharedPtr<IPropertyHandle>& PropertyHandle, bool MultipleValues)
{
	check(Annotation->Values.Num() > 0);

	auto OnSelectionChanged = [PropertyHandle](TSharedPtr<V> Val, ESelectInfo::Type Type) {
		PropertyHandle->SetValue(*Val);
	};

	TArray<TSharedPtr<V>> SharedPtrValues;
	Algo::Transform(Annotation->Values, SharedPtrValues, [](const V& Value) { return MakeShared<V>(Value); });

	V CurrentValue;
	PropertyHandle->GetValue(CurrentValue);
	auto InitialSelectedIndex = Annotation->Values.IndexOfByPredicate([&CurrentValue](const V& Value) { return Value == CurrentValue; });

	if (InitialSelectedIndex == INDEX_NONE)
	{
		// If the value is not present in the enum values we insert it at the beginning (similar behavior to CE inspector)
		if (!IsDefault(CurrentValue))
		{
			SharedPtrValues.Insert(MakeShared<V>(CurrentValue), 0);
		}
		InitialSelectedIndex = 0;
	}
	auto InitialSelectedValue = SharedPtrValues[InitialSelectedIndex];

	// clang-format off
	auto ValueWidget = SNew(SPropertyComboBox<V>)
		.ComboItemList(SharedPtrValues)
		.OnSelectionChanged_Lambda(OnSelectionChanged)
		.InitialValue(InitialSelectedValue)
		.HasMultipleValues(MultipleValues);
	// clang-format on

	return ValueWidget;
}

template <typename C>
void CreateColorPicker(const FLinearColor& InitialColor, C OnCommit)
{
	FColorPickerArgs PickerArgs;
	{
		PickerArgs.bUseAlpha = false;
		PickerArgs.bOnlyRefreshOnOk = true;
		PickerArgs.sRGBOverride = true;
		PickerArgs.DisplayGamma = TAttribute<float>::Create(TAttribute<float>::FGetter::CreateUObject(GEngine, &UEngine::GetDisplayGamma));
		PickerArgs.InitialColor = InitialColor;
		PickerArgs.OnColorCommitted.BindLambda(OnCommit);
	}

	OpenColorPicker(PickerArgs);
}

TSharedPtr<SHorizontalBox> CreateColorInputWidget(const TSharedPtr<IPropertyHandle>& ColorStringProperty)
{
	auto ColorCommitted = [ColorStringProperty](FLinearColor NewColor) {
		ColorStringProperty->SetValue(TEXT("#") + NewColor.ToFColor(true).ToHex());
	};

	auto ColorLambda = [ColorStringProperty]() {
		FString Value;
		ColorStringProperty->GetValue(Value);
		return Value.IsEmpty() ? FLinearColor::White : FLinearColor(FColor::FromHex(Value));
	};

	// clang-format off
	return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot()
		.VAlign(VAlign_Center)
		.Padding(0.0f, 2.0f)
		[
			// Displays the color without alpha
			SNew(SColorBlock)
			.Color_Lambda(ColorLambda)
			.ShowBackgroundForAlpha(false)
			.OnMouseButtonDown_Lambda([ColorLambda, ColorCommitted](const FGeometry& Geometry, const FPointerEvent& Event) -> FReply
			{
				if (Event.GetEffectingButton() != EKeys::LeftMouseButton)
				{
					return FReply::Unhandled();
				}

				CreateColorPicker(ColorLambda(), ColorCommitted);
				return FReply::Handled();
			})
			.UseSRGB(true)
			.AlphaDisplayMode(EColorBlockAlphaDisplayMode::Ignore)
			.Size(FVector2D(35.0f, 12.0f))
		];
	// clang-format on
}

TSharedPtr<SCheckBox> CreateBoolInputWidget(const TSharedPtr<IPropertyHandle>& Property, bool MultipleValues)
{
	auto OnCheckStateChanged = [Property](ECheckBoxState CheckBoxState) -> void {
		Property->SetValue(CheckBoxState == ECheckBoxState::Checked);
	};

	auto ValueWidget = SNew(SCheckBox).OnCheckStateChanged_Lambda(OnCheckStateChanged);

	if (MultipleValues)
	{
		ValueWidget->SetIsChecked(ECheckBoxState::Undetermined);
	}
	else
	{
		bool CurrentValue = false;
		Property->GetValue(CurrentValue);
		ValueWidget->SetIsChecked(CurrentValue);
	}

	return ValueWidget;
}

TSharedPtr<SHorizontalBox> CreateTextInputWidget(const TSharedPtr<IPropertyHandle>& StringProperty, bool MultipleValues)
{
	auto OnTextChanged = [StringProperty](const FText& Text, ETextCommit::Type) {

		if (StringProperty.IsValid())
		{
			FString OldValue;
			StringProperty->GetValue(OldValue);

			if (OldValue != Text.ToString())
			{
				StringProperty->SetValue(Text.ToString());
			}
		}
	};

	// clang-format off
	auto ValueWidget = SNew(SEditableTextBox)
		.Font(IDetailLayoutBuilder::GetDetailFont())
		.IsReadOnly(false)
		.SelectAllTextWhenFocused(true)
		.OnTextCommitted_Lambda(OnTextChanged);
	// clang-format on

	if (MultipleValues)
	{
		ValueWidget->SetText(LOCTEXT("MultipleValues", "Multiple Values"));
	}
	else
	{
		FString Initial;
		StringProperty->GetValue(Initial);
		ValueWidget->SetText(FText::FromString(Initial));
	}

	// clang-format off
	return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot()
		.VAlign(VAlign_Fill)
		.HAlign(HAlign_Fill)
		.FillWidth(1)
		[
			ValueWidget
		];
	// clang-format on
}

template <typename Attr>
TSharedPtr<SHorizontalBox> CreateMultipleValueFloatAttributeWidget(Attr* Attribute, const TSharedPtr<IPropertyHandle>& Property)
{
	auto OnTextChanged = [Property, Attribute](const FText& Text, ETextCommit::Type)
	{
		if (Property->IsValidHandle() && Text.IsNumeric())
		{
			double Value = FCString::Atof(*Text.ToString());

			auto Annotation = Attribute->GetRangeAnnotation();
			if (Annotation)
			{
				if (Annotation->HasMin && Value < Annotation->Min)
				{
					Value = Annotation->Min;
				}

				if (Annotation->HasMax && Value > Annotation->Max)
				{
					Value = Annotation->Max;
				}
			}

			Property->SetValue(Value);
		}
	};

	// clang-format off
	auto ValueWidget = SNew(SEditableTextBox)
		.Font(IDetailLayoutBuilder::GetDetailFont())
		.IsReadOnly(false)
		.SelectAllTextWhenFocused(true)
		.OnTextCommitted_Lambda(OnTextChanged);
	// clang-format on

	ValueWidget->SetText(LOCTEXT("MultipleValues", "Multiple Values"));

	// clang-format off
	return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot()
		.VAlign(VAlign_Fill)
		.HAlign(HAlign_Fill)
		.FillWidth(1)
		[
			ValueWidget
		];
	// clang-format on
}

template <typename Attr>
TSharedPtr<SSpinBox<double>> CreateNumericInputWidget(Attr* Attribute, const TSharedPtr<IPropertyHandle>& FloatProperty)
{
	auto Annotation = Attribute->GetRangeAnnotation();

	auto OnValueCommit = [FloatProperty](double Value, ETextCommit::Type Type) {
		if (FloatProperty->IsValidHandle())
		{
			double OldValue = 0.0f;
			FloatProperty->GetValue(OldValue);

			if (!FMath::IsNearlyEqual(OldValue, Value, UE_DOUBLE_KINDA_SMALL_NUMBER))
			{
				FloatProperty->SetValue(Value);
			}
		}
	};

	// clang-format off
	auto ValueWidget = SNew(SSpinBox<double>)
		.Font(IDetailLayoutBuilder::GetDetailFont())
		.MinValue(Annotation && Annotation->HasMin ? Annotation->Min : TOptional<double>())
		.MaxValue(Annotation && Annotation->HasMax ? Annotation->Max : TOptional<double>())
		.OnValueCommitted_Lambda(OnValueCommit)
		.SliderExponent(1);
	// clang-format on

	if (Annotation)
	{
		ValueWidget->SetDelta(Annotation->StepSize);
	}

	double Value;
	FloatProperty->GetValue(Value);
	ValueWidget->SetValue(Value);

	return ValueWidget;
}

TSharedPtr<SBox> CreateNameWidget(URuleAttribute* Attribute)
{
	// clang-format off
	auto NameWidget = SNew(SBox)
		.Content()
		[
			SNew(STextBlock)
			.Text(FText::FromString(Attribute->DisplayName))
			.Font(Attribute->bUserSet ? IDetailLayoutBuilder::GetDetailFontBold() : IDetailLayoutBuilder::GetDetailFont())
		];
	// clang-format on
	return NameWidget;
}

FResetToDefaultOverride ResetToDefaultOverride(URuleAttribute* Attribute, UVitruvioComponent* VitruvioActor)
{
	FResetToDefaultOverride ResetToDefaultOverride = FResetToDefaultOverride::Create(
		FIsResetToDefaultVisible::CreateLambda([Attribute](TSharedPtr<IPropertyHandle> Property) { return Attribute->bUserSet; }),
		FResetToDefaultHandler::CreateLambda([Attribute, VitruvioActor](TSharedPtr<IPropertyHandle> Property) {
			Attribute->bUserSet = false;
			VitruvioActor->EvaluateRuleAttributes(VitruvioActor->GenerateAutomatically);
		}));
	return ResetToDefaultOverride;
}

IDetailGroup* GetOrCreateGroups(IDetailGroup& Root, const URuleAttribute* Attribute, TMap<FString, IDetailGroup*>& GroupCache)
{
	const FString Delimiter = TEXT(".");
	const TArray<FString>& Groups = Attribute->Groups;

	TArray<FString> Imports;
	Attribute->ImportPath.ParseIntoArray(Imports, *Delimiter);

	IDetailGroup* AttributeGroupRoot = &Root;
	FString AttributeGroupImportPath;

	auto GetOrCreateGroup = [&GroupCache, Delimiter](IDetailGroup& Parent, FString ImportPath, FString FullyQualifiedName,
													 FString DisplayName) -> IDetailGroup* {
		const FString CacheGroupKey = ImportPath + Delimiter + FullyQualifiedName;
		const auto CacheResult = GroupCache.Find(CacheGroupKey);
		if (CacheResult)
		{
			return *CacheResult;
		}
		IDetailGroup& Group = Parent.AddGroup(*CacheGroupKey, FText::FromString(DisplayName));
		GroupCache.Add(CacheGroupKey, &Group);

		return &Group;
	};

	for (const FString& CurrImport : Imports)
	{
		AttributeGroupRoot = GetOrCreateGroup(*AttributeGroupRoot, AttributeGroupImportPath, CurrImport, CurrImport);

		AttributeGroupImportPath += CurrImport + Delimiter;
	}

	if (Groups.Num() == 0)
	{
		return AttributeGroupRoot;
	}

	FString QualifiedIdentifier = Groups[0];
	IDetailGroup* CurrentGroup =
		GetOrCreateGroup(*AttributeGroupRoot, AttributeGroupImportPath + Delimiter, QualifiedIdentifier, QualifiedIdentifier);
	for (auto GroupIndex = 1; GroupIndex < Groups.Num(); ++GroupIndex)
	{
		QualifiedIdentifier += Delimiter + Groups[GroupIndex];
		CurrentGroup = GetOrCreateGroup(*CurrentGroup, AttributeGroupImportPath + Delimiter, QualifiedIdentifier, Groups[GroupIndex]);
	}

	return CurrentGroup;
}

void AddSeparator(IDetailCategoryBuilder& RootCategory)
{
	// clang-format off
	RootCategory.AddCustomRow(FText::FromString(L"Divider"), true).WholeRowContent()
	.VAlign(VAlign_Center)
	.HAlign(HAlign_Fill)
	[
		SNew(SSeparator)
		.Orientation(Orient_Horizontal)
		.Thickness(0.5f)
		.SeparatorImage(new FSlateColorBrush(FLinearColor(FColor(47, 47, 47))))
	];
	// clang-format on
}

template <typename A>
TSharedPtr<SWidget> CreateFloatAttributeWidget(A* Attribute, const TSharedPtr<IPropertyHandle>& PropertyHandle, bool MultipleValues)
{
	if (Attribute->GetEnumAnnotation() && Attribute->GetEnumAnnotation()->Values.Num() > 0)
	{
		return CreateEnumWidget<double, UFloatEnumAnnotation>(Attribute->GetEnumAnnotation(), PropertyHandle, MultipleValues).ToSharedRef();
	}
	else
	{
		if (MultipleValues)
		{
			return CreateMultipleValueFloatAttributeWidget(Attribute, PropertyHandle).ToSharedRef();
		}
		else
		{
			return CreateNumericInputWidget(Attribute, PropertyHandle).ToSharedRef();
		}
	}
}

template <typename A>
TSharedPtr<SWidget> CreateStringAttributeWidget(A* Attribute, const TSharedPtr<IPropertyHandle>& PropertyHandle, bool MultipleValues)
{
	if (Attribute->GetEnumAnnotation() && Attribute->GetEnumAnnotation()->Values.Num() > 0)
	{
		return CreateEnumWidget<FString, UStringEnumAnnotation>(Attribute->GetEnumAnnotation(), PropertyHandle, MultipleValues);
	}
	else if (Attribute->GetColorAnnotation())
	{
		return CreateColorInputWidget(PropertyHandle);
	}
	else
	{
		return CreateTextInputWidget(PropertyHandle, MultipleValues);
	}
}

void AddCopyNameToClipboardAction(FDetailWidgetRow& Row, URuleAttribute* Attribute)
{
	// clang-format off
	Row.AddCustomContextMenuAction(FUIAction(FExecuteAction::CreateLambda([Attribute]() {
		   if (Attribute)
		   {
			   FPlatformApplicationMisc::ClipboardCopy(*Attribute->Name);
		   }
	   })),
	   FText::FromString(TEXT("Copy Fully Qualified Attribute Name")),
	   FText::FromString(TEXT("Copies the fully qualified attribute name to the clipboard.")));
	// clang-format on
}

void AddScalarWidget(const TArray<TSharedRef<IDetailTreeNode>> DetailTreeNodes, IDetailGroup& Group, URuleAttribute* Attribute,
					 UVitruvioComponent* VitruvioActor, bool MultipleValuesSelected)
{
	if (DetailTreeNodes.Num() == 0 || DetailTreeNodes[0]->GetNodeType() != EDetailNodeType::Category)
	{
		return;
	}

	TArray<TSharedRef<IDetailTreeNode>> Root;
	DetailTreeNodes[0]->GetChildren(Root);

	const auto& ValueNode = Root[0];

	const TSharedPtr<IPropertyHandle> PropertyHandle = ValueNode->GetRow()->GetPropertyHandle();
	PropertyHandle->SetPropertyDisplayName(FText::FromString(Attribute->DisplayName));
	FDetailWidgetRow& ValueRow = Group.AddWidgetRow();
	ValueRow.PropertyHandles.Add(PropertyHandle);
	ValueRow.OverrideResetToDefault(ResetToDefaultOverride(Attribute, VitruvioActor));
	ValueRow.FilterTextString = FText::FromString(Attribute->DisplayName);

	AddCopyNameToClipboardAction(ValueRow, Attribute);

	TSharedPtr<SWidget> ValueWidget;
	ValueRow.NameContent()[CreateNameWidget(Attribute).ToSharedRef()];

	if (UFloatAttribute* FloatAttribute = Cast<UFloatAttribute>(Attribute))
	{
		ValueWidget = CreateFloatAttributeWidget(FloatAttribute, PropertyHandle, MultipleValuesSelected);
	}
	else if (UStringAttribute* StringAttribute = Cast<UStringAttribute>(Attribute))
	{
		ValueWidget = CreateStringAttributeWidget(StringAttribute, PropertyHandle, MultipleValuesSelected);
	}
	else if (Cast<UBoolAttribute>(Attribute))
	{
		ValueWidget = CreateBoolInputWidget(PropertyHandle, MultipleValuesSelected);
	}

	if (ValueWidget)
	{
		ValueRow.ValueContent()[ValueWidget.ToSharedRef()];
	}
}

bool AreValuesDifferent(URuleAttribute* Attribute, const FString AttributeKey, const TArray<UVitruvioComponent*> VitruvioComponents)
{
	if (VitruvioComponents.Num() > 1)
	{
		FString AttributeValue = Attribute->GetValueAsString();
		for (const auto& Component : VitruvioComponents)
		{
			auto ComponentAttributes = Component->GetAttributes();
			URuleAttribute* ComponentAttr = ComponentAttributes[AttributeKey];
			FString CompareValue = ComponentAttr->GetValueAsString();
			if (AttributeValue != CompareValue)
			{
				return true;
			}
		}
	}

	return false;
}

void AddArrayWidget(const TArray<TSharedRef<IDetailTreeNode>> DetailTreeNodes, IDetailGroup& Group, URuleAttribute* Attribute,
					UVitruvioComponent* VitruvioActor, bool MultipleValuesSelected)
{
	if (DetailTreeNodes.Num() == 0 || DetailTreeNodes[0]->GetNodeType() != EDetailNodeType::Category)
	{
		return;
	}

	TArray<TSharedRef<IDetailTreeNode>> ArrayRoots;
	DetailTreeNodes[0]->GetChildren(ArrayRoots);

	const TSharedRef<IDetailTreeNode>* ValuesArrayRoot = ArrayRoots.FindByPredicate([](const TSharedRef<IDetailTreeNode>& TreeNode)
	{ 
		return TreeNode->GetRow()->GetPropertyHandle()->GetProperty()->GetName() == TEXT("Values"); 
	});

	if (ValuesArrayRoot)
	{
		const TSharedRef<IDetailTreeNode> ArrayRoot = *ValuesArrayRoot;

		// Header Row
		const TSharedPtr<IDetailPropertyRow> HeaderPropertyRow = ArrayRoot->GetRow();
		FString ArrayGroupKey = Attribute->ImportPath + TEXT(".") + Attribute->Name;
		IDetailGroup& ArrayHeader = Group.AddGroup(*ArrayGroupKey, FText::GetEmpty());
		FDetailWidgetRow& Row = ArrayHeader.HeaderRow();
		Row.FilterTextString = FText::FromString(Attribute->DisplayName);
		Row.PropertyHandles.Add(HeaderPropertyRow->GetPropertyHandle());
		Row.OverrideResetToDefault(ResetToDefaultOverride(Attribute, VitruvioActor));
		AddCopyNameToClipboardAction(Row, Attribute);

		FDetailWidgetRow DefaultWidgetsRow;
		TSharedPtr<SWidget> NameWidget;
		TSharedPtr<SWidget> ValueWidget;
		HeaderPropertyRow->GetDefaultWidgets(NameWidget, ValueWidget, DefaultWidgetsRow, true);
		Row.NameContent()[CreateNameWidget(Attribute).ToSharedRef()];
		Row.ValueContent()[ValueWidget.ToSharedRef()];

		// Value Rows
		TArray<TSharedRef<IDetailTreeNode>> ArrayTreeNodes;
		ArrayRoot->GetChildren(ArrayTreeNodes);

		for (const auto& ChildNode : ArrayTreeNodes)
		{
			const TSharedPtr<IDetailPropertyRow> DetailPropertyRow = ChildNode->GetRow();
			const TSharedPtr<IPropertyHandle> PropertyHandle = DetailPropertyRow->GetPropertyHandle();

			FDetailWidgetRow& ValueRow = ArrayHeader.AddWidgetRow();

			FDetailWidgetRow ArrayDefaultWidgetsRow;
			TSharedPtr<SWidget> ArrayNameWidget;
			TSharedPtr<SWidget> ArrayValueWidget;

			DetailPropertyRow->GetDefaultWidgets(ArrayNameWidget, ArrayValueWidget, ArrayDefaultWidgetsRow, true);
			ValueRow.NameContent()[ArrayNameWidget.ToSharedRef()];

			if (UFloatArrayAttribute* FloatArrayAttribute = Cast<UFloatArrayAttribute>(Attribute))
			{
				ValueWidget = CreateFloatAttributeWidget(FloatArrayAttribute, PropertyHandle, MultipleValuesSelected);
			}
			else if (UStringArrayAttribute* StringArrayAttribute = Cast<UStringArrayAttribute>(Attribute))
			{
				ValueWidget = CreateStringAttributeWidget(StringArrayAttribute, PropertyHandle, MultipleValuesSelected);
			}
			else if (Cast<UBoolArrayAttribute>(Attribute))
			{
				ValueWidget = CreateBoolInputWidget(PropertyHandle, MultipleValuesSelected);
			}

			ValueRow.ValueContent()[ValueWidget.ToSharedRef()];
		}
	}
}

void AddGenerateButton(IDetailCategoryBuilder& RootCategory, UVitruvioComponent* VitruvioComponent)
{
	// clang-format off
	RootCategory.AddCustomRow(FText::FromString(L"Generate"), true)
	.WholeRowContent()
	.VAlign(VAlign_Center)
	.HAlign(HAlign_Center)
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot()
		[
			SNew(SButton)
			.Text(FText::FromString("Generate"))
			.ContentPadding(FMargin(30, 2))
			.OnClicked_Lambda([VitruvioComponent]()
			{
				VitruvioComponent->Generate();
				return FReply::Handled();
			})
		]
		.VAlign(VAlign_Fill)
	];
	// clang-format on
}

template <typename TInstanceDialogType>
void OpenReplacementDialog(UVitruvioComponent* VitruvioComponent, bool bNeedsRegenerate)
{
	if (!VitruvioComponent->GetRpk())
	{
		return;
	}

	auto OnDialogClosed = [](const TSharedRef<SWindow>&) {
		ReplacementDialogOpen = false;
	};

	ReplacementDialogOpen = true;

	if (bNeedsRegenerate)
	{
		UGenerateCompletedCallbackProxy* Proxy = NewObject<UGenerateCompletedCallbackProxy>();
		Proxy->OnGenerateCompleted.AddLambda(
			[VitruvioComponent, OnDialogClosed]() { TInstanceDialogType::OpenDialog(VitruvioComponent, OnDialogClosed, true); });
		VitruvioComponent->Generate(Proxy, {true, true});
	}
	else
	{
		TInstanceDialogType::OpenDialog(VitruvioComponent, OnDialogClosed, false);
	}

	VitruvioEditorModule::Get().BlockUntilGenerated();
}

void AddReplacementButtons(IDetailCategoryBuilder& RootCategory, UVitruvioComponent* VitruvioComponent)
{
	bool bHasReplacement = VitruvioComponent->InstanceReplacement != nullptr || VitruvioComponent->MaterialReplacement != nullptr;

	// clang-format off
	RootCategory.AddCustomRow(FText::FromString(TEXT("Replacements")), false)
	.WholeRowContent()
	.VAlign(VAlign_Center)
	.HAlign(HAlign_Center)
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot()
		.VAlign(VAlign_Fill)
		.Padding(4)
		[
			SNew(SButton)
			.OnClicked_Lambda([VitruvioComponent, bHasReplacement]()
			{
				OpenReplacementDialog<FMaterialReplacementDialog>(VitruvioComponent, bHasReplacement);
				return FReply::Handled();
			})
			.IsEnabled(TAttribute<bool>::CreateLambda([]()
			{
				return !ReplacementDialogOpen;
			}))
			.Content()
			[
				SNew(STextBlock)
				.Text(FText::FromString(FString(TEXT("Replace Materials"))))
				.Font(IDetailLayoutBuilder::GetDetailFont())
			]
		]

		+ SHorizontalBox::Slot()
		.VAlign(VAlign_Fill)
		.Padding(0, 4, 4, 4)
		[
			SNew(SButton)
			.OnClicked_Lambda([VitruvioComponent, bHasReplacement]()
			{
				OpenReplacementDialog<FInstanceReplacementDialog>(VitruvioComponent, bHasReplacement);
				return FReply::Handled();
			})
			.IsEnabled(TAttribute<bool>::CreateLambda([]()
			{
				return !ReplacementDialogOpen;
			}))
			.Content()
			[
				SNew(STextBlock)
				.Text(FText::FromString(FString(TEXT("Replace Instances"))))
				.Font(IDetailLayoutBuilder::GetDetailFont())
			]
		]
	];
	// clang-format on
}

} // namespace

template <typename T>
void SPropertyComboBox<T>::Construct(const FArguments& InArgs)
{
	ComboItemList = InArgs._ComboItemList.Get();
	HasMultipleValues = InArgs._HasMultipleValues.Get();
	auto InitialItem = HasMultipleValues ? NULL : InArgs._InitialValue.Get();

	// clang-format off
	SComboBox<TSharedPtr<T>>::Construct(typename SComboBox<TSharedPtr<T>>::FArguments()
		.InitiallySelectedItem(InitialItem)
		.Content()
		[
			SNew(STextBlock)
			.Text_Lambda([this]
			{
				auto SelectedItem = SComboBox<TSharedPtr<T>>::GetSelectedItem();
				if (HasMultipleValues) {
					return SelectedItem ? FText::FromString(ValueToString(SelectedItem)) : FText::FromString("Multiple Values");
				}
				else {
					return SelectedItem ? FText::FromString(ValueToString(SelectedItem)) : FText::FromString("");
				}
			})
			.Font(IDetailLayoutBuilder::GetDetailFont())
		]
		.OptionsSource(&ComboItemList)
		.OnSelectionChanged(InArgs._OnSelectionChanged)
		.OnGenerateWidget(this, &SPropertyComboBox::OnGenerateComboWidget)
	);
	// clang-format on
}

template <typename T>
TSharedRef<SWidget> SPropertyComboBox<T>::OnGenerateComboWidget(TSharedPtr<T> InValue) const
{
	return SNew(STextBlock).Text(FText::FromString(ValueToString(InValue)));
}

FVitruvioComponentDetails::FVitruvioComponentDetails()
{
	for (const auto& InitialShapeType : UVitruvioComponent::GetInitialShapesClasses())
	{
		FString DisplayName = InitialShapeType->GetMetaData(TEXT("DisplayName"));

		auto InitialShapeOption = MakeShared<FString>(DisplayName);
		InitialShapeTypes.Add(InitialShapeOption);
		InitialShapeTypeMap.Add(InitialShapeOption, *InitialShapeType);
		InitialShapeClassMap.Add(*InitialShapeType, InitialShapeOption);
	}

	UVitruvioComponent::OnAttributesChanged.AddRaw(this, &FVitruvioComponentDetails::OnAttributesChanged);
	UVitruvioComponent::OnHierarchyChanged.AddRaw(this, &FVitruvioComponentDetails::OnVitruvioComponentHierarchyChanged);
}

FVitruvioComponentDetails::~FVitruvioComponentDetails()
{
	UVitruvioComponent::OnAttributesChanged.RemoveAll(this);
	UVitruvioComponent::OnHierarchyChanged.RemoveAll(this);
}

TSharedRef<IDetailCustomization> FVitruvioComponentDetails::MakeInstance()
{
	return MakeShareable(new FVitruvioComponentDetails);
}

void FVitruvioComponentDetails::BuildAttributeEditor(IDetailLayoutBuilder& DetailBuilder, IDetailCategoryBuilder& RootCategory,
													 UVitruvioComponent* VitruvioActor)
{
	if (!VitruvioActor || !VitruvioActor->GetRpk())
	{
		return;
	}

	Generators.Empty();

	IDetailGroup& RootGroup = RootCategory.AddGroup("Attributes", FText::FromString("Attributes"), true);
	TSharedPtr<IPropertyHandle> AttributesHandle = DetailBuilder.GetProperty(FName(TEXT("Attributes")));

	// Create Attributes header widget
	IDetailPropertyRow& HeaderProperty = RootGroup.HeaderProperty(AttributesHandle.ToSharedRef());
	HeaderProperty.ShowPropertyButtons(false);

	FResetToDefaultOverride ResetAllToDefaultOverride = 
		FResetToDefaultOverride::Create(FResetToDefaultHandler::CreateLambda([VitruvioActor](TSharedPtr<IPropertyHandle> Property) {
			for (const auto& AttributeEntry : VitruvioActor->GetAttributes())
			{
				AttributeEntry.Value->bUserSet = false;
			}

			VitruvioActor->EvaluateRuleAttributes(VitruvioActor->GenerateAutomatically);
		}));

	HeaderProperty.OverrideResetToDefault(ResetAllToDefaultOverride);
	FDetailWidgetRow& HeaderWidget = HeaderProperty.CustomWidget();

	// clang-format off
	HeaderWidget.NameContent()
	[
		SNew(SBox)
		.Content()
		[
			SNew(STextBlock)
			.Text(FText::FromString(RootGroup.GetGroupName().ToString()))
			.Font(IDetailLayoutBuilder::GetDetailFont())
		]
	];
	// clang-format on

	TMap<FString, IDetailGroup*> GroupCache;
	FPropertyEditorModule& PropertyEditorModule = FModuleManager::GetModuleChecked<FPropertyEditorModule>("PropertyEditor");
	for (const auto& AttributeEntry : VitruvioActor->GetAttributes())
	{
		URuleAttribute* Attribute = AttributeEntry.Value;
		FString AttributeKey = AttributeEntry.Key;

		IDetailGroup* Group = GetOrCreateGroups(RootGroup, Attribute, GroupCache);

		FPropertyRowGeneratorArgs Args;
		const auto Generator = PropertyEditorModule.CreatePropertyRowGenerator(Args);
		TArray<UObject*> Objects;
		Objects.Add(Attribute);
		Generator->SetObjects(Objects);
		Generator->OnFinishedChangingProperties().AddLambda([this, VitruvioActor, Attribute, AttributeKey](FPropertyChangedEvent Event) {
				if (Event.ChangeType == EPropertyChangeType::ArrayAdd)
				{
					if (UArrayAttribute* ArrayAttribute = Cast<UArrayAttribute>(Attribute))
					{
						Event.ObjectIteratorIndex = 0;
						const int ArrayIndex = Event.GetArrayIndex(Event.Property->GetFName().ToString());
						ArrayAttribute->InitializeDefaultArrayValue(ArrayIndex);
					}
				}
				Attribute->bUserSet = true;
				VitruvioActor->EvaluateRuleAttributes(VitruvioActor->GenerateAutomatically);

				// Apply attribute changes to all selected vitruvio actors (if more than 1)
				// Issue: Undo doesn't undo the change in attributes on all vitruvio actors
				TMap<FString, FString> ChangedAttributes;
				ChangedAttributes.Add(AttributeKey, Attribute->GetValueAsString());
				for (UVitruvioComponent* Component : SelectedVitruvioComponents)
				{
					if (Component != VitruvioActor)
					{
						Component->SetAttribute(AttributeKey, Attribute->GetValueAsString());
					}
				}
			});
		const TArray<TSharedRef<IDetailTreeNode>> DetailTreeNodes = Generator->GetRootTreeNodes();

		Generators.Add(Generator);

		bool MultipleValuesSelected = AreValuesDifferent(Attribute, AttributeKey, SelectedVitruvioComponents);

		if (Cast<UStringArrayAttribute>(Attribute) || Cast<UFloatArrayAttribute>(Attribute) || Cast<UBoolArrayAttribute>(Attribute))
		{
			AddArrayWidget(DetailTreeNodes, *Group, Attribute, VitruvioActor, MultipleValuesSelected);
		}
		else
		{
			AddScalarWidget(DetailTreeNodes, *Group, Attribute, VitruvioActor, MultipleValuesSelected);
		}
	}
}

void FVitruvioComponentDetails::AddSwitchInitialShapeCombobox(IDetailCategoryBuilder& RootCategory,
															  const TSharedPtr<FString>& CurrentInitialShapeType,
															  UVitruvioComponent* VitruvioComponent)
{
	// clang-format off
	FDetailWidgetRow& Row = RootCategory.AddCustomRow(FText::FromString("InitialShape"), false);

	Row.NameContent()
	[
		SNew(SBox)
		.Content()
		[
			SNew(STextBlock)
			.Text(FText::FromString("Initial Shape Type"))
			.Font(IDetailLayoutBuilder::GetDetailFont())
		]
	];
	
	Row.ValueContent()
	   .VAlign(VAlign_Center)
	   .HAlign(HAlign_Left)
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot()
		[
			SAssignNew(ChangeInitialShapeCombo, STextComboBox)
			.Font(IDetailLayoutBuilder::GetDetailFont())
			.InitiallySelectedItem(CurrentInitialShapeType)
			.OnSelectionChanged_Lambda([this, VitruvioComponent](TSharedPtr<FString> Selection, ESelectInfo::Type SelectInfo)
			{
				if (Selection.IsValid())
				{
					UInitialShape* TempInitialShape = NewObject<UInitialShape>(GetTransientPackage(), InitialShapeTypeMap[Selection], NAME_None, RF_Transient | RF_TextExportTransient);

					if (TempInitialShape->ShouldConvert(VitruvioComponent->InitialShape->GetPolygon()))
					{
						GEditor->BeginTransaction(*FGuid::NewGuid().ToString(), FText::FromString("Change Initial Shape Type"), VitruvioComponent->GetOwner());
						VitruvioComponent->Modify();
						VitruvioComponent->SetInitialShapeType(InitialShapeTypeMap[Selection]);
						VitruvioComponent->Generate();

						// Hack to refresh the property editor
						GEditor->SelectActor(VitruvioComponent->GetOwner(), false, true, true, true);
						GEditor->SelectActor(VitruvioComponent->GetOwner(), true, true, true, true);
						GEditor->SelectComponent(VitruvioComponent, true, true, true);
						GEditor->EndTransaction();
					}
					else
					{
						const TSharedPtr<FString>& CurrentSelection = InitialShapeClassMap[VitruvioComponent->InitialShape->GetClass()];
						ChangeInitialShapeCombo->SetSelectedItem(CurrentSelection);
					}
				}
			})
			.OptionsSource(&InitialShapeTypes)
		]
	];
	// clang-format on
}

void FVitruvioComponentDetails::CustomizeDetails(IDetailLayoutBuilder& DetailBuilder)
{
	TSharedPtr<IPropertyHandle> GenerateAutomaticallyProperty =
		DetailBuilder.GetProperty(GET_MEMBER_NAME_CHECKED(UVitruvioComponent, GenerateAutomatically), UVitruvioComponent::StaticClass());
	GenerateAutomaticallyProperty->SetOnPropertyValueChanged(
		FSimpleDelegate::CreateSP(this, &FVitruvioComponentDetails::OnGenerateAutomaticallyChanged));

	const TSharedRef<IPropertyHandle> BatchGenerateHandle = DetailBuilder.GetProperty(TEXT("bBatchGenerate"));
	BatchGenerateHandle->SetOnPropertyValueChanged(FSimpleDelegate::CreateLambda([&DetailBuilder]() 
	{ 
		DetailBuilder.ForceRefreshDetails(); 
	}));

	ObjectsBeingCustomized.Empty();
	DetailBuilder.GetObjectsBeingCustomized(ObjectsBeingCustomized);

	// If there are more than one items selected
	// store all the vitruvio components and check if they have the same rpk
	// If the rpk's differ between the vitruvio components, act as before - just return
	// Otherwise same rpk, build attributes for one, apply to all selected
	SelectedVitruvioComponents.Empty();
	SelectedVitruvioComponents = GetAllVitruvioComponents(ObjectsBeingCustomized);
	if (ObjectsBeingCustomized.Num() > 1)
	{
		DetailBuilder.GetProperty(FName(TEXT("Attributes")))->MarkHiddenByCustomization();

		URulePackage* ComponentRPK = SelectedVitruvioComponents[0]->GetRpk();
		for (UVitruvioComponent* VitruvioComponentSelected : SelectedVitruvioComponents)
		{
			if (ComponentRPK != VitruvioComponentSelected->GetRpk())
			{
				return;
			}
		}
	}

	UVitruvioComponent* VitruvioComponent = nullptr;

	if (SelectedVitruvioComponents.Num() > 0)
	{
		VitruvioComponent = SelectedVitruvioComponents[0];
		DetailBuilder.GetProperty(FName(TEXT("Attributes")))->MarkHiddenByCustomization();

		if (!VitruvioComponent->InitialShape)
		{
			DetailBuilder.GetProperty(GET_MEMBER_NAME_CHECKED(UVitruvioComponent, InitialShape))->MarkHiddenByCustomization();
		}

		IDetailCategoryBuilder& RootCategory = DetailBuilder.EditCategory("Vitruvio");
		RootCategory.SetShowAdvanced(true);

		if (!VitruvioComponent->GenerateAutomatically)
		{
			AddGenerateButton(RootCategory, VitruvioComponent);
		}

		if (!VitruvioComponent->IsBatchGenerated())
		{
			AddReplacementButtons(RootCategory, VitruvioComponent);
		}

		if (VitruvioComponent->InitialShape && VitruvioComponent->CanChangeInitialShapeType())
		{
			TSharedPtr<FString> CurrentInitialShapeType;

			if (VitruvioComponent->InitialShape)
			{
				for (const auto& InitialShapeTypeOptionTuple : InitialShapeTypeMap)
				{
					UClass* IsClass = VitruvioComponent->InitialShape->GetClass();
					if (InitialShapeTypeOptionTuple.Value == IsClass)
					{
						CurrentInitialShapeType = InitialShapeTypeOptionTuple.Key;
						break;
					}
				}
			}

			AddSwitchInitialShapeCombobox(RootCategory, CurrentInitialShapeType, VitruvioComponent);
		}

		BuildAttributeEditor(DetailBuilder, RootCategory, VitruvioComponent);
	}
}

void FVitruvioComponentDetails::CustomizeDetails(const TSharedPtr<IDetailLayoutBuilder>& DetailBuilder)
{
	CachedDetailBuilder = DetailBuilder;
	CustomizeDetails(*DetailBuilder);
}

void FVitruvioComponentDetails::OnGenerateAutomaticallyChanged()
{
	IDetailLayoutBuilder* DetailBuilder = CachedDetailBuilder.Pin().Get();
	if (DetailBuilder)
	{
		DetailBuilder->ForceRefreshDetails();
	}
}

void FVitruvioComponentDetails::OnVitruvioComponentHierarchyChanged(UVitruvioComponent* Component)
{
	FLevelEditorModule& LevelEditor = FModuleManager::GetModuleChecked<FLevelEditorModule>("LevelEditor");

	const auto DetailBuilder = CachedDetailBuilder.Pin().Get();

	if (!DetailBuilder)
	{
		return;
	}

	TArray<TWeakObjectPtr<UObject>> Objects;
	DetailBuilder->GetObjectsBeingCustomized(Objects);

	if (Objects.Num() == 1)
	{
		UObject* ObjectModified = Objects[0].Get();
		AActor* Owner = nullptr;
		if (Component)
		{
			Owner = Component->GetOwner();
		}

		if (ObjectModified == Component || ObjectModified == Owner)
		{
			LevelEditor.OnComponentsEdited().Broadcast();
		}
	}
}

void FVitruvioComponentDetails::OnAttributesChanged(UObject* Object, struct FPropertyChangedEvent& Event)
{
	if (!Event.Property || Event.ChangeType == EPropertyChangeType::Interactive || !CachedDetailBuilder.IsValid())
	{
		return;
	}

	const FName PropertyName = Event.Property->GetFName();
	if (PropertyName == FName(TEXT("Attributes")))
	{
		const auto DetailBuilder = CachedDetailBuilder.Pin().Get();

		if (!DetailBuilder)
		{
			return;
		}

		TArray<TWeakObjectPtr<UObject>> Objects;
		DetailBuilder->GetObjectsBeingCustomized(Objects);

		if (Objects.Num() == 1)
		{
			UObject* ObjectModified = Objects[0].Get();
			UVitruvioComponent* Component = Cast<UVitruvioComponent>(Object);
			AActor* Owner = nullptr;
			if (Component)
			{
				Owner = Component->GetOwner();
			}

			if (ObjectModified == Component || ObjectModified == Owner)
			{
				DetailBuilder->ForceRefreshDetails();
			}
		}
	}
}
