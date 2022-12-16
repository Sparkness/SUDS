﻿#include "SUDSDialogue.h"

#include "SUDSParticipant.h"
#include "SUDSScript.h"
#include "SUDSScriptNode.h"
#include "SUDSScriptNodeEvent.h"
#include "SUDSScriptNodeSet.h"
#include "SUDSScriptNodeText.h"

DEFINE_LOG_CATEGORY(LogSUDSDialogue)

const FText USUDSDialogue::DummyText = FText::FromString("INVALID");
const FString USUDSDialogue::DummyString = "INVALID";


FArchive& operator<<(FArchive& Ar, FSUDSDialogueState& Value)
{
	Ar << Value.TextNodeID;
	Ar << Value.Variables;
	Ar << Value.ChoicesTaken;
	return Ar;
}

void operator<<(FStructuredArchive::FSlot Slot, FSUDSDialogueState& Value)
{
	FStructuredArchive::FRecord Record = Slot.EnterRecord();
	Record
		<< SA_VALUE(TEXT("TextNodeID"), Value.TextNodeID)
		<< SA_VALUE(TEXT("Variables"), Value.Variables)
		<< SA_VALUE(TEXT("ChoicesTaken"), Value.ChoicesTaken);

}

USUDSDialogue::USUDSDialogue()
{
}

void USUDSDialogue::Initialise(const USUDSScript* Script)
{
	BaseScript = Script;
	CurrentSpeakerNode = nullptr;

	InitVariables();

	CurrentSpeakerNode = nullptr;

}

void USUDSDialogue::InitVariables()
{
	VariableState.Empty();
	// Run header nodes immediately (only set nodes)
	RunUntilNextSpeakerNodeOrEnd(BaseScript->GetHeaderNode(), false);
}

void USUDSDialogue::Start(FName Label)
{
	// Only start if not already on a speaker node
	// This makes the restore sequence easier, you don't have to test IsEnded
	if (!IsValid(CurrentSpeakerNode))
	{
		// Note that we don't reset state by default here. This is to allow long-term memory on dialogue, such as
		// knowing whether you've met a character before etc.
		// We also don't re-run headers here since they will have been run on Initialise()
		// This is to allow callers to set variables before Start() that override headers
		Restart(false, Label, false);
	}
}

void USUDSDialogue::SetParticipants(const TArray<UObject*>& InParticipants)
{
	Participants = InParticipants;
	SortParticipants();
}

void USUDSDialogue::AddParticipant(UObject* Participant)
{
	Participants.Add(Participant);
	SortParticipants();
}

void USUDSDialogue::SortParticipants()
{
	if (!Participants.IsEmpty())
	{
		// We order by ascending priority so that higher priority values are later in the list
		// Which means they're called last and get to override values set by earlier ones
		// We'll do a stable sort so that otherwise order is maintained
		Participants.StableSort([](const UObject& A, const UObject& B)
		{
			return ISUDSParticipant::Execute_GetDialogueParticipantPriority(&A) <
				ISUDSParticipant::Execute_GetDialogueParticipantPriority(&B);
		});
	}
}

void USUDSDialogue::RunUntilNextSpeakerNodeOrEnd(USUDSScriptNode* NextNode, bool bRaiseAtEnd)
{
	// We run through nodes which don't require a speaker line prompt
	// E.g. set nodes, select nodes which are all automatically resolved
	// Starting with this node
	while (NextNode && !ShouldStopAtNodeType(NextNode->GetNodeType()))
	{
		NextNode = RunNode(NextNode);
	}

	if (NextNode)
	{
		if (NextNode->GetNodeType() == ESUDSScriptNodeType::Text)
		{
			SetCurrentSpeakerNode(Cast<USUDSScriptNodeText>(NextNode), false);
		}
		else
		{
			// This can happen if for example user creates a choice node as the first thing
			UE_LOG(LogSUDSDialogue,
			       Error,
			       TEXT("Error in %s line %d: Tried to run to next speaker node but encountered unexpected node of type %s"),
			       *BaseScript->GetName(),
			       NextNode->GetSourceLineNo(),
			       *(StaticEnum<ESUDSScriptNodeType>()->GetValueAsString(NextNode->GetNodeType()))
			);
		}
	}
	else
	{
		End(!bRaiseAtEnd);
	}

}

