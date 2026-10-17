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
 *  2. Persist batches before sending; preserve identity for retries.
 *     Caps, crashes before persistence, and disk failures can lose events.
 *  3. Game-thread tracking validates and serializes. Disk writes occur on
 *     flush, also on the game thread; HTTP is async.
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

	/**
	 * Queue an event under a name, with full JSON properties (C++), on the game
	 * thread. Threshold flushes can write to disk. The `event_name` field is what
	 * the dashboard's query templates look for by default — a convention, not
	 * a requirement.
	 */
	void Track(const FString& EventName, TSharedPtr<FJsonObject> Properties = nullptr);

	/**
	 * Queue an event with no name (C++) — the fields are entirely yours.
	 * Nothing is reserved server-side: an event is any set of fields, and the
	 * only rule is that it carries at least one.
	 */
	void TrackFields(TSharedPtr<FJsonObject> Fields);

	/** Queue an event under a name (Blueprint). Values are sent verbatim. */
	UFUNCTION(BlueprintCallable, Category = "QueryRig", meta = (AutoCreateRefTerm = "Properties"))
	void TrackEvent(const FString& EventName, const TMap<FString, FString>& Properties);

	/**
	 * Queue an event with no name (Blueprint) — every key is a field of your
	 * own. Point the dashboard's query templates at whichever of them names
	 * the event.
	 */
	UFUNCTION(BlueprintCallable, Category = "QueryRig", meta = (AutoCreateRefTerm = "Fields", DisplayName = "Track Fields"))
	void TrackFieldMap(const TMap<FString, FString>& Fields);

	/** Override the default (device-derived) player id, e.g. with a platform account id. */
	UFUNCTION(BlueprintCallable, Category = "QueryRig")
	void SetPlayerId(const FString& InPlayerId);

	/** Persist queued events, then start asynchronous HTTP. Disk I/O is synchronous. */
	UFUNCTION(BlueprintCallable, Category = "QueryRig")
	void Flush();

	/** Re-check the remote telemetry gate now instead of waiting for the next poll. */
	UFUNCTION(BlueprintCallable, Category = "QueryRig")
	void RefreshGate();

	/** Effective telemetry state, for game-side debug UI. */
	UFUNCTION(BlueprintPure, Category = "QueryRig")
	bool IsEnabled() const;

	/**
	 * Why Url cannot be an ingest endpoint, or empty if it can: HTTPS to any
	 * host, or plain HTTP to a loopback address for local development.
	 */
	static FString EndpointProblem(const FString& Url);

	/**
	 * The default player id: a hash of the raw device id scoped to the
	 * endpoint's host, so the raw id never leaves the device and different
	 * games on one machine get unrelated ids.
	 */
	static FString PseudonymousPlayerId(const FString& RawDeviceId, const FString& Endpoint);

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
	// Set by the first 401 and cleared once the key is accepted again, so a
	// refused key is reported once per dormant spell, not on every gate poll.
	bool bKeyRefused = false;
	// 403 from the edge (plain HTTP, wrong path): a misconfigured endpoint no
	// retry can fix. Delivery stops until restart; queued batches are kept.
	bool bEdgeBlocked = false;
	bool bSampledIn = true;
	bool bRequestInFlight = false;
	bool bShuttingDown = false;
	FString QueueNamespace;
	TSharedPtr<class IHttpRequest, ESPMode::ThreadSafe> ActiveRequest;

	FString SessionId;
	int64 SessionStartMs = 0;
	int64 LastEventMs = 0;
	FString PlayerId;
	FString CachedBuildVersion;

	// Immutable serialized events; session and tracking state are game-thread-only.
	TArray<FString> Queue;
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
	static TSharedPtr<FJsonObject> MapToJson(const TMap<FString, FString>& Fields);
	TSharedPtr<FJsonObject> Stamp(const TSharedPtr<FJsonObject>& Properties);
	void Enqueue(const TSharedPtr<FJsonObject>& Event);
	bool IsCollecting() const;
	bool OnFlushTick(float DeltaTime);

	// Delivery (game thread)
	void SendNextLiveBatch();
	void SendBatch(TArray<FString> Lines, TFunction<void(ESendOutcome)> OnSettled);
	void NoteSuccess();
	void NoteFailure();
	void NoteKeyRefused(const FString& Url);

	// Disk queue
	FString DataDir() const;
	FString QueueDir() const;
	bool SpillToDisk(const TArray<FString>& Lines);
	void PersistQueue();
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
