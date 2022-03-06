// Copyright Epic Games, Inc. All Rights Reserved.

#include "NiagaraNodeFunctionCall.h"
#include "UObject/UnrealType.h"
#include "NiagaraGraph.h"
#include "NiagaraScriptSource.h"
#include "NiagaraScript.h"
#include "NiagaraNodeInput.h"
#include "NiagaraNodeOutput.h"
#include "EdGraphSchema_Niagara.h"
#include "Modules/ModuleManager.h"
#include "AssetRegistryModule.h"
#include "NiagaraComponent.h"
#include "NiagaraHlslTranslator.h"
#include "NiagaraEditorUtilities.h"
#include "NiagaraNodeParameterMapGet.h"
#include "NiagaraConstants.h"
#include "NiagaraCustomVersion.h"
#include "NiagaraDataInterfaceSkeletalMesh.h"
#include "SNiagaraGraphNodeFunctionCallWithSpecifiers.h"
#include "Misc/SecureHash.h"
#include "NiagaraMessages.h"
#include "ViewModels/Stack/NiagaraStackGraphUtilities.h"

#define LOCTEXT_NAMESPACE "NiagaraNodeFunctionCall"

void UNiagaraNodeFunctionCall::PostLoad()
{
	Super::PostLoad();

	if (FunctionScript)
	{
		FunctionScript->ConditionalPostLoad();

		// We need to make sure that the variables that could potentially be used in AllocateDefaultPins have been properly
		// loaded. Otherwise, we could be out of date.
		if (FunctionScript->GetSource())
		{
			UNiagaraScriptSource* Source = CastChecked<UNiagaraScriptSource>(FunctionScript->GetSource());
			Source->ConditionalPostLoad();
			UNiagaraGraph* Graph = Source->NodeGraph;
			Graph->ConditionalPostLoad();

			// Fix up autogenerated default values if neccessary.
			const int32 NiagaraCustomVersion = GetLinkerCustomVersion(FNiagaraCustomVersion::GUID);
			if (NiagaraCustomVersion < FNiagaraCustomVersion::EnabledAutogeneratedDefaultValuesForFunctionCallNodes)
			{
				FPinCollectorArray InputPins;
				GetInputPins(InputPins);

				TArray<UNiagaraNodeInput*> InputNodes;
				UNiagaraGraph::FFindInputNodeOptions Options;
				Options.bSort = true;
				Options.bFilterDuplicates = true;
				Graph->FindInputNodes(InputNodes, Options);

				for (UEdGraphPin* InputPin : InputPins)
				{
					auto FindInputNodeByName = [&](UNiagaraNodeInput* InputNode) { return InputNode->Input.GetName().ToString() == InputPin->PinName.ToString(); };
					UNiagaraNodeInput** MatchingInputNodePtr = InputNodes.FindByPredicate(FindInputNodeByName);
					if (MatchingInputNodePtr != nullptr)
					{
						UNiagaraNodeInput* MatchingInputNode = *MatchingInputNodePtr;
						SetPinAutoGeneratedDefaultValue(*InputPin, *MatchingInputNode);

						// If the default value wasn't set, update it with the new autogenerated default.
						if (InputPin->DefaultValue.IsEmpty())
						{
							InputPin->DefaultValue = InputPin->AutogeneratedDefaultValue;
						}
					}
				}
			}
		}
	}

	// Allow data interfaces an opportunity to intercept changes
	if (Signature.IsValid() && Signature.bMemberFunction)
	{
		if ((Signature.Inputs.Num() > 0) && Signature.Inputs[0].GetType().IsDataInterface())
		{
			UNiagaraDataInterface* CDO = CastChecked<UNiagaraDataInterface>(Signature.Inputs[0].GetType().GetClass()->GetDefaultObject());
			if (CDO->UpgradeFunctionCall(Signature))
			{
				FunctionDisplayName.Empty();
				ReallocatePins();
			}
		}
	}

	// Clean up invalid old references to propagated parameters
	CleanupPropagatedSwitchValues();
	
	if (FunctionDisplayName.IsEmpty())
	{
		ComputeNodeName();
	}
}

void UNiagaraNodeFunctionCall::UpgradeDIFunctionCalls()
{
	// We no longer use this upgrade path, but leaving here for convience in case we need to use this route again for other things
#if 0
	UClass* InterfaceClass = nullptr;
	UNiagaraDataInterface* InterfaceCDO = nullptr;
	if (Signature.IsValid() && FunctionScript == nullptr)
	{
		if (Signature.Inputs.Num() > 0)
		{
			if (Signature.Inputs[0].GetType().IsDataInterface())
			{
				InterfaceClass = Signature.Inputs[0].GetType().GetClass();
				InterfaceCDO = Cast<UNiagaraDataInterface>(InterfaceClass->GetDefaultObject());
			}
		}
	}

	FString UpgradeNote;
	if (!UpgradeNote.IsEmpty())
	{
		UE_LOG(LogNiagaraEditor, Log, TEXT("Upgradeing Niagara Data Interface fuction call node. This may cause unnessessary recompiles. Please resave these assets if this occurs. Or use fx.UpgradeAllNiagaraAssets."));
		UE_LOG(LogNiagaraEditor, Log, TEXT("Node: %s"), *GetFullName());
		if (InterfaceCDO)
		{
			UE_LOG(LogNiagaraEditor, Log, TEXT("Interface: %s"), *InterfaceCDO->GetFullName());
		}
		UE_LOG(LogNiagaraEditor, Log, TEXT("Function: %s"), *Signature.GetName());
		UE_LOG(LogNiagaraEditor, Log, TEXT("Upgrade Note: %s"),* UpgradeNote);
	}
#endif
}

