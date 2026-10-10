#include "Template/AkUGCTowerDefenseTemplate.h"

#include "Command/AkUGCCommandExecutor.h"
#include "Document/AkUGCDocumentJson.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "JsonObjectConverter.h"
#include "Prefab/AkUGCOfficialPrefabCatalog.h"
#include "Prefab/AkUGCPrefabRegistry.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Validation/AkUGCDocumentValidator.h"

namespace
{
    const FName SpawnType(TEXT("tower_defense.spawn"));
    const FGuid TemplateGameStartId(0xA1100001, 0, 0, 0);
    const FGuid TemplateWaveStartId(0xA1100002, 0, 0, 0);

    struct FWaveNumberField
    {
        const TCHAR* JsonName;
        const TCHAR* PropertyName;
        double FAkUGCTowerDefenseWaveParameters::* Member;
        double Minimum;
        double Maximum;
    };

    const FWaveNumberField WaveNumbers[] = {
        {TEXT("spawnIntervalSeconds"), TEXT("spawnInterval"), &FAkUGCTowerDefenseWaveParameters::SpawnIntervalSeconds, 0.1, 60.0},
        {TEXT("enemyMaxHealth"), TEXT("enemyMaxHealth"), &FAkUGCTowerDefenseWaveParameters::EnemyMaxHealth, 1.0, 100000.0},
        {TEXT("enemyMoveSpeed"), TEXT("enemyMoveSpeed"), &FAkUGCTowerDefenseWaveParameters::EnemyMoveSpeed, 10.0, 2000.0},
        {TEXT("enemyGoalDamage"), TEXT("enemyGoalDamage"), &FAkUGCTowerDefenseWaveParameters::EnemyGoalDamage, 0.0, 100000.0},
        {TEXT("startDelaySeconds"), nullptr, &FAkUGCTowerDefenseWaveParameters::StartDelaySeconds, 0.0, 3600.0}
    };

    bool TemplateFail(FString* OutError, const FString& Message)
    {
        if (OutError) { *OutError = Message; }
        return false;
    }

    bool InRange(double Value, double Minimum, double Maximum)
    {
        return FMath::IsFinite(Value) && Value >= Minimum && Value <= Maximum;
    }

    const FAkUGCValue* Property(const FAkUGCEntityRecord& Entity, FName Type, FName Name)
    {
        const FAkUGCComponentRecord* Component = Entity.Components.FindByPredicate(
            [Type](const FAkUGCComponentRecord& Candidate) { return Candidate.TypeId == Type; });
        return Component ? Component->Properties.Find(Name) : nullptr;
    }

    FAkUGCValue Number(double Value)
    {
        FAkUGCValue Result;
        Result.Type = EAkUGCValueType::Number;
        Result.NumberValue = Value;
        return Result;
    }

    FAkUGCValue Integer(int32 Value)
    {
        FAkUGCValue Result;
        Result.Type = EAkUGCValueType::Integer;
        Result.IntegerValue = Value;
        return Result;
    }

    FName WaveTag(int32 Index)
    {
        return FName(*FString::Printf(TEXT("akugc.l1.wave.%d"), Index));
    }

    bool CheckDocument(const FAkUGCProjectDocument& Document, FString* OutError)
    {
        const FAkUGCValidationResult Validation = FAkUGCDocumentValidator::Validate(Document);
        if (Validation.IsValid()) { return true; }
        const FAkUGCValidationIssue* Issue = Validation.Issues.FindByPredicate(
            [](const FAkUGCValidationIssue& Candidate) { return Candidate.Severity == EAkUGCValidationSeverity::Error; });
        return TemplateFail(OutError, Issue ? Issue->Path + TEXT(": ") + Issue->Message : TEXT("Invalid project document."));
    }

    bool ExactFields(const TSharedPtr<FJsonObject>& Object, const TSet<FString>& Allowed, FString* OutError)
    {
        for (const auto& Pair : Object->Values)
        {
            const FString Key(Pair.Key);
            bool bCanonical = false;
            for (const FString& Name : Allowed)
            {
                bCanonical |= Name.Equals(Key, ESearchCase::CaseSensitive);
            }
            if (!bCanonical)
            {
                return TemplateFail(OutError, FString(TEXT("Unknown or noncanonical parameter field: ")) + Key);
            }
        }
        return true;
    }