USUDSScriptNode* USUDSDialogue::RunNode(USUDSScriptNode* Node)
{
	switch (Node->GetNodeType())
	{
	case ESUDSScriptNodeType::Select:
		return RunSelectNode(Node);
	case ESUDSScriptNodeType::SetVariable:
		return RunSetVariableNode(Node);
	case ESUDSScriptNodeType::Event:
		return RunEventNode(Node);
	default: ;
	}

	UE_LOG(LogSUDSDialogue,
	       Error,
	       TEXT("Error in %s line %d: Attempted to run non-runnable node type %s"),
	       *BaseScript->GetName(),
	       Node->GetSourceLineNo(),
	       *(StaticEnum<ESUDSScriptNodeType>()->GetValueAsString(Node->GetNodeType()))
	)
	return nullptr;
}

USUDSScriptNode* USUDSDialogue::RunSelectNode(USUDSScriptNode* Node)
{
	for (auto& Edge : Node->GetEdges())
	{
		if (Edge.GetCondition().IsValid())
		{
			// use the first satisfied edge
			RaiseExpressionVariablesRequested(Edge.GetCondition());
			if (Edge.GetCondition().EvaluateBoolean(VariableState, BaseScript->GetName()))
			{
				return Edge.GetTargetNode().Get();
			}
		}
	}
	// NOTE: if no valid path, go to end
	// We've already created fall-through else nodes if possible
	return nullptr;
}

USUDSScriptNode* USUDSDialogue::RunEventNode(USUDSScriptNode* Node)
{
	if (USUDSScriptNodeEvent* EvtNode = Cast<USUDSScriptNodeEvent>(Node))
	{
		// Build a resolved args list, because we need to evaluate  expressions
		TArray<FSUDSValue> ArgsResolved;
		
		for (auto& Expr : EvtNode->GetArgs())
		{
			RaiseExpressionVariablesRequested(Expr);
			ArgsResolved.Add(Expr.Evaluate(VariableState));
		}
		
		for (const auto P : Participants)
		{
			if (P->GetClass()->ImplementsInterface(USUDSParticipant::StaticClass()))
			{
				ISUDSParticipant::Execute_OnDialogueEvent(P, this, EvtNode->GetEventName(), ArgsResolved);
			}
		}
		OnEvent.Broadcast(this, EvtNode->GetEventName(), ArgsResolved);
	}
	return GetNextNode(Node);
}

USUDSScriptNode* USUDSDialogue::RunSetVariableNode(USUDSScriptNode* Node)
{
	if (USUDSScriptNodeSet* SetNode = Cast<USUDSScriptNodeSet>(Node))
	{
		if (SetNode->GetExpression().IsValid())
		{
			RaiseExpressionVariablesRequested(SetNode->GetExpression());
			const FSUDSValue OldValue = GetVariable(SetNode->GetIdentifier());
			const FSUDSValue NewValue = SetNode->GetExpression().Evaluate(VariableState);
			if ((OldValue != NewValue).GetBooleanValue())
			{
				VariableState.Add(SetNode->GetIdentifier(), NewValue);
				RaiseVariableChange(SetNode->GetIdentifier(), NewValue, true);
			}
		}
	}

	// Always one edge
	return GetNextNode(Node);
	
}

void USUDSDialogue::RaiseVariableChange(const FName& VarName, const FSUDSValue& Value, bool bFromScript)
{
	for (const auto P : Participants)
	{
		if (P->GetClass()->ImplementsInterface(USUDSParticipant::StaticClass()))
		{
			ISUDSParticipant::Execute_OnDialogueVariableChanged(P, this, VarName, Value, bFromScript);
		}
	}
	OnVariableChanged.Broadcast(this, VarName, Value, bFromScript);

}

void USUDSDialogue::RaiseVariableRequested(const FName& VarName)
{
	// Because variables set by participants should "win", raise event first
	OnVariableRequested.Broadcast(this, VarName);
	for (const auto P : Participants)
	{
		if (P->GetClass()->ImplementsInterface(USUDSParticipant::StaticClass()))
		{
			ISUDSParticipant::Execute_OnDialogueVariableRequested(P, this, VarName);
		}
	}
}

void USUDSDialogue::RaiseExpressionVariablesRequested(const FSUDSExpression& Expression)
{
	for (auto& Var : Expression.GetVariableNames())
	{
		RaiseVariableRequested(Var);
	}
}

void USUDSDialogue::SetCurrentSpeakerNode(USUDSScriptNodeText* Node, bool bQuietly)
{
	CurrentSpeakerNode = Node;

	CurrentSpeakerDisplayName = FText::GetEmpty();
	bParamNamesExtracted = false;

	UpdateChoices();

	if (!bQuietly)
	{
		if (CurrentSpeakerNode)
			RaiseNewSpeakerLine();
		else
			RaiseFinished();
	}

}

