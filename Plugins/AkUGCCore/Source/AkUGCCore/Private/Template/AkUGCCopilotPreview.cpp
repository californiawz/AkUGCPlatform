#include "Template/AkUGCCopilotPreview.h"

#include "Command/AkUGCCommandExecutor.h"
#include "Pack/AkUGCLogicPackBuilder.h"
#include "Pack/AkUGCLogicPackHasher.h"
#include "Prefab/AkUGCOfficialPrefabCatalog.h"
#include "Prefab/AkUGCPrefabRegistry.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Validation/AkUGCInputBudget.h"

namespace AkUGCCopilotGraph
{
    bool Fail(FString* Error, const FString& Message)
    { if (Error) { *Error = Message; } return false; }

    bool Exact(const TSharedPtr<FJsonObject>& Object, const TSet<FString>& Fields)
    {
        if (!Object.IsValid() || Object->Values.Num() != Fields.Num()) { return false; }
        for (const FString& Field : Fields) { if (!Object->HasField(Field)) { return false; } }
        return true;
    }

    bool Number(const TSharedPtr<FJsonObject>& Object, const TCHAR* Name, double Minimum, double Maximum, double& Value)
    {
        const auto Field = Object->Values.Find(Name);
        return Field && Field->IsValid() && (*Field)->Type == EJson::Number && (*Field)->TryGetNumber(Value)
            && FMath::IsFinite(Value) && Value >= Minimum && Value <= Maximum;
    }

    bool String(const TSharedPtr<FJsonObject>& Object, const TCHAR* Name, FString& Value)
    {
        const auto Field = Object->Values.Find(Name);
        return Field && Field->IsValid() && (*Field)->Type == EJson::String && (*Field)->TryGetString(Value);
    }

