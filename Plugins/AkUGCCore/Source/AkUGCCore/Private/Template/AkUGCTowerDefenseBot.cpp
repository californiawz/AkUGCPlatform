#include "Template/AkUGCTowerDefenseBot.h"

#include "Command/AkUGCCommandExecutor.h"
#include "Prefab/AkUGCPrefabRegistry.h"
#include "Template/AkUGCTowerDefenseTemplate.h"

bool FAkUGCTowerDefenseBot::BuildPreparationTransaction(const FAkUGCProjectDocument& Document,
    const FAkUGCPrefabRegistry& Registry, FAkUGCCommandTransaction& OutTransaction,
    int32& OutTowerCount, FString* OutError)
{
    OutTransaction = {};
    OutTowerCount = 0;
    const auto FailBot = [&](const TCHAR* Message)
    {
        if (OutError) { *OutError = Message; }
        return false;
    };
    if (Document.Scenes.Num() != 1) { return FailBot(TEXT("Bot preparation requires one scene.")); }
    const FAkUGCSceneDocument& Scene = Document.Scenes[0];
    FAkUGCTowerDefenseParameters Parameters;
    if (!FAkUGCTowerDefenseTemplate::ReadParameters(Document, Scene.SceneId, Parameters, OutError)) { return false; }
    TArray<const FAkUGCEntityRecord*> Slots;
    for (const FAkUGCEntityRecord& Entity : Scene.Entities)
    {
        if (Entity.PrefabId == TEXT("official.gameplay.tower_slot")) { Slots.Add(&Entity); }
    }
    Slots.Sort([](const FAkUGCEntityRecord& A, const FAkUGCEntityRecord& B)
    {
        return A.EntityId.ToString(EGuidFormats::Digits) < B.EntityId.ToString(EGuidFormats::Digits);
    });
    FAkUGCCommandTransaction Transaction;
    Transaction.TransactionId = FGuid::NewDeterministicGuid(TEXT("akugc-bot-preparation-transaction"));
    Transaction.Label = TEXT("Bot preparation");
    int32 Resources = Scene.Ruleset.InitialResources;
    int32 Count = 0;
    TArray<FVector> PlannedPositions;
    for (const FAkUGCEntityRecord* Slot : Slots)
    {
        if (Count >= MaxBotTowers || Resources < TowerCost) { break; }
        const bool bOccupied = Scene.Entities.ContainsByPredicate([&](const FAkUGCEntityRecord& Entity)
        {
            return Entity.PrefabId == TEXT("official.tower.basic")
                && FVector::DistSquared(Entity.Transform.GetLocation(), Slot->Transform.GetLocation()) < 1.0;
        });
        if (bOccupied || PlannedPositions.ContainsByPredicate([&](const FVector& Position)
        {
            return FVector::DistSquared(Position, Slot->Transform.GetLocation()) < 1.0;
        })) { continue; }
        const FAkUGCComponentRecord* Config = Slot->Components.FindByPredicate(
            [](const FAkUGCComponentRecord& Component) { return Component.TypeId == TEXT("tower_defense.tower_slot"); });
        const FAkUGCValue* Tag = Config ? Config->Properties.Find(TEXT("allowedTowerTag")) : nullptr;
        if (!Tag || Tag->Type != EAkUGCValueType::Name || Tag->NameValue != TEXT("tower.basic")) { continue; }
        const FGuid TowerId = FGuid::NewDeterministicGuid(TEXT("akugc-bot-tower-") + Slot->EntityId.ToString(EGuidFormats::Digits));
        if (Scene.Entities.ContainsByPredicate([&](const FAkUGCEntityRecord& Entity) { return Entity.EntityId == TowerId; }))
        {
            return FailBot(TEXT("Bot tower identity conflicts with an authored entity."));
        }
        FAkUGCCommand Command;
        Command.CommandId = FGuid::NewDeterministicGuid(TEXT("akugc-bot-command-") + Slot->EntityId.ToString(EGuidFormats::Digits));
        Command.SceneId = Scene.SceneId;
        Command.Type = EAkUGCCommandType::AddEntity;
        Command.EntityId = TowerId;
        if (!Registry.CreateEntityRecord(TEXT("official.tower.basic"), TowerId, Slot->Transform, Command.Entity, OutError)) { return false; }
        Command.Entity.Tags.Add(TEXT("akugc.bot"));
        Transaction.Commands.Add(Command);
        PlannedPositions.Add(Slot->Transform.GetLocation());
        Resources -= TowerCost;
        ++Count;
    }
    FAkUGCCommand Settings;
    Settings.CommandId = FGuid::NewDeterministicGuid(TEXT("akugc-bot-resources-command"));
    Settings.SceneId = Scene.SceneId;
    Settings.Type = EAkUGCCommandType::SetRulesetSettings;
    Settings.InitialResources = Resources;
    Settings.WaveIntervalSeconds = Scene.Ruleset.WaveIntervalSeconds;
    Settings.DefeatCondition = Scene.Ruleset.DefeatCondition;
    Settings.VictoryCondition = Scene.Ruleset.VictoryCondition;
    Transaction.Commands.Add(Settings);
    FAkUGCProjectDocument Candidate = Document;
    const auto Result = FAkUGCCommandExecutor::Apply(Candidate, Transaction);
    if (!Result.bSucceeded) { return FailBot(TEXT("Bot command transaction is invalid.")); }
    if (!FAkUGCTowerDefenseTemplate::ValidateDocumentBudgets(Candidate, Registry, OutError)) { return false; }
    OutTransaction = MoveTemp(Transaction);
    OutTowerCount = Count;
    return true;
}