TSharedPtr<SGraphNode> UNiagaraNodeFunctionCall::CreateVisualWidget()
{
	if (!FunctionScript && FunctionSpecifiers.Num() == 0)
	{
		FunctionSpecifiers = Signature.FunctionSpecifiers;
	}
	if (FunctionSpecifiers.Num() == 0)
	{
		return Super::CreateVisualWidget();
	}
	else
	{
		return SNew(SNiagaraGraphNodeFunctionCallWithSpecifiers, this);
	}
}

void UNiagaraNodeFunctionCall::PostEditChangeProperty(struct FPropertyChangedEvent& PropertyChangedEvent)
{
	if (PropertyChangedEvent.Property != nullptr)
	{
		ReallocatePins();
	}
	Super::PostEditChangeProperty(PropertyChangedEvent);

	MarkNodeRequiresSynchronization(__FUNCTION__, true);
	//GetGraph()->NotifyGraphChanged();
}

void UNiagaraNodeFunctionCall::AllocateDefaultPins()
{
	if (FunctionScriptAssetObjectPath != NAME_None && FunctionScript == nullptr)
	{
		FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		FAssetData ScriptAssetData = AssetRegistryModule.Get().GetAssetByObjectPath(FunctionScriptAssetObjectPath);
		if (ScriptAssetData.IsValid())
		{
			FunctionScript = Cast<UNiagaraScript>(ScriptAssetData.GetAsset());
		}
	}

	const UEdGraphSchema_Niagara* Schema = CastChecked<UEdGraphSchema_Niagara>(GetSchema());
	if (FunctionScript)
	{
		UNiagaraScriptSource* Source = CastChecked<UNiagaraScriptSource>(FunctionScript->GetSource());
		UNiagaraGraph* Graph = Source->NodeGraph;

		//These pins must be refreshed and kept in the correct order for the function
		TArray<FNiagaraVariable> Inputs;
		TArray<FNiagaraVariable> Outputs;
		Graph->GetParameters(Inputs, Outputs);

		TArray<UNiagaraNodeInput*> InputNodes;
		UNiagaraGraph::FFindInputNodeOptions Options;
		Options.bSort = true;
		Options.bFilterDuplicates = true;
		Graph->FindInputNodes(InputNodes, Options);

		AdvancedPinDisplay = ENodeAdvancedPins::NoPins;
		for (UNiagaraNodeInput* InputNode : InputNodes)
		{
			if (InputNode->IsExposed())
			{
				UEdGraphPin* NewPin = CreatePin(EGPD_Input, Schema->TypeDefinitionToPinType(InputNode->Input.GetType()), InputNode->Input.GetName());
					
				//An inline pin default only makes sense if we are required. 
				//Non exposed or optional inputs will used their own function input nodes defaults when not directly provided by a link.
				//Special class types cannot have an inline default.
				NewPin->bDefaultValueIsIgnored = !(InputNode->IsRequired() && InputNode->Input.GetType().GetClass() == nullptr);

				SetPinAutoGeneratedDefaultValue(*NewPin, *InputNode);
				NewPin->DefaultValue = NewPin->AutogeneratedDefaultValue;

				//TODO: Some visual indication of Auto bound pins.
				//I tried just linking to null but
// 				FNiagaraVariable AutoBoundVar;
// 				ENiagaraInputNodeUsage AutBoundUsage = ENiagaraInputNodeUsage::Undefined;
// 				bool bCanAutoBind = FindAutoBoundInput(InputNode->AutoBindOptions, NewPin, AutoBoundVar, AutBoundUsage);
// 				if (bCanAutoBind)
// 				{
// 
// 				}

				if (InputNode->IsHidden())
				{
					NewPin->bAdvancedView = true;
					AdvancedPinDisplay = ENodeAdvancedPins::Hidden;
				}
				else
				{
					NewPin->bAdvancedView = false;
				}
			}
		}

		TArray<FNiagaraVariable> SwitchNodeInputs = Graph->FindStaticSwitchInputs();
		for (FNiagaraVariable& Input : SwitchNodeInputs)
		{
			UEdGraphPin* NewPin = CreatePin(EGPD_Input, Schema->TypeDefinitionToPinType(Input.GetType()), Input.GetName());
			NewPin->bNotConnectable = true;
			NewPin->bDefaultValueIsIgnored = FindPropagatedVariable(Input) != nullptr;

			FString PinDefaultValue;
			TOptional<FNiagaraVariableMetaData> MetaData = Graph->GetMetaData(Input);
			if (MetaData.IsSet())
			{
				int32 DefaultValue = MetaData->GetStaticSwitchDefaultValue();
				Input.AllocateData();
				Input.SetValue<FNiagaraInt32>({ DefaultValue });
				
				if (Schema->TryGetPinDefaultValueFromNiagaraVariable(Input, PinDefaultValue))
				{
					NewPin->DefaultValue = PinDefaultValue;
				}
			}
			else
			{
				if (Schema->TryGetPinDefaultValueFromNiagaraVariable(Input, PinDefaultValue))
				{
					NewPin->DefaultValue = PinDefaultValue;
				}
			}
		}

		for (FNiagaraVariable& Output : Outputs)
		{
			UEdGraphPin* NewPin = CreatePin(EGPD_Output, Schema->TypeDefinitionToPinType(Output.GetType()), Output.GetName());
			NewPin->bDefaultValueIsIgnored = true;
		}

		// Make sure to note that we've synchronized with the external version.
		CachedChangeId = Graph->GetChangeID();
	}
	else
	{
		if (Signature.bRequiresExecPin)
		{
			UEdGraphPin* NewPin = CreatePin(EGPD_Input, Schema->TypeDefinitionToPinType(FNiagaraTypeDefinition::GetParameterMapDef()), TEXT(""));
			NewPin->bDefaultValueIsIgnored = true;
		}
		
		for (FNiagaraVariable& Input : Signature.Inputs)
		{
			UEdGraphPin* NewPin = CreatePin(EGPD_Input, Schema->TypeDefinitionToPinType(Input.GetType()), Input.GetName());
			NewPin->bDefaultValueIsIgnored = false;
		}

		if (Signature.bRequiresExecPin)
		{
			UEdGraphPin* NewPin = CreatePin(EGPD_Output, Schema->TypeDefinitionToPinType(FNiagaraTypeDefinition::GetParameterMapDef()), TEXT(""));
			NewPin->bDefaultValueIsIgnored = true;
		}

		for (FNiagaraVariable& Output : Signature.Outputs)
		{
			UEdGraphPin* NewPin = CreatePin(EGPD_Output, Schema->TypeDefinitionToPinType(Output.GetType()), Output.GetName());
			NewPin->bDefaultValueIsIgnored = true;
		}

		if (AllowDynamicPins())
		{
			CreateAddPin(EGPD_Input);
			CreateAddPin(EGPD_Output);
		}

		// We don't reference an external function, so set an invalid id.
		CachedChangeId = FGuid();
	}

	if (FunctionDisplayName.IsEmpty())
	{
		ComputeNodeName();
	}

	UpdateNodeErrorMessage();
}