    bool Append(const FAkUGCProjectDocument& Document, const TSharedPtr<FJsonObject>& Graph,
        FAkUGCCommandTransaction& Transaction, FString& Diff, FString* Error)
    {
        const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
        const TArray<TSharedPtr<FJsonValue>>* Connections = nullptr;
        if (!Exact(Graph, {TEXT("nodes"), TEXT("connections")})
            || !Graph->TryGetArrayField(TEXT("nodes"), Nodes) || !Graph->TryGetArrayField(TEXT("connections"), Connections)
            || Nodes->Num() > 32 || Connections->Num() > 64)
        { return Fail(Error, TEXT("logic: expected nodes <=32 and connections <=64, no extra fields.")); }
        const auto& Scene = Document.Scenes[0];
        TMap<FString, FGuid> Ids;
        TSet<FGuid> EntryIds;
        for (const auto& Node : Scene.LogicGraph.Nodes)
        {
            const FString Id = Node.NodeId.ToString(EGuidFormats::Digits);
            Ids.Add(Id, Node.NodeId);
            if (Node.Type == EAkUGCLogicNodeType::GameStart || Node.Type == EAkUGCLogicNodeType::WaveStart) { EntryIds.Add(Node.NodeId); }
        }
        const auto AddCommand = [&](EAkUGCCommandType Type) -> FAkUGCCommand&
        {
            auto& Command = Transaction.Commands.AddDefaulted_GetRef();
            Command.CommandId = FGuid::NewGuid(); Command.SceneId = Scene.SceneId; Command.Type = Type;
            return Command;
        };
        TSet<FGuid> AddedIds;
        for (const auto& Value : *Nodes)
        {
            const TSharedPtr<FJsonObject>* Object = nullptr;
            FString Id, Type;
            double X = 0, Y = 0;
            if (!Value.IsValid() || !Value->TryGetObject(Object) || !Object || !Object->IsValid()
                || !String(*Object, TEXT("id"), Id) || !String(*Object, TEXT("type"), Type)
                || Id.IsEmpty() || Id.Len() > 32 || Ids.Contains(Id)
                || !Number(*Object, TEXT("x"), -100000, 100000, X) || !Number(*Object, TEXT("y"), -100000, 100000, Y))
            { return Fail(Error, TEXT("logic.nodes: invalid or duplicate id/type/position.")); }
            for (TCHAR Character : Id)
            { if (!((Character >= TEXT('a') && Character <= TEXT('z')) || (Character >= TEXT('A') && Character <= TEXT('Z'))
                || (Character >= TEXT('0') && Character <= TEXT('9')) || Character == TEXT('_')))
                { return Fail(Error, TEXT("logic.nodes.id: only ASCII letters, digits and underscore allowed.")); } }
            FAkUGCLogicNode Node;
            Node.NodeId = FGuid::NewGuid(); Node.PositionX = static_cast<float>(X); Node.PositionY = static_cast<float>(Y);
            if (Type == TEXT("Message"))
            {
                if (!Exact(*Object, {TEXT("id"), TEXT("type"), TEXT("x"), TEXT("y"), TEXT("message")})
                    || !String(*Object, TEXT("message"), Node.Message) || Node.Message.Len() > AkUGCLogicLimits::MaxMessageLength)
                { return Fail(Error, TEXT("logic.nodes.message: invalid fields or message exceeds 1024 characters.")); }
                Node.Type = EAkUGCLogicNodeType::Message;
            }
            else if (Type == TEXT("Timer"))
            {
                if (!Exact(*Object, {TEXT("id"), TEXT("type"), TEXT("x"), TEXT("y"), TEXT("delaySeconds")})
                    || !Number(*Object, TEXT("delaySeconds"), 0, 3600, Node.DelaySeconds))
                { return Fail(Error, TEXT("logic.nodes.delaySeconds: expected finite number 0..3600.")); }
                Node.Type = EAkUGCLogicNodeType::Timer;
            }
            else { return Fail(Error, TEXT("logic.nodes.type: only Message and Timer may be added; Spawn/scripts/event replacement are forbidden.")); }
            Ids.Add(Id, Node.NodeId); AddedIds.Add(Node.NodeId);
            AddCommand(EAkUGCCommandType::AddLogicNode).LogicNode = Node;
            Diff += FString::Printf(TEXT("\n+ 节点 %s [%s] (%.0f, %.0f)%s"), *Id, *Type, X, Y,
                Type == TEXT("Message") ? *(TEXT(" 消息：") + Node.Message) : *FString::Printf(TEXT(" 延时：%g秒"), Node.DelaySeconds));
        }
        TSet<FString> Edges;
        TMultiMap<FGuid, FGuid> Targets;
        for (const auto& Value : *Connections)
        {
            const TSharedPtr<FJsonObject>* Object = nullptr;
            FString Source, Target;
            if (!Value.IsValid() || !Value->TryGetObject(Object) || !Object || !Exact(*Object, {TEXT("source"), TEXT("target")})
                || !String(*Object, TEXT("source"), Source) || !String(*Object, TEXT("target"), Target)
                || !Ids.Contains(Source) || !Ids.Contains(Target) || !AddedIds.Contains(Ids.FindChecked(Target))
                || (!AddedIds.Contains(Ids.FindChecked(Source)) && !EntryIds.Contains(Ids.FindChecked(Source)))
                || Source == Target || Edges.Contains(Source + TEXT(">") + Target))
            { return Fail(Error, TEXT("logic.connections: invalid reference/duplicate/self edge; connect existing entry or added node to added node only.")); }
            Edges.Add(Source + TEXT(">") + Target);
            auto& Connection = AddCommand(EAkUGCCommandType::ConnectLogicNode).LogicConnection;
            Connection.SourceNodeId = Ids.FindChecked(Source); Connection.TargetNodeId = Ids.FindChecked(Target);
            Targets.Add(Connection.SourceNodeId, Connection.TargetNodeId);
            Diff += TEXT("\n+ 连接 ") + Source + TEXT(" -> ") + Target;
        }
        TArray<FGuid> Queue = EntryIds.Array();
        TSet<FGuid> Reached = EntryIds;
        for (int32 Index = 0; Index < Queue.Num(); ++Index)
        {
            TArray<FGuid> Next;
            Targets.MultiFind(Queue[Index], Next);
            for (const auto& Id : Next) { if (!Reached.Contains(Id)) { Reached.Add(Id); Queue.Add(Id); } }
        }
        for (const auto& Id : AddedIds)
        { if (!Reached.Contains(Id)) { return Fail(Error, TEXT("logic.nodes: every new node must be reachable from an existing event entry.")); } }
        return true;
    }
}

