#include "Misc/AutomationTest.h"

#include "Command/AkUGCCommandExecutor.h"
#include "Document/AkUGCDocumentJson.h"
#include "Dom/JsonObject.h"
#include "Pack/AkUGCLogicPackBuilder.h"
#include "Pack/AkUGCLogicPackHasher.h"
#include "Prefab/AkUGCOfficialPrefabCatalog.h"
#include "Prefab/AkUGCPrefabRegistry.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Template/AkUGCTowerDefenseTemplate.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAkUGCTemplateParameterJsonTest,
    "AkUGC.Core.Template.ParameterJson", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCTemplateParameterJsonTest::RunTest(const FString& Parameters)
{
    FAkUGCTowerDefenseParameters Source;
    Source.Waves[1].EnemyMaxHealth = 275.0;
    Source.Waves[2].EnemyMoveSpeed = 900.0;
    Source.InitialResources = 450;
    FString Json, Error;
    TestTrue(TEXT("Parameters serialize"), FAkUGCTowerDefenseTemplate::SerializeParameters(Source, Json, &Error));
    FAkUGCTowerDefenseParameters Restored;
    TestTrue(TEXT("Parameters deserialize"), FAkUGCTowerDefenseTemplate::DeserializeParameters(Json, Restored, &Error));
    TestEqual(TEXT("Resources round trip"), Restored.InitialResources, 450);
    TestEqual(TEXT("Wave health round trips"), Restored.Waves[1].EnemyMaxHealth, 275.0);
    TestEqual(TEXT("Wave speed round trips"), Restored.Waves[2].EnemyMoveSpeed, 900.0);

    TSharedPtr<FJsonObject> Root;
    if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root)) { return false; }
    const auto Export = [&]()
    {
        FString Result;
        FJsonSerializer::Serialize(Root.ToSharedRef(), TJsonWriterFactory<>::Create(&Result));
        return Result;
    };
    Root->SetNumberField(TEXT("schemaVersion"), 0);
    Root->RemoveField(TEXT("initialResources"));
    TestTrue(TEXT("Legacy parameter schema migrates"), FAkUGCTowerDefenseTemplate::DeserializeParameters(Export(), Restored, &Error));
    TestEqual(TEXT("Legacy schema becomes version one"), Restored.SchemaVersion, 1);
    TestEqual(TEXT("Legacy resources remain zero"), Restored.InitialResources, 0);
    Root->SetNumberField(TEXT("schemaVersion"), 2);
    TestFalse(TEXT("Future schema rejected"), FAkUGCTowerDefenseTemplate::DeserializeParameters(Export(), Restored, &Error));
    Root->SetNumberField(TEXT("schemaVersion"), 1);
    TestFalse(TEXT("Current resources required"), FAkUGCTowerDefenseTemplate::DeserializeParameters(Export(), Restored, &Error));
    Root->SetStringField(TEXT("initialResources"), TEXT("200"));
    TestFalse(TEXT("String integer rejected"), FAkUGCTowerDefenseTemplate::DeserializeParameters(Export(), Restored, &Error));
    Root->SetNumberField(TEXT("initialResources"), 200.5);
    TestFalse(TEXT("Fractional integer rejected"), FAkUGCTowerDefenseTemplate::DeserializeParameters(Export(), Restored, &Error));
    Root->SetNumberField(TEXT("initialResources"), 200);
    const FString Valid = Export();
    TestFalse(TEXT("Alias rejected"), FAkUGCTowerDefenseTemplate::DeserializeParameters(
        Valid.Replace(TEXT("\"initialResources\""), TEXT("\"InitialResources\""), ESearchCase::CaseSensitive), Restored, &Error));
    TestFalse(TEXT("Duplicate schema key rejected"), FAkUGCTowerDefenseTemplate::DeserializeParameters(
        TEXT("{\"schemaVersion\":1,") + Valid.RightChop(1), Restored, &Error));
    TestFalse(TEXT("Condition value casing rejected"), FAkUGCTowerDefenseTemplate::DeserializeParameters(
        Valid.Replace(TEXT("BaseHealthDepleted"), TEXT("basehealthdepleted"), ESearchCase::CaseSensitive), Restored, &Error));
    TestFalse(TEXT("Enemy value casing rejected"), FAkUGCTowerDefenseTemplate::DeserializeParameters(
        Valid.Replace(TEXT("official.unit.basic_enemy"), TEXT("OFFICIAL.UNIT.BASIC_ENEMY"), ESearchCase::CaseSensitive), Restored, &Error));
    TestEqual(TEXT("Failure preserves last accepted parameter value"), Restored.InitialResources, 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAkUGCTemplateTransactionTest,
    "AkUGC.Core.Template.TransactionAndBudgets", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCTemplateTransactionTest::RunTest(const FString& Parameters)
{
    FAkUGCPrefabRegistry Registry;
    FAkUGCProjectDocument Document;
    FString Error;
    if (!FAkUGCOfficialPrefabCatalog::RegisterTowerDefense(Registry, &Error)
        || !FAkUGCTowerDefenseTemplate::CreateDefaultDocument(Registry, Document, &Error))
    {
        AddError(Error);
        return false;
    }
    const FGuid SceneId = Document.Scenes[0].SceneId;
    TestTrue(TEXT("Default graph is controlled template graph"),
        FAkUGCTowerDefenseTemplate::IsTemplateOwnedLogicGraph(Document.Scenes[0]));
    const FAkUGCLogicPackBuildResult Pack = FAkUGCLogicPackBuilder::Build(Document);
    TestTrue(TEXT("Default template builds a pack"), Pack.bSucceeded);
    TestTrue(TEXT("Spawned enemy dependency included"),
        Pack.Pack.Manifest.AssetDependencies.Contains(TEXT("official.unit.basic_enemy")));
    const FString Before = FAkUGCLogicPackHasher::HashDocument(Document);
    FAkUGCTowerDefenseParameters Draft;
    TestTrue(TEXT("Read authored parameters"), FAkUGCTowerDefenseTemplate::ReadParameters(Document, SceneId, Draft, &Error));
    const FGuid FirstWave = Document.Scenes[0].Ruleset.Waves[0].WaveId;
    const FGuid FirstSpawn = Document.Scenes[0].Ruleset.Waves[0].SpawnPointEntityId;
    Draft.Waves.SetNum(1);
    Draft.Waves[0].EnemyCount = 3;
    Draft.BaseMaxHealth = 500.0;
    FAkUGCCommandTransaction Transaction, Inverse;
    TestTrue(TEXT("Build one transaction"), FAkUGCTowerDefenseTemplate::BuildTransaction(Document, SceneId, Draft, Registry, Transaction, &Error));
    TestTrue(TEXT("Apply transaction"), FAkUGCCommandExecutor::Apply(Document, Transaction, &Inverse).bSucceeded);
    TestEqual(TEXT("Existing wave ID remains stable"), Document.Scenes[0].Ruleset.Waves[0].WaveId, FirstWave);
    TestEqual(TEXT("Existing spawn ID remains stable"), Document.Scenes[0].Ruleset.Waves[0].SpawnPointEntityId, FirstSpawn);
    TestTrue(TEXT("Undo complete transaction"), FAkUGCCommandExecutor::Apply(Document, Inverse).bSucceeded);
    TestEqual(TEXT("Undo restores exact document hash"), FAkUGCLogicPackHasher::HashDocument(Document), Before);

    Draft.Waves.SetNum(3);
    for (auto& Wave : Draft.Waves) { Wave.EnemyCount = 100; }
    TestFalse(TEXT("Dynamic physics/script budget rejects count below hard limit"),
        FAkUGCTowerDefenseTemplate::BuildTransaction(Document, SceneId, Draft, Registry, Transaction, &Error));
    TestEqual(TEXT("Budget rejection does not mutate source"), FAkUGCLogicPackHasher::HashDocument(Document), Before);
    TestTrue(TEXT("Failed output transaction is empty"), Transaction.Commands.IsEmpty());

    FAkUGCSceneDocument& Scene = Document.Scenes[0];
    Scene.Ruleset.Waves[1].SpawnPointEntityId = FirstSpawn;
    Scene.Ruleset.Waves[2].SpawnPointEntityId = FirstSpawn;
    Scene.Entities.FindByPredicate([&](const FAkUGCEntityRecord& Entity) { return Entity.EntityId == FirstSpawn; })
        ->Transform.SetLocation(FVector(321.0, 123.0, 0.0));
    TestTrue(TEXT("Read legacy shared spawn"), FAkUGCTowerDefenseTemplate::ReadParameters(Document, SceneId, Draft, &Error));
    const FString SharedBefore = FAkUGCLogicPackHasher::HashDocument(Document);
    TestTrue(TEXT("Split shared spawn transaction"), FAkUGCTowerDefenseTemplate::BuildTransaction(Document, SceneId, Draft, Registry, Transaction, &Error));
    TestTrue(TEXT("Apply split"), FAkUGCCommandExecutor::Apply(Document, Transaction, &Inverse).bSucceeded);
    TSet<FGuid> SpawnIds;
    for (const auto& Wave : Document.Scenes[0].Ruleset.Waves) { SpawnIds.Add(Wave.SpawnPointEntityId); }
    TestEqual(TEXT("Every wave has independent spawn"), SpawnIds.Num(), 3);
    for (const FGuid& Id : SpawnIds)
    {
        const FAkUGCEntityRecord* Spawn = Document.Scenes[0].Entities.FindByPredicate(
            [&](const FAkUGCEntityRecord& Entity) { return Entity.EntityId == Id; });
        TestEqual(TEXT("Split preserves authored shared position"), Spawn->Transform.GetLocation(), FVector(321.0, 123.0, 0.0));
    }
    TestTrue(TEXT("Undo shared spawn split"), FAkUGCCommandExecutor::Apply(Document, Inverse).bSucceeded);
    TestEqual(TEXT("Split undo preserves source exact hash"), FAkUGCLogicPackHasher::HashDocument(Document), SharedBefore);
    return true;
}

#endif
