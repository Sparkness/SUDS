﻿#include "SUDSDialogue.h"
#include "SUDSLibrary.h"
#include "SUDSScript.h"
#include "SUDSScriptImporter.h"
#include "TestUtils.h"
#include "Internationalization/StringTable.h"
#include "Internationalization/StringTableRegistry.h"
#include "Misc/AutomationTest.h"

PRAGMA_DISABLE_OPTIMIZATION

const FString SimpleRunnerInput = R"RAWSUD(
:start
Player: Hello there
NPC: Salutations fellow human
	:choice
	* Actually no
		NPC: How rude, bye then
		[goto end]
	* Nested option
		:nestedstart
		NPC: Some nesting
		* Actually bye
			Player: Gotta go!
			[go to goodbye] 
		* A fallthrough choice
			NPC: This should fall through to latterhalf
		* A goto choice
			[goto latterhalf]
	* Another option
		Player: What now?
		NPC: This is another fallthrough
:latterhalf
Player: This is the latter half of the discussion
NPC: Yep, sure is
	* Go back to choice
		NPC: Okay!
		[goto choice]
	* Return to the start
		NPC: Gotcha
		[goto start]
	* Continue
		Player: OK I'd like to carry on now 
		NPC: Right you are guv, falling through
:goodbye
NPC: Bye!
)RAWSUD";

const FString SetVariableRunnerInput = R"RAWSUD(
===
# Set some vars in header
# Text var with an existing localised ID
[set SpeakerName.Player "Protagonist"] @12345@
# Text var no localised ID
[set ValetName "Bob"]
[set SomeFloat 12.5]
===

Player: Hello
[set SomeInt 99]
NPC: Wotcha
# Test that inserting a set node in between text and choice doesn't break link 
[set SomeGender masculine]
	* Choice 1
		[set SomeBoolean True]
		NPC: Truth
	* Choice 2
		NPC: Surprise
		[set ValetName "Kate"]
		[set SomeGender feminine]
Player: Well
	
)RAWSUD";

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTestSimpleRunning,
								 "SUDSTest.TestSimpleRunning",
								 EAutomationTestFlags::EditorContext |
								 EAutomationTestFlags::ClientContext |
								 EAutomationTestFlags::ProductFilter)



