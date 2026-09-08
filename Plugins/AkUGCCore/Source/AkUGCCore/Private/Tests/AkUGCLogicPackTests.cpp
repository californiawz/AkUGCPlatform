#include "Misc/AutomationTest.h"

#include "Document/AkUGCDocument.h"
#include "Logic/AkUGCLogicCompiler.h"
#include "Pack/AkUGCLogicPack.h"
#include "Pack/AkUGCLogicPackHasher.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
    /** 构造一个覆盖全部字段类型（GUID/整数/浮点/字符串/枚举/数组/映射/结构体）的作品文档。 */
    FAkUGCProjectDocument MakePackDocument()
    {
        FAkUGCProjectDocument Document;
        Document.Manifest.SchemaVersion = AkUGCSchema::CurrentProjectDocumentVersion;
        Document.Manifest.ProjectId = FGuid(0x11111111, 0x22222222, 0x33333333, 0x44444444);
        Document.Manifest.DisplayName = TEXT("Pack Test");
        Document.Manifest.TemplateId = TEXT("official.tower_defense");
        Document.Manifest.Capabilities.Add(TEXT("logic"));
        Document.Manifest.Capabilities.Add(TEXT("ruleset"));

        FAkUGCSceneDocument& Scene = Document.Scenes.AddDefaulted_GetRef();
        Scene.SceneId = FGuid(0xAAAAAAAA, 0xBBBBBBBB, 0xCCCCCCCC, 0xDDDDDDDD);
        Scene.DisplayName = TEXT("Main");

        FAkUGCEntityRecord& Entity = Scene.Entities.AddDefaulted_GetRef();
        Entity.EntityId = FGuid(1, 0, 0, 0);
        Entity.PrefabId = TEXT("official.gameplay.path_node");
        Entity.Transform = FTransform::Identity;
        Entity.Tags.Add(TEXT("path"));
        Entity.Tags.Add(TEXT("node"));

        FAkUGCComponentRecord& Component = Entity.Components.AddDefaulted_GetRef();
        Component.TypeId = TEXT("tower_defense.path_node");
        Component.SchemaVersion = 1;

        FAkUGCValue OrderValue;
        OrderValue.Type = EAkUGCValueType::Integer;
        OrderValue.IntegerValue = 3;
        Component.Properties.Add(TEXT("order"), OrderValue);

        FAkUGCValue LabelValue;
        LabelValue.Type = EAkUGCValueType::String;
        LabelValue.StringValue = TEXT("spawn");
        Component.Properties.Add(TEXT("label"), LabelValue);

        FAkUGCValue SpeedValue;
        SpeedValue.Type = EAkUGCValueType::Number;
        SpeedValue.NumberValue = 250.5;
        Component.Properties.Add(TEXT("moveSpeed"), SpeedValue);

        FAkUGCValue BoolValue;
        BoolValue.Type = EAkUGCValueType::Bool;
        BoolValue.BoolValue = true;
        Component.Properties.Add(TEXT("blocking"), BoolValue);

        FAkUGCValue NameValue;
        NameValue.Type = EAkUGCValueType::Name;
        NameValue.NameValue = FName(TEXT("enemy_basic"));
        Component.Properties.Add(TEXT("spawnType"), NameValue);

        FAkUGCValue VectorValue;
        VectorValue.Type = EAkUGCValueType::Vector;
        VectorValue.VectorValue = FVector(1.0, 2.0, 3.0);
        Component.Properties.Add(TEXT("offset"), VectorValue);

        FAkUGCValue RotatorValue;
        RotatorValue.Type = EAkUGCValueType::Rotator;
        RotatorValue.RotatorValue = FRotator(10.0, 20.0, 30.0);
        Component.Properties.Add(TEXT("facing"), RotatorValue);

        FAkUGCLogicNode& Start = Scene.LogicGraph.Nodes.AddDefaulted_GetRef();
        Start.NodeId = FGuid(2, 0, 0, 0);
        Start.Type = EAkUGCLogicNodeType::GameStart;

        FAkUGCLogicNode& Message = Scene.LogicGraph.Nodes.AddDefaulted_GetRef();
        Message.NodeId = FGuid(3, 0, 0, 0);
        Message.Type = EAkUGCLogicNodeType::Message;
        Message.Message = TEXT("Wave ready");

        FAkUGCLogicConnection& Connection = Scene.LogicGraph.Connections.AddDefaulted_GetRef();
        Connection.SourceNodeId = Start.NodeId;
        Connection.TargetNodeId = Message.NodeId;

        for (int32 WaveIndex = 0; WaveIndex < 3; ++WaveIndex)
        {
            FAkUGCTowerDefenseWave& Wave = Scene.Ruleset.Waves.AddDefaulted_GetRef();
            Wave.WaveId = FGuid(10 + WaveIndex, 0, 0, 0);
            Wave.SpawnPointEntityId = Entity.EntityId;
            Wave.StartDelaySeconds = WaveIndex * 5.0;
        }
        Scene.Ruleset.WaveIntervalSeconds = 8.0;

        return Document;
    }

    FAkUGCLogicPackManifest MakePackManifest()
    {
        FAkUGCLogicPackManifest Manifest;
        Manifest.ReleaseId = FGuid(0x12345678, 0x9ABCDEF0, 0x13579BDF, 0x2468ACE0);
        Manifest.SchemaVersion = AkUGCSchema::CurrentProjectDocumentVersion;
        Manifest.ProjectId = FGuid(0x11111111, 0x22222222, 0x33333333, 0x44444444);
        Manifest.TemplateId = TEXT("official.tower_defense");
        Manifest.Capabilities.Add(TEXT("logic"));
        Manifest.Capabilities.Add(TEXT("ruleset"));
        Manifest.AssetDependencies.Add(TEXT("/Game/Prefabs/Enemy.Enemy"));
        Manifest.ContentHash = TEXT("0000000000000000000000000000000000000000000000000000000000000000");
        return Manifest;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAkUGCLogicPackSha256Test,
    "AkUGC.Core.Pack.Sha256",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCLogicPackSha256Test::RunTest(const FString& Parameters)
{
    TestEqual(TEXT("SHA-256 of empty string"),
        FAkUGCLogicPackHasher::Sha256Hex(TEXT("")),
        FString(TEXT("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855")));

    TestEqual(TEXT("SHA-256 of 'abc'"),
        FAkUGCLogicPackHasher::Sha256Hex(TEXT("abc")),
        FString(TEXT("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAkUGCLogicPackDeterministicHashTest,
    "AkUGC.Core.Pack.DeterministicDocumentHash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCLogicPackDeterministicHashTest::RunTest(const FString& Parameters)
{
    const FAkUGCProjectDocument Document = MakePackDocument();
    FString Error;

    FString JsonA;
    TestTrue(TEXT("Document serializes deterministically"),
        FAkUGCLogicPackHasher::DeterministicSerialize(Document, JsonA, &Error));
    FString JsonB;
    TestTrue(TEXT("Document serializes deterministically again"),
        FAkUGCLogicPackHasher::DeterministicSerialize(Document, JsonB, &Error));
    TestEqual(TEXT("Same document yields identical JSON"), JsonA, JsonB);
    TestTrue(TEXT("Deterministic JSON is an object"), JsonA.StartsWith(TEXT("{")) && JsonA.EndsWith(TEXT("}")));

    const FAkUGCProjectDocument DocumentAgain = MakePackDocument();
    TestEqual(TEXT("Equivalent documents share the same hash"),
        FAkUGCLogicPackHasher::HashDocument(Document, &Error),
        FAkUGCLogicPackHasher::HashDocument(DocumentAgain, &Error));

    const FString BaseHash = FAkUGCLogicPackHasher::HashDocument(Document, &Error);
    TestEqual(TEXT("Document hash is 64 lowercase hex characters"), BaseHash.Len(), 64);

    FAkUGCProjectDocument Mutated = MakePackDocument();
    Mutated.Manifest.DisplayName = TEXT("Pack Test Mutated");
    TestNotEqual(TEXT("DisplayName change alters hash"),
        FAkUGCLogicPackHasher::HashDocument(Mutated), BaseHash);

    Mutated = MakePackDocument();
    Mutated.Scenes[0].Ruleset.WaveIntervalSeconds = 8.25;
    TestNotEqual(TEXT("Double field change alters hash"),
        FAkUGCLogicPackHasher::HashDocument(Mutated), BaseHash);

    Mutated = MakePackDocument();
    Mutated.Scenes[0].LogicGraph.Nodes[1].Type = EAkUGCLogicNodeType::Timer;
    TestNotEqual(TEXT("Enum field change alters hash"),
        FAkUGCLogicPackHasher::HashDocument(Mutated), BaseHash);

    Mutated = MakePackDocument();
    FAkUGCValue& OrderValue = Mutated.Scenes[0].Entities[0].Components[0].Properties[TEXT("order")];
    OrderValue.IntegerValue = 4;
    TestNotEqual(TEXT("Map value change alters hash"),
        FAkUGCLogicPackHasher::HashDocument(Mutated), BaseHash);

    Mutated = MakePackDocument();
    Mutated.Scenes[0].Entities[0].Transform.SetLocation(FVector(5.0, 6.0, 7.0));
    TestNotEqual(TEXT("Transform change alters hash"),
        FAkUGCLogicPackHasher::HashDocument(Mutated), BaseHash);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAkUGCLogicPackManifestHashTest,
    "AkUGC.Core.Pack.ManifestHash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCLogicPackManifestHashTest::RunTest(const FString& Parameters)
{
    const FAkUGCLogicPackManifest Manifest = MakePackManifest();
    FString Error;

    FString Json;
    TestTrue(TEXT("Manifest serializes"),
        FAkUGCLogicPackHasher::DeterministicSerializeManifest(Manifest, Json, &Error));
    TestTrue(TEXT("Manifest JSON is an object"), Json.StartsWith(TEXT("{")) && Json.EndsWith(TEXT("}")));

    const FString HashA = FAkUGCLogicPackHasher::HashManifest(Manifest, &Error);
    const FString HashB = FAkUGCLogicPackHasher::HashManifest(Manifest, &Error);
    TestEqual(TEXT("Manifest hash is deterministic"), HashA, HashB);
    TestEqual(TEXT("Manifest hash is 64 hex characters"), HashA.Len(), 64);

    FAkUGCLogicPackManifest Mutated = MakePackManifest();
    Mutated.ReleaseId = FGuid::NewGuid();
    TestNotEqual(TEXT("ReleaseId change alters manifest hash"),
        FAkUGCLogicPackHasher::HashManifest(Mutated), HashA);

    Mutated = MakePackManifest();
    Mutated.Capabilities.Add(TEXT("extra"));
    TestNotEqual(TEXT("Capability change alters manifest hash"),
        FAkUGCLogicPackHasher::HashManifest(Mutated), HashA);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAkUGCLogicPackProgramHashTest,
    "AkUGC.Core.Pack.ProgramHash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCLogicPackProgramHashTest::RunTest(const FString& Parameters)
{
    const FAkUGCProjectDocument Document = MakePackDocument();
    const FAkUGCLogicCompileResult Compile = FAkUGCLogicCompiler::Compile(Document.Scenes[0].LogicGraph);
    TestTrue(TEXT("Pack document logic graph compiles"), Compile.bSucceeded);

    FString Error;
    FString Json;
    TestTrue(TEXT("Program serializes deterministically"),
        FAkUGCLogicPackHasher::DeterministicSerializeProgram(Compile.Program, Json, &Error));

    const FString HashA = FAkUGCLogicPackHasher::HashLogicProgram(Compile.Program, &Error);
    const FString HashB = FAkUGCLogicPackHasher::HashLogicProgram(Compile.Program, &Error);
    TestEqual(TEXT("Program hash is deterministic"), HashA, HashB);
    TestNotEqual(TEXT("Program hash differs from document hash"),
        HashA, FAkUGCLogicPackHasher::HashDocument(Document, &Error));

    if (Compile.Program.Instructions.Num() > 0)
    {
        FAkUGCLogicProgram Mutated = Compile.Program;
        Mutated.Instructions[0].Operand = TEXT("Changed");
        TestNotEqual(TEXT("Instruction change alters program hash"),
            FAkUGCLogicPackHasher::HashLogicProgram(Mutated), HashA);
    }

    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