// Returns true if this node is deprecated
bool UNiagaraNodeFunctionCall::IsDeprecated() const
{
	if (FunctionScript)
	{
		return FunctionScript->bDeprecated;
	}
	return false;
}

FText UNiagaraNodeFunctionCall::GetNodeTitle(ENodeTitleType::Type TitleType) const
{
	FString DetectedName = FunctionScript ? FunctionScript->GetName() : Signature.GetName();
	if (DetectedName.IsEmpty())
	{
		return FText::FromString(TEXT("Missing ( Was\"") + FunctionDisplayName + TEXT("\")"));
	}
	else
	{
		return FText::FromString(FName::NameToDisplayString(FunctionDisplayName, false));
	}
}

FText UNiagaraNodeFunctionCall::GetTooltipText()const
{
	if (FunctionScript != nullptr)
	{
		return FunctionScript->GetDescription();
	}
	else if (Signature.IsValid())
	{
		return Signature.Description;
	} 
	else
	{
		return LOCTEXT("NiagaraFuncCallUnknownSignatureTooltip", "Unknown function call");
	}
}

FLinearColor UNiagaraNodeFunctionCall::GetNodeTitleColor() const
{
	return UEdGraphSchema_Niagara::NodeTitleColor_FunctionCall;
}

bool UNiagaraNodeFunctionCall::CanAddToGraph(UNiagaraGraph* TargetGraph, FString& OutErrorMsg) const
{
	if (Super::CanAddToGraph(TargetGraph, OutErrorMsg) == false)
	{
		return false;
	}
	UPackage* TargetPackage = TargetGraph->GetOutermost();

	TArray<const UNiagaraGraph*> FunctionGraphs;
	UNiagaraScript* SpawningFunctionScript = FunctionScript;
	
	// We probably haven't loaded the script yet. Let's do so now so that we can trace its lineage.
	if (FunctionScriptAssetObjectPath != NAME_None && FunctionScript == nullptr)
	{
		FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		FAssetData ScriptAssetData = AssetRegistryModule.Get().GetAssetByObjectPath(FunctionScriptAssetObjectPath);
		if (ScriptAssetData.IsValid())
		{
			SpawningFunctionScript = Cast<UNiagaraScript>(ScriptAssetData.GetAsset());
		}
	}

	// Now we need to get the graphs referenced by the script that we are about to spawn in.
	if (SpawningFunctionScript && SpawningFunctionScript->GetSource())
	{
		UNiagaraScriptSource* Source = Cast<UNiagaraScriptSource>(SpawningFunctionScript->GetSource());
		if (Source)
		{
			UNiagaraGraph* FunctionGraph = Source->NodeGraph;
			if (FunctionGraph)
			{
				FunctionGraph->GetAllReferencedGraphs(FunctionGraphs);
			}
		}
	}

	// Iterate over each graph referenced by this spawning function call and see if any of them reference the graph that we are about to be spawned into. If 
	// a match is found, then adding us would introduce a cycle and we need to abort the add.
	for (const UNiagaraGraph* Graph : FunctionGraphs)
	{
		UPackage* FunctionPackage = Graph->GetOutermost();
		if (FunctionPackage != nullptr && TargetPackage != nullptr && FunctionPackage == TargetPackage)
		{
			OutErrorMsg = LOCTEXT("NiagaraFuncCallCannotAddToGraph", "Cannot add to graph because the Function Call used by this node would lead to a cycle.").ToString();
			return false;
		}
	}

	return true;
}

UNiagaraGraph* UNiagaraNodeFunctionCall::GetCalledGraph() const
{
	if (FunctionScript)
	{
		UNiagaraScriptSource* Source = CastChecked<UNiagaraScriptSource>(FunctionScript->GetSource());
		if (Source)
		{
			UNiagaraGraph* FunctionGraph = Source->NodeGraph;
			return FunctionGraph;
		}
	}

	return nullptr;
}


ENiagaraScriptUsage UNiagaraNodeFunctionCall::GetCalledUsage() const
{
	if (FunctionScript)
	{
		return FunctionScript->GetUsage();
	}
	return ENiagaraScriptUsage::Function;
}

