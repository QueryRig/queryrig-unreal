#include "QueryRigConformanceCommandlet.h"

#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HttpManager.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "QueryRigSubsystem.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

DEFINE_LOG_CATEGORY_STATIC(LogQueryRigConformance, Log, All);

namespace
{
	FString GHarness = TEXT("http://127.0.0.1:8788");
	int32 GFailures = 0;

	// The commandlet owns the "frame loop": HTTP completion delegates and
	// FTSTicker delegates only fire when the game thread ticks, so every wait
	// pumps both.
	void Pump(double Seconds)
	{
		const double End = FPlatformTime::Seconds() + Seconds;
		while (FPlatformTime::Seconds() < End)
		{
			FTSTicker::GetCoreTicker().Tick(0.05f);
			FHttpModule::Get().GetHttpManager().Tick(0.05f);
			FPlatformProcess::Sleep(0.05f);
		}
	}

	bool WaitUntil(const TFunction<bool()>& Cond, double TimeoutS)
	{
		const double End = FPlatformTime::Seconds() + TimeoutS;
		while (FPlatformTime::Seconds() < End)
		{
			if (Cond())
			{
				return true;
			}
			Pump(0.1);
		}
		return Cond();
	}

	/** Blocking HTTP against the harness control API. */
	bool Req(const FString& Verb, const FString& Path, const FString& Body, FString* OutBody = nullptr)
	{
		const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
		Request->SetURL(GHarness + Path);
		Request->SetVerb(Verb);
		if (!Body.IsEmpty())
		{
			Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
			Request->SetContentAsString(Body);
		}
		Request->SetTimeout(5.0f);

		bool bDone = false;
		bool bOk = false;
		FString Received;
		Request->OnProcessRequestComplete().BindLambda(
			[&bDone, &bOk, &Received](FHttpRequestPtr, FHttpResponsePtr Response, bool bConnected)
			{
				bOk = bConnected && Response.IsValid() && Response->GetResponseCode() < 400;
				if (Response.IsValid())
				{
					Received = Response->GetContentAsString();
				}
				bDone = true;
			});
		Request->ProcessRequest();
		const double End = FPlatformTime::Seconds() + 6.0;
		while (!bDone && FPlatformTime::Seconds() < End)
		{
			FTSTicker::GetCoreTicker().Tick(0.05f);
			FHttpModule::Get().GetHttpManager().Tick(0.05f);
			FPlatformProcess::Sleep(0.02f);
		}
		if (OutBody)
		{
			*OutBody = Received;
		}
		return bDone && bOk;
	}

	TSharedPtr<FJsonObject> ReqObject(const FString& Path)
	{
		FString Text;
		Req(TEXT("GET"), Path, TEXT(""), &Text);
		TSharedPtr<FJsonObject> Parsed;
		FJsonSerializer::Deserialize(TJsonReaderFactory<TCHAR>::Create(Text), Parsed);
		return Parsed;
	}

	TArray<TSharedPtr<FJsonValue>> ReqArray(const FString& Path)
	{
		FString Text;
		Req(TEXT("GET"), Path, TEXT(""), &Text);
		TArray<TSharedPtr<FJsonValue>> Parsed;
		FJsonSerializer::Deserialize(TJsonReaderFactory<TCHAR>::Create(Text), Parsed);
		return Parsed;
	}

	int32 HarnessRows()
	{
		const TSharedPtr<FJsonObject> State = ReqObject(TEXT("/__harness/state"));
		return State.IsValid() ? static_cast<int32>(State->GetNumberField(TEXT("rows"))) : -1;
	}

	int32 EventsRequestCount()
	{
		int32 Count = 0;
		for (const TSharedPtr<FJsonValue>& Value : ReqArray(TEXT("/__harness/requests")))
		{
			const TSharedPtr<FJsonObject>* Obj;
			if (Value->TryGetObject(Obj) && (*Obj)->GetStringField(TEXT("path")) == TEXT("/v1/events"))
			{
				Count++;
			}
		}
		return Count;
	}

	void Check(bool bCond, const TCHAR* Label)
	{
		if (bCond)
		{
			UE_LOG(LogQueryRigConformance, Display, TEXT("  PASS  %s"), Label);
		}
		else
		{
			UE_LOG(LogQueryRigConformance, Error, TEXT("  FAIL  %s"), Label);
			GFailures++;
		}
	}

	// The SDK's strongest config source below env/cmdline is Saved/Config —
	// each phase rewrites it before booting a fresh GameInstance.
	void WriteSdkConfig(const FString& EnabledLiteral)
	{
		const FString Json = FString::Printf(
			TEXT("{\"api_key\":\"test-key\",\"endpoint\":\"%s/v1/events\",\"enabled\":%s,")
			TEXT("\"flush_interval_s\":999,\"gate_poll_interval_s\":999}"),
			*GHarness, *EnabledLiteral);
		FFileHelper::SaveStringToFile(Json, *(FPaths::ProjectSavedDir() / TEXT("Config/queryrig.json")));
	}

