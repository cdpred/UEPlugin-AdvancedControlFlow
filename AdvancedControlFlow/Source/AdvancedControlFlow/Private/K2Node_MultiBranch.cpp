#pragma once

#include "K2Node_MultiBranch.h"

#include "BlueprintNodeSpawner.h"
#include "EdGraphSchema_K2.h"
#include "EdGraphUtilities.h"
#include "EditorCategoryUtils.h"
#include "GraphEditorSettings.h"
#include "Kismet/KismetMathLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "KismetCompiledFunctionContext.h"
#include "KismetCompiler.h"
#include "KismetCompilerMisc.h"
#include "ToolMenu.h"

#define LOCTEXT_NAMESPACE "K2Node"

namespace
{
static const FName DefaultExecPinName(TEXT("DefaultExec"));
static const FName CaseExecPinNamePrefix(TEXT("CaseExec"));
static const FName CaseCondPinNamePrefix(TEXT("CaseCond"));
static const FName DefaultExecPinFriendlyName(TEXT("Default"));
static const FName CaseExecPinFriendlyNamePrefix(TEXT("Case"));
static const FName CaseCondPinFriendlyNamePrefix(TEXT("Case"));
}	 // namespace

class FKCHandler_MultiBranch : public FNodeHandlingFunctor
{
	TMap<UEdGraphNode*, FBPTerminal*> BoolTermMap;

public:
	FKCHandler_MultiBranch(FKismetCompilerContext& InCompilerContext) : FNodeHandlingFunctor(InCompilerContext)
	{
	}

	virtual void RegisterNets(FKismetFunctionContext& Context, UEdGraphNode* Node) override
	{
		UK2Node_MultiBranch* MultiBranchNode = CastChecked<UK2Node_MultiBranch>(Node);

		FNodeHandlingFunctor::RegisterNets(Context, Node);

		FBPTerminal* BoolTerm = Context.CreateLocalTerminal();
		BoolTerm->Type.PinCategory = UEdGraphSchema_K2::PC_Boolean;
		BoolTerm->Source = Node;
		BoolTerm->Name = Context.NetNameMap->MakeValidName(Node, TEXT("Inverted"));
		BoolTermMap.Add(Node, BoolTerm);
	}

	virtual void Compile(FKismetFunctionContext& Context, UEdGraphNode* Node) override
	{
		UK2Node_MultiBranch* MultiBranchNode = CastChecked<UK2Node_MultiBranch>(Node);

		FEdGraphPinType ExpectedExecPinType;
		ExpectedExecPinType.PinCategory = UEdGraphSchema_K2::PC_Exec;

		{
			UEdGraphPin* ExecTriggeringPin =
				Context.FindRequiredPinByName(MultiBranchNode, UEdGraphSchema_K2::PN_Execute, EGPD_Input);
			if ((ExecTriggeringPin == nullptr) || !Context.ValidatePinType(ExecTriggeringPin, ExpectedExecPinType))
			{
				CompilerContext.MessageLog.Error(
					*LOCTEXT("NoValidExecutionPinForMultiBranch_Error", "@@ must have a valid execution pin @@").ToString(),
					MultiBranchNode, ExecTriggeringPin);
				return;
			}
			else if (ExecTriggeringPin->LinkedTo.Num() == 0)
			{
				CompilerContext.MessageLog.Warning(
					*LOCTEXT("NodeNeverExecuted_Warning", "@@ will never be executed").ToString(), MultiBranchNode);
				return;
			}
		}

		UEdGraphPin* DefaultExecPin = MultiBranchNode->GetDefaultExecPin();

		UEdGraphPin* FunctionPin = MultiBranchNode->GetFunctionPin();
		FBPTerminal* FunctionContext = Context.NetMap.FindRef(FunctionPin);
		UClass* FunctionClass = Cast<UClass>(FunctionPin->PinType.PinSubCategoryObject.Get());
		UFunction* FunctionPtr = FindUField<UFunction>(FunctionClass, FunctionPin->PinName);
		check(FunctionPtr);

		FBPTerminal* BoolTerm = BoolTermMap.FindRef(MultiBranchNode);

		for (auto PinIt = MultiBranchNode->Pins.CreateIterator(); PinIt; ++PinIt)
		{
			UEdGraphPin* ExecPin = *PinIt;

			if (ExecPin->Direction != EGPD_Output)
			{
				continue;
			}
			if (ExecPin->GetFName() == DefaultExecPinName)
			{
				continue;
			}
			if (ExecPin->LinkedTo.Num() == 0)
			{
				continue;
			}

			UEdGraphPin* CondPin = MultiBranchNode->GetCondPinFromExecPin(ExecPin);
			UEdGraphPin* CondNet = FEdGraphUtilities::GetNetFromPin(CondPin);
			FBPTerminal* CondValueTerm = Context.NetMap.FindRef(CondNet);

			// Goto if Not_PreBool(Cond)
			{
				FBlueprintCompiledStatement& CallFuncStatement = Context.AppendStatementForNode(MultiBranchNode);
				CallFuncStatement.Type = KCST_CallFunction;
				CallFuncStatement.FunctionToCall = FunctionPtr;
				CallFuncStatement.FunctionContext = FunctionContext;
				CallFuncStatement.bIsParentContext = false;
				CallFuncStatement.LHS = BoolTerm;
				CallFuncStatement.RHS.Add(CondValueTerm);

				FBlueprintCompiledStatement& GotoStatement = Context.AppendStatementForNode(MultiBranchNode);
				GotoStatement.Type = KCST_GotoIfNot;
				GotoStatement.LHS = BoolTerm;

				Context.GotoFixupRequestMap.Add(&GotoStatement, ExecPin);
			}
		}

		// Goto default
		GenerateSimpleThenGoto(Context, *MultiBranchNode, DefaultExecPin);
	}
};

