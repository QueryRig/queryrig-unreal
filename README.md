# QueryRig SDK for Unreal Engine 5

Game analytics for [QueryRig](https://github.com/prhym/queryrig): batched,
durable event tracking with a runtime on/off switch. Implements the QueryRig
engine SDK spec (`docs/sdk-spec.md` in the queryrig repo).

- **Batched** — events queue in memory and flush every 5 s / 100 events, one
  HTTP request instead of dozens. `Track()` just stamps and enqueues; HTTP is
  async (`FHttpModule`) and delivery failures spill to disk.
- **Durable** — failed batches land in `Saved/QueryRig/queue/` and replay with
  exponential backoff. Events carry client-generated `event_id`s, so retries
  are deduplicated server-side: crash-safe, duplicate-free, offline-tolerant.
  On shutdown the remaining queue is persisted and replays next launch.
- **Runtime-switchable** — telemetry on/off is never a rebuild: a
  `queryrig.json` config file, an env var, or the remote gate (flip the ingest
  key in the QueryRig dashboard and the installed fleet obeys within ~5
  minutes).

## Install

Copy this repo into your project's `Plugins/QueryRig/` folder (or
`Engine/Plugins/Marketplace/` for an engine-wide install) and enable it in
Edit → Plugins. C++ projects pick it up on the next build; Blueprint-only
projects can use a prebuilt package (see Packaging below).

## Use

C++ (`"QueryRig"` in your module's dependencies):

```cpp
#include "QueryRigSubsystem.h"

UQueryRigSubsystem* Rig = GetGameInstance()->GetSubsystem<UQueryRigSubsystem>();
TSharedPtr<FJsonObject> Props = MakeShared<FJsonObject>();
Props->SetNumberField(TEXT("level"), 3);
Props->SetNumberField(TEXT("duration_s"), 82.5);
Rig->Track(TEXT("level_complete"), Props);
```

Blueprint: get the **QueryRig** subsystem from your Game Instance and call
**Track Event** (map values that parse as numbers/booleans are sent typed),
**Set Player Id**, **Flush**, or **Is Enabled**.

`session_id`, `player_id`, `platform`, `build_version`, `timestamp`, and
`event_id` are stamped automatically (explicit properties win).
`session_start` / `session_end` events are emitted for you, with a 30-minute
idle rollover.

## Configure

Copy `queryrig.example.json` to `Config/queryrig.json` in your project — the
key and endpoint come from the Keys tab of your QueryRig project:

```json
{
  "api_key": "qr_ingest_...",
  "endpoint": "https://ingest.example.com/v1/events",
  "enabled": "remote"
}
```

Resolution order: built-in defaults < `<Project>/Config/queryrig.json` <
`queryrig.json` **next to the executable** < `Saved/Config/queryrig.json` <
`QUERYRIG_API_KEY` / `QUERYRIG_ENDPOINT` / `QUERYRIG_ENABLED` env vars <
`-queryrig-*=` command-line args. Promote the same binary from dev to playtest
by dropping a different `queryrig.json` next to the executable — never by
rebuilding.

**Packaged builds:** `Config/queryrig.json` is not staged automatically. Add
`Config` to *Project Settings → Packaging → Additional Non-Asset Directories
to Copy*, or ship the file next to the executable instead.

`enabled` is `true`, `false`, or `"remote"` (default):

| Value | Behavior |
|---|---|
| `false` | Hard off. Zero network calls — verifiable with a packet capture. |
| `true` | Send unconditionally. |
| `"remote"` | The server decides: the SDK polls `/v1/config` and obeys the ingest key's enabled state, so telemetry can be flipped after the build has shipped. |

Optional fields: `flush_interval_s` (5), `flush_max_events` (100),
`disk_queue_max_bytes` (8 MB), `sample_rate` (1.0, sampled per session so
funnels stay coherent).

## Local development

Run the SDK conformance harness from the queryrig repo (`npm run
sdk-harness`, port 8788) and point `Saved/Config/queryrig.json` at
`http://127.0.0.1:8788/v1/events` with key `test-key`.

## Packaging

Build a distributable (prebuilt binaries, usable by Blueprint-only projects):

```
"C:\Program Files\Epic Games\UE_5.8\Engine\Build\BatchFiles\RunUAT.bat" ^
  BuildPlugin -Plugin="<this repo>\QueryRig.uplugin" ^
  -Package="<output dir>" -TargetPlatforms=Win64
```