    bool ReadNumber(const TSharedPtr<FJsonObject>& Object, const TCHAR* Name,
        double Minimum, double Maximum, double& OutValue, FString* OutError)
    {
        const TSharedPtr<FJsonValue>* Value = Object->Values.Find(Name);
        if (!Value || !Value->IsValid() || (*Value)->Type != EJson::Number
            || !InRange((*Value)->AsNumber(), Minimum, Maximum))
        {
            return TemplateFail(OutError, FString::Printf(TEXT("%s must be a finite Number from %g to %g."), Name, Minimum, Maximum));
        }
        OutValue = (*Value)->AsNumber();
        return true;
    }

    bool ReadInteger(const TSharedPtr<FJsonObject>& Object, const TCHAR* Name,
        int32 Minimum, int32 Maximum, int32& OutValue, FString* OutError)
    {
        double Value = 0.0;
        if (!ReadNumber(Object, Name, Minimum, Maximum, Value, OutError)) { return false; }
        if (Value != FMath::TruncToDouble(Value)) { return TemplateFail(OutError, FString(Name) + TEXT(" must be an integer.")); }
        OutValue = static_cast<int32>(Value);
        return true;
    }
}

bool FAkUGCTowerDefenseTemplate::Validate(const FAkUGCTowerDefenseParameters& Parameters,
    const FAkUGCPrefabRegistry& Registry, FString* OutError)
{
    if (Parameters.SchemaVersion != 1
        || Parameters.Waves.Num() < AkUGCTowerDefenseRulesetLimits::MinWaveCount
        || Parameters.Waves.Num() > AkUGCTowerDefenseRulesetLimits::MaxWaveCount)
    {
        return TemplateFail(OutError, TEXT("Parameter schema must be version 1 with 1 to 3 waves."));
    }
    if (!InRange(Parameters.BaseMaxHealth, 1.0, 100000.0)
        || !InRange(Parameters.WaveIntervalSeconds, 0.0, 3600.0)
        || Parameters.InitialResources < 0 || Parameters.InitialResources > AkUGCTowerDefenseRulesetLimits::MaxInitialResources
        || Parameters.DefeatCondition != EAkUGCTowerDefenseDefeatCondition::BaseHealthDepleted
        || Parameters.VictoryCondition != EAkUGCTowerDefenseVictoryCondition::AllWavesCleared)
    {
        return TemplateFail(OutError, TEXT("Base health, resources, wave interval or match conditions violate the template schema."));
    }
    int64 Total = 0;
    for (int32 Index = 0; Index < Parameters.Waves.Num(); ++Index)
    {
        const FAkUGCTowerDefenseWaveParameters& Wave = Parameters.Waves[Index];
        if (Wave.EnemyPrefabId != TEXT("official.unit.basic_enemy") || !Registry.Find(Wave.EnemyPrefabId)
            || Wave.EnemyCount < 1 || Wave.EnemyCount > 500)
        {
            return TemplateFail(OutError, FString::Printf(TEXT("Wave %d requires an official Basic Enemy and a count from 1 to 500."), Index + 1));
        }
        for (const FWaveNumberField& Field : WaveNumbers)
        {
            if (!InRange(Wave.*Field.Member, Field.Minimum, Field.Maximum))
            {
                return TemplateFail(OutError, FString::Printf(TEXT("Wave %d: %s must be finite and from %g to %g."),
                    Index + 1, Field.JsonName, Field.Minimum, Field.Maximum));
            }
        }
        Total += Wave.EnemyCount;
    }
    return Total <= AkUGCTowerDefenseRulesetLimits::MaxTotalEnemyCount
        ? true : TemplateFail(OutError, TEXT("Total wave enemies exceed the hard limit of 500."));
}