FText USUDSDialogue::ResolveParameterisedText(const TArray<FName> Params, const FTextFormat& TextFormat)
{
	for (const auto& P : Params)
	{
		RaiseVariableRequested(P);
	}
	// Need to make a temp arg list for compatibility
	// Also lets us just set the ones we need to
	FFormatNamedArguments Args;
	GetTextFormatArgs(Params, Args);
	return FText::Format(TextFormat, Args);
	
}

void USUDSDialogue::GetTextFormatArgs(const TArray<FName>& ArgNames, FFormatNamedArguments& OutArgs) const
{
	for (auto& Name : ArgNames)
	{
		if (const FSUDSValue* Value = VariableState.Find(Name))
		{
			// Use the operator conversion
			OutArgs.Add(Name.ToString(), Value->ToFormatArg());
		}
	}
}

FText USUDSDialogue::GetText()
{
	if (CurrentSpeakerNode)
	{
		if (CurrentSpeakerNode->HasParameters())
		{
			return ResolveParameterisedText(CurrentSpeakerNode->GetParameterNames(), CurrentSpeakerNode->GetTextFormat());
		}
		else
		{
			return CurrentSpeakerNode->GetText();
		}
	}
	return DummyText;
}

const FString& USUDSDialogue::GetSpeakerID() const
{
	if (CurrentSpeakerNode)
		return CurrentSpeakerNode->GetSpeakerID();
	
	return DummyString;
}

FText USUDSDialogue::GetSpeakerDisplayName() const
{
	if (CurrentSpeakerDisplayName.IsEmpty())
	{
		// Derive speaker display name
		// Is just a special variable "SpeakerName.SpeakerID"
		// or just the SpeakerID if none specified
		static const FString SpeakerIDPrefix = "SpeakerName.";
		FName Key(SpeakerIDPrefix + GetSpeakerID());
		if (auto Arg = VariableState.Find(Key))
		{
			if (Arg->GetType() == ESUDSValueType::Text)
			{
				CurrentSpeakerDisplayName = Arg->GetTextValue();
			}
			else
			{
				UE_LOG(LogSUDSDialogue,
				       Error,
				       TEXT("Error in %s: %s was set to a value that was not text, cannot use"),
				       *BaseScript->GetName(),
				       *Key.ToString());
			}
		}
		if (CurrentSpeakerDisplayName.IsEmpty())
		{
			// If no display name was specified, use the (non-localised) speaker ID
			CurrentSpeakerDisplayName = FText::FromString(GetSpeakerID());
		}
	}
	return CurrentSpeakerDisplayName;
}

USUDSScriptNode* USUDSDialogue::GetNextNode(const USUDSScriptNode* Node) const
{
	return BaseScript->GetNextNode(Node);
}

bool USUDSDialogue::ShouldStopAtNodeType(ESUDSScriptNodeType Type)
{
	return Type != ESUDSScriptNodeType::SetVariable &&
		Type != ESUDSScriptNodeType::Select &&
		Type != ESUDSScriptNodeType::Event;
}

const USUDSScriptNode* USUDSDialogue::RunUntilNextChoiceNode(const USUDSScriptNodeText* FromTextNode)
{
	if (FromTextNode && FromTextNode->GetEdgeCount() == 1)
	{
		auto NextNode = GetNextNode(FromTextNode);
		// We skip over set nodes
		while (NextNode && !ShouldStopAtNodeType(NextNode->GetNodeType()))
		{
			NextNode = RunNode(NextNode);
		}

		return NextNode;
	}

	return nullptr;
	
}

const TArray<FSUDSScriptEdge>& USUDSDialogue::GetChoices() const
{
	return CurrentChoices;
}

void USUDSDialogue::RecurseAppendChoices(const USUDSScriptNode* Node, TArray<FSUDSScriptEdge>& OutChoices)
{
	if (!Node)
		return;

	check(Node->GetNodeType() == ESUDSScriptNodeType::Choice || Node->GetNodeType() == ESUDSScriptNodeType::Select);
	
	for (auto& Edge : Node->GetEdges())
	{
		switch (Edge.GetType())
		{
		case ESUDSEdgeType::Decision:
			OutChoices.Add(Edge);
			break;
		case ESUDSEdgeType::Condition:
			// Conditional edges are under selects
			if (Edge.GetCondition().IsValid())
			{
				RaiseExpressionVariablesRequested(Edge.GetCondition());
				if (Edge.GetCondition().EvaluateBoolean(VariableState, BaseScript->GetName()))
				{
					RecurseAppendChoices(Edge.GetTargetNode().Get(), OutChoices);
					// When we choose a path on a select, we don't check the other paths, we can only go down one
					return;
				}
			}
			break;
		case ESUDSEdgeType::Chained:
			RecurseAppendChoices(Edge.GetTargetNode().Get(), OutChoices);
			break;
		default:
		case ESUDSEdgeType::Continue:
			UE_LOG(LogSUDSDialogue, Fatal, TEXT("Should not have encountered invalid edge in RecurseAppendChoices"))			
			break;
		};
		
	}
}