static FText GetFormattedDeprecationMessage(const UNiagaraScript* FunctionScript, const FString& FunctionDisplayName)
{
	FFormatNamedArguments Args;
	Args.Add(TEXT("NodeName"), FText::FromString(FunctionDisplayName));

	if (FunctionScript->DeprecationRecommendation != nullptr)
	{
		Args.Add(TEXT("Recommendation"), FText::FromString(FunctionScript->DeprecationRecommendation->GetPathName()));
	}

	if (FunctionScript->DeprecationMessage.IsEmptyOrWhitespace() == false)
	{
		Args.Add(TEXT("Message"), FunctionScript->DeprecationMessage);
	}

	FText FormatString = LOCTEXT("DeprecationErrorFmtUnknown", "Function call \"{NodeName}\" is deprecated. No recommendation was provided.");

	if (FunctionScript->DeprecationRecommendation != nullptr && FunctionScript->DeprecationMessage.IsEmptyOrWhitespace() == false)
	{
		FormatString = LOCTEXT("DeprecationErrorFmtMessageAndRecommendation", "Function call \"{NodeName}\" is deprecated. Reason:\n{Message}.\nPlease use {Recommendation} instead.");
	}
	else if (FunctionScript->DeprecationRecommendation != nullptr)
	{
		FormatString = LOCTEXT("DeprecationErrorFmtRecommendation", "Function call \"{NodeName}\" is deprecated. Please use {Recommendation} instead.");
	}
	else if (FunctionScript->DeprecationMessage.IsEmptyOrWhitespace() == false)
	{
		FormatString = LOCTEXT("DeprecationErrorFmtMessage", "Function call \"{NodeName}\" is deprecated. Reason:\n{Message} ");
	}

	return FText::Format(FormatString, Args);
}

void UNiagaraNodeFunctionCall::Compile(class FHlslNiagaraTranslator* Translator, TArray<int32>& Outputs)
{
	TArray<int32> Inputs;

	bool bError = false;

	const UEdGraphSchema_Niagara* Schema = CastChecked<UEdGraphSchema_Niagara>(GetSchema());
	UNiagaraGraph* CallerGraph = GetNiagaraGraph();
	if (FunctionScript)
	{
		if (FunctionScript->bDeprecated && IsNodeEnabled())
		{
			FText DeprecationMessage = GetFormattedDeprecationMessage(FunctionScript, FunctionDisplayName);

			Translator->Warning(DeprecationMessage, this, nullptr);
		}

		FPinCollectorArray CallerInputPins;
		GetInputPins(CallerInputPins);
		
		UNiagaraScriptSource* Source = CastChecked<UNiagaraScriptSource>(FunctionScript->GetSource());
		UNiagaraGraph* FunctionGraph = Source->NodeGraph;

		TArray<UNiagaraNodeInput*> FunctionInputNodes;
		UNiagaraGraph::FFindInputNodeOptions Options;
		Options.bSort = true;
		Options.bFilterDuplicates = true;
		FunctionGraph->FindInputNodes(FunctionInputNodes, Options);

		// We check which module inputs are not used so we can later remove them from the compilation of the
		// parameter map that sets the input values for our function. This is mainly done to prevent data interfaces being
		// initialized as parameter when they are not used in the function or module.
		TSet<FName> HiddenPinNames;
		for (UEdGraphPin* Pin : FNiagaraStackGraphUtilities::GetUnusedFunctionInputPins(*this, FCompileConstantResolver(Translator)))
		{
			HiddenPinNames.Add(Pin->PinName);
		}
		Translator->EnterFunctionCallNode(HiddenPinNames);

		for (UNiagaraNodeInput* FunctionInputNode : FunctionInputNodes)
		{
			//Finds the matching Pin in the caller.
			UEdGraphPin** PinPtr = CallerInputPins.FindByPredicate([&](UEdGraphPin* InPin) { return Schema->PinToNiagaraVariable(InPin).IsEquivalent(FunctionInputNode->Input); });
			if (!PinPtr)
			{
				if (FunctionInputNode->IsExposed())
				{
					//Couldn't find the matching pin for an exposed input. Probably a stale function call node that needs to be refreshed.
					Translator->Error(LOCTEXT("StaleFunctionCallError", "Function call is stale and needs to be refreshed."), this, nullptr);
					bError = true;
				}
				else if (FunctionInputNode->ExposureOptions.bRequired == true)
				{
					// Not exposed, but required. This means we should just add as a constant.
					Inputs.Add(Translator->GetConstant(FunctionInputNode->Input));
					continue;
				}


				Inputs.Add(INDEX_NONE);
				continue;
			}

			UEdGraphPin* CallerPin = *PinPtr;
			UEdGraphPin* CallerLinkedTo = CallerPin->LinkedTo.Num() > 0 ? UNiagaraNode::TraceOutputPin(CallerPin->LinkedTo[0]) : nullptr;			
			FNiagaraVariable PinVar = Schema->PinToNiagaraVariable(CallerPin);
			if (!CallerLinkedTo)
			{
				//if (Translator->CanReadAttributes())
				{
					//Try to auto bind if we're not linked to by the caller.
					FNiagaraVariable AutoBoundVar;
					ENiagaraInputNodeUsage AutBoundUsage = ENiagaraInputNodeUsage::Undefined;
					if (FindAutoBoundInput(FunctionInputNode, CallerPin, AutoBoundVar, AutBoundUsage))
					{
						UNiagaraNodeInput* NewNode = NewObject<UNiagaraNodeInput>(CallerGraph);
						NewNode->Input = PinVar;
						NewNode->Usage = AutBoundUsage;
						NewNode->AllocateDefaultPins();
						CallerLinkedTo = NewNode->GetOutputPin(0);
						CallerPin->BreakAllPinLinks();
						CallerPin->MakeLinkTo(CallerLinkedTo);
					}
				}
			}

			if (CallerLinkedTo)
			{
				//Param is provided by the caller. Typical case.
				Inputs.Add(Translator->CompilePin(CallerPin));
				continue;
			}
			else
			{
				if (FunctionInputNode->IsRequired())
				{
					if (CallerPin->bDefaultValueIsIgnored)
					{
						//This pin can't use a default and it is required so flag an error.
						Translator->Error(FText::Format(LOCTEXT("RequiredInputUnboundErrorFmt", "Required input {0} was not bound and could not be automatically bound."), CallerPin->GetDisplayName()),
							this, CallerPin);
						bError = true;
						//We weren't linked to anything and we couldn't auto bind so tell the compiler this input isn't provided and it should use it's local default.
						Inputs.Add(INDEX_NONE);
					}
					else
					{
						//We also compile the pin anyway if it is required as we'll be attempting to use it's inline default.
						Inputs.Add(Translator->CompilePin(CallerPin));
					}
				}
				else
				{
					//We optional, weren't linked to anything and we couldn't auto bind so tell the compiler this input isn't provided and it should use it's local default.
					Inputs.Add(INDEX_NONE);
				}
			}
		}

		FCompileConstantResolver ConstantResolver(Translator);
		FNiagaraEditorUtilities::SetStaticSwitchConstants(GetCalledGraph(), CallerInputPins, ConstantResolver);
		Translator->ExitFunctionCallNode();
	}
	else if (Signature.IsValid())
	{
		if (Signature.Inputs.Num() > 0)
		{
			if (Signature.Inputs[0].GetType().IsDataInterface() && GetValidateDataInterfaces())
			{
				UClass* DIClass = Signature.Inputs[0].GetType().GetClass();
				if (UNiagaraDataInterface* DataInterfaceCDO = Cast<UNiagaraDataInterface>(DIClass->GetDefaultObject()))
				{
					TArray<FText> ValidationErrors;
					DataInterfaceCDO->ValidateFunction(Signature, ValidationErrors);

					bError = ValidationErrors.Num() > 0;

					for (FText& ValidationError : ValidationErrors)
					{
						Translator->Error(ValidationError, this, nullptr);
					}

					if (bError)
					{
						return;
					}
				}
			}
		}
		Translator->EnterFunctionCallNode(TSet<FName>());
		Signature.FunctionSpecifiers = FunctionSpecifiers;
		bError = CompileInputPins(Translator, Inputs);
		Translator->ExitFunctionCallNode();
	}		
	else
	{
		Translator->Error(FText::Format(LOCTEXT("UnknownFunction", "Unknown Function Call! Missing Script or Data Interface Signature. Stack Name: {0}"), FText::FromString(GetFunctionName())), this, nullptr);
		bError = true;
	}

	if (!bError)
	{
		Translator->FunctionCall(this, Inputs, Outputs);
	}
}