bool FAkUGCTowerDefenseTemplate::ReadParameters(const FAkUGCProjectDocument& Document,
    const FGuid& SceneId, FAkUGCTowerDefenseParameters& OutParameters, FString* OutError)
{
    if (Document.Manifest.TemplateId != TEXT("official.tower_defense") || !CheckDocument(Document, OutError))
    {
        return TemplateFail(OutError, TEXT("A valid official tower defense document is required."));
    }
    const FAkUGCSceneDocument* Scene = Document.Scenes.FindByPredicate(
        [&SceneId](const FAkUGCSceneDocument& Candidate) { return Candidate.SceneId == SceneId; });
    if (!Scene) { return TemplateFail(OutError, TEXT("The active scene does not exist.")); }
    const FAkUGCEntityRecord* Base = nullptr;
    int32 BaseCount = 0;
    int32 GoalCount = 0;
    int32 PathCount = 0;
    for (const FAkUGCEntityRecord& Entity : Scene->Entities)
    {
        if (Entity.PrefabId == TEXT("official.gameplay.base")) { Base = &Entity; ++BaseCount; }
        if (Entity.PrefabId == TEXT("official.gameplay.goal")) { ++GoalCount; }
        if (Entity.PrefabId == TEXT("official.gameplay.path_node")) { ++PathCount; }
    }
    if (BaseCount != 1 || GoalCount != 1 || PathCount < 2
        || !Scene->LogicGraph.Nodes.ContainsByPredicate(
            [](const FAkUGCLogicNode& Node) { return Node.Type == EAkUGCLogicNodeType::WaveStart; }))
    {
        return TemplateFail(OutError, TEXT("Simple parameters require exactly one Base and Goal, at least two path nodes and a WaveStart entry."));
    }
    const FAkUGCValue* Health = Property(*Base, TEXT("core.health"), TEXT("maxHealth"));
    if (!Health || Health->Type != EAkUGCValueType::Number) { return TemplateFail(OutError, TEXT("Base maxHealth must be a Number.")); }
    FAkUGCTowerDefenseParameters Result;
    Result.BaseMaxHealth = Health->NumberValue;
    Result.InitialResources = Scene->Ruleset.InitialResources;
    Result.WaveIntervalSeconds = Scene->Ruleset.WaveIntervalSeconds;
    Result.DefeatCondition = Scene->Ruleset.DefeatCondition;
    Result.VictoryCondition = Scene->Ruleset.VictoryCondition;
    Result.Waves.Reset();
    for (const FAkUGCTowerDefenseWave& Wave : Scene->Ruleset.Waves)
    {
        const FAkUGCEntityRecord* Spawn = Scene->Entities.FindByPredicate(
            [&Wave](const FAkUGCEntityRecord& Entity) { return Entity.EntityId == Wave.SpawnPointEntityId; });
        if (!Spawn || Spawn->PrefabId != TEXT("official.gameplay.enemy_spawn"))
        {
            return TemplateFail(OutError, TEXT("Every wave must reference an official Enemy Spawn in this scene."));
        }
        const FAkUGCValue* Prefab = Property(*Spawn, SpawnType, TEXT("enemyPrefab"));
        const FAkUGCValue* Count = Property(*Spawn, SpawnType, TEXT("enemyCount"));
        const FAkUGCValue* Interval = Property(*Spawn, SpawnType, TEXT("spawnInterval"));
        if (!Prefab || Prefab->Type != EAkUGCValueType::Name || !Count || Count->Type != EAkUGCValueType::Integer
            || Count->IntegerValue < 1 || Count->IntegerValue > 500 || !Interval || Interval->Type != EAkUGCValueType::Number)
        {
            return TemplateFail(OutError, TEXT("Enemy Spawn requires typed enemyPrefab, enemyCount and spawnInterval properties."));
        }
        FAkUGCTowerDefenseWaveParameters& Item = Result.Waves.AddDefaulted_GetRef();
        Item.EnemyPrefabId = Prefab->NameValue;
        Item.EnemyCount = static_cast<int32>(Count->IntegerValue);
        Item.SpawnIntervalSeconds = Interval->NumberValue;
        Item.StartDelaySeconds = Wave.StartDelaySeconds;
        for (const FWaveNumberField& Field : WaveNumbers)
        {
            if (!Field.PropertyName || Field.Member == &FAkUGCTowerDefenseWaveParameters::SpawnIntervalSeconds) { continue; }
            if (const FAkUGCValue* Value = Property(*Spawn, SpawnType, Field.PropertyName))
            {
                if (Value->Type != EAkUGCValueType::Number) { return TemplateFail(OutError, TEXT("Enemy overrides must be Number properties.")); }
                Item.*Field.Member = Value->NumberValue;
            }
        }
    }
    FAkUGCPrefabRegistry Registry;
    if (!FAkUGCOfficialPrefabCatalog::RegisterTowerDefense(Registry, OutError) || !Validate(Result, Registry, OutError)) { return false; }
    OutParameters = MoveTemp(Result);
    return true;
}

