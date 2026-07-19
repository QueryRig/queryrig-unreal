#include "QueryRigSubsystem.h"

#include "Async/Async.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/CommandLine.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/CoreDelegates.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

DEFINE_LOG_CATEGORY_STATIC(LogQueryRig, Log, All);

namespace
{
	const TCHAR* SdkVersion = TEXT("0.1.0");

	// Spec limits (server-enforced; batches are built to stay inside them).
	constexpr int32 MaxBatchEvents = 500;
	constexpr int32 FlushMaxBytes = 200000; // headroom under the 256 KB payload cap
	constexpr int32 MemoryQueueMax = 10000;
	constexpr int64 SessionGapMs = 30 * 60 * 1000;
	constexpr int64 BackoffBaseMs = 5000;
	constexpr int64 BackoffCapMs = 5 * 60 * 1000;
	constexpr int64 RejectedFileMaxBytes = 262144;
	constexpr float HttpTimeoutS = 5.0f;
}

// ---------------------------------------------------------------- lifecycle

void UQueryRigSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	LoadConfig();

	if (Config.Mode == EEnabledMode::HardOff || Config.ApiKey.IsEmpty())
	{
		// Hard-off (or unconfigured): zero network calls, verifiable with a
		// packet capture. Track() drops immediately.
		bInert = true;
		return;
	}
	bInert = false;

	IFileManager::Get().MakeDirectory(*QueueDir(), true);
	PlayerId = DefaultPlayerId();
	GConfig->GetString(TEXT("/Script/EngineSettings.GeneralProjectSettings"),
		TEXT("ProjectVersion"), CachedBuildVersion, GGameIni);

	const bool bGate = Config.Mode == EEnabledMode::Remote ? LoadCachedGate() : true;
	bDormant = !bGate;

	FlushTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateUObject(this, &UQueryRigSubsystem::OnFlushTick),
		FMath::Max(0.1, Config.FlushIntervalS));

	if (Config.Mode == EEnabledMode::Remote)
	{
		TWeakObjectPtr<UQueryRigSubsystem> WeakThis(this);
		GateTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateLambda([WeakThis](float)
			{
				if (WeakThis.IsValid())
				{
					WeakThis->CheckGate();
				}
				return true;
			}),
			FMath::Max(1.0, Config.GatePollIntervalS));
		CheckGate();
	}

	// Lifecycle flushes (spec §3.4): pause / focus loss.
	BackgroundHandle = FCoreDelegates::ApplicationWillEnterBackgroundDelegate.AddUObject(
		this, &UQueryRigSubsystem::Flush);
	DeactivateHandle = FCoreDelegates::ApplicationWillDeactivateDelegate.AddUObject(
		this, &UQueryRigSubsystem::Flush);

	StartSession();
}

void UQueryRigSubsystem::Deinitialize()
{
	if (!bInert)
	{
		FTSTicker::GetCoreTicker().RemoveTicker(FlushTickerHandle);
		FTSTicker::GetCoreTicker().RemoveTicker(GateTickerHandle);
		FCoreDelegates::ApplicationWillEnterBackgroundDelegate.Remove(BackgroundHandle);
		FCoreDelegates::ApplicationWillDeactivateDelegate.Remove(DeactivateHandle);

		EndSession();

		// Shutdown persistence: HTTP can't be awaited during teardown, so the
		// whole remaining queue goes to disk and replays next launch with its
		// original event_ids and timestamps (deduped server-side).
		TArray<TSharedPtr<FJsonObject>> Remaining;
		{
			FScopeLock Lock(&QueueLock);
			Remaining = MoveTemp(Queue);
			Queue.Reset();
		}
		TArray<FString> Lines;
		for (const TSharedPtr<FJsonObject>& Event : Remaining)
		{
			Lines.Add(SerializeEvent(Event));
			if (Lines.Num() >= MaxBatchEvents)
			{
				SpillToDisk(Lines);
				Lines.Reset();
			}
		}
		if (Lines.Num() > 0)
		{
			SpillToDisk(Lines);
		}
	}
	Super::Deinitialize();
}

// ---------------------------------------------------------------- public API