	struct FPhase
	{
		UGameInstance* GameInstance = nullptr;
		UQueryRigSubsystem* Rig = nullptr;

		explicit FPhase(const FString& EnabledLiteral)
		{
			WriteSdkConfig(EnabledLiteral);
			GameInstance = NewObject<UGameInstance>(GEngine);
			GameInstance->AddToRoot();
			GameInstance->InitializeStandalone();
			Rig = GameInstance->GetSubsystem<UQueryRigSubsystem>();
		}

		~FPhase()
		{
			// Tear the dummy world down explicitly: GameInstance::Shutdown()
			// alone leaves its world subsystems initialized, which trips an
			// ensure (and a nonzero process exit) when GC collects them.
			UWorld* World = GameInstance->GetWorld();
			GameInstance->Shutdown();
			if (World)
			{
				World->DestroyWorld(false);
			}
			GameInstance->RemoveFromRoot();
		}
	};

	TSharedPtr<FJsonObject> MakeProps(std::initializer_list<TPair<FString, double>> Numbers)
	{
		TSharedPtr<FJsonObject> Props = MakeShared<FJsonObject>();
		for (const TPair<FString, double>& Pair : Numbers)
		{
			Props->SetNumberField(Pair.Key, Pair.Value);
		}
		return Props;
	}
}