UK2Node_MultiBranch::UK2Node_MultiBranch(const FObjectInitializer& ObjectInitializer) : Super(ObjectInitializer)
{
	ConditionPreProcessFuncClass = UKismetMathLibrary::StaticClass();
	ConditionPreProcessFuncName = TEXT("Not_PreBool");
}

void UK2Node_MultiBranch::AllocateDefaultPins()
{
	// Pin structure
	//   N: Number of case pin pair
	// -----
	// 0: Execution Triggering (In, Exec)
	// 1: Default Execution (Out, Exec)
	// 2: Internal function (Hidden, Object)
	// 3 - 2+N: Case Conditional (In, Boolean)
	// 2+N+1 - 2*(N+1): Case Execution (Out, Exec)

	CreateExecTriggeringPin();

	CreateDefaultExecPin();

	CreateFunctionPin();

	Super::AllocateDefaultPins();
}

FText UK2Node_MultiBranch::GetTooltipText() const
{
	return LOCTEXT("MultiBranchStatement_Tooltip", "Multi Branch Statement\nExecution goes where condition is true");
}

FLinearColor UK2Node_MultiBranch::GetNodeTitleColor() const
{
	return GetDefault<UGraphEditorSettings>()->ExecBranchNodeTitleColor;
}

FText UK2Node_MultiBranch::GetNodeTitle(ENodeTitleType::Type TitleType) const
{
	return LOCTEXT("MultiBranch", "MultiBranch");
}

FSlateIcon UK2Node_MultiBranch::GetIconAndTint(FLinearColor& OutColor) const
{
	static FSlateIcon Icon("EditorStyle", "GraphEditor.Switch_16x");
	return Icon;
}