bool FAkUGCCopilotValidation::Preview(const FAkUGCProjectDocument& Document, const FString& ParameterJson,
    FAkUGCCopilotPreview& OutPreview, FString* OutError)
{
    OutPreview = {};
    if (Document.Scenes.Num() != 1)
    {
        if (OutError) { *OutError = TEXT("Copilot requires a single scene."); }
        return false;
    }
    if (!AkUGCInputBudget::ValidateJson(
            ParameterJson,
            AkUGCInputBudget::MaxCopilotProposalBytes,
            TEXT("Copilot proposal"),
            OutError))
    {
        return false;
    }
    TSharedPtr<FJsonObject> Root;
    double Version = 0;
    if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(ParameterJson), Root) || !Root.IsValid()
        || !AkUGCCopilotGraph::Number(Root, TEXT("schemaVersion"), 1, 1, Version))
    { if (OutError) { *OutError = TEXT("Copilot response must be a JSON object with schemaVersion 1."); } return false; }
    FString ParametersJson = ParameterJson;
    TSharedPtr<FJsonObject> Graph;
    if (Root->HasField(TEXT("parameters")))
    {
        const TSharedPtr<FJsonObject>* ParametersObject = nullptr;
        const TSharedPtr<FJsonObject>* GraphObject = nullptr;
        if (!AkUGCCopilotGraph::Exact(Root, {TEXT("schemaVersion"), TEXT("parameters"), TEXT("logic")})
            || !Root->TryGetObjectField(TEXT("parameters"), ParametersObject) || !ParametersObject->IsValid()
            || !Root->TryGetObjectField(TEXT("logic"), GraphObject) || !GraphObject->IsValid()
            || !FJsonSerializer::Serialize(ParametersObject->ToSharedRef(), TJsonWriterFactory<>::Create(&ParametersJson)))
        { return AkUGCCopilotGraph::Fail(OutError, TEXT("proposal: expected schemaVersion, parameters object and logic object only.")); }
        double ParameterVersion = 0;
        if (!AkUGCCopilotGraph::Number(*ParametersObject, TEXT("schemaVersion"), 1, 1, ParameterVersion))
        { return AkUGCCopilotGraph::Fail(OutError, TEXT("parameters.schemaVersion must be 1.")); }
        Graph = *GraphObject;
    }
    FAkUGCCopilotPreview Result;
    FAkUGCTowerDefenseParameters Before;
    FAkUGCPrefabRegistry Registry;
    FAkUGCCommandTransaction Transaction;
    if (!FAkUGCOfficialPrefabCatalog::RegisterTowerDefense(Registry, OutError)
        || !FAkUGCTowerDefenseTemplate::ReadParameters(Document, Document.Scenes[0].SceneId, Before, OutError)
        || !FAkUGCTowerDefenseTemplate::DeserializeParameters(ParametersJson, Result.Parameters, OutError)) { return false; }
    if (Result.Parameters.SchemaVersion != 1)
    { if (OutError) { *OutError = TEXT("Copilot parameter schemaVersion must be 1."); } return false; }
    if (!FAkUGCTowerDefenseTemplate::BuildTransaction(Document, Document.Scenes[0].SceneId, Result.Parameters, Registry, Transaction, OutError)) { return false; }
    FString GraphDiff;
    if (Graph.IsValid() && !AkUGCCopilotGraph::Append(Document, Graph, Transaction, GraphDiff, OutError)) { return false; }
    FAkUGCProjectDocument Candidate = Document;
    const auto Applied = FAkUGCCommandExecutor::Apply(Candidate, Transaction);
    if (!Applied.bSucceeded) { if (OutError) { *OutError = Applied.ErrorPath + TEXT(": ") + Applied.ErrorMessage; } return false; }
    if (!FAkUGCTowerDefenseTemplate::ValidateDocumentBudgets(Candidate, Registry, OutError)) { return false; }
    const auto Build = FAkUGCLogicPackBuilder::Build(Candidate);
    if (!Build.bSucceeded) { if (OutError) { *OutError = Build.ErrorMessage; } return false; }
    Result.BaseDocumentHash = FAkUGCLogicPackHasher::HashDocument(Document);
    FString OldJson, NewJson;
    if (!FAkUGCTowerDefenseTemplate::SerializeParameters(Before, OldJson, OutError)
        || !FAkUGCTowerDefenseTemplate::SerializeParameters(Result.Parameters, NewJson, OutError)) { return false; }
    Result.Diff = TEXT("当前参数：\n") + OldJson + TEXT("\n\n提议参数（尚未应用）：\n") + NewJson
        + TEXT("\n\nLogic增量（不删除或覆盖现有图）：") + (GraphDiff.IsEmpty() ? TEXT("无") : GraphDiff);
    Result.ProposalJson = ParameterJson;
    Result.ProposalHash = FAkUGCLogicPackHasher::Sha256Hex(ParameterJson);
    Transaction.Label = TEXT("Apply Copilot validated proposal");
    Result.Transaction = MoveTemp(Transaction);
    Result.bValidated = true;
    OutPreview = MoveTemp(Result);
    return true;
}