void USUDSDialogue::UpdateChoices()
{
	CurrentChoices.Reset();
	if (CurrentSpeakerNode)
	{
		if (CurrentSpeakerNode->HasChoices())
		{
			// Root choice node might not be directly underneath. For example, we may go through set nodes first
			if (const USUDSScriptNode* ChoiceNode = BaseScript->GetNextChoiceNode(CurrentSpeakerNode))
			{
				// Once we've found the root choice, there can be potentially a tree of mixed choice/select nodes
				// for supporting conditional choices
				RecurseAppendChoices(ChoiceNode, CurrentChoices);
			}
		}
		else
		{
			if (auto Edge = CurrentSpeakerNode->GetEdge(0))
			{
				// Simple no-choice progression (text->text)
				CurrentChoices.Add(*Edge);
			}			
		}
	}
}


int USUDSDialogue::GetNumberOfChoices() const
{
	return CurrentChoices.Num();
}

FText USUDSDialogue::GetChoiceText(int Index)
{

	if (CurrentChoices.IsValidIndex(Index))
	{
		auto& Choice = CurrentChoices[Index];
		if (Choice.HasParameters())
		{
			return ResolveParameterisedText(Choice.GetParameterNames(), Choice.GetTextFormat());
		}
		else
		{
			return Choice.GetText();
		}
	}
	else
	{
		UE_LOG(LogSUDSDialogue, Error, TEXT("Invalid choice index %d on node %s"), Index, *GetText().ToString());
	}

	return DummyText;
}

bool USUDSDialogue::HasChoiceIndexBeenTakenPreviously(int Index)
{
	if (CurrentChoices.IsValidIndex(Index))
	{
		return HasChoiceBeenTakenPreviously(CurrentChoices[Index]);
	}
	return false;
}

bool USUDSDialogue::HasChoiceBeenTakenPreviously(const FSUDSScriptEdge& Choice)
{
	return ChoicesTaken.Contains(Choice.GetTextID());
}

bool USUDSDialogue::Continue()
{
	if (GetNumberOfChoices() == 1)
	{
		return Choose(0);		
	}
	return !IsEnded();
}

bool USUDSDialogue::Choose(int Index)
{
	if (CurrentChoices.IsValidIndex(Index))
	{
		// ONLY run to choice node if there is one!
		// This method is called for Continue() too, which has no choice node
		if (CurrentSpeakerNode->HasChoices())
		{
			ChoicesTaken.Add(CurrentChoices[Index].GetTextID());
			
			RaiseChoiceMade(Index);
			RaiseProceeding();
			// Run any e.g. set nodes between text and choice
			// These can be set nodes directly under the text and before the first choice, which get run for all choices
			RunUntilNextChoiceNode(CurrentSpeakerNode);
		}
		else
		{
			RaiseProceeding();
		}
		// Then choose path
		RunUntilNextSpeakerNodeOrEnd(CurrentChoices[Index].GetTargetNode().Get(), true);
		return !IsEnded();
	}
	else
	{
		UE_LOG(LogSUDSDialogue, Error, TEXT("Invalid choice index %d on node %s"), Index, *GetText().ToString());
	}
	return false;
}

bool USUDSDialogue::IsEnded() const
{
	return CurrentSpeakerNode == nullptr;
}

void USUDSDialogue::End(bool bQuietly)
{
	SetCurrentSpeakerNode(nullptr, bQuietly);
}

void USUDSDialogue::ResetState(bool bResetVariables, bool bResetPosition, bool bResetVisited)
{
	if (bResetVariables)
		InitVariables();
	if (bResetPosition)
		SetCurrentSpeakerNode(nullptr, true);
	if (bResetVisited)
		ChoicesTaken.Reset();
}

FSUDSDialogueState USUDSDialogue::GetSavedState() const
{
	const FString CurrentNodeId = CurrentSpeakerNode
		                              ? FTextInspector::GetTextId(CurrentSpeakerNode->GetText()).GetKey().GetChars()
		                              : FString();
	return FSUDSDialogueState(CurrentNodeId, VariableState, ChoicesTaken);
		  
}