UObject*  UNiagaraNodeFunctionCall::GetReferencedAsset() const
{
	if (FunctionScript && FunctionScript->GetOutermost() != GetOutermost())
	{
		return FunctionScript;
	}
	else
	{
		return nullptr;
	}
}

void UNiagaraNodeFunctionCall::UpdateNodeErrorMessage()
{
	if (FunctionScript)
	{
		if (FunctionScript->bDeprecated)
		{
			UEdGraphNode::bHasCompilerMessage = true;
			ErrorType = EMessageSeverity::Warning;
			
			UEdGraphNode::ErrorMsg = GetFormattedDeprecationMessage(FunctionScript, FunctionDisplayName).ToString();
		}
		else if (FunctionScript->bExperimental)
		{
			UEdGraphNode::bHasCompilerMessage = true;
			UEdGraphNode::ErrorType = EMessageSeverity::Info;

			if (FunctionScript->ExperimentalMessage.IsEmptyOrWhitespace())
			{
				UEdGraphNode::NodeUpgradeMessage = LOCTEXT("FunctionExperimental", "This function is marked as experimental, use with care!");
			}
			else
			{
				FFormatNamedArguments Args;
				Args.Add(TEXT("Message"), FunctionScript->ExperimentalMessage);
				UEdGraphNode::NodeUpgradeMessage = FText::Format(LOCTEXT("FunctionExperimentalReason", "This function is marked as experimental, reason:\n{Message}."), Args);
			}
		}
		else
		{
			UEdGraphNode::bHasCompilerMessage = false;
			UEdGraphNode::ErrorMsg = FString();
		}
	}
	else if (Signature.IsValid())
	{
		if (Signature.bSoftDeprecatedFunction)
		{
			UEdGraphNode::bHasCompilerMessage = true;
			UEdGraphNode::ErrorType = EMessageSeverity::Info;

			UEdGraphNode::NodeUpgradeMessage = LOCTEXT("FunctionDeprecatedSoftly", "There is a newer version of this function, consider switching over to it.");
		}
		else if (Signature.bExperimental)
		{
			UEdGraphNode::bHasCompilerMessage = true;
			UEdGraphNode::ErrorType = EMessageSeverity::Info;

			if (Signature.ExperimentalMessage.IsEmptyOrWhitespace())
			{
				UEdGraphNode::NodeUpgradeMessage = LOCTEXT("FunctionExperimental", "This function is marked as experimental, use with care!");
			}
			else
			{
				FFormatNamedArguments Args;
				Args.Add(TEXT("Message"), Signature.ExperimentalMessage);
				UEdGraphNode::NodeUpgradeMessage = FText::Format(LOCTEXT("FunctionExperimentalReason", "This function is marked as experimental, reason:\n{Message}."), Args);
			}
		}
	}
}

bool UNiagaraNodeFunctionCall::RefreshFromExternalChanges()
{
	bool bReload = false;
	if (FunctionScript)
	{
		UNiagaraScriptSource* Source = Cast<UNiagaraScriptSource>(FunctionScript->GetSource());
		if (ensureMsgf(Source != nullptr, TEXT("No source found for FunctionScript %s in RefreshFromExternalChanges for %s"), *GetPathNameSafe(FunctionScript), *GetPathNameSafe(this)))
		{
			bReload = CachedChangeId != Source->NodeGraph->GetChangeID();
		}
	}
	else if (Signature.IsValid())
	{
		bReload = true;		
	}

	UpdateNodeErrorMessage();

	// Go over the static switch parameters to set their propagation status on the pins
	UNiagaraGraph* CalledGraph = GetCalledGraph();
	if (CalledGraph)
	{
		CleanupPropagatedSwitchValues();
		FPinCollectorArray InputPins;
		GetInputPins(InputPins);
		for (FNiagaraVariable InputVar : CalledGraph->FindStaticSwitchInputs())
		{
			for (UEdGraphPin* Pin : InputPins)
			{
				if (InputVar.GetName().IsEqual(Pin->GetFName()))
				{
					Pin->bDefaultValueIsIgnored = FindPropagatedVariable(InputVar) != nullptr;
					break;
				}
			}
		}
	}

	if (bReload)
	{
		// TODO - Leverage code in reallocate pins to determine if any pins have changed...
		ReallocatePins(false);
		return true;
	}
	else
	{
		return false;
	}
}