void UK2Node_MultiBranch::GetNodeContextMenuActions(class UToolMenu* Menu, class UGraphNodeContextMenuContext* Context) const
{
	Super::GetNodeContextMenuActions(Menu, Context);

	if (!Context->bIsDebugging)
	{
		FToolMenuSection& Section =
			Menu->AddSection("K2NodeMultiBranch", NSLOCTEXT("K2Nodes", "MultiBranchHeader", "Multi Branch"));

		if (Context->Node->Pins.Num() >= 1)
		{
			Section.AddMenuEntry("AddCasePinBefore", LOCTEXT("AddCasePinBefore", "Add case pin before"),
				LOCTEXT("AddCasePinBeforeTooltip", "Add case pin before this pin on this node"), FSlateIcon(),
				FUIAction(FExecuteAction::CreateUObject(const_cast<UK2Node_MultiBranch*>(this),
					&UK2Node_MultiBranch::AddCasePinBefore, const_cast<UEdGraphPin*>(Context->Pin))));
			Section.AddMenuEntry("AddCasePinAfter", LOCTEXT("AddCasePinAfter", "Add case pin after"),
				LOCTEXT("AddCasePinAfterTooltip", "Add case pin after this pin on this node"), FSlateIcon(),
				FUIAction(FExecuteAction::CreateUObject(const_cast<UK2Node_MultiBranch*>(this),
					&UK2Node_MultiBranch::AddCasePinAfter, const_cast<UEdGraphPin*>(Context->Pin))));
			Section.AddMenuEntry("RemoveFirstCasePin", LOCTEXT("RemoveFirstCasePin", "Remove first case pin"),
				LOCTEXT("RemoveFirstCasePinTooltip", "Remove first case pin on this node"), FSlateIcon(),
				FUIAction(FExecuteAction::CreateUObject(
					const_cast<UK2Node_MultiBranch*>(this), &UK2Node_MultiBranch::RemoveFirstCasePin)));
			Section.AddMenuEntry("RemoveLastCasePin", LOCTEXT("RemoveLastCasePin", "Remove last case pin"),
				LOCTEXT("RemoveLastCasePinTooltip", "Remove last case pin on this node"), FSlateIcon(),
				FUIAction(FExecuteAction::CreateUObject(
					const_cast<UK2Node_MultiBranch*>(this), &UK2Node_MultiBranch::RemoveLastCasePin)));
		}

		if (Context->Pin != nullptr)
		{
			Section.AddMenuEntry("RemoveThisCasePin", LOCTEXT("RemoveThisCasePin", "Remove this case pin"),
				LOCTEXT("RemoveThisCasePinTooltip", "Remove this case pin on this node"), FSlateIcon(),
				FUIAction(FExecuteAction::CreateUObject(const_cast<UK2Node_MultiBranch*>(this),
					&UK2Node_MultiBranch::RemoveInputPin, const_cast<UEdGraphPin*>(Context->Pin))));
		}
	}
}

void UK2Node_MultiBranch::ReallocatePinsDuringReconstruction(TArray<UEdGraphPin*>& OldPins)
{
	Super::AllocateDefaultPins();

	CreateExecTriggeringPin();

	CreateDefaultExecPin();

	CreateFunctionPin();

	int32 CasePinCount = 0;
	for (auto& Pin : OldPins)
	{
		if (UEdGraphSchema_K2::IsExecPin(*Pin) && (Pin->Direction == EGPD_Output) && IsCasePin(Pin))
		{
			++CasePinCount;
		}
	}

	for (int32 Index = 0; Index < CasePinCount; ++Index)
	{
		AddCasePinPair(Index);
	}
}

class FNodeHandlingFunctor* UK2Node_MultiBranch::CreateNodeHandler(class FKismetCompilerContext& CompilerContext) const
{
	return new FKCHandler_MultiBranch(CompilerContext);
}

void UK2Node_MultiBranch::GetMenuActions(FBlueprintActionDatabaseRegistrar& ActionRegistrar) const
{
	UClass* ActionKey = GetClass();
	if (ActionRegistrar.IsOpenForRegistration(ActionKey))
	{
		UBlueprintNodeSpawner* NodeSpawner = UBlueprintNodeSpawner::Create(GetClass());
		check(NodeSpawner != nullptr);

		ActionRegistrar.AddBlueprintAction(ActionKey, NodeSpawner);
	}
}

FText UK2Node_MultiBranch::GetMenuCategory() const
{
	return FEditorCategoryUtils::GetCommonCategory(FCommonEditorCategory::FlowControl);
}

void UK2Node_MultiBranch::AddInputPin()
{
	Modify();

	int32 N = GetCasePinCount();

	AddCasePinPair(N);
}

