#include "AkUGCPackToolCommandlet.h"

#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "AkUGCPlayableSceneFactory.h"
#include "Pack/AkUGCLogicPack.h"
#include "Pack/AkUGCLogicPackBuilder.h"
#include "Pack/AkUGCLogicPackCodec.h"
#include "Pack/AkUGCLogicPackSignature.h"

int32 UAkUGCPackToolCommandlet::Main(const FString& Params)
{
    FString OutputPath;
    if (!FParse::Value(*Params, TEXT("Output="), OutputPath))
    {
        UE_LOG(LogTemp, Error, TEXT("[AkUGCPackTool] Missing -Output=<path>."));
        return 1;
    }

    FString PublicKeyPath;
    FParse::Value(*Params, TEXT("PublicKey="), PublicKeyPath);

    // 1. 构造完整塔防场景。
    FAkUGCProjectDocument Document;
    FString Error;
    if (!AkUGCPlayableSceneFactory::MakePlayableTowerDefenseDocument(Document, &Error))
    {
        UE_LOG(LogTemp, Error, TEXT("[AkUGCPackTool] Scene construction failed: %s"), *Error);
        return 2;
    }

    // 2. 生成密钥对。
    FAkUGCLogicPackKeyPair KeyPair;
    if (!FAkUGCLogicPackSigner::GenerateKeyPair(KeyPair, &Error))
    {
        UE_LOG(LogTemp, Error, TEXT("[AkUGCPackTool] Key generation failed: %s"), *Error);
        return 3;
    }

    // 3. 构建发布包。
    const FAkUGCLogicPackBuildResult Build = FAkUGCLogicPackBuilder::Build(Document);
    if (!Build.bSucceeded)
    {
        UE_LOG(LogTemp, Error, TEXT("[AkUGCPackTool] Build failed: %s"), *Build.ErrorMessage);
        return 4;
    }

    // 4. 签名。
    FAkUGCLogicPack Pack = Build.Pack;
    if (!FAkUGCLogicPackSigner::Sign(Pack.Manifest, KeyPair.PrivateKey, Pack.Signature, &Error))
    {
        UE_LOG(LogTemp, Error, TEXT("[AkUGCPackTool] Sign failed: %s"), *Error);
        return 5;
    }

    // 5. 序列化并写盘。
    FString Json;
    if (!FAkUGCLogicPackCodec::Serialize(Pack, Json, &Error))
    {
        UE_LOG(LogTemp, Error, TEXT("[AkUGCPackTool] Serialize failed: %s"), *Error);
        return 6;
    }

    if (!FFileHelper::SaveStringToFile(Json, *OutputPath))
    {
        UE_LOG(LogTemp, Error, TEXT("[AkUGCPackTool] Failed to write %s"), *OutputPath);
        return 7;
    }
    UE_LOG(LogTemp, Log, TEXT("[AkUGCPackTool] Wrote pack: %s"), *OutputPath);

    // 6. 写出可信公钥（供 DS 端 -LogicPackKey= 使用）。
    if (!PublicKeyPath.IsEmpty())
    {
        if (!FFileHelper::SaveStringToFile(KeyPair.PublicKey, *PublicKeyPath))
        {
            UE_LOG(LogTemp, Error, TEXT("[AkUGCPackTool] Failed to write public key %s"), *PublicKeyPath);
            return 8;
        }
        UE_LOG(LogTemp, Log, TEXT("[AkUGCPackTool] Wrote public key: %s"), *PublicKeyPath);
    }
    else
    {
        UE_LOG(LogTemp, Log, TEXT("[AkUGCPackTool] Public key: %s"), *KeyPair.PublicKey);
    }

    return 0;
}