void UNiagaraNodeFunctionCall::SubsumeExternalDependencies(TMap<const UObject*, UObject*>& ExistingConversions)
{
	if (FunctionScript && FunctionScript->GetOutermost() != this->GetOutermost())
	{
		if (ExistingConversions.Contains(FunctionScript))
		{
			FunctionScript = CastChecked<UNiagaraScript>(ExistingConversions[FunctionScript]);
			check(FunctionScript->HasAnyFlags(RF_Standalone) == false);
			check(FunctionScript->HasAnyFlags(RF_Public) == false);
		}
		else
		{
			FunctionScript = FunctionScript->MakeRecursiveDeepCopy(this, ExistingConversions);
		}
	}
}

void UNiagaraNodeFunctionCall::GatherExternalDependencyData(ENiagaraScriptUsage InMasterUsage, const FGuid& InMasterUsageId, TArray<FNiagaraCompileHash>& InReferencedCompileHashes, TArray<FString>& InReferencedObjs) const
{
	if (FunctionScript)
	{
		UNiagaraScriptSource* Source = CastChecked<UNiagaraScriptSource>(FunctionScript->GetSource());
		UNiagaraGraph* FunctionGraph = CastChecked<UNiagaraGraph>(Source->NodeGraph);
		
		// We don't know which graph type we're referencing, so we try them all... may need to replace this with something faster in the future.
		if (FunctionGraph)
		{
			FunctionGraph->RebuildCachedCompileIds();
			for (int32 i = (int32)ENiagaraScriptUsage::Function; i <= (int32)ENiagaraScriptUsage::DynamicInput; i++)
			{
				FGuid FoundGuid = FunctionGraph->GetBaseId((ENiagaraScriptUsage)i, FGuid(0, 0, 0, 0));
				FNiagaraCompileHash FoundCompileHash = FunctionGraph->GetCompileDataHash((ENiagaraScriptUsage)i, FGuid(0, 0, 0, 0));
				if (FoundGuid.IsValid() && FoundCompileHash.IsValid())
				{
					InReferencedCompileHashes.Add(FoundCompileHash);
					InReferencedObjs.Add(FunctionGraph->GetPathName());
					FunctionGraph->GatherExternalDependencyData((ENiagaraScriptUsage)i, FGuid(0, 0, 0, 0), InReferencedCompileHashes, InReferencedObjs);
				}
			}
		}
	}
}

void UNiagaraNodeFunctionCall::UpdateCompileHashForNode(FSHA1& HashState) const
{
	Super::UpdateCompileHashForNode(HashState);
	HashState.UpdateWithString(*GetFunctionName(), GetFunctionName().Len());
}

bool UNiagaraNodeFunctionCall::ScriptIsValid() const
{
	if (FunctionScript != nullptr)
	{
		return true;
	}
	else if (Signature.IsValid())
	{
		return true;
	}
	return false;
}

void UNiagaraNodeFunctionCall::BuildParameterMapHistory(FNiagaraParameterMapHistoryBuilder& OutHistory, bool bRecursive /*= true*/, bool bFilterForCompilation /*= true*/) const
{
	Super::BuildParameterMapHistory(OutHistory, bRecursive, bFilterForCompilation);
	if (!IsNodeEnabled() && OutHistory.GetIgnoreDisabled())
	{
		RouteParameterMapAroundMe(OutHistory, bRecursive);
		return;
	}

	const UEdGraphSchema_Niagara* Schema = CastChecked<UEdGraphSchema_Niagara>(GetSchema());
	if (FunctionScript)
	{
		UNiagaraScriptSource* Source = CastChecked<UNiagaraScriptSource>(FunctionScript->GetSource());
		UNiagaraGraph* FunctionGraph = CastChecked<UNiagaraGraph>(Source->NodeGraph);

		UNiagaraNodeOutput* OutputNode = FunctionGraph->FindOutputNode(ENiagaraScriptUsage::Function);
		if (OutputNode == nullptr)
		{
			OutputNode = FunctionGraph->FindOutputNode(ENiagaraScriptUsage::Module);
		}
		if (OutputNode == nullptr)
		{
			OutputNode = FunctionGraph->FindOutputNode(ENiagaraScriptUsage::DynamicInput);
		}

		FPinCollectorArray InputPins;
		GetInputPins(InputPins);
		FNiagaraEditorUtilities::SetStaticSwitchConstants(FunctionGraph, InputPins, OutHistory.ConstantResolver);

		int32 ParamMapIdx = INDEX_NONE;
		uint32 NodeIdx = INDEX_NONE;
		const UEdGraphPin* CandidateParamMapPin = GetInputPin(0);
		if (CandidateParamMapPin && CandidateParamMapPin->LinkedTo.Num() != 0 && Schema->PinToTypeDefinition(CandidateParamMapPin) == FNiagaraTypeDefinition::GetParameterMapDef())
		{
			if (bRecursive)
			{
				ParamMapIdx = OutHistory.TraceParameterMapOutputPin(UNiagaraNode::TraceOutputPin(GetInputPin(0)->LinkedTo[0]));
			}
		}

		OutHistory.EnterFunction(GetFunctionName(), FunctionScript, FunctionGraph, this);
		if (ParamMapIdx != INDEX_NONE)
		{
			NodeIdx = OutHistory.BeginNodeVisitation(ParamMapIdx, this);
		}
		OutputNode->BuildParameterMapHistory(OutHistory, true, bFilterForCompilation);

		// Since we're about to lose the pin calling context, we finish up the function call parameter map pin wiring
		// here when we have the calling context and the child context still available to us...
		FPinCollectorArray OutputPins;
		GetOutputPins(OutputPins);

		TArray<TPair<UEdGraphPin*, int32>, TInlineAllocator<16> > MatchedPairs;

		// Find the matches of names and types of the sub-graph output pins and this function call nodes' outputs.
		for (UEdGraphPin* ChildOutputNodePin : OutputNode->GetAllPins())
		{
			FNiagaraVariable VarChild = Schema->PinToNiagaraVariable(ChildOutputNodePin);

			if (ChildOutputNodePin->LinkedTo.Num() > 0 && VarChild.GetType() == FNiagaraTypeDefinition::GetParameterMapDef())
			{
				for (int32 i = 0; i < OutputPins.Num(); i++)
				{
					FNiagaraVariable OutputVar = Schema->PinToNiagaraVariable(OutputPins[i]);
					if (OutputVar.IsEquivalent(VarChild))
					{
						TPair<UEdGraphPin*, int32> Pair;
						Pair.Key = OutputPins[i];
						Pair.Value = OutHistory.TraceParameterMapOutputPin(UNiagaraNode::TraceOutputPin(ChildOutputNodePin->LinkedTo[0]));
						MatchedPairs.Add(Pair);
					}
				}
			}
		}

		if (ParamMapIdx != INDEX_NONE)
		{
			OutHistory.EndNodeVisitation(ParamMapIdx, NodeIdx);
		}

		OutHistory.ExitFunction(GetFunctionName(), FunctionScript, this);

		for (int32 i = 0; i < MatchedPairs.Num(); i++)
		{
			OutHistory.RegisterParameterMapPin(MatchedPairs[i].Value, MatchedPairs[i].Key);
		}
	}
	else if (!ScriptIsValid() || Signature.bRequiresExecPin)
	{
		RouteParameterMapAroundMe(OutHistory, bRecursive);
	}
}