void USUDSDialogue::RestoreSavedState(const FSUDSDialogueState& State)
{
	// Don't just empty variables
	// Re-run init to ensure header state is initialised then merge; important for it script is altered since state saved
	InitVariables();
	VariableState.Append(State.GetVariables());
	ChoicesTaken.Empty();
	ChoicesTaken.Append(State.GetChoicesTaken());

	// If not found this will be null
	if (!State.GetTextNodeID().IsEmpty())
	{
		USUDSScriptNodeText* Node = BaseScript->GetNodeByTextID(State.GetTextNodeID());
		SetCurrentSpeakerNode(Node, true);
	}
	else
	{
		SetCurrentSpeakerNode(nullptr, true);
	}
}

void USUDSDialogue::Restart(bool bResetState, FName StartLabel, bool bReRunHeader)
{
	if (bResetState)
	{
		ResetState();
	}

	RaiseStarting(StartLabel);

	if (!bResetState && bReRunHeader)
	{
		// Run header nodes but don't re-init
		RunUntilNextSpeakerNodeOrEnd(BaseScript->GetHeaderNode(), false);
	}

	if (StartLabel != NAME_None)
	{
		// Check that StartLabel leads to a text node
		// Labels can lead to choices or select nodes for looping, but there has to be a text node to start with.
		auto StartNode = BaseScript->GetNodeByLabel(StartLabel);
		if (!StartNode)
		{
			UE_LOG(LogSUDSDialogue, Error, TEXT("No start label called %s in dialogue %s"), *StartLabel.ToString(), *BaseScript->GetName());
			StartNode = BaseScript->GetFirstNode();
		}
		else if (StartNode->GetNodeType() == ESUDSScriptNodeType::Choice)
		{
			UE_LOG(LogSUDSDialogue,
			       Error,
			       TEXT("Label %s in dialogue %s cannot be used as a start point, points to a choice."),
			       *StartLabel.ToString(),
			       *BaseScript->GetName());
			StartNode = BaseScript->GetFirstNode();
		}
		RunUntilNextSpeakerNodeOrEnd(StartNode, true);
	}
	else
	{
		RunUntilNextSpeakerNodeOrEnd(BaseScript->GetFirstNode(), true);
	}
	
}


TSet<FName> USUDSDialogue::GetParametersInUse()
{
	// Build on demand, may not be needed
	if (!bParamNamesExtracted)
	{
		CurrentRequestedParamNames.Reset();
		if (CurrentSpeakerNode && CurrentSpeakerNode->HasParameters())
		{
			CurrentRequestedParamNames.Append(CurrentSpeakerNode->GetParameterNames());
		}
		for (auto& Choice : CurrentChoices)
		{
			if (Choice.HasParameters())
			{
				CurrentRequestedParamNames.Append(Choice.GetParameterNames());
			}
		}
		bParamNamesExtracted = true;
	}

	return CurrentRequestedParamNames;
	
}

void USUDSDialogue::RaiseStarting(FName StartLabel)
{
	for (const auto P : Participants)
	{
		if (P->GetClass()->ImplementsInterface(USUDSParticipant::StaticClass()))
		{
			ISUDSParticipant::Execute_OnDialogueStarting(P, this, StartLabel);
		}
	}
	OnStarting.Broadcast(this, StartLabel);
}

void USUDSDialogue::RaiseFinished()
{
	for (const auto P : Participants)
	{
		if (P->GetClass()->ImplementsInterface(USUDSParticipant::StaticClass()))
		{
			ISUDSParticipant::Execute_OnDialogueFinished(P, this);
		}
	}
	OnFinished.Broadcast(this);

}

void USUDSDialogue::RaiseNewSpeakerLine()
{
	for (const auto P : Participants)
	{
		if (P->GetClass()->ImplementsInterface(USUDSParticipant::StaticClass()))
		{
			ISUDSParticipant::Execute_OnDialogueSpeakerLine(P, this);
		}
	}
	
	// Event listeners get it after
	OnSpeakerLine.Broadcast(this);
}

void USUDSDialogue::RaiseChoiceMade(int Index)
{
	for (const auto P : Participants)
	{
		if (P->GetClass()->ImplementsInterface(USUDSParticipant::StaticClass()))
		{
			ISUDSParticipant::Execute_OnDialogueChoiceMade(P, this, Index);
		}
	}
	// Event listeners get it after
	OnChoice.Broadcast(this, Index);
}

void USUDSDialogue::RaiseProceeding()
{
	for (const auto P : Participants)
	{
		if (P->GetClass()->ImplementsInterface(USUDSParticipant::StaticClass()))
		{
			ISUDSParticipant::Execute_OnDialogueProceeding(P, this);
		}
	}
	// Event listeners get it after
	OnProceeding.Broadcast(this);
}
