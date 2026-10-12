#pragma once

#include "Widgets/SCompoundWidget.h"
#include "Copilot/AkUGCCopilotTransport.h"
#include "Template/AkUGCCopilotPreview.h"

class UAkUGCEditorSubsystem;
struct FAkUGCCopilotTestAccess;

class SAkUGCCopilotPanel final : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SAkUGCCopilotPanel) {}
    SLATE_END_ARGS()
    void Construct(const FArguments& InArgs, UAkUGCEditorSubsystem* InSubsystem);
    virtual ~SAkUGCCopilotPanel() override;
    virtual void Tick(const FGeometry& Geometry, double CurrentTime, float DeltaTime) override;
private:
#if WITH_DEV_AUTOMATION_TESTS
    friend struct FAkUGCCopilotTestAccess;
#endif
    float RequestTimeoutSeconds = 60.0f;
    int32 LastNativeCode = 0;
    int32 LastHttpCode = 0;
#if WITH_DEV_AUTOMATION_TESTS
    FString TestCaBundle;
#endif
    FReply Generate();
    FReply Explain();
    FReply Apply();
    FReply Cancel();
    bool CanExplain() const;
    bool bExplaining = false;
    FString LastValidationError;
    FString ModelExplanation;
    void Complete(FAkUGCCopilotRequestPtr Request, FAkUGCCopilotTransportResult Result);
    TWeakObjectPtr<UAkUGCEditorSubsystem> Subsystem;
    FAkUGCCopilotRequestPtr Pending;
    FAkUGCProjectDocument RequestDocument;
    FAkUGCCopilotPreview Preview;
    uint64 RequestedRevision = 0;
    FString Endpoint;
    FString Model;
    FString ApiKey;
    FString Prompt;
    FString Status;
};