bool FAkUGCTowerDefenseTemplate::ValidateDocumentBudgets(const FAkUGCProjectDocument& Document,
    const FAkUGCPrefabRegistry& Registry, FString* OutError)
{
    int64 Entities = 0, Render = 0, Physics = 0, Script = 0;
    const auto AddCost = [&](FName PrefabId, int64 Count)
    {
        const FAkUGCPrefabDefinition* Prefab = Registry.Find(PrefabId);
        if (!Prefab || Count < 0 || Prefab->Cost.EntityUnits < 0 || Prefab->Cost.RenderUnits < 0
            || Prefab->Cost.PhysicsUnits < 0 || Prefab->Cost.ScriptUnits < 0)
        {
            return TemplateFail(OutError, TEXT("Content budget requires registered Prefabs with nonnegative costs."));
        }
        Entities += Count * Prefab->Cost.EntityUnits;
        Render += Count * Prefab->Cost.RenderUnits;
        Physics += Count * Prefab->Cost.PhysicsUnits;
        Script += Count * Prefab->Cost.ScriptUnits;
        return true;
    };
    for (const FAkUGCSceneDocument& Scene : Document.Scenes)
    {
        for (const FAkUGCEntityRecord& Entity : Scene.Entities)
        {
            if (!AddCost(Entity.PrefabId, 1)) { return false; }
        }
        int64 TotalEnemies = 0;
        for (const FAkUGCTowerDefenseWave& Wave : Scene.Ruleset.Waves)
        {
            const FAkUGCEntityRecord* Spawn = Scene.Entities.FindByPredicate(
                [&Wave](const FAkUGCEntityRecord& Entity) { return Entity.EntityId == Wave.SpawnPointEntityId; });
            const FAkUGCValue* Prefab = Spawn ? Property(*Spawn, SpawnType, TEXT("enemyPrefab")) : nullptr;
            const FAkUGCValue* Count = Spawn ? Property(*Spawn, SpawnType, TEXT("enemyCount")) : nullptr;
            if (!Prefab || Prefab->Type != EAkUGCValueType::Name || Prefab->NameValue != TEXT("official.unit.basic_enemy")
                || !Count || Count->Type != EAkUGCValueType::Integer || Count->IntegerValue < 1 || Count->IntegerValue > 500)
            {
                return TemplateFail(OutError, TEXT("Wave budgets require a supported Enemy Spawn configuration."));
            }
            TotalEnemies += Count->IntegerValue;
            if (TotalEnemies > 500 || !AddCost(Prefab->NameValue, Count->IntegerValue))
            {
                return TemplateFail(OutError, TEXT("Wave enemies exceed the supported count or content budget."));
            }
        }
        TArray<FGuid> Pending;
        for (const FAkUGCLogicNode& Node : Scene.LogicGraph.Nodes)
        {
            if (Node.Type == EAkUGCLogicNodeType::GameStart) { Pending.Add(Node.NodeId); }
        }
        int32 ReadIndex = 0;
        while (ReadIndex < Pending.Num())
        {
            if (Pending.Num() > AkUGCLogicLimits::MaxExecutedInstructions)
            {
                return TemplateFail(OutError, TEXT("Logic content budget exceeds the execution limit."));
            }
            const FGuid NodeId = Pending[ReadIndex++];
            const FAkUGCLogicNode* Node = Scene.LogicGraph.Nodes.FindByPredicate(
                [&NodeId](const FAkUGCLogicNode& Candidate) { return Candidate.NodeId == NodeId; });
            if (!Node) { return TemplateFail(OutError, TEXT("Logic budget references an unknown node.")); }
            if (Node->Type == EAkUGCLogicNodeType::Spawn)
            {
                int64 Count = 1;
                if (Node->SpawnAtEntityId.IsValid())
                {
                    const FAkUGCEntityRecord* Spawn = Scene.Entities.FindByPredicate(
                        [&](const FAkUGCEntityRecord& Entity) { return Entity.EntityId == Node->SpawnAtEntityId; });
                    const FAkUGCValue* Value = Spawn ? Property(*Spawn, SpawnType, TEXT("enemyCount")) : nullptr;
                    if (Value && Value->Type == EAkUGCValueType::Integer && Value->IntegerValue >= 1 && Value->IntegerValue <= 500)
                    {
                        Count = Value->IntegerValue;
                    }
                }
                TotalEnemies += Count;
                if (TotalEnemies > 500 || !AddCost(Node->SpawnPrefabId, Count)) { return TemplateFail(OutError, TEXT("Logic Spawn exceeds the dynamic enemy budget.")); }
            }
            for (const FAkUGCLogicConnection& Connection : Scene.LogicGraph.Connections)
            {
                if (Connection.SourceNodeId == NodeId) { Pending.Add(Connection.TargetNodeId); }
            }
        }
    }
    if (Entities > AkUGCMobileBudgetLimits::MaxEntityUnits || Render > AkUGCMobileBudgetLimits::MaxRenderUnits
        || Physics > AkUGCMobileBudgetLimits::MaxPhysicsUnits || Script > AkUGCMobileBudgetLimits::MaxScriptUnits)
    {
        return TemplateFail(OutError, FString::Printf(TEXT("Content budget including wave enemies exceeded: entity=%lld/%d render=%lld/%d physics=%lld/%d script=%lld/%d."),
            Entities, AkUGCMobileBudgetLimits::MaxEntityUnits, Render, AkUGCMobileBudgetLimits::MaxRenderUnits,
            Physics, AkUGCMobileBudgetLimits::MaxPhysicsUnits, Script, AkUGCMobileBudgetLimits::MaxScriptUnits));
    }
    return true;
}

