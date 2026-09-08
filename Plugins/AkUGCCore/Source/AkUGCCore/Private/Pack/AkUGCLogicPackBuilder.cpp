#include "Pack/AkUGCLogicPackBuilder.h"

#include "Logic/AkUGCLogicCompiler.h"
#include "Pack/AkUGCLogicPackHasher.h"
#include "Validation/AkUGCDocumentValidator.h"

namespace
{
	/**
	 * 收集作品文档中引用的全部 PrefabId（实体 + Spawn 节点），排序去重。
	 * 作为发布包的资产依赖声明（AssetDependencies）。
	 */
	TArray<FString> CollectAssetDependencies(const FAkUGCProjectDocument& Document)
	{
		TSet<FName> PrefabIds;
		for (const FAkUGCSceneDocument& Scene : Document.Scenes)
		{
			for (const FAkUGCEntityRecord& Entity : Scene.Entities)
			{
				if (!Entity.PrefabId.IsNone())
				{
					PrefabIds.Add(Entity.PrefabId);
				}
			}
			for (const FAkUGCLogicNode& Node : Scene.LogicGraph.Nodes)
			{
				if (Node.Type == EAkUGCLogicNodeType::Spawn && !Node.SpawnPrefabId.IsNone())
				{
					PrefabIds.Add(Node.SpawnPrefabId);
				}
			}
		}

		TArray<FString> Dependencies;
		Dependencies.Reserve(PrefabIds.Num());
		for (const FName& PrefabId : PrefabIds)
		{
			Dependencies.Add(PrefabId.ToString());
		}
		Dependencies.Sort();
		return Dependencies;
	}
}

FString FAkUGCLogicPackBuilder::ComputeContentHash(
	const FAkUGCProjectDocument& Document,
	const FAkUGCLogicProgram& Program)
{
	const FString DocumentHash = FAkUGCLogicPackHasher::HashDocument(Document);
	const FString ProgramHash = FAkUGCLogicPackHasher::HashLogicProgram(Program);
	return FAkUGCLogicPackHasher::Sha256Hex(DocumentHash + TEXT(":") + ProgramHash);
}

FAkUGCLogicPackBuildResult FAkUGCLogicPackBuilder::Build(const FAkUGCProjectDocument& Document)
{
	FAkUGCLogicPackBuildResult Result;

	// 1. 文档合法性校验。
	const FAkUGCValidationResult Validation = FAkUGCDocumentValidator::Validate(Document);
	if (!Validation.IsValid())
	{
		const FAkUGCValidationIssue* FirstError = Validation.Issues.FindByPredicate(
			[](const FAkUGCValidationIssue& Issue)
			{
				return Issue.Severity == EAkUGCValidationSeverity::Error;
			});
		Result.ErrorMessage = FirstError
			? FString::Printf(TEXT("%s: %s"), *FirstError->Path, *FirstError->Message)
			: TEXT("Logic Pack 构建失败：作品文档校验未通过。");
		return Result;
	}

	// 2. 当前切片要求恰好一个主场景。
	if (Document.Scenes.Num() != 1)
	{
		Result.ErrorMessage = FString::Printf(
			TEXT("Logic Pack 构建失败：当前仅支持单场景作品（实际 %d 个场景）。"),
			Document.Scenes.Num());
		return Result;
	}

	// 3. 编译主场景 Logic Graph。
	const FAkUGCLogicCompileResult Compile = FAkUGCLogicCompiler::Compile(Document.Scenes[0].LogicGraph);
	if (!Compile.bSucceeded)
	{
		Result.ErrorMessage = Compile.ErrorPath.IsEmpty()
			? Compile.ErrorMessage
			: FString::Printf(TEXT("%s: %s"), *Compile.ErrorPath, *Compile.ErrorMessage);
		return Result;
	}

	// 4. 组装发布包。
	FAkUGCLogicPack Pack;
	Pack.Document = Document;
	Pack.Program = Compile.Program;

	Pack.Manifest.ReleaseId = FGuid::NewGuid();
	Pack.Manifest.SchemaVersion = Document.Manifest.SchemaVersion;
	Pack.Manifest.ProjectId = Document.Manifest.ProjectId;
	Pack.Manifest.TemplateId = Document.Manifest.TemplateId;
	Pack.Manifest.Capabilities = Document.Manifest.Capabilities;
	Pack.Manifest.AssetDependencies = CollectAssetDependencies(Document);
	Pack.Manifest.ContentHash = ComputeContentHash(Document, Compile.Program);

	Result.bSucceeded = true;
	Result.Pack = MoveTemp(Pack);
	return Result;
}