void UQueryRigSubsystem::Track(const FString& EventName, TSharedPtr<FJsonObject> Properties)
{
	const TSharedPtr<FJsonObject> Fields = MakeShared<FJsonObject>();
	if (Properties.IsValid())
	{
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Properties->Values)
		{
			Fields->SetField(Pair.Key, Pair.Value);
		}
	}
	Fields->SetStringField(TEXT("event_name"), EventName);
	TrackFields(Fields);
}

void UQueryRigSubsystem::TrackFields(TSharedPtr<FJsonObject> Fields)
{
	if (bInert || !IsCollecting() || !bSampledIn)
	{
		return;
	}
	const int64 Now = NowMs();
	if (LastEventMs > 0 && Now - LastEventMs > SessionGapMs)
	{
		StartSession(); // 30-minute idle rollover (spec §3.5)
	}
	LastEventMs = Now;
	Enqueue(Stamp(Fields));
}

void UQueryRigSubsystem::TrackEvent(const FString& EventName, const TMap<FString, FString>& Properties)
{
	Track(EventName, MapToJson(Properties));
}

void UQueryRigSubsystem::TrackFieldMap(const TMap<FString, FString>& Fields)
{
	TrackFields(MapToJson(Fields));
}

// Blueprint hands us strings, so strings are what we send. Guessing at numbers
// here would only corrupt them: the server coerces every scalar to a string
// anyway (one type per field, so queries are predictable), and round-tripping
// through a double first loses leading zeros, trailing zeros in a version like
// "1.10", and precision on ids longer than 15 digits.
TSharedPtr<FJsonObject> UQueryRigSubsystem::MapToJson(const TMap<FString, FString>& Fields)
{
	if (Fields.Num() == 0)
	{
		return nullptr;
	}
	const TSharedPtr<FJsonObject> Props = MakeShared<FJsonObject>();
	for (const TPair<FString, FString>& Pair : Fields)
	{
		Props->SetStringField(Pair.Key, Pair.Value);
	}
	return Props;
}

void UQueryRigSubsystem::SetPlayerId(const FString& InPlayerId)
{
	PlayerId = InPlayerId;
}

void UQueryRigSubsystem::Flush()
{
	if (bInert)
	{
		return;
	}
	if (IsInGameThread())
	{
		SendNextLiveBatch();
		return;
	}
	TWeakObjectPtr<UQueryRigSubsystem> WeakThis(this);
	AsyncTask(ENamedThreads::GameThread, [WeakThis]()
	{
		if (WeakThis.IsValid())
		{
			WeakThis->SendNextLiveBatch();
		}
	});
}

void UQueryRigSubsystem::RefreshGate()
{
	if (!bInert && Config.Mode == EEnabledMode::Remote)
	{
		CheckGate();
	}
}

bool UQueryRigSubsystem::IsEnabled() const
{
	return !bInert && !bDormant;
}

// ---------------------------------------------------------------- config

// Precedence (spec §2.1): built-in defaults < <Project>/Config/queryrig.json
// < queryrig.json next to the executable < Saved/Config/queryrig.json
// < environment variables < command line.
void UQueryRigSubsystem::LoadConfig()
{
	ApplyJsonConfigFile(FPaths::ProjectConfigDir() / TEXT("queryrig.json"));
	ApplyJsonConfigFile(FPaths::LaunchDir() / TEXT("queryrig.json"));
	ApplyJsonConfigFile(FPaths::ProjectSavedDir() / TEXT("Config/queryrig.json"));

	FString Value = FPlatformMisc::GetEnvironmentVariable(TEXT("QUERYRIG_API_KEY"));
	if (!Value.IsEmpty())
	{
		Config.ApiKey = Value;
	}
	Value = FPlatformMisc::GetEnvironmentVariable(TEXT("QUERYRIG_ENDPOINT"));
	if (!Value.IsEmpty())
	{
		Config.Endpoint = Value;
	}
	Value = FPlatformMisc::GetEnvironmentVariable(TEXT("QUERYRIG_ENABLED"));
	if (!Value.IsEmpty())
	{
		Config.Mode = ParseEnabledString(Value, Config.Mode);
	}

	if (FParse::Value(FCommandLine::Get(), TEXT("queryrig-api-key="), Value))
	{
		Config.ApiKey = Value;
	}
	if (FParse::Value(FCommandLine::Get(), TEXT("queryrig-endpoint="), Value))
	{
		Config.Endpoint = Value;
	}
	if (FParse::Value(FCommandLine::Get(), TEXT("queryrig-enabled="), Value))
	{
		Config.Mode = ParseEnabledString(Value, Config.Mode);
	}
}

