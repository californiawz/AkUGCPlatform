#include "AkUGCPackToolCommandlet.h"

#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "AkUGCPlayableSceneFactory.h"
#include "Pack/AkUGCLogicPack.h"
#include "Pack/AkUGCLogicPackBuilder.h"
#include "Pack/AkUGCLogicPackCodec.h"
#include "Pack/AkUGCLogicPackSignature.h"
#include "Pack/AkUGCLogicPackHasher.h"
#include "Pack/AkUGCLogicPackLoader.h"
#include "Pack/AkUGCWorkLibrary.h"
#include "Misc/Paths.h"
#include "Command/AkUGCCommandExecutor.h"
#include "Prefab/AkUGCOfficialPrefabCatalog.h"
#include "Prefab/AkUGCPrefabRegistry.h"
#include "Template/AkUGCTowerDefenseTemplate.h"

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
    if (FParse::Param(*Params, TEXT("NetworkVisualFixture")))
    {
        FAkUGCPrefabRegistry Registry;
        FAkUGCTowerDefenseParameters Parameters;
        Parameters.Waves.SetNum(1);
        Parameters.Waves[0].EnemyCount = 1;
        Parameters.Waves[0].StartDelaySeconds = 20.0;
        Parameters.Waves[0].EnemyMoveSpeed = 50.0;
        Parameters.Waves[0].EnemyGoalDamage = 7.0;
        Parameters.InitialResources = 321;
        if (FParse::Param(*Params, TEXT("RoomFixture")))
        {
            int32 Seed = 0;
            FParse::Value(*Params, TEXT("Seed="), Seed);
            Parameters.Waves[0].EnemyCount = 1 + FMath::Clamp(Seed, 0, 9) % 3;
            Parameters.Waves[0].StartDelaySeconds = 8.0;
            Parameters.Waves[0].EnemyMoveSpeed = 100.0 + FMath::Clamp(Seed, 0, 9) * 5.0;
        }
        FAkUGCCommandTransaction Transaction;
        if (!FAkUGCOfficialPrefabCatalog::RegisterTowerDefense(Registry, &Error)
            || !FAkUGCTowerDefenseTemplate::CreateDefaultDocument(Registry, Document, &Error)
            || !FAkUGCTowerDefenseTemplate::BuildTransaction(Document, Document.Scenes[0].SceneId,
                Parameters, Registry, Transaction, &Error)
            || !FAkUGCCommandExecutor::Apply(Document, Transaction).bSucceeded)
        {
            UE_LOG(LogTemp, Error, TEXT("[AkUGCPackTool] Network fixture failed: %s"), *Error);
            return 2;
        }
        Document.Manifest.DisplayName = TEXT("Network Visual Verification");
        for (FAkUGCEntityRecord& Entity : Document.Scenes[0].Entities)
        {
            if (Entity.PrefabId != TEXT("official.tower.basic")) { continue; }
            for (FAkUGCComponentRecord& Component : Entity.Components)
            {
                if (Component.TypeId == TEXT("tower_defense.tower"))
                {
                    Component.Properties.FindChecked(TEXT("attackDamage")).NumberValue = 0.0;
                }
            }
        }
    }
    else if (FParse::Param(*Params, TEXT("PublishWork")))
    {
        FAkUGCPrefabRegistry Registry;
        if (!FAkUGCOfficialPrefabCatalog::RegisterTowerDefense(Registry, &Error)
            || !FAkUGCTowerDefenseTemplate::CreateDefaultDocument(Registry, Document, &Error))
        { UE_LOG(LogTemp, Error, TEXT("[AkUGCPackTool] Template failed: %s"), *Error); return 2; }
    }
    else if (!AkUGCPlayableSceneFactory::MakePlayableTowerDefenseDocument(Document, &Error))
    {
        UE_LOG(LogTemp, Error, TEXT("[AkUGCPackTool] Scene construction failed: %s"), *Error);
        return 2;
    }

    if (FParse::Param(*Params, TEXT("PublishWork")))
    {
        FString Directory, PrivateKey, Share;
        if (!FParse::Value(*Params, TEXT("Library="), Directory)) { return 10; }
        FAkUGCWorkEntry Entry;
        if (!FAkUGCWorkLibrary::GetOrCreatePublisherKey(FPaths::Combine(Directory, TEXT("Publisher/private.key")), PrivateKey, &Error)
            || !FAkUGCWorkLibrary(Directory).Publish(Document, PrivateKey, FPaths::Combine(Directory, TEXT("Share")), Entry, Share, &Error))
        { UE_LOG(LogTemp, Error, TEXT("[AkUGCPackTool] Publish failed: %s"), *Error); return 11; }
        PrivateKey.Reset();
        const FString Result = FString::Printf(TEXT("{\"releaseId\":\"%s\",\"sharePath\":\"%s\",\"publicKey\":\"%s\"}"),
            *Entry.ReleaseId.ToString(EGuidFormats::Digits), *Share.Replace(TEXT("\\"), TEXT("/")), *Entry.TrustedPublicKey);
        if (!FFileHelper::SaveStringToFile(Result, *OutputPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) { return 12; }
        UE_LOG(LogTemp, Display, TEXT("[AkUGCPackTool] Published Work=%s"), *Entry.ReleaseId.ToString(EGuidFormats::Digits));
        return 0;
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

    const FAkUGCLogicPackLoadResult Verification = FAkUGCLogicPackLoader::LoadVerified(Json, KeyPair.PublicKey);
    if (!Verification.bSucceeded)
    {
        FString OriginalCanonical, RestoredCanonical;
        FAkUGCLogicPackHasher::DeterministicSerialize(Pack.Document, OriginalCanonical);
        FAkUGCLogicPackHasher::DeterministicSerialize(Verification.Pack.Document, RestoredCanonical);
        FFileHelper::SaveStringToFile(OriginalCanonical, *(OutputPath + TEXT(".original.json")));
        FFileHelper::SaveStringToFile(RestoredCanonical, *(OutputPath + TEXT(".restored.json")));
        UE_LOG(LogTemp, Error, TEXT("[AkUGCPackTool] Serialized pack verification failed: %s"), *Verification.ErrorMessage);
        return 9;
    }

    if (!FFileHelper::SaveStringToFile(Json, *OutputPath))
    {
        UE_LOG(LogTemp, Error, TEXT("[AkUGCPackTool] Failed to write %s"), *OutputPath);
        return 7;
    }
    UE_LOG(LogTemp, Log, TEXT("[AkUGCPackTool] Wrote pack: %s"), *OutputPath);
    if (FParse::Param(*Params, TEXT("NetworkVisualFixture")))
    {
        FString Canonical;
        FAkUGCLogicPackHasher::DeterministicSerialize(Pack.Document, Canonical);
        FFileHelper::SaveStringToFile(Canonical, *(OutputPath + TEXT(".canonical.json")));
    }

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