bool FAkUGCTowerDefenseTemplate::BuildTransaction(const FAkUGCProjectDocument& Document, const FGuid& SceneId,
    const FAkUGCTowerDefenseParameters& Parameters, const FAkUGCPrefabRegistry& Registry,
    FAkUGCCommandTransaction& OutTransaction, FString* OutError)
{
    OutTransaction = FAkUGCCommandTransaction();
    FAkUGCTowerDefenseParameters Current;
    if (!Validate(Parameters, Registry, OutError) || !ReadParameters(Document, SceneId, Current, OutError)) { return false; }
    const FAkUGCSceneDocument& Scene = *Document.Scenes.FindByPredicate(
        [&SceneId](const FAkUGCSceneDocument& Candidate) { return Candidate.SceneId == SceneId; });
    const FAkUGCEntityRecord& Base = *Scene.Entities.FindByPredicate(
        [](const FAkUGCEntityRecord& Entity) { return Entity.PrefabId == TEXT("official.gameplay.base"); });
    FAkUGCCommandTransaction Result;
    Result.TransactionId = FGuid::NewGuid();
    Result.Label = TEXT("Apply tower defense parameters");
    const auto Command = [&](EAkUGCCommandType Type) -> FAkUGCCommand&
    {
        FAkUGCCommand& Item = Result.Commands.AddDefaulted_GetRef();
        Item.CommandId = FGuid::NewGuid();
        Item.SceneId = SceneId;
        Item.Type = Type;
        return Item;
    };
    const auto SetProperty = [&](FGuid EntityId, FName Type, FName Name, FAkUGCValue Value)
    {
        FAkUGCCommand& Item = Command(EAkUGCCommandType::SetProperty);
        Item.EntityId = EntityId;
        Item.ComponentTypeId = Type;
        Item.PropertyId = Name;
        Item.PropertyValue = MoveTemp(Value);
    };
    SetProperty(Base.EntityId, TEXT("core.health"), TEXT("maxHealth"), Number(Parameters.BaseMaxHealth));
    FAkUGCCommand& Settings = Command(EAkUGCCommandType::SetRulesetSettings);
    Settings.InitialResources = Parameters.InitialResources;
    Settings.WaveIntervalSeconds = Parameters.WaveIntervalSeconds;
    Settings.DefeatCondition = Parameters.DefeatCondition;
    Settings.VictoryCondition = Parameters.VictoryCondition;
    for (int32 Index = Scene.Ruleset.Waves.Num() - 1; Index >= Parameters.Waves.Num(); --Index)
    {
        Command(EAkUGCCommandType::DeleteWave).Wave.WaveId = Scene.Ruleset.Waves[Index].WaveId;
    }
    TSet<FGuid> UsedSpawns;
    for (int32 Index = 0; Index < Parameters.Waves.Num(); ++Index)
    {
        const bool bExistingWave = Scene.Ruleset.Waves.IsValidIndex(Index);
        FAkUGCTowerDefenseWave Wave = bExistingWave ? Scene.Ruleset.Waves[Index] : FAkUGCTowerDefenseWave();
        const FAkUGCEntityRecord* Spawn = bExistingWave ? Scene.Entities.FindByPredicate(
            [&Wave](const FAkUGCEntityRecord& Entity) { return Entity.EntityId == Wave.SpawnPointEntityId; }) : nullptr;
        const FAkUGCEntityRecord* CloneSource = Spawn;
        if (Spawn && UsedSpawns.Contains(Spawn->EntityId)) { Spawn = nullptr; }
        if (!bExistingWave)
        {
            Spawn = Scene.Entities.FindByPredicate([&](const FAkUGCEntityRecord& Entity)
            {
                return Entity.PrefabId == TEXT("official.gameplay.enemy_spawn") && Entity.Tags.Contains(WaveTag(Index))
                    && !UsedSpawns.Contains(Entity.EntityId)
                    && !Scene.Ruleset.Waves.ContainsByPredicate([&](const FAkUGCTowerDefenseWave& Existing)
                    {
                        return Existing.SpawnPointEntityId == Entity.EntityId;
                    });
            });
        }
        FAkUGCEntityRecord NewSpawn;
        if (!Spawn)
        {
            if (CloneSource) { NewSpawn = *CloneSource; NewSpawn.EntityId = FGuid::NewGuid(); }
            else if (!Registry.CreateEntityRecord(TEXT("official.gameplay.enemy_spawn"), FGuid::NewGuid(),
                FTransform(FVector(0.0, 0.0, 0.0)), NewSpawn, OutError)) { return false; }
            NewSpawn.Tags.AddUnique(WaveTag(Index));
            FAkUGCCommand& Add = Command(EAkUGCCommandType::AddEntity);
            Add.EntityId = NewSpawn.EntityId;
            Add.Entity = NewSpawn;
            Spawn = &NewSpawn;
        }
        Wave.SpawnPointEntityId = Spawn->EntityId;
        Wave.StartDelaySeconds = Parameters.Waves[Index].StartDelaySeconds;
        if (!bExistingWave) { Wave.WaveId = FGuid::NewGuid(); }
        UsedSpawns.Add(Spawn->EntityId);
        FAkUGCValue EnemyPrefab;
        EnemyPrefab.Type = EAkUGCValueType::Name;
        EnemyPrefab.NameValue = Parameters.Waves[Index].EnemyPrefabId;
        SetProperty(Spawn->EntityId, SpawnType, TEXT("enemyPrefab"), EnemyPrefab);
        SetProperty(Spawn->EntityId, SpawnType, TEXT("enemyCount"), Integer(Parameters.Waves[Index].EnemyCount));
        for (const FWaveNumberField& Field : WaveNumbers)
        {
            if (Field.PropertyName) { SetProperty(Spawn->EntityId, SpawnType, Field.PropertyName, Number(Parameters.Waves[Index].*Field.Member)); }
        }
        FAkUGCCommand& SetWave = Command(bExistingWave ? EAkUGCCommandType::UpdateWave : EAkUGCCommandType::AddWave);
        SetWave.Wave = Wave;
        SetWave.WaveIndex = Index;
    }
    FAkUGCProjectDocument Candidate = Document;
    const FAkUGCCommandExecutionResult Applied = FAkUGCCommandExecutor::Apply(Candidate, Result);
    if (!Applied.bSucceeded) { return TemplateFail(OutError, Applied.ErrorPath + TEXT(": ") + Applied.ErrorMessage); }
    if (!ValidateDocumentBudgets(Candidate, Registry, OutError)) { return false; }
    OutTransaction = MoveTemp(Result);
    return true;
}

