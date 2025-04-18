﻿// Copyright Steve Streeting 2022
// Released under the MIT license https://opensource.org/license/MIT/
#include "SUDSMessageLogger.h"
#include "IMessageLogListing.h"
#include "MessageLogModule.h"
#include "Modules/ModuleManager.h"

FSUDSMessageLogger::~FSUDSMessageLogger()
{
	if (bWriteToMessageLog)
	{
		//Always clear the old message after an import or re-import
		const TCHAR* LogTitle = TEXT("SUDS");
		FMessageLogModule& MessageLogModule = FModuleManager::LoadModuleChecked<FMessageLogModule>("MessageLog");
		TSharedPtr<class IMessageLogListing> LogListing = MessageLogModule.GetLogListing(LogTitle);
		LogListing->SetLabel(FText::FromString("SUDS"));
		// Should NOT clear messages here, otherwise when FSUDSMessageLogger is used multiple times in the import process,
		// each one is clearing messages from previous stages of the import.
		//LogListing->ClearMessages();

		if(ErrorMessages.Num() > 0)
		{
			LogListing->AddMessages(MoveTemp(ErrorMessages));
			LogListing->NotifyIfAnyMessages(NSLOCTEXT("SUDS", "ImportErrors", "There were issues with the import."), EMessageSeverity::Warning);
			MessageLogModule.OpenMessageLog(LogTitle);
		}
	}
}

bool FSUDSMessageLogger::HasErrors() const
{
	for (const TSharedRef<FTokenizedMessage>& Msg : ErrorMessages)
	{
		if (Msg->GetSeverity() == EMessageSeverity::Error)
		{
			return true;
		}
	}
	return false;		
}

int FSUDSMessageLogger::NumErrors() const
{
	int Errs = 0;
	for (const TSharedRef<FTokenizedMessage>& Msg : ErrorMessages)
	{
		if (Msg->GetSeverity() == EMessageSeverity::Error)
		{
			++Errs;
		}
	}
	return Errs;
}

void FSUDSMessageLogger::AddMessage(EMessageSeverity::Type Severity, const FText& Text)
{
	ErrorMessages.Add(FTokenizedMessage::Create(Severity, Text));
}

void FSUDSMessageLogger::ClearMessages()
{
	const TCHAR* LogTitle = TEXT("SUDS");
	FMessageLogModule& MessageLogModule = FModuleManager::LoadModuleChecked<FMessageLogModule>("MessageLog");
	TSharedPtr<class IMessageLogListing> LogListing = MessageLogModule.GetLogListing(LogTitle);
	LogListing->SetLabel(FText::FromString("SUDS"));
	LogListing->ClearMessages();
}

