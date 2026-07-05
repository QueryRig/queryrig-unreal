#pragma once

#include "Commandlets/Commandlet.h"
#include "QueryRigConformanceCommandlet.generated.h"

/**
 * Headless conformance test (spec §7) — drives the SDK against the QueryRig
 * SDK harness (queryrig repo: `npm run sdk-harness`, port 8788).
 *
 * Run from any host project with the plugin enabled:
 *   UnrealEditor-Cmd.exe <Host.uproject> -run=QueryRigConformance -unattended -nullrhi
 * Env: QUERYRIG_TEST_HARNESS to point at a non-default harness.
 *
 * Exit code 0 on pass, 1 on assertion failures, 2 if the harness is down.
 */
UCLASS()
class UQueryRigConformanceCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	virtual int32 Main(const FString& Params) override;
};