void UK2Node_MultiBranch::RemoveInputPin(UEdGraphPin* Pin)
{
	if (Pin == nullptr)
	{
		return;
	}

	UK2Node_MultiBranch* OwnerNode = Cast<UK2Node_MultiBranch>(Pin->GetOwningNode());

	if (OwnerNode)
	{
		Modify();

		int32 CaseIndex = GetCaseIndexFromCasePin(Pin);
		RemoveCasePinAt(CaseIndex);
	}
}

void UK2Node_MultiBranch::AddCasePinAfter(UEdGraphPin* Pin)
{
	if (Pin == nullptr)
	{
		return;
	}

	UK2Node_MultiBranch* OwnerNode = Cast<UK2Node_MultiBranch>(Pin->GetOwningNode());

	if (OwnerNode)
	{
		Modify();

		CasePinPair Pair = GetCasePinPair(Pin);

		UEdGraphPin* CaseCondAfterPin = Pair.Key;
		UEdGraphPin* CaseExecAfterPin = Pair.Value;
		int32 CaseIndexAfter = GetCaseIndexFromCaseCondPin(CaseCondAfterPin);
		check(CaseIndexAfter == GetCaseIndexFromCaseExecPin(CaseExecAfterPin));

		// Get current cond-exec pin pair.
		TArray<CasePinPair> CasePairs = GetCasePinPairs();

		// Add new pin pair.
		AddCasePinPair(CaseIndexAfter + 1);

		// Restore cond-exec pin pair name.
		for (int32 Index = CaseIndexAfter + 1; Index < CasePairs.Num(); ++Index)
		{
			UEdGraphPin* CaseCondPin = CasePairs[Index].Key;
			UEdGraphPin* CaseExecPin = CasePairs[Index].Value;

			CaseExecPin->PinName = *GetCasePinName(CaseExecPinNamePrefix.ToString(), Index + 1);
			CaseExecPin->PinFriendlyName =
				FText::AsCultureInvariant(GetCasePinFriendlyName(CaseExecPinFriendlyNamePrefix.ToString(), Index + 1));
			CaseCondPin->PinName = *GetCasePinName(CaseCondPinNamePrefix.ToString(), Index + 1);
			CaseCondPin->PinFriendlyName =
				FText::AsCultureInvariant(GetCasePinFriendlyName(CaseCondPinFriendlyNamePrefix.ToString(), Index + 1));
		}

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(GetBlueprint());
	}
}

void UK2Node_MultiBranch::AddCasePinBefore(UEdGraphPin* Pin)
{
	if (Pin == nullptr)
	{
		return;
	}

	UK2Node_MultiBranch* OwnerNode = Cast<UK2Node_MultiBranch>(Pin->GetOwningNode());

	if (OwnerNode)
	{
		Modify();

		CasePinPair Pair = GetCasePinPair(Pin);

		UEdGraphPin* CaseCondBeforePin = Pair.Key;
		UEdGraphPin* CaseExecBeforePin = Pair.Value;
		int32 CaseIndexBefore = GetCaseIndexFromCaseCondPin(CaseCondBeforePin);
		check(CaseIndexBefore == GetCaseIndexFromCaseExecPin(CaseExecBeforePin));

		// Get current cond-exec pin pair.
		TArray<CasePinPair> CasePairs = GetCasePinPairs();

		// Add new pin pair.
		AddCasePinPair(CaseIndexBefore);

		// Restore cond-exec pin pair name.
		for (int32 Index = CaseIndexBefore; Index < CasePairs.Num(); ++Index)
		{
			UEdGraphPin* CaseCondPin = CasePairs[Index].Key;
			UEdGraphPin* CaseExecPin = CasePairs[Index].Value;

			CaseExecPin->PinName = *GetCasePinName(CaseExecPinNamePrefix.ToString(), Index + 1);
			CaseExecPin->PinFriendlyName =
				FText::AsCultureInvariant(GetCasePinFriendlyName(CaseExecPinFriendlyNamePrefix.ToString(), Index + 1));
			CaseCondPin->PinName = *GetCasePinName(CaseCondPinNamePrefix.ToString(), Index + 1);
			CaseCondPin->PinFriendlyName =
				FText::AsCultureInvariant(GetCasePinFriendlyName(CaseCondPinFriendlyNamePrefix.ToString(), Index + 1));
		}

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(GetBlueprint());
	}
}

