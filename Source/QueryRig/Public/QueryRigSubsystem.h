#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "QueryRigSubsystem.generated.h"

/**
 * QueryRig analytics SDK for Unreal Engine 5 — implements the QueryRig engine
 * SDK spec (docs/sdk-spec.md in the queryrig repo).
 *
 * Design contract, in order:
 *  1. Telemetry behavior is a runtime decision — config file, env var, or the
 *     remote gate. Never a rebuild.
 *  2. Accepted events are never lost: memory queue -> disk spill -> replay,
 *     with client-generated event_ids so retries are dedupe-safe server-side.
 *  3. Track() never costs a frame: it stamps and enqueues; HTTP is async and
 *     delivery failures go to disk.
 *
 * Usage (auto-instantiated per game instance):
 *   GetGameInstance()->GetSubsystem<UQueryRigSubsystem>()->Track(
 *       TEXT("level_complete"), Props);
 * Blueprint: the QueryRig category exposes Track Event / Flush / Is Enabled.
 */
UCLASS()
class QUERYRIG_API UQueryRigSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	//~ Begin UGameInstanceSubsystem
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	//~ End UGameInstanceSubsystem

	/** Queue an event with full JSON properties (C++). Cheap; never blocks on I/O. */
	void Track(const FString& EventName, TSharedPtr<FJsonObject> Properties = nullptr);

	/**
	 * Queue an event (Blueprint). Property values that parse as numbers or
	 * true/false are sent as numbers/booleans; everything else as strings.
	 */
	UFUNCTION(BlueprintCallable, Category = "QueryRig", meta = (AutoCreateRefTerm = "Properties"))
	void TrackEvent(const FString& EventName, const TMap<FString, FString>& Properties);

	/** Override the default (device-derived) player id, e.g. with a platform account id. */
	UFUNCTION(BlueprintCallable, Category = "QueryRig")
	void SetPlayerId(const FString& InPlayerId);

	/** Force an immediate flush (asynchronous — returns at once). */
	UFUNCTION(BlueprintCallable, Category = "QueryRig")
	void Flush();

	/** Re-check the remote telemetry gate now instead of waiting for the next poll. */
	UFUNCTION(BlueprintCallable, Category = "QueryRig")
	void RefreshGate();

	/** Effective telemetry state, for game-side debug UI. */
	UFUNCTION(BlueprintPure, Category = "QueryRig")
	bool IsEnabled() const;

private:
	enum class EEnabledMode : uint8 { HardOff, HardOn, Remote };
	enum class ESendOutcome : uint8 { Delivered, Rejected, Retryable, Dormant };

	struct FRigConfig
	{
		FString ApiKey;
		FString Endpoint;
		EEnabledMode Mode = EEnabledMode::Remote;
		double FlushIntervalS = 5.0;
		int32 FlushMaxEvents = 100;
		int64 DiskQueueMaxBytes = 8 * 1024 * 1024;
		double SampleRate = 1.0;
		double GatePollIntervalS = 300.0;
	};

	FRigConfig Config;
	bool bInert = true;   // hard-off or no api_key: Track() no-ops, zero network
	bool bDormant = false; // 401 or gate-off: drop new events, keep disk queue
	bool bSampledIn = true;
	bool bRequestInFlight = false;

	FString SessionId;
	int64 SessionStartMs = 0;
	int64 LastEventMs = 0;
	FString PlayerId;
	FString CachedBuildVersion;

	// Memory queue. Guarded: Track() is callable from any thread.
	TArray<TSharedPtr<FJsonObject>> Queue;
	FCriticalSection QueueLock;
	int32 DroppedOverflow = 0;

	int32 ConsecutiveFailures = 0;
	int64 NextDrainMs = 0;

	FTSTicker::FDelegateHandle FlushTickerHandle;
	FTSTicker::FDelegateHandle GateTickerHandle;
	FDelegateHandle BackgroundHandle;
	FDelegateHandle DeactivateHandle;

	// Config
	void LoadConfig();
	void ApplyJsonConfigFile(const FString& FilePath);
	static EEnabledMode ParseEnabledString(const FString& Value, EEnabledMode Fallback);

	// Sessions
	void StartSession();
	void EndSession();

	// Queueing
	TSharedPtr<FJsonObject> Stamp(const FString& EventName, const TSharedPtr<FJsonObject>& Properties);
	void Enqueue(const TSharedPtr<FJsonObject>& Event);
	bool IsCollecting() const;
	bool OnFlushTick(float DeltaTime);

	// Delivery (game thread)
	void SendNextLiveBatch();
	void SendBatch(TArray<FString> Lines, TFunction<void(ESendOutcome)> OnSettled);
	void NoteSuccess();
	void NoteFailure();

	// Disk queue
	FString DataDir() const;
	FString QueueDir() const;
	void SpillToDisk(const TArray<FString>& Lines);
	void EnforceDiskCap();
	void DrainDiskOne();
	void AppendRejected(const TArray<FString>& Lines);

	// Remote gate
	void CheckGate();
	bool LoadCachedGate() const;
	void SaveCachedGate(bool bEnabled) const;
	FString EndpointOrigin() const;

	// Utils
	FString DefaultPlayerId() const;
	static FString SerializeEvent(const TSharedPtr<FJsonObject>& Event);
	static int64 NowMs();
	static FString NewUuid();
};