bool FTestSimpleRunning::RunTest(const FString& Parameters)
{
	FSUDSScriptImporter Importer;
	TestTrue("Import should succeed", Importer.ImportFromBuffer(GetData(SimpleRunnerInput), SimpleRunnerInput.Len(), "SimpleRunnerInput", true));

	auto Script = NewObject<USUDSScript>(GetTransientPackage(), "Test");
	auto StringTable = NewObject<UStringTable>(GetTransientPackage(), "TestStrings");
	Importer.PopulateAsset(Script, StringTable);

	// Script shouldn't be the owner of the dialogue but it's the only UObject we've got right now so why not
	auto Dlg = USUDSLibrary::CreateDialogue(Script, Script);
	Dlg->Start();

	TestDialogueText(this, "First node", Dlg, "Player", "Hello there");
	TestEqual("First node choices", Dlg->GetNumberOfChoices(), 1);
	TestTrue("First node choice text", Dlg->GetChoiceText(0).IsEmpty());

	TestTrue("Continue", Dlg->Continue());

	TestDialogueText(this, "Node 2", Dlg, "NPC", "Salutations fellow human");
	TestEqual("Node 2 choices", Dlg->GetNumberOfChoices(), 3);
	TestEqual("Node 2 choice text 0", Dlg->GetChoiceText(0).ToString(), "Actually no");
	TestEqual("Node 2 choice text 1", Dlg->GetChoiceText(1).ToString(), "Nested option");
	TestEqual("Node 2 choice text 2", Dlg->GetChoiceText(2).ToString(), "Another option");

	TestTrue("Choice 1", Dlg->Choose(0));
	TestDialogueText(this, "Choice 1 Text", Dlg, "NPC", "How rude, bye then");
	// Goes straight to end
	TestFalse("Choice 1 Follow On", Dlg->Continue());
	TestTrue("Should be at end", Dlg->IsEnded());

	// Start again
	Dlg->Restart();
	TestDialogueText(this, "First node", Dlg, "Player", "Hello there");
	TestEqual("First node choices", Dlg->GetNumberOfChoices(), 1);
	TestTrue("First node choice text", Dlg->GetChoiceText(0).IsEmpty());
	TestTrue("Continue", Dlg->Continue());
	TestDialogueText(this, "Node 2", Dlg, "NPC", "Salutations fellow human");

	TestTrue("Choice 2", Dlg->Choose(1));
	TestDialogueText(this, "Choice 2 Text", Dlg, "NPC", "Some nesting");
	TestEqual("Choice 2 nested choices", Dlg->GetNumberOfChoices(), 3);
	TestEqual("Choice 2 nested choice text 0", Dlg->GetChoiceText(0).ToString(), "Actually bye");
	TestEqual("Choice 2 nested choice text 1", Dlg->GetChoiceText(1).ToString(), "A fallthrough choice");
	TestEqual("Choice 2 nested choice text 2", Dlg->GetChoiceText(2).ToString(), "A goto choice");
	
	TestTrue("Nested choice made", Dlg->Choose(0));
	TestDialogueText(this, "Nested choice made text", Dlg, "Player", "Gotta go!");
	TestTrue("Nested choice follow On", Dlg->Continue());
	TestDialogueText(this, "Nested choice follow on text", Dlg, "NPC", "Bye!");
	TestFalse("Nested choice follow On 2", Dlg->Continue());
	TestTrue("Should be at end", Dlg->IsEnded());

	// Start again, this time from nested choice
	Dlg->Restart(true, "nestedstart");
	TestDialogueText(this, "nestedchoice restart Text", Dlg, "NPC", "Some nesting");
	TestTrue("Nested choice made", Dlg->Choose(1));
	TestDialogueText(this, "Nested choice 2 Text", Dlg, "NPC", "This should fall through to latterhalf");
	TestTrue("Nested choice 2 follow On", Dlg->Continue());
	// Should have fallen through
	TestDialogueText(this, "Fallthrough Text", Dlg, "Player", "This is the latter half of the discussion");
	TestTrue("Continue", Dlg->Continue());
	TestDialogueText(this, "Fallthrough Text 2", Dlg, "NPC", "Yep, sure is");
	TestEqual("Fallthrough choices", Dlg->GetNumberOfChoices(), 3);
	TestEqual("Fallthrough choice text 0", Dlg->GetChoiceText(0).ToString(), "Go back to choice");
	TestEqual("Fallthrough choice text 1", Dlg->GetChoiceText(1).ToString(), "Return to the start");
	TestEqual("Fallthrough choice text 2", Dlg->GetChoiceText(2).ToString(), "Continue");

	// Go back to choice
	TestTrue("Fallthrough choice made", Dlg->Choose(0));
	TestDialogueText(this, "Fallthrough Choice Text", Dlg, "NPC", "Okay!");
	// The Goto choice should have collapsed the choices such that we can get them immediately
	TestEqual("Fallthrough then goto choices", Dlg->GetNumberOfChoices(), 3);
	TestEqual("Fallthrough then goto choice text 0", Dlg->GetChoiceText(0).ToString(), "Actually no");
	TestEqual("Fallthrough then goto choice text 1", Dlg->GetChoiceText(1).ToString(), "Nested option");
	TestEqual("Fallthrough then goto choice text 2", Dlg->GetChoiceText(2).ToString(), "Another option");

	// Restart to test another path
	Dlg->Restart(true, "nestedstart");
	TestDialogueText(this, "nestedchoice restart Text", Dlg, "NPC", "Some nesting");
	TestTrue("Nested choice made", Dlg->Choose(2));
	// This should be a direct goto to latterhalf
	TestDialogueText(this, "Direct goto", Dlg, "Player", "This is the latter half of the discussion");
	
	
	Dlg->Restart(true);
	TestTrue("Continue", Dlg->Continue());
	TestTrue("Choice 3", Dlg->Choose(2));
	TestDialogueText(this, "Choice 3 Text", Dlg, "Player", "What now?");
	TestTrue("Continue", Dlg->Continue());
	TestDialogueText(this, "Choice 3 Text 2", Dlg, "NPC", "This is another fallthrough");
	TestTrue("Continue", Dlg->Continue());
	// Should have fallen through
	TestDialogueText(this, "Direct goto", Dlg, "Player", "This is the latter half of the discussion");


	// Tidy up string table
	// Constructor registered this table
	FStringTableRegistry::Get().UnregisterStringTable(StringTable->GetStringTableId());
	
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTestSetVariableRunning,
								 "SUDSTest.TestSetVariableRunning",
								 EAutomationTestFlags::EditorContext |
								 EAutomationTestFlags::ClientContext |
								 EAutomationTestFlags::ProductFilter)



