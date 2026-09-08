#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"

#include "Document/AkUGCDocument.h"
#include "Logic/AkUGCLogicRunner.h"
#include "Pack/AkUGCLogicPack.h"
#include "Pack/AkUGCLogicPackBuilder.h"
#include "Pack/AkUGCLogicPackCodec.h"
#include "Pack/AkUGCLogicPackHasher.h"
#include "Pack/AkUGCLogicPackLoader.h"
#include "Pack/AkUGCLogicPackSignature.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
    /** RAII 临时文件：析构时删除，中间提前返回也能清理。 */
    struct FScopedTempFile
    {
        FString Path;

        explicit FScopedTempFile(const TCHAR* Extension)
        {
            Path = FPaths::CreateTempFilename(
                *FPaths::ProjectSavedDir(),
                TEXT("AkUGCLogicPack"),
                Extension);
        }

        ~FScopedTempFile()
        {
            if (!Path.IsEmpty())
            {
                IFileManager::Get().Delete(*Path, /*bRequireExists=*/false, /*bEvenReadOnly=*/true);
            }
        }
    };

    /** 构造含 enemy_spawn 与三波 Ruleset 的基础文档（Logic 由调用方补充）。 */
    FAkUGCProjectDocument MakeBaseDocument()
    {
        FAkUGCProjectDocument Document;
        Document.Manifest.SchemaVersion = AkUGCSchema::CurrentProjectDocumentVersion;
        Document.Manifest.ProjectId = FGuid(0x11111111, 0x22222222, 0x33333333, 0x44444444);
        Document.Manifest.DisplayName = TEXT("Headless Pack");
        Document.Manifest.TemplateId = TEXT("official.tower_defense");
        Document.Manifest.Capabilities.Add(TEXT("logic"));
        Document.Manifest.Capabilities.Add(TEXT("ruleset"));

        FAkUGCSceneDocument& Scene = Document.Scenes.AddDefaulted_GetRef();
        Scene.SceneId = FGuid(0xAAAAAAAA, 0xBBBBBBBB, 0xCCCCCCCC, 0xDDDDDDDD);
        Scene.DisplayName = TEXT("Main");
        Scene.Ruleset.WaveIntervalSeconds = 5.0;

        FAkUGCEntityRecord& Spawn = Scene.Entities.AddDefaulted_GetRef();
        Spawn.EntityId = FGuid(0x00000001, 0, 0, 0);
        Spawn.PrefabId = TEXT("official.gameplay.enemy_spawn");
        Spawn.Transform = FTransform::Identity;

        FAkUGCComponentRecord& SpawnComponent = Spawn.Components.AddDefaulted_GetRef();
        SpawnComponent.TypeId = TEXT("tower_defense.enemy_spawn");
        SpawnComponent.SchemaVersion = 1;
        FAkUGCValue EnemyPrefab;
        EnemyPrefab.Type = EAkUGCValueType::Name;
        EnemyPrefab.NameValue = FName(TEXT("official.gameplay.basic_enemy"));
        SpawnComponent.Properties.Add(TEXT("enemyPrefab"), EnemyPrefab);

        for (int32 WaveIndex = 0; WaveIndex < 3; ++WaveIndex)
        {
            FAkUGCTowerDefenseWave& Wave = Scene.Ruleset.Waves.AddDefaulted_GetRef();
            Wave.WaveId = FGuid(10 + WaveIndex, 0, 0, 0);
            Wave.SpawnPointEntityId = Spawn.EntityId;
            Wave.StartDelaySeconds = WaveIndex * 5.0;
        }

        return Document;
    }

    /** 三波事件文档：GameStart → Message，WaveStart → Message。 */
    FAkUGCProjectDocument MakeThreeWaveDocument()
    {
        FAkUGCProjectDocument Document = MakeBaseDocument();
        FAkUGCLogicGraph& Graph = Document.Scenes[0].LogicGraph;

        FAkUGCLogicNode& GameStart = Graph.Nodes.AddDefaulted_GetRef();
        GameStart.NodeId = FGuid(2, 0, 0, 0);
        GameStart.Type = EAkUGCLogicNodeType::GameStart;

        FAkUGCLogicNode& GameStartMessage = Graph.Nodes.AddDefaulted_GetRef();
        GameStartMessage.NodeId = FGuid(3, 0, 0, 0);
        GameStartMessage.Type = EAkUGCLogicNodeType::Message;
        GameStartMessage.Message = TEXT("Match ready");

        FAkUGCLogicNode& WaveStart = Graph.Nodes.AddDefaulted_GetRef();
        WaveStart.NodeId = FGuid(4, 0, 0, 0);
        WaveStart.Type = EAkUGCLogicNodeType::WaveStart;

        FAkUGCLogicNode& WaveMessage = Graph.Nodes.AddDefaulted_GetRef();
        WaveMessage.NodeId = FGuid(5, 0, 0, 0);
        WaveMessage.Type = EAkUGCLogicNodeType::Message;
        WaveMessage.Message = TEXT("Wave ready");

        FAkUGCLogicConnection& GameStartConnection = Graph.Connections.AddDefaulted_GetRef();
        GameStartConnection.SourceNodeId = GameStart.NodeId;
        GameStartConnection.TargetNodeId = GameStartMessage.NodeId;

        FAkUGCLogicConnection& WaveConnection = Graph.Connections.AddDefaulted_GetRef();
        WaveConnection.SourceNodeId = WaveStart.NodeId;
        WaveConnection.TargetNodeId = WaveMessage.NodeId;

        return Document;
    }

    /** Spawn 文档：GameStart → Spawn（发敌人），无 WaveStart 节点。 */
    FAkUGCProjectDocument MakeSpawnDocument()
    {
        FAkUGCProjectDocument Document = MakeBaseDocument();
        FAkUGCLogicGraph& Graph = Document.Scenes[0].LogicGraph;

        FAkUGCLogicNode& GameStart = Graph.Nodes.AddDefaulted_GetRef();
        GameStart.NodeId = FGuid(2, 0, 0, 0);
        GameStart.Type = EAkUGCLogicNodeType::GameStart;

        FAkUGCLogicNode& SpawnNode = Graph.Nodes.AddDefaulted_GetRef();
        SpawnNode.NodeId = FGuid(3, 0, 0, 0);
        SpawnNode.Type = EAkUGCLogicNodeType::Spawn;
        SpawnNode.SpawnPrefabId = FName(TEXT("official.gameplay.basic_enemy"));
        SpawnNode.SpawnAtEntityId = Document.Scenes[0].Entities[0].EntityId;

        FAkUGCLogicConnection& Connection = Graph.Connections.AddDefaulted_GetRef();
        Connection.SourceNodeId = GameStart.NodeId;
        Connection.TargetNodeId = SpawnNode.NodeId;

        return Document;
    }

    /** 对多次 Logic 运行结果做确定性序列化并求 SHA-256，作为最终状态哈希。 */
    FString ComputeRunStateHash(const TArray<FAkUGCLogicRunResult>& RunResults)
    {
        FString State;
        for (const FAkUGCLogicRunResult& Run : RunResults)
        {
            State += FString::Printf(TEXT("c:%d;"), Run.ExecutedInstructionCount);
            for (const FAkUGCLogicMessageEvent& Message : Run.Messages)
            {
                State += FString::Printf(
                    TEXT("m:%s|%s;"),
                    *Message.SourceNodeId.ToString(EGuidFormats::Digits),
                    *Message.Message);
            }
            for (const FAkUGCLogicSpawnEffect& Spawn : Run.SpawnEffects)
            {
                State += FString::Printf(
                    TEXT("s:%s|%s|%s;"),
                    *Spawn.SourceNodeId.ToString(EGuidFormats::Digits),
                    *Spawn.PrefabId.ToString(),
                    *Spawn.SpawnAtEntityId.ToString(EGuidFormats::Digits));
            }
            for (const FAkUGCLogicDelayRequest& Delay : Run.Delays)
            {
                State += FString::Printf(
                    TEXT("d:%s|%.3f;"),
                    *Delay.SourceNodeId.ToString(EGuidFormats::Digits),
                    Delay.DelaySeconds);
            }
        }
        return FAkUGCLogicPackHasher::Sha256Hex(State);
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAkUGCLogicPackFileLoadTest,
    "AkUGC.Core.Pack.FileLoad",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCLogicPackFileLoadTest::RunTest(const FString& Parameters)
{
    // 1. 不存在的文件被拒绝。
    const FAkUGCLogicPackLoadResult Missing = FAkUGCLogicPackLoader::LoadFromFile(
        TEXT("E:/__definitely_missing__.json"));
    TestFalse(TEXT("Missing file is rejected"), Missing.bSucceeded);
    TestFalse(TEXT("Missing file rejection carries an error"), Missing.ErrorMessage.IsEmpty());

    // 2. 合法文件能加载。
    const FAkUGCLogicPackBuildResult Build = FAkUGCLogicPackBuilder::Build(MakeThreeWaveDocument());
    TestTrue(*Build.ErrorMessage, Build.bSucceeded);
    if (!Build.bSucceeded)
    {
        return false;
    }

    FString Error;
    FString Json;
    TestTrue(*Error, FAkUGCLogicPackCodec::Serialize(Build.Pack, Json, &Error));

    FScopedTempFile TempFile(TEXT(".json"));
    TestTrue(TEXT("Pack JSON is written to disk"), FFileHelper::SaveStringToFile(Json, *TempFile.Path));

    const FAkUGCLogicPackLoadResult Loaded = FAkUGCLogicPackLoader::LoadFromFile(TempFile.Path);
    TestTrue(*Loaded.ErrorMessage, Loaded.bSucceeded);
    TestEqual(TEXT("File-loaded pack keeps content hash"),
        Loaded.Pack.Manifest.ContentHash, Build.Pack.Manifest.ContentHash);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAkUGCLogicPackVerifiedFileLoadTest,
    "AkUGC.Core.Pack.Signature.VerifiedFileLoad",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCLogicPackVerifiedFileLoadTest::RunTest(const FString& Parameters)
{
    FAkUGCLogicPackKeyPair KeyPair;
    FString Error;
    TestTrue(*Error, FAkUGCLogicPackSigner::GenerateKeyPair(KeyPair, &Error));

    const FAkUGCLogicPackBuildResult Build = FAkUGCLogicPackBuilder::Build(MakeThreeWaveDocument());
    TestTrue(*Build.ErrorMessage, Build.bSucceeded);
    if (!Build.bSucceeded)
    {
        return false;
    }

    FAkUGCLogicPack SignedPack = Build.Pack;
    TestTrue(*Error, FAkUGCLogicPackSigner::Sign(SignedPack.Manifest, KeyPair.PrivateKey, SignedPack.Signature, &Error));

    FString Json;
    TestTrue(*Error, FAkUGCLogicPackCodec::Serialize(SignedPack, Json, &Error));

    FScopedTempFile TempFile(TEXT(".json"));
    TestTrue(TEXT("Signed pack JSON is written to disk"), FFileHelper::SaveStringToFile(Json, *TempFile.Path));

    // 1. 合法签名文件通过验签加载。
    const FAkUGCLogicPackLoadResult Verified = FAkUGCLogicPackLoader::LoadVerifiedFromFile(
        TempFile.Path, KeyPair.PublicKey);
    TestTrue(*Verified.ErrorMessage, Verified.bSucceeded);

    // 2. 未签名文件被验签加载拒绝。
    FString UnsignedJson;
    TestTrue(*Error, FAkUGCLogicPackCodec::Serialize(Build.Pack, UnsignedJson, &Error));
    FScopedTempFile UnsignedFile(TEXT(".json"));
    TestTrue(TEXT("Unsigned pack JSON is written to disk"),
        FFileHelper::SaveStringToFile(UnsignedJson, *UnsignedFile.Path));
    const FAkUGCLogicPackLoadResult UnsignedLoad = FAkUGCLogicPackLoader::LoadVerifiedFromFile(
        UnsignedFile.Path, KeyPair.PublicKey);
    TestFalse(TEXT("Unsigned pack is rejected by verified file load"), UnsignedLoad.bSucceeded);

    // 3. 错误公钥拒绝。
    FAkUGCLogicPackKeyPair Other;
    TestTrue(*Error, FAkUGCLogicPackSigner::GenerateKeyPair(Other, &Error));
    const FAkUGCLogicPackLoadResult WrongKey = FAkUGCLogicPackLoader::LoadVerifiedFromFile(
        TempFile.Path, Other.PublicKey);
    TestFalse(TEXT("Wrong trusted public key is rejected"), WrongKey.bSucceeded);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAkUGCLogicPackThreeWaveHeadlessTest,
    "AkUGC.Core.Pack.ThreeWaveHeadless",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCLogicPackThreeWaveHeadlessTest::RunTest(const FString& Parameters)
{
    // 1. 同一 Document 两次独立构建 → ContentHash 一致（内容确定性）。
    const FAkUGCLogicPackBuildResult BuildA = FAkUGCLogicPackBuilder::Build(MakeThreeWaveDocument());
    const FAkUGCLogicPackBuildResult BuildB = FAkUGCLogicPackBuilder::Build(MakeThreeWaveDocument());
    TestTrue(*BuildA.ErrorMessage, BuildA.bSucceeded);
    TestTrue(*BuildB.ErrorMessage, BuildB.bSucceeded);
    if (!BuildA.bSucceeded || !BuildB.bSucceeded)
    {
        return false;
    }
    TestEqual(TEXT("Same document yields identical ContentHash"),
        BuildA.Pack.Manifest.ContentHash, BuildB.Pack.Manifest.ContentHash);
    TestNotEqual(TEXT("Independent builds still get distinct ReleaseId"),
        BuildA.Pack.Manifest.ReleaseId, BuildB.Pack.Manifest.ReleaseId);

    // 2. 同一 Release 经序列化/反序列化（文件往返）后，运行结果哈希一致。
    FString Error;
    FString Json;
    TestTrue(*Error, FAkUGCLogicPackCodec::Serialize(BuildA.Pack, Json, &Error));

    FScopedTempFile TempFile(TEXT(".json"));
    TestTrue(TEXT("Three-wave pack JSON is written to disk"), FFileHelper::SaveStringToFile(Json, *TempFile.Path));

    const FAkUGCLogicPackLoadResult Loaded = FAkUGCLogicPackLoader::LoadFromFile(TempFile.Path);
    TestTrue(*Loaded.ErrorMessage, Loaded.bSucceeded);
    if (!Loaded.bSucceeded)
    {
        return false;
    }

    const auto RunThreeWaves = [](const FAkUGCLogicProgram& Program)
    {
        TArray<FAkUGCLogicRunResult> Results;
        Results.Add(FAkUGCLogicRunner::RunGameStart(Program));
        Results.Add(FAkUGCLogicRunner::RunWaveStart(Program, 0));
        Results.Add(FAkUGCLogicRunner::RunWaveStart(Program, 1));
        Results.Add(FAkUGCLogicRunner::RunWaveStart(Program, 2));
        return Results;
    };

    const TArray<FAkUGCLogicRunResult> RunsA = RunThreeWaves(BuildA.Pack.Program);
    const TArray<FAkUGCLogicRunResult> RunsLoaded = RunThreeWaves(Loaded.Pack.Program);

    TestEqual(TEXT("GameStart plus three WaveStart runs all succeed"),
        RunsA.Num(), 4);
    for (int32 Index = 0; Index < RunsA.Num(); ++Index)
    {
        TestTrue(FString::Printf(TEXT("Run %d succeeds"), Index), RunsA[Index].bSucceeded);
    }

    // 3. Logic 事件顺序：GameStart 广播 1 条，三波各广播 1 条。
    TestEqual(TEXT("GameStart emits one message"), RunsA[0].Messages.Num(), 1);
    if (RunsA[0].Messages.Num() == 1)
    {
        TestEqual(TEXT("GameStart message text"), RunsA[0].Messages[0].Message, FString(TEXT("Match ready")));
    }
    for (int32 WaveIndex = 1; WaveIndex <= 3; ++WaveIndex)
    {
        TestEqual(FString::Printf(TEXT("Wave %d emits one message"), WaveIndex - 1),
            RunsA[WaveIndex].Messages.Num(), 1);
        if (RunsA[WaveIndex].Messages.Num() == 1)
        {
            TestEqual(FString::Printf(TEXT("Wave %d message text"), WaveIndex - 1),
                RunsA[WaveIndex].Messages[0].Message, FString(TEXT("Wave ready")));
        }
    }

    // 4. 最终状态哈希：文件往返后的 Release 与原始 Release 一致。
    const FString HashA = ComputeRunStateHash(RunsA);
    const FString HashLoaded = ComputeRunStateHash(RunsLoaded);
    TestEqual(TEXT("File round-trip preserves final state hash"), HashA, HashLoaded);
    TestEqual(TEXT("Final state hash is 64 lowercase hex characters"), HashA.Len(), 64);

    // 5. 内容相同的另一 Release 运行结果哈希也一致（一致性报告核心）。
    const TArray<FAkUGCLogicRunResult> RunsB = RunThreeWaves(BuildB.Pack.Program);
    TestEqual(TEXT("Different ReleaseId with same content yields identical state hash"),
        HashA, ComputeRunStateHash(RunsB));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAkUGCLogicPackSpawnDeterminismTest,
    "AkUGC.Core.Pack.SpawnDeterminism",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCLogicPackSpawnDeterminismTest::RunTest(const FString& Parameters)
{
    const FAkUGCLogicPackBuildResult Build = FAkUGCLogicPackBuilder::Build(MakeSpawnDocument());
    TestTrue(*Build.ErrorMessage, Build.bSucceeded);
    if (!Build.bSucceeded)
    {
        return false;
    }

    const auto RunSpawn = [](const FAkUGCLogicProgram& Program)
    {
        return FAkUGCLogicRunner::RunGameStart(Program);
    };

    const FAkUGCLogicRunResult RunA = RunSpawn(Build.Pack.Program);
    const FAkUGCLogicRunResult RunB = RunSpawn(Build.Pack.Program);

    TestTrue(TEXT("Spawn graph runs Game Start"), RunA.bSucceeded);
    TestEqual(TEXT("Spawn graph emits one spawn effect"), RunA.SpawnEffects.Num(), 1);
    if (RunA.SpawnEffects.Num() == 1)
    {
        TestEqual(TEXT("Spawn PrefabId is deterministic"),
            RunA.SpawnEffects[0].PrefabId, FName(TEXT("official.gameplay.basic_enemy")));
        TestEqual(TEXT("Spawn anchor entity is deterministic"),
            RunA.SpawnEffects[0].SpawnAtEntityId, Build.Pack.Document.Scenes[0].Entities[0].EntityId);
    }

    // 同一 Release 两次运行，Spawn 顺序与指令计数一致。
    TArray<FAkUGCLogicRunResult> RunsA;
    RunsA.Add(RunA);
    TArray<FAkUGCLogicRunResult> RunsB;
    RunsB.Add(RunB);
    TestEqual(TEXT("Spawn final state hash is deterministic"),
        ComputeRunStateHash(RunsA), ComputeRunStateHash(RunsB));
    TestEqual(TEXT("Spawn instruction count is deterministic"),
        RunA.ExecutedInstructionCount, RunB.ExecutedInstructionCount);

    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