bool FAkUGCTowerDefenseTemplate::IsTemplateOwnedLogicGraph(const FAkUGCSceneDocument& Scene)
{
    if (Scene.LogicGraph.Nodes.Num() != 2 || !Scene.LogicGraph.Connections.IsEmpty()) { return false; }
    bool bStart = false, bWave = false;
    for (const FAkUGCLogicNode& Node : Scene.LogicGraph.Nodes)
    {
        if (!Node.Message.IsEmpty() || Node.DelaySeconds != 0.0 || !Node.SpawnPrefabId.IsNone()
            || Node.SpawnAtEntityId.IsValid() || !FMath::IsFinite(Node.PositionX) || !FMath::IsFinite(Node.PositionY)) { return false; }
        bStart |= Node.Type == EAkUGCLogicNodeType::GameStart && Node.NodeId == TemplateGameStartId;
        bWave |= Node.Type == EAkUGCLogicNodeType::WaveStart && Node.NodeId == TemplateWaveStartId;
    }
    return bStart && bWave;
}

bool FAkUGCTowerDefenseTemplate::CreateDefaultDocument(const FAkUGCPrefabRegistry& Registry,
    FAkUGCProjectDocument& OutDocument, FString* OutError)
{
    FAkUGCProjectDocument Document;
    Document.Manifest.ProjectId = FGuid::NewGuid();
    Document.Manifest.DisplayName = TEXT("Tower Defense Project");
    Document.Manifest.TemplateId = TEXT("official.tower_defense");
    Document.Manifest.Capabilities = {TEXT("logic"), TEXT("ruleset"), TEXT("world.spawn"), TEXT("rules.wave")};
    FAkUGCSceneDocument& Scene = Document.Scenes.AddDefaulted_GetRef();
    Scene.SceneId = FGuid::NewGuid();
    Scene.DisplayName = TEXT("Main");
    const auto Add = [&](FName PrefabId, FVector Position, FAkUGCEntityRecord*& OutEntity)
    {
        FAkUGCEntityRecord Entity;
        if (!Registry.CreateEntityRecord(PrefabId, FGuid::NewGuid(), FTransform(Position), Entity, OutError)) { return false; }
        Scene.Entities.Add(MoveTemp(Entity));
        OutEntity = &Scene.Entities.Last();
        return true;
    };
    FAkUGCEntityRecord* Entity = nullptr;
    if (!Add(TEXT("official.environment.ground"), FVector(750.0, 0.0, -50.0), Entity)) { return false; }
    Entity->Transform.SetScale3D(FVector(20.0, 10.0, 1.0));
    if (!Add(TEXT("official.gameplay.base"), FVector(1600.0, 0.0, 0.0), Entity)
        || !Add(TEXT("official.gameplay.goal"), FVector(1500.0, 0.0, 0.0), Entity)) { return false; }
    for (int32 Index = 0; Index < 2; ++Index)
    {
        if (!Add(TEXT("official.gameplay.path_node"), FVector(Index == 0 ? 200.0 : 1500.0, 0.0, 0.0), Entity)) { return false; }
        Entity->Components[0].Properties.FindChecked(TEXT("order")) = Integer(Index);
    }
    if (!Add(TEXT("official.tower.basic"), FVector(650.0, 150.0, 0.0), Entity)
        || !Add(TEXT("official.tower.basic"), FVector(1100.0, -150.0, 0.0), Entity)) { return false; }
    for (int32 Index = 0; Index < 3; ++Index)
    {
        if (!Add(TEXT("official.gameplay.enemy_spawn"), FVector::ZeroVector, Entity)) { return false; }
        Entity->Tags.Add(WaveTag(Index));
        FAkUGCTowerDefenseWave& Wave = Scene.Ruleset.Waves.AddDefaulted_GetRef();
        Wave.WaveId = FGuid::NewGuid();
        Wave.SpawnPointEntityId = Entity->EntityId;
    }
    FAkUGCLogicNode Start;
    Start.NodeId = TemplateGameStartId;
    Start.Type = EAkUGCLogicNodeType::GameStart;
    Scene.LogicGraph.Nodes.Add(Start);
    Start.NodeId = TemplateWaveStartId;
    Start.Type = EAkUGCLogicNodeType::WaveStart;
    Start.PositionX = 250.0f;
    Scene.LogicGraph.Nodes.Add(Start);
    if (!CheckDocument(Document, OutError) || !ValidateDocumentBudgets(Document, Registry, OutError)) { return false; }
    OutDocument = MoveTemp(Document);
    return true;
}

