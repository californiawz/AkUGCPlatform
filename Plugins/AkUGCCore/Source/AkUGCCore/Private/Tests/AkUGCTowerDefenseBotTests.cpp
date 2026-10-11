#include "Misc/AutomationTest.h"
#include "Command/AkUGCCommandExecutor.h"
#include "Pack/AkUGCLogicPackHasher.h"
#include "Prefab/AkUGCOfficialPrefabCatalog.h"
#include "Prefab/AkUGCPrefabRegistry.h"
#include "Template/AkUGCTowerDefenseBot.h"
#include "Template/AkUGCTowerDefenseTemplate.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAkUGCBotCrossTargetTest, "AkUGC.Core.Bot.DeterministicPreparation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
        | EAutomationTestFlags::ServerContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCBotCrossTargetTest::RunTest(const FString& Parameters)
{
    FAkUGCPrefabRegistry Registry;
    FAkUGCProjectDocument Source;
    FString Error;
    if (!FAkUGCOfficialPrefabCatalog::RegisterTowerDefense(Registry, &Error)
        || !FAkUGCTowerDefenseTemplate::CreateDefaultDocument(Registry, Source, &Error)) { AddError(Error); return false; }
    Source.Manifest.ProjectId = FGuid(0x14100001, 0, 0, 0);
    FAkUGCSceneDocument& Scene = Source.Scenes[0];
    Scene.SceneId = FGuid(0x14100002, 0, 0, 0);
    Scene.Entities.RemoveAll([](const FAkUGCEntityRecord& Entity) { return Entity.PrefabId == TEXT("official.tower.basic"); });
    TMap<FGuid, FGuid> Ids;
    for (int32 Index = 0; Index < Scene.Entities.Num(); ++Index)
    {
        Ids.Add(Scene.Entities[Index].EntityId, FGuid(0x14100100 + Index, 0, 0, 0));
        Scene.Entities[Index].EntityId = Ids.FindChecked(Scene.Entities[Index].EntityId);
    }
    for (int32 Index = 0; Index < Scene.Ruleset.Waves.Num(); ++Index)
    {
        Scene.Ruleset.Waves[Index].WaveId = FGuid(0x14100200 + Index, 0, 0, 0);
        Scene.Ruleset.Waves[Index].SpawnPointEntityId = Ids.FindChecked(Scene.Ruleset.Waves[Index].SpawnPointEntityId);
    }
    for (int32 Index = 0; Index < 2; ++Index)
    {
        FAkUGCEntityRecord Slot;
        Registry.CreateEntityRecord(TEXT("official.gameplay.tower_slot"), FGuid(0x14100300 + Index, 0, 0, 0),
            FTransform(FVector(650.0 + Index * 450.0, 150.0, 0.0)), Slot, &Error);
        Scene.Entities.Add(Slot);
    }
    const FString Before = FAkUGCLogicPackHasher::HashDocument(Source);
    FAkUGCCommandTransaction Transaction, Inverse;
    int32 Count = 0;
    TestTrue(TEXT("Bot preparation plans"), FAkUGCTowerDefenseBot::BuildPreparationTransaction(Source, Registry, Transaction, Count, &Error));
    TestEqual(TEXT("Bot spends two affordable towers"), Count, 2);
    FAkUGCProjectDocument Candidate = Source;
    TestTrue(TEXT("Bot transaction applies"), FAkUGCCommandExecutor::Apply(Candidate, Transaction, &Inverse).bSucceeded);
    TestEqual(TEXT("Bot resources debit"), Candidate.Scenes[0].Ruleset.InitialResources, 0);
    UE_LOG(LogTemp, Display, TEXT("[AkUGCBot] CandidateHash=%s Towers=2 Resources=0"), *FAkUGCLogicPackHasher::HashDocument(Candidate));
    TestEqual(TEXT("Source remains unchanged"), FAkUGCLogicPackHasher::HashDocument(Source), Before);
    TestTrue(TEXT("Bot transaction undo succeeds"), FAkUGCCommandExecutor::Apply(Candidate, Inverse).bSucceeded);
    TestEqual(TEXT("Bot undo restores exact document"), FAkUGCLogicPackHasher::HashDocument(Candidate), Before);
    return true;
}
#endif
