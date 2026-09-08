#include "Misc/AutomationTest.h"

#include "Document/AkUGCDocument.h"
#include "Logic/AkUGCLogicCompiler.h"
#include "Pack/AkUGCLogicPack.h"
#include "Pack/AkUGCLogicPackBuilder.h"
#include "Pack/AkUGCLogicPackCodec.h"
#include "Pack/AkUGCLogicPackHasher.h"
#include "Pack/AkUGCLogicPackLoader.h"
#include "Pack/AkUGCLogicPackSignature.h"

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

    /** 构造一个能通过完整文档校验的可发布作品（含 enemy_spawn 与三波 Ruleset）。 */
    FAkUGCProjectDocument MakePlayablePackDocument()
    {
        FAkUGCProjectDocument Document;
        Document.Manifest.SchemaVersion = AkUGCSchema::CurrentProjectDocumentVersion;
        Document.Manifest.ProjectId = FGuid(0x11111111, 0x22222222, 0x33333333, 0x44444444);
        Document.Manifest.DisplayName = TEXT("Playable Pack");
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

        return Document;
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAkUGCLogicPackBuilderTest,
    "AkUGC.Core.Pack.Builder",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCLogicPackBuilderTest::RunTest(const FString& Parameters)
{
    const FAkUGCProjectDocument Document = MakePlayablePackDocument();
    const FAkUGCLogicPackBuildResult Build = FAkUGCLogicPackBuilder::Build(Document);

    TestTrue(*Build.ErrorMessage, Build.bSucceeded);
    if (!Build.bSucceeded)
    {
        return false;
    }

    const FAkUGCLogicPack& Pack = Build.Pack;
    TestTrue(TEXT("ReleaseId is valid"), Pack.Manifest.ReleaseId.IsValid());
    TestEqual(TEXT("SchemaVersion matches document"),
        Pack.Manifest.SchemaVersion, AkUGCSchema::CurrentProjectDocumentVersion);
    TestEqual(TEXT("ProjectId is propagated"), Pack.Manifest.ProjectId, Document.Manifest.ProjectId);
    TestEqual(TEXT("TemplateId is propagated"), Pack.Manifest.TemplateId, Document.Manifest.TemplateId);
    TestEqual(TEXT("Capabilities are propagated"),
        Pack.Manifest.Capabilities.Num(), Document.Manifest.Capabilities.Num());
    TestEqual(TEXT("ContentHash is 64 lowercase hex characters"), Pack.Manifest.ContentHash.Len(), 64);
    TestEqual(TEXT("ContentHash matches recomputation"),
        Pack.Manifest.ContentHash,
        FAkUGCLogicPackBuilder::ComputeContentHash(Pack.Document, Pack.Program));
    TestTrue(TEXT("AssetDependencies includes enemy_spawn"),
        Pack.Manifest.AssetDependencies.Contains(TEXT("official.gameplay.enemy_spawn")));
    TestTrue(TEXT("Program has instructions"), Pack.Program.Instructions.Num() > 0);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAkUGCLogicPackCodecTest,
    "AkUGC.Core.Pack.Codec",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCLogicPackCodecTest::RunTest(const FString& Parameters)
{
    const FAkUGCLogicPackBuildResult Build = FAkUGCLogicPackBuilder::Build(MakePlayablePackDocument());
    TestTrue(*Build.ErrorMessage, Build.bSucceeded);
    if (!Build.bSucceeded)
    {
        return false;
    }

    FString Error;
    FString Json;
    TestTrue(*Error, FAkUGCLogicPackCodec::Serialize(Build.Pack, Json, &Error));
    if (!Json.StartsWith(TEXT("{")) || !Json.EndsWith(TEXT("}")))
    {
        AddError(TEXT("Serialized pack must be a JSON object."));
        return false;
    }

    FAkUGCLogicPack Decoded;
    TestTrue(*Error, FAkUGCLogicPackCodec::Deserialize(Json, Decoded, &Error));
    TestEqual(TEXT("Manifest ContentHash survives round-trip"),
        Decoded.Manifest.ContentHash, Build.Pack.Manifest.ContentHash);
    TestEqual(TEXT("ReleaseId survives round-trip"),
        Decoded.Manifest.ReleaseId, Build.Pack.Manifest.ReleaseId);
    TestEqual(TEXT("Document hash survives round-trip"),
        FAkUGCLogicPackHasher::HashDocument(Decoded.Document),
        FAkUGCLogicPackHasher::HashDocument(Build.Pack.Document));
    TestEqual(TEXT("Program hash survives round-trip"),
        FAkUGCLogicPackHasher::HashLogicProgram(Decoded.Program),
        FAkUGCLogicPackHasher::HashLogicProgram(Build.Pack.Program));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAkUGCLogicPackLoadTest,
    "AkUGC.Core.Pack.Load",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCLogicPackLoadTest::RunTest(const FString& Parameters)
{
    const FAkUGCLogicPackBuildResult Build = FAkUGCLogicPackBuilder::Build(MakePlayablePackDocument());
    TestTrue(*Build.ErrorMessage, Build.bSucceeded);
    if (!Build.bSucceeded)
    {
        return false;
    }

    FString Error;
    FString Json;
    TestTrue(*Error, FAkUGCLogicPackCodec::Serialize(Build.Pack, Json, &Error));

    const FAkUGCLogicPackLoadResult Load = FAkUGCLogicPackLoader::Load(Json);
    TestTrue(*Load.ErrorMessage, Load.bSucceeded);
    if (!Load.bSucceeded)
    {
        return false;
    }
    TestEqual(TEXT("Loaded pack keeps content hash"),
        Load.Pack.Manifest.ContentHash, Build.Pack.Manifest.ContentHash);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAkUGCLogicPackLoadTamperingTest,
    "AkUGC.Core.Pack.LoadRejectsTampering",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCLogicPackLoadTamperingTest::RunTest(const FString& Parameters)
{
    const FAkUGCLogicPackBuildResult Build = FAkUGCLogicPackBuilder::Build(MakePlayablePackDocument());
    TestTrue(*Build.ErrorMessage, Build.bSucceeded);
    if (!Build.bSucceeded)
    {
        return false;
    }

    FString Error;

    // 1. 篡改文档字段（不改 ContentHash）→ 哈希不匹配拒绝。
    FAkUGCLogicPack TamperedDoc = Build.Pack;
    TamperedDoc.Document.Manifest.DisplayName = TEXT("Hacked");
    FString TamperedDocJson;
    TestTrue(*Error, FAkUGCLogicPackCodec::Serialize(TamperedDoc, TamperedDocJson, &Error));
    TestFalse(TEXT("Tampered document is rejected"),
        FAkUGCLogicPackLoader::Load(TamperedDocJson).bSucceeded);

    // 2. 篡改 Logic IR 指令（不改 ContentHash）→ 哈希不匹配拒绝。
    FAkUGCLogicPack TamperedProg = Build.Pack;
    if (TamperedProg.Program.Instructions.Num() > 0)
    {
        TamperedProg.Program.Instructions[0].Operand = TEXT("Hacked");
    }
    FString TamperedProgJson;
    TestTrue(*Error, FAkUGCLogicPackCodec::Serialize(TamperedProg, TamperedProgJson, &Error));
    TestFalse(TEXT("Tampered program is rejected"),
        FAkUGCLogicPackLoader::Load(TamperedProgJson).bSucceeded);

    // 3. 版本不兼容 → 拒绝。
    FAkUGCLogicPack Incompatible = Build.Pack;
    Incompatible.Manifest.SchemaVersion = AkUGCSchema::CurrentProjectDocumentVersion - 1;
    FString IncompatibleJson;
    TestTrue(*Error, FAkUGCLogicPackCodec::Serialize(Incompatible, IncompatibleJson, &Error));
    TestFalse(TEXT("Incompatible version is rejected"),
        FAkUGCLogicPackLoader::Load(IncompatibleJson).bSucceeded);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAkUGCLogicPackBuilderRejectsInvalidTest,
    "AkUGC.Core.Pack.BuilderRejectsInvalid",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCLogicPackBuilderRejectsInvalidTest::RunTest(const FString& Parameters)
{
    // MakePackDocument 的波次引用 path_node 而非 enemy_spawn，无法通过完整校验。
    const FAkUGCProjectDocument Invalid = MakePackDocument();
    const FAkUGCLogicPackBuildResult Build = FAkUGCLogicPackBuilder::Build(Invalid);
    TestFalse(TEXT("Invalid document is rejected by builder"), Build.bSucceeded);
    TestFalse(TEXT("Rejection carries an error message"), Build.ErrorMessage.IsEmpty());

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAkUGCLogicPackKeyPairTest,
    "AkUGC.Core.Pack.Signature.KeyPair",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCLogicPackKeyPairTest::RunTest(const FString& Parameters)
{
    FAkUGCLogicPackKeyPair KeyPair;
    FString Error;
    TestTrue(*Error, FAkUGCLogicPackSigner::GenerateKeyPair(KeyPair, &Error));

    TestEqual(TEXT("Private key is 64 hex characters"), KeyPair.PrivateKey.Len(), 64);
    TestEqual(TEXT("Public key is 64 hex characters"), KeyPair.PublicKey.Len(), 64);
    TestNotEqual(TEXT("Private and public keys differ"), KeyPair.PrivateKey, KeyPair.PublicKey);

    FAkUGCLogicPackKeyPair Another;
    TestTrue(*Error, FAkUGCLogicPackSigner::GenerateKeyPair(Another, &Error));
    TestNotEqual(TEXT("Independent key pairs differ"), KeyPair.PublicKey, Another.PublicKey);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAkUGCLogicPackSignVerifyTest,
    "AkUGC.Core.Pack.Signature.SignVerify",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCLogicPackSignVerifyTest::RunTest(const FString& Parameters)
{
    FAkUGCLogicPackKeyPair KeyPair;
    FString Error;
    TestTrue(*Error, FAkUGCLogicPackSigner::GenerateKeyPair(KeyPair, &Error));

    const FAkUGCLogicPackBuildResult Build = FAkUGCLogicPackBuilder::Build(MakePlayablePackDocument());
    TestTrue(*Build.ErrorMessage, Build.bSucceeded);
    if (!Build.bSucceeded)
    {
        return false;
    }

    FAkUGCLogicPackSignature Signature;
    TestTrue(*Error, FAkUGCLogicPackSigner::Sign(Build.Pack.Manifest, KeyPair.PrivateKey, Signature, &Error));
    TestEqual(TEXT("Signature algorithm is ed25519"), Signature.Algorithm, TEXT("ed25519"));
    TestEqual(TEXT("Signature public key matches key pair"), Signature.PublicKey, KeyPair.PublicKey);
    TestEqual(TEXT("Signature value is 128 hex characters"), Signature.Signature.Len(), 128);

    // 验签通过。
    TestTrue(*Error, FAkUGCLogicPackVerifier::Verify(Build.Pack.Manifest, Signature, KeyPair.PublicKey, &Error));

    // 篡改清单字段 → 验签失败。
    FAkUGCLogicPackManifest Tampered = Build.Pack.Manifest;
    Tampered.ContentHash = TEXT("deadbeef");
    TestFalse(TEXT("Tampered manifest is rejected"),
        FAkUGCLogicPackVerifier::Verify(Tampered, Signature, KeyPair.PublicKey, &Error));

    // 用错误公钥验签 → 失败。
    FAkUGCLogicPackKeyPair Other;
    TestTrue(*Error, FAkUGCLogicPackSigner::GenerateKeyPair(Other, &Error));
    TestFalse(TEXT("Wrong trusted public key is rejected"),
        FAkUGCLogicPackVerifier::Verify(Build.Pack.Manifest, Signature, Other.PublicKey, &Error));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAkUGCLogicPackSignedLoadTest,
    "AkUGC.Core.Pack.Signature.SignedLoad",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAkUGCLogicPackSignedLoadTest::RunTest(const FString& Parameters)
{
    FAkUGCLogicPackKeyPair KeyPair;
    FString Error;
    TestTrue(*Error, FAkUGCLogicPackSigner::GenerateKeyPair(KeyPair, &Error));

    const FAkUGCLogicPackBuildResult Build = FAkUGCLogicPackBuilder::Build(MakePlayablePackDocument());
    TestTrue(*Build.ErrorMessage, Build.bSucceeded);
    if (!Build.bSucceeded)
    {
        return false;
    }

    // 签名后放入包，序列化。
    FAkUGCLogicPack SignedPack = Build.Pack;
    TestTrue(*Error, FAkUGCLogicPackSigner::Sign(SignedPack.Manifest, KeyPair.PrivateKey, SignedPack.Signature, &Error));

    FString Json;
    TestTrue(*Error, FAkUGCLogicPackCodec::Serialize(SignedPack, Json, &Error));

    // 反序列化后签名保留。
    FAkUGCLogicPack Decoded;
    TestTrue(*Error, FAkUGCLogicPackCodec::Deserialize(Json, Decoded, &Error));
    TestEqual(TEXT("Signature survives round-trip"), Decoded.Signature.Signature, SignedPack.Signature.Signature);

    // LoadVerified 通过。
    const FAkUGCLogicPackLoadResult Verified = FAkUGCLogicPackLoader::LoadVerified(Json, KeyPair.PublicKey);
    TestTrue(*Verified.ErrorMessage, Verified.bSucceeded);

    // 篡改 ReleaseId（不被 ContentHash 覆盖，但被签名覆盖）→ LoadVerified 拒绝。
    FAkUGCLogicPack Tampered = SignedPack;
    Tampered.Manifest.ReleaseId = FGuid::NewGuid();
    FString TamperedJson;
    TestTrue(*Error, FAkUGCLogicPackCodec::Serialize(Tampered, TamperedJson, &Error));
    const FAkUGCLogicPackLoadResult TamperedLoad = FAkUGCLogicPackLoader::LoadVerified(TamperedJson, KeyPair.PublicKey);
    TestFalse(TEXT("Tampered ReleaseId is rejected by verification"), TamperedLoad.bSucceeded);

    // 未签名包被 LoadVerified 拒绝。
    FString UnsignedJson;
    TestTrue(*Error, FAkUGCLogicPackCodec::Serialize(Build.Pack, UnsignedJson, &Error));
    const FAkUGCLogicPackLoadResult UnsignedLoad = FAkUGCLogicPackLoader::LoadVerified(UnsignedJson, KeyPair.PublicKey);
    TestFalse(TEXT("Unsigned pack is rejected by LoadVerified"), UnsignedLoad.bSucceeded);

    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
