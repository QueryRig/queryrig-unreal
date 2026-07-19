#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "QueryRigBlueprintLibrary.generated.h"

class UQueryRigSubsystem;

/**
 * One-node Blueprint entry points for QueryRig — callable from any graph with
 * a World Context (level Blueprints, widgets, actors) without first fetching
 * the Game Instance and its QueryRig subsystem by hand. Functionally
 * identical to calling the subsystem's own functions directly; this is purely
 * a convenience wrapper so a designer can drag one node instead of three.
 */
UCLASS()
class QUERYRIG_API UQueryRigBlueprintLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/** Queue an analytics event under a name. Property values are sent verbatim. */
	UFUNCTION(BlueprintCallable, Category = "QueryRig", meta = (WorldContext = "WorldContextObject", AutoCreateRefTerm = "Properties", DisplayName = "QueryRig: Track Event"))
	static void TrackEvent(const UObject* WorldContextObject, const FString& EventName, const TMap<FString, FString>& Properties);

	/** Queue an event with no name — every key is a field of your own. */
	UFUNCTION(BlueprintCallable, Category = "QueryRig", meta = (WorldContext = "WorldContextObject", AutoCreateRefTerm = "Fields", DisplayName = "QueryRig: Track Fields"))
	static void TrackFields(const UObject* WorldContextObject, const TMap<FString, FString>& Fields);

	/** Override the default (device-derived) player id, e.g. with a platform account id. */
	UFUNCTION(BlueprintCallable, Category = "QueryRig", meta = (WorldContext = "WorldContextObject", DisplayName = "QueryRig: Set Player Id"))
	static void SetQueryRigPlayerId(const UObject* WorldContextObject, const FString& PlayerId);

	/** Force an immediate flush (asynchronous — returns at once). */
	UFUNCTION(BlueprintCallable, Category = "QueryRig", meta = (WorldContext = "WorldContextObject", DisplayName = "QueryRig: Flush"))
	static void FlushQueryRig(const UObject* WorldContextObject);

	/** Re-check the remote telemetry gate now instead of waiting for the next poll. */
	UFUNCTION(BlueprintCallable, Category = "QueryRig", meta = (WorldContext = "WorldContextObject", DisplayName = "QueryRig: Refresh Gate"))
	static void RefreshQueryRigGate(const UObject* WorldContextObject);

	/** Effective telemetry state, for game-side debug UI. */
	UFUNCTION(BlueprintPure, Category = "QueryRig", meta = (WorldContext = "WorldContextObject", DisplayName = "QueryRig: Is Enabled"))
	static bool IsQueryRigEnabled(const UObject* WorldContextObject);

private:
	static UQueryRigSubsystem* GetSubsystem(const UObject* WorldContextObject);
};