int32 UQueryRigConformanceCommandlet::Main(const FString& Params)
{
	const FString EnvHarness = FPlatformMisc::GetEnvironmentVariable(TEXT("QUERYRIG_TEST_HARNESS"));
	if (!EnvHarness.IsEmpty())
	{
		GHarness = EnvHarness;
	}

	if (!Req(TEXT("GET"), TEXT("/v1/health"), TEXT("")))
	{
		UE_LOG(LogQueryRigConformance, Error,
			TEXT("Harness not reachable at %s — start it with `npm run sdk-harness` in the queryrig repo"), *GHarness);
		return 2;
	}
	Req(TEXT("POST"), TEXT("/__harness/reset"), TEXT("{}"));
	IFileManager::Get().DeleteDirectory(*(FPaths::ProjectSavedDir() / TEXT("QueryRig")), false, true);

	// ---- Phase A: hard-on — delivery, stamping, and the outage cycle.
	{
		FPhase Phase(TEXT("true"));
		Phase.Rig->Track(TEXT("level_complete"), MakeProps({{TEXT("level"), 3}, {TEXT("duration_s"), 82.5}}));
		Phase.Rig->Track(TEXT("item_pickup"), nullptr);
		Phase.Rig->Track(TEXT("enemy_killed"), nullptr);
		// No event_name at all: nothing is reserved, so an event is any set of
		// fields. Proves the server accepts the studio's own vocabulary — and
		// goes through the Blueprint path, which sends values verbatim.
		TMap<FString, FString> BossFields;
		BossFields.Add(TEXT("kind"), TEXT("boss_defeated"));
		BossFields.Add(TEXT("boss"), TEXT("warden"));
		Phase.Rig->TrackFieldMap(BossFields);
		Phase.Rig->Flush();

		// 4 tracked + the automatic session_start.
		Check(WaitUntil([] { return HarnessRows() >= 5; }, 5.0), TEXT("delivery: 5 rows arrive after flush"));

		TSet<FString> Names, Sessions, Ids;
		bool bStamped = true;
		TSharedPtr<FJsonObject> LevelProps, UnnamedProps;
		for (const TSharedPtr<FJsonValue>& Value : ReqArray(TEXT("/__harness/rows")))
		{
			const TSharedPtr<FJsonObject>* Row;
			const TSharedPtr<FJsonObject>* Props;
			// Only timestamp/received_at/event_id (+ project_id) are columns;
			// every other field lands in `properties` under its own name.
			if (!Value->TryGetObject(Row) || !(*Row)->TryGetObjectField(TEXT("properties"), Props))
			{
				continue;
			}
			const FString Name = (*Props)->HasField(TEXT("event_name"))
				? (*Props)->GetStringField(TEXT("event_name")) : FString();
			Names.Add(Name);
			Sessions.Add((*Props)->GetStringField(TEXT("session_id")));
			bool bUnique = false;
			Ids.Add((*Row)->GetStringField(TEXT("event_id")), &bUnique);
			bStamped = bStamped && !(*Props)->GetStringField(TEXT("player_id")).IsEmpty()
				&& !(*Props)->GetStringField(TEXT("platform")).IsEmpty();
			if (Name == TEXT("level_complete"))
			{
				LevelProps = *Props;
			}
			else if (Name.IsEmpty())
			{
				UnnamedProps = *Props;
			}
		}
		Check(Names.Contains(TEXT("session_start")) && Names.Contains(TEXT("level_complete")),
			TEXT("delivery: session_start + tracked events present"));
		Check(bStamped, TEXT("stamping: player_id + platform set on every row"));
		Check(Sessions.Num() == 1, TEXT("stamping: one session_id across the batch"));
		Check(Ids.Num() == 5, TEXT("stamping: event_ids unique"));
		Check(UnnamedProps.IsValid()
			&& UnnamedProps->GetStringField(TEXT("kind")) == TEXT("boss_defeated")
			&& UnnamedProps->GetStringField(TEXT("boss")) == TEXT("warden"),
			TEXT("fields: an event with no event_name is accepted and keeps its own fields"));
		// Every property scalar is stored as a string, whatever the client sent
		// — one type per path, so equality in a query always works.
		Check(LevelProps.IsValid()
			&& LevelProps->GetStringField(TEXT("level")) == TEXT("3")
			&& LevelProps->GetStringField(TEXT("duration_s")) == TEXT("82.5"),
			TEXT("stamping: property values arrive as strings"));

		bool bUserAgent = false;
		for (const TSharedPtr<FJsonValue>& Value : ReqArray(TEXT("/__harness/requests")))
		{
			const TSharedPtr<FJsonObject>* RequestRow;
			if (Value->TryGetObject(RequestRow)
				&& (*RequestRow)->GetStringField(TEXT("userAgent")).Contains(TEXT("queryrig-unreal/")))
			{
				bUserAgent = true;
			}
		}
		Check(bUserAgent, TEXT("wire: User-Agent identifies the SDK"));

		// Acknowledgement after the 15s CH deadline must not trigger a retry.
		Pump(0.2);
		const int32 SlowBefore = HarnessRows();
		const int32 SlowRequestsBefore = EventsRequestCount();
		Req(TEXT("POST"), TEXT("/__harness/mode"), TEXT("{\"events\":\"slow-fallback\"}"));
		Phase.Rig->Track(TEXT("slow_fallback"), nullptr);
		Phase.Rig->Flush();
		Check(WaitUntil([SlowBefore] { return HarnessRows() >= SlowBefore + 1; }, 22.0),
			TEXT("fallback: a 16-second R2 acknowledgement arrives"));
		Pump(0.3);
		Phase.Rig->Flush();
		Pump(0.5);
		Check(EventsRequestCount() == SlowRequestsBefore + 1, TEXT("fallback: exactly one POST, no premature retry"));
		Req(TEXT("POST"), TEXT("/__harness/mode"), TEXT("{\"events\":\"ok\"}"));

		// ---- Outage: break the backend, verify spill + zero-loss drain.
		const int32 Before = HarnessRows();
		Req(TEXT("POST"), TEXT("/__harness/mode"), TEXT("{\"events\":\"unavailable\"}"));
		Phase.Rig->Track(TEXT("during_outage_1"), nullptr);
		Phase.Rig->Track(TEXT("during_outage_2"), nullptr);
		Phase.Rig->Track(TEXT("during_outage_3"), nullptr);
		Phase.Rig->Flush();
		Pump(1.5);
		Check(HarnessRows() == Before, TEXT("outage: nothing stored while backend is down"));

		Req(TEXT("POST"), TEXT("/__harness/mode"), TEXT("{\"events\":\"ok\"}"));
		Pump(7.0); // ride out the first retry backoff (~5s + jitter)
		Phase.Rig->Flush(); // empty queue -> drains one disk batch
		Check(WaitUntil([Before] { return HarnessRows() >= Before + 3; }, 10.0),
			TEXT("outage: disk queue drains after recovery — zero loss"));
		const TSharedPtr<FJsonObject> State = ReqObject(TEXT("/__harness/state"));
		Check(State.IsValid() && static_cast<int32>(State->GetNumberField(TEXT("duplicates"))) == 0,
			TEXT("outage: replay produced no duplicates"));
	}

	// ---- Regression: immutable payloads, UTF-8 batches, and shutdown during HTTP.
	Req(TEXT("POST"), TEXT("/__harness/mode"), TEXT("{\"events\":\"hang\"}"));
	{
		FPhase Phase(TEXT("true"));
		TSharedPtr<FJsonObject> Nested = MakeShared<FJsonObject>();
		Nested->SetStringField(TEXT("value"), TEXT("before"));
		TSharedPtr<FJsonObject> Props = MakeShared<FJsonObject>();
		Props->SetObjectField(TEXT("nested"), Nested);
		Phase.Rig->Track(TEXT("immutable_regression"), Props);
		Nested->SetStringField(TEXT("value"), TEXT("after"));
		for (int32 i = 0; i < 90; ++i)
		{
			TSharedPtr<FJsonObject> Unicode = MakeShared<FJsonObject>();
			Unicode->SetStringField(TEXT("text"), FString::ChrN(1000, TCHAR(0x4E2D)));
			Phase.Rig->Track(TEXT("unicode_regression"), Unicode);
		}
		Phase.Rig->Flush();
		Pump(0.3); // request remains unacknowledged while the game shuts down
		Phase.Rig->Track(TEXT("pending_shutdown_regression"), nullptr);
	}
	{
		TArray<FString> Files;
		IFileManager::Get().FindFilesRecursive(Files, *FPaths::ProjectSavedDir(), TEXT("*.ndjson"), true, false);
		int32 UnicodeCount = 0;
		bool bImmutable = false, bPending = false, bSizeValid = true;
		for (const FString& File : Files)
		{
			TArray<FString> Lines;
			FFileHelper::LoadFileToStringArray(Lines, *File);
			const FString Body = TEXT("[") + FString::Join(Lines, TEXT(",")) + TEXT("]");
			bSizeValid &= FTCHARToUTF8(*Body).Length() <= 200000 && Lines.Num() <= 500;
			for (const FString& Line : Lines)
			{
				TSharedPtr<FJsonObject> Event;
				if (!FJsonSerializer::Deserialize(TJsonReaderFactory<TCHAR>::Create(Line), Event)) continue;
				FString Name;
				if (!Event->TryGetStringField(TEXT("event_name"), Name)) continue;
				if (Name == TEXT("unicode_regression")) ++UnicodeCount;
				if (Name == TEXT("pending_shutdown_regression")) bPending = true;
				if (Name == TEXT("immutable_regression"))
					bImmutable = Event->GetObjectField(TEXT("nested"))->GetStringField(TEXT("value")) == TEXT("before");
			}
		}
		Check(bSizeValid, TEXT("regression: persisted batches respect UTF-8 byte and event limits"));
		Check(UnicodeCount == 90 && bPending, TEXT("regression: shutdown retains in-flight and pending events"));
		Check(bImmutable, TEXT("regression: nested mutation after Track cannot alter queued events"));
	}
	Req(TEXT("POST"), TEXT("/__harness/mode"), TEXT("{\"events\":\"ok\"}"));

	// ---- Phase B: remote — the kill switch, both directions.
	{
		FPhase Phase(TEXT("\"remote\""));
		Check(WaitUntil([&Phase] { return Phase.Rig->IsEnabled(); }, 5.0),
			TEXT("gate: starts enabled with a live key"));

		Req(TEXT("POST"), TEXT("/__harness/keys"), TEXT("{\"key\":\"test-key\",\"disabled\":true}"));
		Phase.Rig->RefreshGate();
		Check(WaitUntil([&Phase] { return !Phase.Rig->IsEnabled(); }, 5.0),
			TEXT("gate: dashboard disable turns the SDK off"));

		const int32 EventsBefore = EventsRequestCount();
		Phase.Rig->Track(TEXT("should_be_dropped"), nullptr);
		Phase.Rig->Flush();
		Pump(1.0);
		Check(EventsRequestCount() == EventsBefore, TEXT("gate: dormant SDK sends nothing"));

		Req(TEXT("POST"), TEXT("/__harness/keys"), TEXT("{\"key\":\"test-key\",\"disabled\":false}"));
		Phase.Rig->RefreshGate();
		Check(WaitUntil([&Phase] { return Phase.Rig->IsEnabled(); }, 5.0),
			TEXT("gate: re-enable turns the SDK back on"));
	}

	// ---- Phase C: hard-off — total silence.
	{
		FPhase Phase(TEXT("false"));
		// Boot first (the previous phase's shutdown persistence has already
		// happened), then reset so the request log starts clean.
		Req(TEXT("POST"), TEXT("/__harness/reset"), TEXT("{}"));
		Phase.Rig->Track(TEXT("never_sent"), nullptr);
		Phase.Rig->Flush();
		Pump(1.0);
		const TSharedPtr<FJsonObject> State = ReqObject(TEXT("/__harness/state"));
		Check(State.IsValid() && static_cast<int32>(State->GetNumberField(TEXT("requests"))) == 0,
			TEXT("hard-off: zero network calls of any kind"));
	}

	if (GFailures == 0)
	{
		UE_LOG(LogQueryRigConformance, Display, TEXT("conformance: ALL PASS"));
	}
	return GFailures == 0 ? 0 : 1;
}