void UQueryRigSubsystem::ApplyJsonConfigFile(const FString& FilePath)
{
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *FilePath))
	{
		return;
	}
	TSharedPtr<FJsonObject> Obj;
	const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(Text);
	if (!FJsonSerializer::Deserialize(Reader, Obj) || !Obj.IsValid())
	{
		UE_LOG(LogQueryRig, Warning, TEXT("Ignoring unparseable config at %s"), *FilePath);
		return;
	}

	Obj->TryGetStringField(TEXT("api_key"), Config.ApiKey);
	Obj->TryGetStringField(TEXT("endpoint"), Config.Endpoint);
	Obj->TryGetNumberField(TEXT("flush_interval_s"), Config.FlushIntervalS);
	Obj->TryGetNumberField(TEXT("flush_max_events"), Config.FlushMaxEvents);
	Obj->TryGetNumberField(TEXT("disk_queue_max_bytes"), Config.DiskQueueMaxBytes);
	Obj->TryGetNumberField(TEXT("sample_rate"), Config.SampleRate);
	Obj->TryGetNumberField(TEXT("gate_poll_interval_s"), Config.GatePollIntervalS);

	// `enabled` is bool OR the string "remote".
	if (const TSharedPtr<FJsonValue>* Enabled = Obj->Values.Find(TEXT("enabled")))
	{
		if ((*Enabled)->Type == EJson::Boolean)
		{
			Config.Mode = (*Enabled)->AsBool() ? EEnabledMode::HardOn : EEnabledMode::HardOff;
		}
		else if ((*Enabled)->Type == EJson::String)
		{
			Config.Mode = ParseEnabledString((*Enabled)->AsString(), Config.Mode);
		}
	}
}

UQueryRigSubsystem::EEnabledMode UQueryRigSubsystem::ParseEnabledString(const FString& Value, EEnabledMode Fallback)
{
	const FString Lower = Value.ToLower();
	if (Lower == TEXT("true") || Lower == TEXT("1") || Lower == TEXT("on") || Lower == TEXT("yes"))
	{
		return EEnabledMode::HardOn;
	}
	if (Lower == TEXT("false") || Lower == TEXT("0") || Lower == TEXT("off") || Lower == TEXT("no"))
	{
		return EEnabledMode::HardOff;
	}
	if (Lower == TEXT("remote"))
	{
		return EEnabledMode::Remote;
	}
	return Fallback;
}

// ---------------------------------------------------------------- sessions

void UQueryRigSubsystem::StartSession()
{
	SessionId = NewUuid();
	SessionStartMs = NowMs();
	LastEventMs = SessionStartMs;
	// Per-session sampling (spec §3.8): decide once so sampled-in sessions
	// are complete and funnels stay coherent.
	const double Rate = FMath::Clamp(Config.SampleRate, 0.0, 1.0);
	bSampledIn = static_cast<int32>((GetTypeHash(SessionId) & 0x7fffffff) % 10000) < static_cast<int32>(Rate * 10000.0);
	if (bSampledIn && IsCollecting())
	{
		const TSharedPtr<FJsonObject> Props = MakeShared<FJsonObject>();
		Props->SetStringField(TEXT("event_name"), TEXT("session_start"));
		Enqueue(Stamp(Props));
	}
}

void UQueryRigSubsystem::EndSession()
{
	if (SessionId.IsEmpty() || !bSampledIn || !IsCollecting())
	{
		return;
	}
	const TSharedPtr<FJsonObject> Props = MakeShared<FJsonObject>();
	Props->SetStringField(TEXT("event_name"), TEXT("session_end"));
	Props->SetNumberField(TEXT("duration_s"), (NowMs() - SessionStartMs) / 1000.0);
	Enqueue(Stamp(Props));
}

