#include "Document/AkUGCDocumentJson.h"

#include "Document/AkUGCDocument.h"
#include "JsonObjectConverter.h"

bool FAkUGCDocumentJson::Serialize(
    const FAkUGCProjectDocument& Document,
    FString& OutJson,
    FString* OutError)
{
    OutJson.Reset();
    const bool bSucceeded = FJsonObjectConverter::UStructToJsonObjectString(Document, OutJson);
    if (!bSucceeded && OutError)
    {
        *OutError = TEXT("Failed to serialize UGC project document.");
    }
    return bSucceeded;
}

bool FAkUGCDocumentJson::Deserialize(
    const FString& Json,
    FAkUGCProjectDocument& OutDocument,
    FString* OutError)
{
    OutDocument = FAkUGCProjectDocument{};
    const bool bSucceeded = FJsonObjectConverter::JsonObjectStringToUStruct(Json, &OutDocument);
    if (!bSucceeded && OutError)
    {
        *OutError = TEXT("Failed to deserialize UGC project document.");
    }
    return bSucceeded;
}
