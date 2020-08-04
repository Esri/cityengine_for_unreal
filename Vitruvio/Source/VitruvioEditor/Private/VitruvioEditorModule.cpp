// Copyright 2019 - 2020 Esri. All Rights Reserved.

#include "VitruvioEditorModule.h"

#include "RulePackageAssetTypeActions.h"
#include "VitruvioComponentDetails.h"

#include "AssetToolsModule.h"
#include "Core.h"
#include "IAssetTools.h"
#include "Modules/ModuleManager.h"

#define LOCTEXT_NAMESPACE "VitruvioEditorModule"

void VitruvioEditorModule::StartupModule()
{
	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
	AssetTools.RegisterAssetTypeActions(MakeShareable(new FRulePackageAssetTypeActions()));

	FPropertyEditorModule& PropertyModule = FModuleManager::GetModuleChecked<FPropertyEditorModule>("PropertyEditor");
	PropertyModule.RegisterCustomClassLayout(UVitruvioComponent::StaticClass()->GetFName(),
											 FOnGetDetailCustomizationInstance::CreateStatic(&FVitruvioComponentDetails::MakeInstance));
}

void VitruvioEditorModule::ShutdownModule()
{
	FPropertyEditorModule& PropertyModule = FModuleManager::GetModuleChecked<FPropertyEditorModule>("PropertyEditor");
	PropertyModule.UnregisterCustomClassLayout(UVitruvioComponent::StaticClass()->GetFName());
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(VitruvioEditorModule, VitruvioEditor)