// ---------------------------------------------------------------- queueing

// Stamp everything at enqueue time (spec §3.3) so retries can't change the
// event. Explicit values in the properties win over SDK defaults.
//
// Values go out as their natural JSON types; the server coerces every scalar to
// a string so each field has one predictable type to query. Never pre-stringify
// here — that would lose the distinction between "not sent" and "sent empty".
TSharedPtr<FJsonObject> UQueryRigSubsystem::Stamp(const TSharedPtr<FJsonObject>& Properties)
{
	const TSharedPtr<FJsonObject> Event = MakeShared<FJsonObject>();
	if (Properties.IsValid())
	{
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Properties->Values)
		{
			Event->SetField(Pair.Key, Pair.Value);
		}
	}
	if (!Event->HasField(TEXT("event_id")))
	{
		Event->SetStringField(TEXT("event_id"), NewUuid());
	}
	if (!Event->HasField(TEXT("timestamp")))
	{
		Event->SetNumberField(TEXT("timestamp"), static_cast<double>(NowMs()));
	}
	if (!Event->HasField(TEXT("session_id")))
	{
		Event->SetStringField(TEXT("session_id"), SessionId);
	}
	if (!Event->HasField(TEXT("player_id")))
	{
		Event->SetStringField(TEXT("player_id"), PlayerId);
	}
	if (!Event->HasField(TEXT("platform")))
	{
		Event->SetStringField(TEXT("platform"), FString(FPlatformProperties::IniPlatformName()).ToLower());
	}
	if (!Event->HasField(TEXT("build_version")))
	{
		Event->SetStringField(TEXT("build_version"), CachedBuildVersion);
	}
	return Event;
}

void UQueryRigSubsystem::Enqueue(const TSharedPtr<FJsonObject>& Event)
{
	bool bRequestFlush = false;
	{
		FScopeLock Lock(&QueueLock);
		Queue.Add(Event);
		while (Queue.Num() > MemoryQueueMax) // drop OLDEST on overflow
		{
			Queue.RemoveAt(0);
			DroppedOverflow++;
		}
		bRequestFlush = Queue.Num() >= Config.FlushMaxEvents;
	}
	if (bRequestFlush)
	{
		Flush();
	}
}

bool UQueryRigSubsystem::IsCollecting() const
{
	switch (Config.Mode)
	{
	case EEnabledMode::HardOn: return true;
	case EEnabledMode::HardOff: return false;
	default: return !bDormant; // remote: the gate decides
	}
}

bool UQueryRigSubsystem::OnFlushTick(float /*DeltaTime*/)
{
	int32 Pending = 0;
	{
		FScopeLock Lock(&QueueLock);
		Pending = Queue.Num();
	}
	if (Pending > 0)
	{
		SendNextLiveBatch();
	}
	else
	{
		DrainDiskOne(); // idle intervals still trickle the disk queue
	}
	return true; // keep ticking
}

// ---------------------------------------------------------------- delivery