void UK2Node_MultiBranch::RemoveFirstCasePin()
{
	Modify();

	RemoveCasePinAt(0);
}

void UK2Node_MultiBranch::RemoveLastCasePin()
{
	Modify();

	RemoveCasePinAt(GetCasePinCount() - 1);
}

UEdGraphPin* UK2Node_MultiBranch::GetCaseCondPinFromCaseIndex(int32 CaseIndex) const
{
	for (auto& P : Pins)
	{
		if (!UEdGraphSchema_K2::IsExecPin(*P) && (P->Direction == EGPD_Input) && IsCasePin(P))
		{
			int32 ActualIndex = GetCaseIndexFromCaseCondPin(P);

			if ((ActualIndex != -1) && (ActualIndex == CaseIndex))
			{
				return P;
			}
		}
	}

	return nullptr;
}

UEdGraphPin* UK2Node_MultiBranch::GetCaseExecPinFromCaseIndex(int32 CaseIndex) const
{
	for (auto& P : Pins)
	{
		if (UEdGraphSchema_K2::IsExecPin(*P) && (P->Direction == EGPD_Output) && IsCasePin(P))
		{
			int32 ActualIndex = GetCaseIndexFromCaseExecPin(P);

			if ((ActualIndex != -1) && (ActualIndex == CaseIndex))
			{
				return P;
			}
		}
	}

	return nullptr;
}

CasePinPair UK2Node_MultiBranch::GetCasePinPair(UEdGraphPin* Pin) const
{
	CasePinPair Pair;

	if (UEdGraphSchema_K2::IsExecPin(*Pin))
	{
		Pair.Key = GetCondPinFromExecPin(Pin);
		Pair.Value = Pin;
		check(Pair.Key);
	}
	else
	{
		Pair.Key = Pin;
		Pair.Value = GetExecPinFromCondPin(Pin);
		check(Pair.Value);
	}

	return Pair;
}

int32 UK2Node_MultiBranch::GetCaseIndexFromCasePin(UEdGraphPin* Pin) const
{
	check(IsCasePin(Pin));

	if (UEdGraphSchema_K2::IsExecPin(*Pin))
	{
		return GetCaseIndexFromCaseExecPin(Pin);
	}

	return GetCaseIndexFromCaseCondPin(Pin);
}

int32 UK2Node_MultiBranch::GetCaseIndexFromCasePin(const FString& Prefix, UEdGraphPin* Pin) const
{
	check(IsCasePin(Pin));

	FString PinName = Pin->GetFName().ToString();
	FString Dummy;
	FString IndexStr;
	if (!PinName.Split(Prefix + "_", &Dummy, &IndexStr, ESearchCase::CaseSensitive))
	{
		return -1;
	}

	return FCString::Atoi(*IndexStr);
}

int32 UK2Node_MultiBranch::GetCaseIndexFromCaseExecPin(UEdGraphPin* Pin) const
{
	check(Pin->Direction == EGPD_Output);
	check(IsCasePin(Pin));
	check(UEdGraphSchema_K2::IsExecPin(*Pin));

	return GetCaseIndexFromCasePin(CaseExecPinNamePrefix.ToString(), Pin);
}

int32 UK2Node_MultiBranch::GetCaseIndexFromCaseCondPin(UEdGraphPin* Pin) const
{
	check(Pin->Direction == EGPD_Input);
	check(IsCasePin(Pin));
	check(!UEdGraphSchema_K2::IsExecPin(*Pin));

	return GetCaseIndexFromCasePin(CaseCondPinNamePrefix.ToString(), Pin);
}