bool FAkUGCTowerDefenseTemplate::SerializeParameters(const FAkUGCTowerDefenseParameters& Parameters,
    FString& OutJson, FString* OutError)
{
    OutJson.Reset();
    FAkUGCPrefabRegistry Registry;
    if (!FAkUGCOfficialPrefabCatalog::RegisterTowerDefense(Registry, OutError) || !Validate(Parameters, Registry, OutError)) { return false; }
    if (!FJsonObjectConverter::UStructToJsonObjectString(Parameters, OutJson))
    {
        return TemplateFail(OutError, TEXT("Failed to serialize tower defense parameters."));
    }
    return true;
}

bool FAkUGCTowerDefenseTemplate::DeserializeParameters(const FString& Json,
    FAkUGCTowerDefenseParameters& OutParameters, FString* OutError)
{
    if (!FAkUGCDocumentJson::ValidateNoDuplicateFields(Json, OutError)) { return false; }
    TSharedPtr<FJsonObject> Root;
    if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root) || !Root.IsValid())
    {
        return TemplateFail(OutError, TEXT("Parameter JSON must be an object."));
    }
    const TSet<FString> RootFields = {TEXT("schemaVersion"), TEXT("waves"), TEXT("initialResources"),
        TEXT("baseMaxHealth"), TEXT("waveIntervalSeconds"), TEXT("defeatCondition"), TEXT("victoryCondition")};
    if (!ExactFields(Root, RootFields, OutError)) { return false; }
    FAkUGCTowerDefenseParameters Result;
    int32 Version = 0;
    if (!ReadInteger(Root, TEXT("schemaVersion"), 0, 1, Version, OutError)
        || !ReadNumber(Root, TEXT("baseMaxHealth"), 1.0, 100000.0, Result.BaseMaxHealth, OutError)
        || !ReadNumber(Root, TEXT("waveIntervalSeconds"), 0.0, 3600.0, Result.WaveIntervalSeconds, OutError)) { return false; }
    if (Version == 0 && !Root->HasField(TEXT("initialResources"))) { Result.InitialResources = 0; }
    else if (!ReadInteger(Root, TEXT("initialResources"), 0, AkUGCTowerDefenseRulesetLimits::MaxInitialResources,
        Result.InitialResources, OutError)) { return false; }
    FString Defeat, Victory;
    if (!Root->TryGetStringField(TEXT("defeatCondition"), Defeat) || !Defeat.Equals(TEXT("BaseHealthDepleted"), ESearchCase::CaseSensitive)
        || !Root->TryGetStringField(TEXT("victoryCondition"), Victory) || !Victory.Equals(TEXT("AllWavesCleared"), ESearchCase::CaseSensitive))
    {
        return TemplateFail(OutError, TEXT("Only BaseHealthDepleted and AllWavesCleared conditions are supported."));
    }
    const TArray<TSharedPtr<FJsonValue>>* Waves = nullptr;
    if (!Root->TryGetArrayField(TEXT("waves"), Waves) || Waves->Num() < 1 || Waves->Num() > 3)
    {
        return TemplateFail(OutError, TEXT("waves must be an array of 1 to 3 wave objects."));
    }
    TSet<FString> WaveFields = {TEXT("enemyPrefabId"), TEXT("enemyCount")};
    for (const FWaveNumberField& Field : WaveNumbers) { WaveFields.Add(Field.JsonName); }
    Result.Waves.Reset();
    for (const TSharedPtr<FJsonValue>& Value : *Waves)
    {
        if (!Value.IsValid() || Value->Type != EJson::Object) { return TemplateFail(OutError, TEXT("Every wave must be an object.")); }
        const TSharedPtr<FJsonObject> Wave = Value->AsObject();
        if (!ExactFields(Wave, WaveFields, OutError)) { return false; }
        FAkUGCTowerDefenseWaveParameters& Item = Result.Waves.AddDefaulted_GetRef();
        FString Enemy;
        if (!Wave->TryGetStringField(TEXT("enemyPrefabId"), Enemy) || !Enemy.Equals(TEXT("official.unit.basic_enemy"), ESearchCase::CaseSensitive))
        {
            return TemplateFail(OutError, TEXT("enemyPrefabId must be official.unit.basic_enemy."));
        }
        if (!ReadInteger(Wave, TEXT("enemyCount"), 1, 500, Item.EnemyCount, OutError)) { return false; }
        for (const FWaveNumberField& Field : WaveNumbers)
        {
            if (Version == 0 && Field.PropertyName && FCString::Strncmp(Field.PropertyName, TEXT("enemy"), 5) == 0
                && !Wave->HasField(Field.JsonName)) { continue; }
            if (!ReadNumber(Wave, Field.JsonName, Field.Minimum, Field.Maximum, Item.*Field.Member, OutError)) { return false; }
        }
    }
    FAkUGCPrefabRegistry Registry;
    if (!FAkUGCOfficialPrefabCatalog::RegisterTowerDefense(Registry, OutError) || !Validate(Result, Registry, OutError)) { return false; }
    OutParameters = MoveTemp(Result);
    return true;
}
