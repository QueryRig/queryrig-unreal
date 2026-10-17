# QueryRig SDK for Unreal Engine 5

Game analytics for [QueryRig](https://queryrig.com): batched, durable event
tracking with a runtime on/off switch.

- **Batched** — events queue in memory and flush every 5 s / 100 events, one
  HTTP request instead of dozens. `Track()` validates, stamps, and serializes
  an immutable snapshot; HTTP is async (`FHttpModule`).
- **Persistent retries** — batches are written to a scoped disk queue before
  sending and retained until acknowledged. Retries preserve event IDs and
  timestamps and honor exponential backoff. See the limits below.
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

**Blueprint — no C++ required.** In any Blueprint graph with a World Context
(a level Blueprint, a widget, an actor), open the node search and type
"QueryRig". Drag in **QueryRig: Track Event**, set the *Event Name* pin (e.g.
`level_complete`) and add entries to the *Properties* map (e.g. `level` → `3`,
`duration_s` → `82.5`). That's the whole integration — one node, no subsystem
to fetch first. Values are sent exactly as typed: the server stores every
property as text, so a build number like `1.10` or an id with leading zeros
survives intact.

The same search also surfaces **QueryRig: Track Fields** (an event with no
name — see below), **QueryRig: Set Player Id** (override the default
device-derived id, e.g. with a platform account id), **QueryRig: Flush**
(force an immediate send), **QueryRig: Refresh Gate** (re-check the remote
on/off switch now), and **QueryRig: Is Enabled** (for a debug HUD).

C++ (`"QueryRig"` in your module's dependencies) — equivalent to the
Blueprint node above, for code-driven projects:

```cpp
#include "QueryRigSubsystem.h"

UQueryRigSubsystem* Rig = GetGameInstance()->GetSubsystem<UQueryRigSubsystem>();
TSharedPtr<FJsonObject> Props = MakeShared<FJsonObject>();
Props->SetNumberField(TEXT("level"), 3);
Props->SetNumberField(TEXT("duration_s"), 82.5);
Rig->Track(TEXT("level_complete"), Props);
```

`session_id`, `player_id`, `platform`, `build_version`, `timestamp`, and
`event_id` are stamped automatically (explicit properties win).
`session_start` / `session_end` events are emitted for you, with a 30-minute
idle rollover.

The default player id is pseudonymous: a hash of the device id scoped to your
project's endpoint, so the raw hardware id never leaves the device and the
same machine has unrelated ids in different games. If you pass your own id,
prefer an opaque account id over a name or email address, and if your game is
played by children, send only pseudonymous ids (see the QueryRig terms).

### Your fields, your names

Nothing is reserved. Apart from `timestamp` and `event_id`, every field you
send is stored under the name you gave it — including `event_name`, which is
just the name the dashboard's query templates look for by default. If your
game already has its own vocabulary, send it and skip the event name entirely
with **QueryRig: Track Fields**, or in C++:

```cpp
TSharedPtr<FJsonObject> Fields = MakeShared<FJsonObject>();
Fields->SetStringField(TEXT("kind"), TEXT("boss_defeated"));
Fields->SetStringField(TEXT("boss"), TEXT("warden"));
Rig->TrackFields(Fields);
```

Then point the templates at `kind` instead of `event_name` (every template
takes the field name). Use `Track()` when you have no reason not to — matching
the convention means charts work with no configuration.

Values are stored as text, so query them quoted (`properties.level = '3'`) and
cast when you need arithmetic (`toInt64OrNull(toString(properties.level))`).
Send natural JSON types from C++ — the conversion happens server-side.

## Configure

Copy `queryrig.example.json` to `Config/queryrig.json` in your project — the
key and endpoint come from the Keys tab of your QueryRig project:

```json
{
  "api_key": "qr_ingest_...",
  "endpoint": "https://3fa91c0e.qrig.net/v1/events",
  "enabled": "remote"
}
```

Every project has its own endpoint, `https://<ingest id>.qrig.net/v1/events`,
and it accepts only that project's key, so always copy the two together. The
endpoint must use `https://` (plain `http://` is allowed only for loopback
addresses, for local development). With a key but no usable endpoint, the SDK
logs one warning at startup and stays off for that run. (Warnings, not
errors: an `Error` log fails any commandlet or automation run that boots a game
instance.)

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
| `true` | Collect locally; authentication failures pause sending, with gate checks allowing recovery. |
| `"remote"` | The server decides: the SDK polls `/v1/config` and obeys the ingest key's enabled state, so telemetry can be flipped after the build has shipped. |

Optional fields: `flush_interval_s` (5), `flush_max_events` (100),
`disk_queue_max_bytes` (8 MB), `sample_rate` (1.0, sampled per session so
funnels stay coherent).

## Delivery limits and upgrading

Event uploads allow 30 seconds for a complete acknowledgement, leaving room
for the server's 15-second ClickHouse timeout and R2 fallback. Config checks
still time out after 5 seconds. A successful R2 acknowledgement completes the
batch; a timeout retains it for backoff/retry with the same IDs and timestamps.
Shutdown cancels outstanding uploads instead of waiting for the deadline.
This does not guarantee a response within 30 seconds on every network.

Call the SDK on Unreal's game thread. Tracking serializes JSON immediately,
so changing nested properties after the call cannot alter queued events.
Cycles, non-finite numbers, excessive depth, and events exceeding 200 KB in
UTF-8 are discarded individually with a warning. Flushes write to disk on the
game thread before starting async HTTP, including a threshold-triggered flush
inside `Track`; profile this cost on target hardware.

Batches are written via temporary files and renamed before HTTP starts.
Shutdown cancels the active request, retains its disk batch, and attempts to
persist pending events. Local disk operations can still block or fail. Events
still in memory can be lost on a crash before a flush. Memory (10,000 events)
and disk (8 MB by default) are capped and evict oldest entries. This is not an
unlimited or exactly-once delivery guarantee. Server retention still applies.

Queue and gate-cache paths are now `Saved/QueryRig/<endpoint-and-key-fingerprint>/`.
Changing the endpoint **or key** isolates the old queue, including rotations
within the same project. Drain before rotating credentials. Old unscoped files
are left intact but are **not automatically replayed**; recover them only after
verifying the destination project. The backend must preserve historical
timestamps for retry-safe deduplication beyond seven days.

How the SDK reacts when it cannot deliver:

| What happens | SDK behavior |
|---|---|
| 401 (key unknown, disabled, or used on another project's endpoint) | Dormant: keeps queued events, re-checks the remote gate on its poll interval. |
| 403 from the edge (plain HTTP, or a path outside the ingest API) | Logs one warning naming the endpoint and stops delivery until the game restarts. Queued events are kept. |
| Endpoint host does not resolve (the project is deleted, or paused while clients kept sending) | Treated as a network error: keeps events on disk and backs off (5 s doubling to 5 min). |

### Upgrading to 0.2.0

- `endpoint` is now required with `api_key` and must use `https://` off
  loopback. A config that relied on the old shared ingest host must switch to
  the project's own endpoint from the Keys tab.
- The default `player_id` is now a hash (see Use), so players tracked by
  0.1.0 with the default id get a new id after the upgrade. Ids you set with
  **Set Player Id** are unchanged.
- Platforms without a login id keep their random fallback id across key
  rotations: it now lives at `Saved/QueryRig/device_id`.

## Tests

The plugin ships an editor-only `QueryRigConformance` module (never packaged
into games) with a commandlet run before each release. It covers delivery and
stamping (including the hashed player id), an outage (zero loss, zero
duplicates after disk-queue replay), an edge 403 treated as permanent without
losing the batch, an unresolvable host backed off as a network error (live and
at cold start), inert startup with a missing or non-HTTPS endpoint, the remote
kill switch in both directions, hard-off silence, immutable nested properties,
UTF-8 batch limits, and persistence of pending and in-flight events during
shutdown. It runs against QueryRig's SDK test server, which is not public.

## Packaging

Build a distributable (prebuilt binaries, usable by Blueprint-only projects):

```
"C:\Program Files\Epic Games\UE_5.8\Engine\Build\BatchFiles\RunUAT.bat" ^
  BuildPlugin -Plugin="<this repo>\QueryRig.uplugin" ^
  -Package="<output dir>" -TargetPlatforms=Win64
```