bool FAkUGCCopilotValidation::IsCurrent(const FAkUGCProjectDocument& Document, const FAkUGCCopilotPreview& Preview)
{
    return Preview.bValidated && !Preview.BaseDocumentHash.IsEmpty()
        && Preview.BaseDocumentHash == FAkUGCLogicPackHasher::HashDocument(Document)
        && !Preview.ProposalHash.IsEmpty() && Preview.ProposalHash == FAkUGCLogicPackHasher::Sha256Hex(Preview.ProposalJson);
}

FString FAkUGCCopilotValidation::ExplainError(const FString& Error)
{
    FString Advice = TEXT("检查字段名称、数值类型和范围；返回当前schemaVersion=1的完整参数或受控组合对象，不包含Markdown、脚本或额外指令。");
    if (Error.Contains(TEXT("cycle"))) { Advice = TEXT("Logic存在循环。删除回连，使事件 -> Timer -> Message单向执行；不能将节点连回上游。"); }
    else if (Error.Contains(TEXT("reference")) || Error.Contains(TEXT("reachable")))
    { Advice = TEXT("Logic引用或可达性错误。source使用提供的现有事件ID或新增节点id，target只能指向新增节点；所有新节点都必须从事件可达。"); }
    else if (Error.Contains(TEXT("Terminal")) || Error.Contains(TEXT("outgoing")))
    { Advice = TEXT("Message是终端节点，不能有出边；Timer最多一条出边。需要多个消息时从事件分别建立支路。"); }
    else if (Error.Contains(TEXT("budget")) || Error.Contains(TEXT("exceeds")))
    { Advice = TEXT("提案超过预算。减少波次敌人数、节点/连线数量或消息长度；不能绕过平台上限。"); }
    else if (Error.Contains(TEXT("schemaVersion"))) { Advice = TEXT("使用schemaVersion=1；旧格式不会为模型响应自动迁移。"); }
    else if (Error.Contains(TEXT("stale")) || Error.Contains(TEXT("过期"))) { Advice = TEXT("项目在生成期间已变化。保留当前编辑结果，重新生成预览，不覆盖旧修订。"); }
    return TEXT("提案未应用，当前项目保持不变。\n原始校验：") + Error + TEXT("\n建议：") + Advice
        + TEXT("\n这是基于校验类别的本地说明，不是模型解释；建议不能绕过编译和预算校验。");
}
