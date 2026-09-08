#include "Pack/AkUGCLogicPackLoader.h"

#include "Document/AkUGCDocument.h"
#include "Logic/AkUGCLogicCompiler.h"
#include "Pack/AkUGCLogicPackBuilder.h"
#include "Pack/AkUGCLogicPackCodec.h"
#include "Pack/AkUGCLogicPackHasher.h"
#include "Pack/AkUGCLogicPackSignature.h"
#include "Validation/AkUGCCapabilityValidator.h"
#include "Validation/AkUGCDocumentValidator.h"

FAkUGCLogicPackLoadResult FAkUGCLogicPackLoader::Load(const FString& Json)
{
	FAkUGCLogicPackLoadResult Result;

	// 1. 反序列化。
	FString Error;
	if (!FAkUGCLogicPackCodec::Deserialize(Json, Result.Pack, &Error))
	{
		Result.ErrorMessage = MoveTemp(Error);
		return Result;
	}

	// 2. Schema 版本兼容（发布物不做迁移，不兼容直接拒绝）。
	if (Result.Pack.Manifest.SchemaVersion != AkUGCSchema::CurrentProjectDocumentVersion)
	{
		Result.ErrorMessage = FString::Printf(
			TEXT("Logic Pack 版本不兼容：清单声明 Schema %d，运行端支持 %d。"),
			Result.Pack.Manifest.SchemaVersion,
			AkUGCSchema::CurrentProjectDocumentVersion);
		return Result;
	}

	// 2.5 能力白名单校验（Client 与 Server 重复执行，拒绝越权/未知能力）。
	FString CapabilityError;
	if (!FAkUGCCapabilityValidator::Validate(Result.Pack.Manifest.Capabilities, &CapabilityError))
	{
		Result.ErrorMessage = MoveTemp(CapabilityError);
		return Result;
	}

	// 3. 内容确定性哈希一致（拒绝被修改的包）。
	const FString ExpectedHash = FAkUGCLogicPackBuilder::ComputeContentHash(
		Result.Pack.Document,
		Result.Pack.Program);
	if (ExpectedHash != Result.Pack.Manifest.ContentHash)
	{
		Result.ErrorMessage = TEXT("Logic Pack 完整性校验失败：内容哈希不匹配，包可能已被修改。");
		return Result;
	}

	// 4. 文档合法性校验。
	const FAkUGCValidationResult Validation = FAkUGCDocumentValidator::Validate(Result.Pack.Document);
	if (!Validation.IsValid())
	{
		const FAkUGCValidationIssue* FirstError = Validation.Issues.FindByPredicate(
			[](const FAkUGCValidationIssue& Issue)
			{
				return Issue.Severity == EAkUGCValidationSeverity::Error;
			});
		Result.ErrorMessage = FirstError
			? FString::Printf(TEXT("%s: %s"), *FirstError->Path, *FirstError->Message)
			: TEXT("Logic Pack 加载失败：作品文档校验未通过。");
		return Result;
	}

	// 5. Logic IR 与文档重新编译结果一致（拒绝文档与 IR 不匹配的包）。
	if (Result.Pack.Document.Scenes.Num() != 1)
	{
		Result.ErrorMessage = FString::Printf(
			TEXT("Logic Pack 加载失败：当前仅支持单场景作品（实际 %d 个场景）。"),
			Result.Pack.Document.Scenes.Num());
		return Result;
	}
	const FAkUGCLogicCompileResult Compile = FAkUGCLogicCompiler::Compile(Result.Pack.Document.Scenes[0].LogicGraph);
	if (!Compile.bSucceeded)
	{
		Result.ErrorMessage = Compile.ErrorPath.IsEmpty()
			? Compile.ErrorMessage
			: FString::Printf(TEXT("%s: %s"), *Compile.ErrorPath, *Compile.ErrorMessage);
		return Result;
	}
	if (FAkUGCLogicPackHasher::HashLogicProgram(Compile.Program)
		!= FAkUGCLogicPackHasher::HashLogicProgram(Result.Pack.Program))
	{
		Result.ErrorMessage = TEXT("Logic Pack 一致性校验失败：Logic IR 与作品文档不匹配。");
		return Result;
	}

	Result.bSucceeded = true;
	return Result;
}

FAkUGCLogicPackLoadResult FAkUGCLogicPackLoader::LoadVerified(const FString& Json, const FString& TrustedPublicKeyHex)
{
	// 先执行完整性与一致性校验，再验签。
	FAkUGCLogicPackLoadResult Result = Load(Json);
	if (!Result.bSucceeded)
	{
		return Result;
	}

	FString VerifyError;
	if (!FAkUGCLogicPackVerifier::Verify(Result.Pack.Manifest, Result.Pack.Signature, TrustedPublicKeyHex, &VerifyError))
	{
		Result.bSucceeded = false;
		Result.ErrorMessage = MoveTemp(VerifyError);
	}
	return Result;
}