UEdGraphPin* UNiagaraNodeFunctionCall::FindParameterMapDefaultValuePin(const FName VariableName, ENiagaraScriptUsage InParentUsage, FCompileConstantResolver ConstantResolver) const
{
	if (FunctionScript)
	{
		UNiagaraScriptSource* ScriptSource = Cast<UNiagaraScriptSource>(FunctionScript->GetSource());
		if (ScriptSource != nullptr && ScriptSource->NodeGraph != nullptr)
		{
			// Set the static switch values so we traverse the correct node paths
			TArray<UEdGraphPin*> InputPins;
			GetInputPins(InputPins);
			FNiagaraEditorUtilities::SetStaticSwitchConstants(ScriptSource->NodeGraph, InputPins, ConstantResolver);
			
			return ScriptSource->NodeGraph->FindParameterMapDefaultValuePin(VariableName, FunctionScript->GetUsage(), InParentUsage);
		}
	}
	return nullptr;
}

UEdGraphPin* UNiagaraNodeFunctionCall::FindStaticSwitchInputPin(const FName& VariableName) const
{
	UNiagaraGraph* CalledGraph = GetCalledGraph();
	if (CalledGraph == nullptr)
	{
		return nullptr;
	}
	FPinCollectorArray InputPins;
	GetInputPins(InputPins);
	for (FNiagaraVariable InputVar : CalledGraph->FindStaticSwitchInputs())
	{
		if (InputVar.GetName().IsEqual(VariableName))
		{			
			for (UEdGraphPin* Pin : InputPins)
			{
				if (VariableName.IsEqual(Pin->GetFName()))
				{
					return Pin;
				}
			}
		}
	}
	return nullptr;
}

void UNiagaraNodeFunctionCall::SuggestName(FString SuggestedName, bool bForceSuggestion)
{
	ComputeNodeName(SuggestedName, bForceSuggestion);
}

UNiagaraNodeFunctionCall::FOnInputsChanged& UNiagaraNodeFunctionCall::OnInputsChanged()
{
	return OnInputsChangedDelegate;
}

FNiagaraPropagatedVariable* UNiagaraNodeFunctionCall::FindPropagatedVariable(const FNiagaraVariable& Variable)
{
	for (FNiagaraPropagatedVariable& Propagated : PropagatedStaticSwitchParameters)
	{
		if (Propagated.SwitchParameter == Variable)
		{
			return &Propagated;
		}
	}
	return nullptr;
}

void UNiagaraNodeFunctionCall::RemovePropagatedVariable(const FNiagaraVariable& Variable)
{
	for (int i = 0; i < PropagatedStaticSwitchParameters.Num(); i++)
	{
		if (PropagatedStaticSwitchParameters[i].SwitchParameter == Variable)
		{
			PropagatedStaticSwitchParameters.RemoveAt(i);
			return;
		}
	}
}

ENiagaraNumericOutputTypeSelectionMode UNiagaraNodeFunctionCall::GetNumericOutputTypeSelectionMode() const
{
	if (FunctionScript)
	{
		return FunctionScript->NumericOutputTypeSelectionMode;
	}	
	return ENiagaraNumericOutputTypeSelectionMode::None;
}

void UNiagaraNodeFunctionCall::AutowireNewNode(UEdGraphPin* FromPin)
{
	UNiagaraNode::AutowireNewNode(FromPin);
	ComputeNodeName();
}