void UQueryRigSubsystem::SendNextLiveBatch()
{
	if (bDormant || bRequestInFlight)
	{
		return;
	}

	// Pull up to a spec-sized batch; put byte-cap overflow back at the front.
	TArray<TSharedPtr<FJsonObject>> Events;
	int32 Dropped = 0;
	{
		FScopeLock Lock(&QueueLock);
		const int32 Take = FMath::Min(Queue.Num(), MaxBatchEvents);
		if (Take == 0)
		{
			DrainDiskOne();
			return;
		}
		Events.Append(Queue.GetData(), Take);
		Queue.RemoveAt(0, Take);
		Dropped = DroppedOverflow;
		DroppedOverflow = 0;
	}
	if (Dropped > 0)
	{
		UE_LOG(LogQueryRig, Warning, TEXT("Memory queue overflowed; dropped %d oldest events"), Dropped);
	}

	TArray<FString> Lines;
	int32 Bytes = 0;
	int32 Used = 0;
	for (const TSharedPtr<FJsonObject>& Event : Events)
	{
		const FString Line = SerializeEvent(Event);
		if (Lines.Num() > 0 && Bytes + Line.Len() > FlushMaxBytes)
		{
			break;
		}
		Bytes += Line.Len();
		Lines.Add(Line);
		Used++;
	}
	if (Used < Events.Num())
	{
		FScopeLock Lock(&QueueLock);
		for (int32 i = Events.Num() - 1; i >= Used; --i)
		{
			Queue.Insert(Events[i], 0);
		}
	}

	TWeakObjectPtr<UQueryRigSubsystem> WeakThis(this);
	SendBatch(Lines, [WeakThis, Lines](ESendOutcome Outcome)
	{
		if (!WeakThis.IsValid())
		{
			return;
		}
		UQueryRigSubsystem* Self = WeakThis.Get();
		switch (Outcome)
		{
		case ESendOutcome::Delivered:
			Self->SendNextLiveBatch(); // continue the queue, or drain disk
			break;
		case ESendOutcome::Rejected:
			Self->AppendRejected(Lines);
			break;
		default: // Retryable or Dormant: spill; the drain loop owns retries
			Self->SpillToDisk(Lines);
			break;
		}
	});
}

// One batch -> one POST. Pure delivery attempt — the CALLER decides what to
// do with the lines, so a disk-drained batch is never re-spilled into a
// duplicate file.
void UQueryRigSubsystem::SendBatch(TArray<FString> Lines, TFunction<void(ESendOutcome)> OnSettled)
{
	const FString Body = TEXT("[") + FString::Join(Lines, TEXT(",")) + TEXT("]");
	const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(Config.Endpoint);
	Request->SetVerb(TEXT("POST"));
	Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	Request->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *Config.ApiKey));
	Request->SetHeader(TEXT("User-Agent"), FString::Printf(TEXT("queryrig-unreal/%s"), SdkVersion));
	Request->SetTimeout(HttpTimeoutS);
	Request->SetContentAsString(Body);

	bRequestInFlight = true;
	TWeakObjectPtr<UQueryRigSubsystem> WeakThis(this);
	Request->OnProcessRequestComplete().BindLambda(
		[WeakThis, OnSettled](FHttpRequestPtr, FHttpResponsePtr Response, bool bConnected)
		{
			if (!WeakThis.IsValid())
			{
				return;
			}
			UQueryRigSubsystem* Self = WeakThis.Get();
			Self->bRequestInFlight = false;
			const int32 Code = (bConnected && Response.IsValid()) ? Response->GetResponseCode() : 0;

			if (Code == 200)
			{
				Self->NoteSuccess();
				TSharedPtr<FJsonObject> Parsed;
				const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(Response->GetContentAsString());
				if (FJsonSerializer::Deserialize(Reader, Parsed) && Parsed.IsValid())
				{
					int32 Rejected = 0;
					if (Parsed->TryGetNumberField(TEXT("rejected"), Rejected) && Rejected > 0)
					{
						UE_LOG(LogQueryRig, Warning, TEXT("Server rejected %d event(s) — integration bug, see response results"), Rejected);
					}
				}
				OnSettled(ESendOutcome::Delivered);
			}
			else if (Code == 401)
			{
				// Key unknown/disabled: go dormant, keep events on disk, let
				// the gate poll decide when to wake up (spec §1 responses).
				Self->bDormant = true;
				OnSettled(ESendOutcome::Dormant);
			}
			else if (Code == 400 || Code == 413)
			{
				UE_LOG(LogQueryRig, Warning, TEXT("Batch rejected with HTTP %d — dropped (see Saved/QueryRig/rejected.ndjson)"), Code);
				OnSettled(ESendOutcome::Rejected);
			}
			else
			{
				// Network error, timeout, 5xx: retryable.
				Self->NoteFailure();
				OnSettled(ESendOutcome::Retryable);
			}
		});
	Request->ProcessRequest();
}

void UQueryRigSubsystem::NoteSuccess()
{
	ConsecutiveFailures = 0;
	NextDrainMs = 0;
}