void UK2Node_MultiBranch::RemoveCasePinAt(int32 CaseIndex)
{
	UEdGraphPin* CaseExecPinToRemove = GetCaseExecPinFromCaseIndex(CaseIndex);
	UEdGraphPin* CaseCondPinToRemove = GetCaseCondPinFromCaseIndex(CaseIndex);
	check(CaseExecPinToRemove);
	check(CaseCondPinToRemove);

	Pins.Remove(CaseExecPinToRemove);
	Pins.Remove(CaseCondPinToRemove);
	CaseExecPinToRemove->MarkAsGarbage();
	CaseCondPinToRemove->MarkAsGarbage();

	int32 Index = 0;
	for (auto& P : Pins)
	{
		if (UEdGraphSchema_K2::IsExecPin(*P) && (P->Direction == EGPD_Output) && IsCasePin(P))
		{
			UEdGraphPin* CaseExecPin = P;
			UEdGraphPin* CaseCondPin = GetCondPinFromExecPin(CaseExecPin);

			CaseExecPin->PinName = *GetCasePinName(CaseExecPinNamePrefix.ToString(), Index);
			CaseExecPin->PinFriendlyName =
				FText::AsCultureInvariant(GetCasePinFriendlyName(CaseExecPinFriendlyNamePrefix.ToString(), Index));
			CaseCondPin->PinName = *GetCasePinName(CaseCondPinNamePrefix.ToString(), Index);
			CaseCondPin->PinFriendlyName =
				FText::AsCultureInvariant(GetCasePinFriendlyName(CaseCondPinFriendlyNamePrefix.ToString(), Index));

			++Index;
		}
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(GetBlueprint());
}

int32 UK2Node_MultiBranch::GetCasePinCount() const
{
	// TODO: need to optimize.
	for (int32 Index = Pins.Num() / 2; Index >= 0; --Index)
	{
		UEdGraphPin* CaseCondPinIndex = GetCaseCondPinFromCaseIndex(Index);
		UEdGraphPin* CaseExecPinIndex = GetCaseExecPinFromCaseIndex(Index);

		if ((CaseCondPinIndex != nullptr) && (CaseExecPinIndex != nullptr))
		{
			return Index + 1;
		}
	}

	return 0;
}

TArray<CasePinPair> UK2Node_MultiBranch::GetCasePinPairs() const
{
	TArray<CasePinPair> CasePairs;
	CasePairs.SetNum(GetCasePinCount());

	for (auto& P : Pins)
	{
		if (UEdGraphSchema_K2::IsExecPin(*P) && (P->Direction == EGPD_Output) && IsCasePin(P))
		{
			UEdGraphPin* CaseExecPin = P;
			int32 Index = GetCaseIndexFromCaseExecPin(CaseExecPin);
			CasePairs[Index] = GetCasePinPair(CaseExecPin);
		}
	}

	return CasePairs;
}

void UK2Node_MultiBranch::AddCasePinAt(int32 Index)
{
}

bool UK2Node_MultiBranch::IsCasePin(UEdGraphPin* Pin) const
{
	TArray<FName> NonCasePinName = {
		UEdGraphSchema_K2::PN_Execute,
		DefaultExecPinName,
		ConditionPreProcessFuncName,
	};

	for (auto& Name : NonCasePinName)
	{
		if (Pin->GetFName() == Name)
		{
			return false;
		}
	}

	return true;
}

CasePinPair UK2Node_MultiBranch::AddCasePinPair(int32 CaseIndex)
{
	CasePinPair Pair;
	int N = GetCasePinCount();

	{
		FCreatePinParams Params;
		Params.Index = 3 + CaseIndex;
		Pair.Key = CreatePin(
			EGPD_Input, UEdGraphSchema_K2::PC_Boolean, *GetCasePinName(CaseCondPinNamePrefix.ToString(), CaseIndex), Params);
		Pair.Key->PinFriendlyName =
			FText::AsCultureInvariant(GetCasePinFriendlyName(CaseCondPinFriendlyNamePrefix.ToString(), CaseIndex));
	}
	{
		FCreatePinParams Params;
		Params.Index = 3 + N + CaseIndex + 1;
		Pair.Value = CreatePin(
			EGPD_Output, UEdGraphSchema_K2::PC_Exec, *GetCasePinName(CaseExecPinNamePrefix.ToString(), CaseIndex), Params);
		Pair.Value->PinFriendlyName =
			FText::AsCultureInvariant(GetCasePinFriendlyName(CaseExecPinFriendlyNamePrefix.ToString(), CaseIndex));
	}

	return Pair;
}

FString UK2Node_MultiBranch::GetCasePinName(const FString& Prefix, int32 CaseIndex) const
{
	return FString::Printf(TEXT("%s_%d"), *Prefix, CaseIndex);
}

FString UK2Node_MultiBranch::GetCasePinFriendlyName(const FString& Prefix, int32 CaseIndex) const
{
	return FString::Printf(TEXT("%s %d"), *Prefix, CaseIndex);
}

UEdGraphPin* UK2Node_MultiBranch::GetDefaultExecPin() const
{
	return FindPin(DefaultExecPinName);
}

UEdGraphPin* UK2Node_MultiBranch::GetFunctionPin() const
{
	return FindPin(ConditionPreProcessFuncName);
}

UEdGraphPin* UK2Node_MultiBranch::GetCondPinFromExecPin(const UEdGraphPin* ExecPin) const
{
	FString ExecPinName = ExecPin->GetFName().ToString();

	FString Dummy;
	FString Suffix;
	if (!ExecPinName.Split(CaseExecPinNamePrefix.ToString(), &Dummy, &Suffix, ESearchCase::CaseSensitive))
	{
		return nullptr;
	}

	return FindPin(CaseCondPinNamePrefix.ToString() + Suffix);
}

UEdGraphPin* UK2Node_MultiBranch::GetExecPinFromCondPin(const UEdGraphPin* CondPin) const
{
	FString CondPinName = CondPin->GetFName().ToString();

	FString Dummy;
	FString Suffix;
	if (!CondPinName.Split(CaseCondPinNamePrefix.ToString(), &Dummy, &Suffix, ESearchCase::CaseSensitive))
	{
		return nullptr;
	}

	return FindPin(CaseExecPinNamePrefix.ToString() + Suffix);
}

void UK2Node_MultiBranch::CreateExecTriggeringPin()
{
	FCreatePinParams Params;
	Params.Index = 0;
	CreatePin(EGPD_Input, UEdGraphSchema_K2::PC_Exec, UEdGraphSchema_K2::PN_Execute, Params);
}

void UK2Node_MultiBranch::CreateDefaultExecPin()
{
	FCreatePinParams Params;
	Params.Index = 1;
	UEdGraphPin* DefaultExecPin = CreatePin(EGPD_Output, UEdGraphSchema_K2::PC_Exec, DefaultExecPinName, Params);
	DefaultExecPin->PinFriendlyName = FText::AsCultureInvariant(DefaultExecPinFriendlyName.ToString());
}

void UK2Node_MultiBranch::CreateFunctionPin()
{
	FCreatePinParams Params;
	Params.Index = 2;
	UEdGraphPin* FunctionPin =
		CreatePin(EGPD_Input, UEdGraphSchema_K2::PC_Object, ConditionPreProcessFuncClass, ConditionPreProcessFuncName, Params);
	FunctionPin->bDefaultValueIsReadOnly = true;
	FunctionPin->bNotConnectable = true;
	FunctionPin->bHidden = true;

	UFunction* Function = FindUField<UFunction>(ConditionPreProcessFuncClass, ConditionPreProcessFuncName);
	if (Function != nullptr && Function->HasAllFunctionFlags(FUNC_Static))
	{
		UBlueprint* Blueprint = GetBlueprint();
		if (Blueprint != nullptr)
		{
			UClass* FunctionOwnerClass = Function->GetOuterUClass();
			if (!Blueprint->SkeletonGeneratedClass->IsChildOf(FunctionOwnerClass))
			{
				FunctionPin->DefaultObject = FunctionOwnerClass->GetDefaultObject();
			}
		}
	}
}

#undef LOCTEXT_NAMESPACE