void UNiagaraNodeFunctionCall::ComputeNodeName(FString SuggestedName, bool bForceSuggestion)
{
	FString FunctionName = FunctionScript ? FunctionScript->GetName() : Signature.GetName();
	FName ProposedName;
	if (SuggestedName.IsEmpty() == false)
	{ 
		// If we have a suggested name and and either there is no function name, or it is a permutation of the function name
		// it can be used as the proposed name.
		if (bForceSuggestion || FunctionName.IsEmpty() || SuggestedName == FunctionName || (SuggestedName.StartsWith(FunctionName) && SuggestedName.RightChop(FunctionName.Len()).IsNumeric()))
		{
			ProposedName = *SuggestedName;
		}
	}

	if(ProposedName == NAME_None)
	{
		const FString CurrentName = FunctionDisplayName;
		if (FunctionName.IsEmpty() == false)
		{
			ProposedName = *FunctionName;
		}
		else
		{
			ProposedName = *CurrentName;
		}
	}

	UNiagaraGraph* Graph = GetNiagaraGraph();
	TArray<UNiagaraNodeFunctionCall*> Nodes;
	Graph->GetNodesOfClass(Nodes);

	TSet<FName> Names;
	for (UNiagaraNodeFunctionCall* Node : Nodes)
	{
		CA_ASSUME(Node != nullptr);
		if (Node != this)
		{
			Names.Add(*Node->GetFunctionName());
		}
	}

	FString NewName = FNiagaraUtilities::GetUniqueName(ProposedName, Names).ToString();
	if (!FunctionDisplayName.Equals(NewName))
	{
		FunctionDisplayName = NewName;
	}
}

void UNiagaraNodeFunctionCall::SetPinAutoGeneratedDefaultValue(UEdGraphPin& FunctionInputPin, UNiagaraNodeInput& FunctionScriptInputNode)
{
	if (FunctionInputPin.bDefaultValueIsIgnored == false)
	{
		FPinCollectorArray InputPins;
		FunctionScriptInputNode.GetInputPins(InputPins);
		if (InputPins.Num() == 1 && InputPins[0]->bDefaultValueIsIgnored == false)
		{
			// If the function graph's input node had an input pin, and that pin's default wasn't ignored, use that value. 
			FunctionInputPin.AutogeneratedDefaultValue = InputPins[0]->DefaultValue;
		}
		else
		{
			const UEdGraphSchema_Niagara* Schema = CastChecked<UEdGraphSchema_Niagara>(GetSchema());
			FString PinDefaultValue;
			if (Schema->TryGetPinDefaultValueFromNiagaraVariable(FunctionScriptInputNode.Input, PinDefaultValue))
			{
				FunctionInputPin.AutogeneratedDefaultValue = PinDefaultValue;
			}
		}
	}
}

void UNiagaraNodeFunctionCall::CleanupPropagatedSwitchValues()
{
	for (int i = PropagatedStaticSwitchParameters.Num() - 1; i >= 0; i--)
	{
		FNiagaraPropagatedVariable& Propagated = PropagatedStaticSwitchParameters[i];
		if (Propagated.SwitchParameter.GetName().IsNone() || !IsValidPropagatedVariable(Propagated.SwitchParameter))
		{
			PropagatedStaticSwitchParameters.RemoveAt(i);
		}
	}
}

bool UNiagaraNodeFunctionCall::IsValidPropagatedVariable(const FNiagaraVariable& Variable) const
{
	UNiagaraGraph* Graph = GetCalledGraph();
	if (!Graph)
	{
		return false;
	}
	for (const FNiagaraVariable& Var : Graph->FindStaticSwitchInputs(false))
	{
		if (Var == Variable)
		{
			return true;
		}
	}
	return false;
}

bool UNiagaraNodeFunctionCall::FindAutoBoundInput(UNiagaraNodeInput* InputNode, UEdGraphPin* PinToAutoBind, FNiagaraVariable& OutFoundVar, ENiagaraInputNodeUsage& OutNodeUsage)
{
	check(InputNode && InputNode->IsExposed());
	if (PinToAutoBind->LinkedTo.Num() > 0 || !InputNode->CanAutoBind())
	{
		return false;
	}

	const UEdGraphSchema_Niagara* Schema = CastChecked<UEdGraphSchema_Niagara>(GetSchema());
	FNiagaraVariable PinVar = Schema->PinToNiagaraVariable(PinToAutoBind);

	//See if we can auto bind this pin to something in the caller script.
	UNiagaraGraph* CallerGraph = GetNiagaraGraph();
	check(CallerGraph);
	UNiagaraNodeOutput* CallerOutputNodeSpawn = CallerGraph->FindOutputNode(ENiagaraScriptUsage::ParticleSpawnScript);
	UNiagaraNodeOutput* CallerOutputNodeUpdate = CallerGraph->FindOutputNode(ENiagaraScriptUsage::ParticleUpdateScript);

	//First, let's see if we're an attribute of this emitter. Only valid if we're a module call off the primary script.
	if (CallerOutputNodeSpawn || CallerOutputNodeUpdate)
	{
		UNiagaraNodeOutput* CallerOutputNode = CallerOutputNodeSpawn != nullptr ? CallerOutputNodeSpawn : CallerOutputNodeUpdate;
		check(CallerOutputNode);
		{
			FNiagaraVariable* AttrVarPtr = CallerOutputNode->Outputs.FindByPredicate([&](const FNiagaraVariable& Attr) { return PinVar.IsEquivalent(Attr); });
			if (AttrVarPtr)
			{
				OutFoundVar = *AttrVarPtr;
				OutNodeUsage = ENiagaraInputNodeUsage::Attribute;
				return true;
			}
		}
	}
	
	//Next, lets see if we are a system constant.
	//Do we need a smarter (possibly contextual) handling of system constants?
	const TArray<FNiagaraVariable>& SysConstants = FNiagaraConstants::GetEngineConstants();
	if (SysConstants.Contains(PinVar))
	{
		OutFoundVar = PinVar;
		OutNodeUsage = ENiagaraInputNodeUsage::SystemConstant;
		return true;
	}

	//Unable to auto bind.
	return false;
}

#undef LOCTEXT_NAMESPACE