void UQueryRigSubsystem::NoteFailure()
{
	ConsecutiveFailures++;
	int64 Backoff = FMath::Min(BackoffBaseMs << FMath::Min(ConsecutiveFailures - 1, 10), BackoffCapMs);
	Backoff += static_cast<int64>(Backoff * FMath::FRandRange(-0.2, 0.2)); // jitter
	NextDrainMs = NowMs() + Backoff;
}

// ---------------------------------------------------------------- disk queue

FString UQueryRigSubsystem::DataDir() const
{
	return FPaths::ProjectSavedDir() / TEXT("QueryRig");
}

FString UQueryRigSubsystem::QueueDir() const
{
	return DataDir() / TEXT("queue");
}

void UQueryRigSubsystem::SpillToDisk(const TArray<FString>& Lines)
{
	const FString Path = FString::Printf(TEXT("%s/%lld_%06d.ndjson"), *QueueDir(), NowMs(), FMath::Rand() % 1000000);
	if (!FFileHelper::SaveStringToFile(FString::Join(Lines, TEXT("\n")), *Path,
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		UE_LOG(LogQueryRig, Warning, TEXT("Cannot write disk queue; %d events lost"), Lines.Num());
		return;
	}
	EnforceDiskCap();
}

void UQueryRigSubsystem::EnforceDiskCap()
{
	TArray<FString> Files;
	IFileManager::Get().FindFiles(Files, *(QueueDir() / TEXT("*.ndjson")), true, false);
	Files.Sort(); // timestamp-named => oldest first
	int64 Total = 0;
	TArray<int64> Sizes;
	for (const FString& File : Files)
	{
		const int64 Size = IFileManager::Get().FileSize(*(QueueDir() / File));
		Sizes.Add(Size);
		Total += Size;
	}
	for (int32 i = 0; Total > Config.DiskQueueMaxBytes && i < Files.Num(); ++i) // drop OLDEST first
	{
		IFileManager::Get().Delete(*(QueueDir() / Files[i]));
		Total -= Sizes[i];
	}
}

// Send at most one disk batch (spec §3.6: a trickle, not a burst), honoring
// the failure backoff. Original event_ids/timestamps ride along unchanged.
void UQueryRigSubsystem::DrainDiskOne()
{
	if (bDormant || bRequestInFlight || NowMs() < NextDrainMs)
	{
		return;
	}
	TArray<FString> Files;
	IFileManager::Get().FindFiles(Files, *(QueueDir() / TEXT("*.ndjson")), true, false);
	if (Files.Num() == 0)
	{
		return;
	}
	Files.Sort();
	const FString Path = QueueDir() / Files[0];

	TArray<FString> Lines;
	FFileHelper::LoadFileToStringArrayWithPredicate(Lines, *Path,
		[](const FString& Line) { return !Line.TrimStartAndEnd().IsEmpty(); });
	if (Lines.Num() == 0)
	{
		IFileManager::Get().Delete(*Path);
		return;
	}

	TWeakObjectPtr<UQueryRigSubsystem> WeakThis(this);
	SendBatch(Lines, [WeakThis, Path, Lines](ESendOutcome Outcome)
	{
		if (!WeakThis.IsValid())
		{
			return;
		}
		switch (Outcome)
		{
		case ESendOutcome::Delivered:
			// Delete only after success — a crash in between resends, and the
			// server dedupes on event_id (same contract as the server's own
			// R2 replay).
			IFileManager::Get().Delete(*Path);
			break;
		case ESendOutcome::Rejected:
			WeakThis->AppendRejected(Lines);
			IFileManager::Get().Delete(*Path);
			break;
		default: // Retryable (backoff set) or Dormant: leave the file for later
			break;
		}
	});
}

void UQueryRigSubsystem::AppendRejected(const TArray<FString>& Lines)
{
	const FString Path = DataDir() / TEXT("rejected.ndjson");
	if (IFileManager::Get().FileSize(*Path) > RejectedFileMaxBytes)
	{
		return; // capped, best-effort
	}
	FFileHelper::SaveStringToFile(FString::Join(Lines, TEXT("\n")) + TEXT("\n"), *Path,
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM, &IFileManager::Get(), FILEWRITE_Append);
}

// ---------------------------------------------------------------- remote gate

void UQueryRigSubsystem::CheckGate()
{
	const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(EndpointOrigin() + TEXT("/v1/config"));
	Request->SetVerb(TEXT("GET"));
	Request->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *Config.ApiKey));
	Request->SetHeader(TEXT("User-Agent"), FString::Printf(TEXT("queryrig-unreal/%s"), SdkVersion));
	Request->SetTimeout(HttpTimeoutS);

	TWeakObjectPtr<UQueryRigSubsystem> WeakThis(this);
	Request->OnProcessRequestComplete().BindLambda(
		[WeakThis](FHttpRequestPtr, FHttpResponsePtr Response, bool bConnected)
		{
			if (!WeakThis.IsValid())
			{
				return;
			}
			UQueryRigSubsystem* Self = WeakThis.Get();
			const int32 Code = (bConnected && Response.IsValid()) ? Response->GetResponseCode() : 0;

			bool bEnabled = false;
			if (Code == 200)
			{
				TSharedPtr<FJsonObject> Parsed;
				const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(Response->GetContentAsString());
				if (!FJsonSerializer::Deserialize(Reader, Parsed) || !Parsed.IsValid()
					|| !Parsed->TryGetBoolField(TEXT("enabled"), bEnabled))
				{
					return;
				}
			}
			else if (Code != 401)
			{
				// Fetch failure: keep the cached answer (spec §3.2) — losing
				// data over a flaky config fetch is worse than sending.
				return;
			}
			Self->SaveCachedGate(bEnabled);
			Self->bDormant = !bEnabled;
		});
	Request->ProcessRequest();
}

