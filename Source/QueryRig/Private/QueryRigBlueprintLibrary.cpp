#include "QueryRigBlueprintLibrary.h"

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "QueryRigSubsystem.h"

UQueryRigSubsystem* UQueryRigBlueprintLibrary::GetSubsystem(const UObject* WorldContextObject)
{
	if (!WorldContextObject || !GEngine)
	{
		return nullptr;
	}
	const UWorld* World = GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::LogAndReturnNull);
	const UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
	return GameInstance ? GameInstance->GetSubsystem<UQueryRigSubsystem>() : nullptr;
}

void UQueryRigBlueprintLibrary::TrackEvent(const UObject* WorldContextObject, const FString& EventName, const TMap<FString, FString>& Properties)
{
	if (UQueryRigSubsystem* Rig = GetSubsystem(WorldContextObject))
	{
		Rig->TrackEvent(EventName, Properties);
	}
}

void UQueryRigBlueprintLibrary::TrackFields(const UObject* WorldContextObject, const TMap<FString, FString>& Fields)
{
	if (UQueryRigSubsystem* Rig = GetSubsystem(WorldContextObject))
	{
		Rig->TrackFieldMap(Fields);
	}
}

void UQueryRigBlueprintLibrary::SetQueryRigPlayerId(const UObject* WorldContextObject, const FString& PlayerId)
{
	if (UQueryRigSubsystem* Rig = GetSubsystem(WorldContextObject))
	{
		Rig->SetPlayerId(PlayerId);
	}
}

void UQueryRigBlueprintLibrary::FlushQueryRig(const UObject* WorldContextObject)
{
	if (UQueryRigSubsystem* Rig = GetSubsystem(WorldContextObject))
	{
		Rig->Flush();
	}
}

void UQueryRigBlueprintLibrary::RefreshQueryRigGate(const UObject* WorldContextObject)
{
	if (UQueryRigSubsystem* Rig = GetSubsystem(WorldContextObject))
	{
		Rig->RefreshGate();
	}
}

bool UQueryRigBlueprintLibrary::IsQueryRigEnabled(const UObject* WorldContextObject)
{
	const UQueryRigSubsystem* Rig = GetSubsystem(WorldContextObject);
	return Rig && Rig->IsEnabled();
}