bool FTestSetVariableRunning::RunTest(const FString& Parameters)
{
	FSUDSScriptImporter Importer;
	TestTrue("Import should succeed", Importer.ImportFromBuffer(GetData(SetVariableRunnerInput), SetVariableRunnerInput.Len(), "SetVariableRunnerInput", true));

	auto Script = NewObject<USUDSScript>(GetTransientPackage(), "Test");
	auto StringTable = NewObject<UStringTable>(GetTransientPackage(), "TestStrings");
	Importer.PopulateAsset(Script, StringTable);

	// Script shouldn't be the owner of the dialogue but it's the only UObject we've got right now so why not
	auto Dlg = USUDSLibrary::CreateDialogue(Script, Script);
	Dlg->Start();

	// Check headers have run & initial variables are set
	TestEqual("Header: Player name", Dlg->GetVariableText("SpeakerName.Player").ToString(), "Protagonist");
	TestEqual("Header: Valet name", Dlg->GetVariableText("ValetName").ToString(), "Bob");
	TestEqual("Header: Some float", Dlg->GetVariableFloat("SomeFloat"), 12.5f);

	// Check initial values
	TestEqual("Initial: Some int", Dlg->GetVariableInt("SomeInt"), 0);
	TestEqual("Initial: Some boolean", Dlg->GetVariableBoolean("SomeBoolean"), false);
	TestEqual("Initial: Some gender", Dlg->GetVariableGender("SomeGender"), ETextGender::Neuter);


	TestDialogueText(this, "Node 1", Dlg, "Player", "Hello");
	TestTrue("Continue", Dlg->Continue());
	// Set node should have run
	TestEqual("Initial: Some int", Dlg->GetVariableInt("SomeInt"), 99);
	TestDialogueText(this, "Node 2", Dlg, "NPC", "Wotcha");
	TestEqual("Choices count", Dlg->GetNumberOfChoices(), 2);
	TestEqual("Choice 1 text", Dlg->GetChoiceText(0).ToString(), "Choice 1");
	TestEqual("Choice 2 text", Dlg->GetChoiceText(1).ToString(), "Choice 2");
	TestTrue("Choose 1", Dlg->Choose(0));
	TestEqual("Gender should be set", Dlg->GetVariableGender("SomeGender"), ETextGender::Masculine);
	TestEqual("Some boolean should be set", Dlg->GetVariableBoolean("SomeBoolean"), true);
	TestEqual("Valet name should not have changed", Dlg->GetVariableText("ValetName").ToString(), "Bob");
	TestEqual("Gender should not have changed", Dlg->GetVariableGender("SomeGender"), ETextGender::Masculine);
	TestDialogueText(this, "Choice end text", Dlg, "NPC", "Truth");
	TestTrue("Continue", Dlg->Continue());
	TestDialogueText(this, "Final node", Dlg, "Player", "Well");
	TestFalse("Continue", Dlg->Continue());
	TestTrue("At end", Dlg->IsEnded());

	// Restart and DON'T reset state
	Dlg->Restart(false);

	// Variables should be the same
	// Except for the headers, which will have run again
	TestEqual("Player name should have been set again", Dlg->GetVariableText("SpeakerName.Player").ToString(), "Protagonist");
	TestEqual("Valet name should have been set again", Dlg->GetVariableText("ValetName").ToString(), "Bob");
	TestEqual("Some float should have been set again", Dlg->GetVariableFloat("SomeFloat"), 12.5f);
	TestEqual("Int should still be set", Dlg->GetVariableInt("SomeInt"), 99);
	TestEqual("Gender should still be set", Dlg->GetVariableGender("SomeGender"), ETextGender::Masculine);
	TestEqual("Some boolean should still be set", Dlg->GetVariableBoolean("SomeBoolean"), true);

	// Restart and DO reset state
	Dlg->Restart(true);
	TestEqual("Player name should have been set again", Dlg->GetVariableText("SpeakerName.Player").ToString(), "Protagonist");
	TestEqual("Valet name should have been set again", Dlg->GetVariableText("ValetName").ToString(), "Bob");
	TestEqual("Some float should have been set again", Dlg->GetVariableFloat("SomeFloat"), 12.5f);
	TestEqual("Int should have been reset", Dlg->GetVariableInt("SomeInt"), 0);
	TestEqual("Gender should have been reset", Dlg->GetVariableGender("SomeGender"), ETextGender::Neuter);
	TestEqual("Some boolean should have been reset", Dlg->GetVariableBoolean("SomeBoolean"), false);

	// Try the other path
	TestTrue("Continue", Dlg->Continue());
	TestTrue("Choose 2", Dlg->Choose(1));
	TestDialogueText(this, "Choice 2 text", Dlg, "NPC", "Surprise");
	TestEqual("Gender should not be changed yet", Dlg->GetVariableGender("SomeGender"), ETextGender::Masculine);
	TestEqual("Valet name should not be changed yet", Dlg->GetVariableText("ValetName").ToString(), "Bob");
	TestTrue("Continue", Dlg->Continue());
	TestDialogueText(this, "Final node", Dlg, "Player", "Well");
	TestEqual("Gender should have changed", Dlg->GetVariableGender("SomeGender"), ETextGender::Feminine);
	TestEqual("Valet name should have changed", Dlg->GetVariableText("ValetName").ToString(), "Kate");
	TestFalse("Continue", Dlg->Continue());
	TestTrue("At end", Dlg->IsEnded());
	
	// Tidy up string table
	// Constructor registered this table
	FStringTableRegistry::Get().UnregisterStringTable(StringTable->GetStringTableId());

	return true;

}

PRAGMA_ENABLE_OPTIMIZATION