bool UQueryRigSubsystem::LoadCachedGate() const
{
	FString Text;
	if (FFileHelper::LoadFileToString(Text, *(DataDir() / TEXT("gate.json"))))
	{
		TSharedPtr<FJsonObject> Parsed;
		const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(Text);
		bool bEnabled = true;
		if (FJsonSerializer::Deserialize(Reader, Parsed) && Parsed.IsValid()
			&& Parsed->TryGetBoolField(TEXT("enabled"), bEnabled))
		{
			return bEnabled;
		}
	}
	return true; // first run: default enabled
}

void UQueryRigSubsystem::SaveCachedGate(bool bEnabled) const
{
	FFileHelper::SaveStringToFile(
		FString::Printf(TEXT("{\"enabled\":%s}"), bEnabled ? TEXT("true") : TEXT("false")),
		*(DataDir() / TEXT("gate.json")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

FString UQueryRigSubsystem::EndpointOrigin() const
{
	const int32 SchemeEnd = Config.Endpoint.Find(TEXT("://"));
	if (SchemeEnd == INDEX_NONE)
	{
		return Config.Endpoint;
	}
	const int32 PathStart = Config.Endpoint.Find(TEXT("/"), ESearchCase::CaseSensitive, ESearchDir::FromStart, SchemeEnd + 3);
	return PathStart == INDEX_NONE ? Config.Endpoint : Config.Endpoint.Left(PathStart);
}

// ---------------------------------------------------------------- utils

FString UQueryRigSubsystem::DefaultPlayerId() const
{
	FString Id = FPlatformMisc::GetLoginId();
	if (!Id.IsEmpty())
	{
		return Id;
	}
	// Some platforms return "": persist a random device id instead.
	const FString Path = DataDir() / TEXT("device_id");
	if (FFileHelper::LoadFileToString(Id, *Path) && !Id.TrimStartAndEnd().IsEmpty())
	{
		return Id.TrimStartAndEnd();
	}
	Id = NewUuid();
	FFileHelper::SaveStringToFile(Id, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	return Id;
}

FString UQueryRigSubsystem::SerializeEvent(const TSharedPtr<FJsonObject>& Event)
{
	FString Line;
	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Line);
	FJsonSerializer::Serialize(Event.ToSharedRef(), Writer);
	return Line;
}

int64 UQueryRigSubsystem::NowMs()
{
	return static_cast<int64>((FDateTime::UtcNow() - FDateTime(1970, 1, 1)).GetTotalMilliseconds());
}

FString UQueryRigSubsystem::NewUuid()
{
	return FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens).ToLower();
}
