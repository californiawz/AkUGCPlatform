#include "Pack/AkUGCLogicPackHasher.h"

#include "Containers/StringConv.h"
#include "HAL/PlatformMemory.h"
#include "Misc/Guid.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"

namespace
{
    /** SHA-256 的 64 个轮常量（FIPS 180-4）。 */
    static const uint32 G_Sha256K[64] =
    {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
    };

    /** SHA-256 初始哈希值（FIPS 180-4）。 */
    static const uint32 G_Sha256InitH[8] =
    {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
    };

    uint32 Sha256RotateRight(uint32 Value, int32 Shift)
    {
        return (Value >> Shift) | (Value << (32 - Shift));
    }

    /**
     * 计算 SHA-256 摘要，返回 64 个字符的小写十六进制字符串。
     * 纯 C++ 实现，跨平台确定（不依赖任何平台 API）。
     */
    FString Sha256HexDigest(const uint8* Input, int32 InputLength)
    {
        uint32 H[8];
        for (int32 i = 0; i < 8; ++i)
        {
            H[i] = G_Sha256InitH[i];
        }

        // 消息填充：追加 0x80、若干 0x00，最后 8 字节写入 64 位大端消息长度（单位：bit）。
        const uint64 MessageBits = static_cast<uint64>(InputLength) * 8ULL;
        const int32 Remaining = InputLength % 64;
        const int32 PadLength = (Remaining < 56) ? (56 - Remaining) : (120 - Remaining);
        const int32 TotalLength = InputLength + PadLength + 8;

        TArray<uint8> Message;
        Message.SetNumUninitialized(TotalLength);
        FMemory::Memcpy(Message.GetData(), Input, InputLength);
        Message[InputLength] = 0x80;
        FMemory::Memzero(Message.GetData() + InputLength + 1, PadLength - 1);
        for (int32 i = 0; i < 8; ++i)
        {
            Message[TotalLength - 1 - i] = static_cast<uint8>(MessageBits >> (i * 8));
        }

        for (int32 Offset = 0; Offset < TotalLength; Offset += 64)
        {
            uint32 W[64];
            for (int32 t = 0; t < 16; ++t)
            {
                const int32 p = Offset + t * 4;
                W[t] = (static_cast<uint32>(Message[p]) << 24)
                     | (static_cast<uint32>(Message[p + 1]) << 16)
                     | (static_cast<uint32>(Message[p + 2]) << 8)
                     | static_cast<uint32>(Message[p + 3]);
            }
            for (int32 t = 16; t < 64; ++t)
            {
                const uint32 S0 = Sha256RotateRight(W[t - 15], 7) ^ Sha256RotateRight(W[t - 15], 18) ^ (W[t - 15] >> 3);
                const uint32 S1 = Sha256RotateRight(W[t - 2], 17) ^ Sha256RotateRight(W[t - 2], 19) ^ (W[t - 2] >> 10);
                W[t] = W[t - 16] + S0 + W[t - 7] + S1;
            }

            uint32 a = H[0], b = H[1], c = H[2], d = H[3];
            uint32 e = H[4], f = H[5], g = H[6], h = H[7];

            for (int32 t = 0; t < 64; ++t)
            {
                const uint32 S1 = Sha256RotateRight(e, 6) ^ Sha256RotateRight(e, 11) ^ Sha256RotateRight(e, 25);
                const uint32 Ch = (e & f) ^ ((~e) & g);
                const uint32 Temp1 = h + S1 + Ch + G_Sha256K[t] + W[t];
                const uint32 S0 = Sha256RotateRight(a, 2) ^ Sha256RotateRight(a, 13) ^ Sha256RotateRight(a, 22);
                const uint32 Maj = (a & b) ^ (a & c) ^ (b & c);
                const uint32 Temp2 = S0 + Maj;

                h = g;
                g = f;
                f = e;
                e = d + Temp1;
                d = c;
                c = b;
                b = a;
                a = Temp1 + Temp2;
            }

            H[0] += a; H[1] += b; H[2] += c; H[3] += d;
            H[4] += e; H[5] += f; H[6] += g; H[7] += h;
        }

        FString Hex;
        Hex.Reserve(64);
        for (int32 i = 0; i < 8; ++i)
        {
            for (int32 Byte = 3; Byte >= 0; --Byte)
            {
                Hex += FString::Printf(TEXT("%02x"), static_cast<uint32>((H[i] >> (Byte * 8)) & 0xFFu));
            }
        }
        return Hex;
    }

    /**
     * 将 FString 以 UTF-8 字节流读出，按 JSON 字符串转义规则输出。
     * 非 ASCII 字符统一输出为 \uXXXX（或代理对），保证跨平台一致。
     */
    void AppendJsonString(FString& Out, const FString& Value)
    {
        FTCHARToUTF8 Converter(*Value, Value.Len());
        const uint8* Bytes = reinterpret_cast<const uint8*>(Converter.Get());
        const int32 Length = Converter.Length();

        Out.AppendChar(TEXT('"'));
        int32 Index = 0;
        while (Index < Length)
        {
            const uint8 Byte = Bytes[Index];
            uint32 CodePoint = 0;
            int32 Advance = 1;

            if (Byte < 0x80)
            {
                CodePoint = Byte;
            }
            else if ((Byte & 0xE0) == 0xC0)
            {
                CodePoint = Byte & 0x1F;
                Advance = 2;
            }
            else if ((Byte & 0xF0) == 0xE0)
            {
                CodePoint = Byte & 0x0F;
                Advance = 3;
            }
            else if ((Byte & 0xF8) == 0xF0)
            {
                CodePoint = Byte & 0x07;
                Advance = 4;
            }
            else
            {
                CodePoint = 0xFFFD;
            }

            for (int32 Continuation = 1; Continuation < Advance && Index + Continuation < Length; ++Continuation)
            {
                CodePoint = (CodePoint << 6) | (Bytes[Index + Continuation] & 0x3F);
            }
            Index += Advance;

            switch (CodePoint)
            {
            case TCHAR('"'):  Out += TEXT("\\\""); break;
            case TCHAR('\\'): Out += TEXT("\\\\"); break;
            case TCHAR('\b'): Out += TEXT("\\b"); break;
            case TCHAR('\f'): Out += TEXT("\\f"); break;
            case TCHAR('\n'): Out += TEXT("\\n"); break;
            case TCHAR('\r'): Out += TEXT("\\r"); break;
            case TCHAR('\t'): Out += TEXT("\\t"); break;
            default:
                if (CodePoint >= 0x20 && CodePoint <= 0x7E)
                {
                    Out.AppendChar(static_cast<TCHAR>(CodePoint));
                }
                else if (CodePoint <= 0xFFFF)
                {
                    Out += FString::Printf(TEXT("\\u%04x"), CodePoint);
                }
                else
                {
                    const uint32 Adjusted = CodePoint - 0x10000;
                    const uint32 High = 0xD800 + (Adjusted >> 10);
                    const uint32 Low = 0xDC00 + (Adjusted & 0x3FF);
                    Out += FString::Printf(TEXT("\\u%04x\\u%04x"), High, Low);
                }
                break;
            }
        }
        Out.AppendChar(TEXT('"'));
    }

    bool AppendStructValue(UScriptStruct* Struct, const void* StructPtr, FString& Out, FString& Error);

    /**
     * 按属性类型递归输出确定性的 JSON 值片段。
     */
    bool AppendValue(FProperty* Property, const void* ValuePtr, FString& Out, FString& Error)
    {
        if (const FBoolProperty* BoolProp = CastField<FBoolProperty>(Property))
        {
            Out += BoolProp->GetPropertyValue(ValuePtr) ? TEXT("true") : TEXT("false");
            return true;
        }

        if (const FEnumProperty* EnumProp = CastField<FEnumProperty>(Property))
        {
            const int64 Value = EnumProp->GetUnderlyingProperty()->GetSignedIntPropertyValue(ValuePtr);
            const UEnum* Enum = EnumProp->GetEnum();
            AppendJsonString(Out, Enum ? Enum->GetNameStringByValue(Value) : FString::Printf(TEXT("%lld"), Value));
            return true;
        }

        if (const FByteProperty* ByteProp = CastField<FByteProperty>(Property))
        {
            const uint8 Value = ByteProp->GetPropertyValue(ValuePtr);
            if (ByteProp->Enum)
            {
                AppendJsonString(Out, ByteProp->Enum->GetNameStringByValue(Value));
            }
            else
            {
                Out += LexToString(static_cast<uint32>(Value));
            }
            return true;
        }

        if (const FInt8Property* Int8Prop = CastField<FInt8Property>(Property))
        {
            Out += LexToString(static_cast<int32>(Int8Prop->GetPropertyValue(ValuePtr)));
            return true;
        }
        if (const FInt16Property* Int16Prop = CastField<FInt16Property>(Property))
        {
            Out += LexToString(static_cast<int32>(Int16Prop->GetPropertyValue(ValuePtr)));
            return true;
        }
        if (const FIntProperty* IntProp = CastField<FIntProperty>(Property))
        {
            Out += LexToString(IntProp->GetPropertyValue(ValuePtr));
            return true;
        }
        if (const FInt64Property* Int64Prop = CastField<FInt64Property>(Property))
        {
            Out += LexToString(Int64Prop->GetPropertyValue(ValuePtr));
            return true;
        }
        if (const FUInt16Property* UInt16Prop = CastField<FUInt16Property>(Property))
        {
            Out += LexToString(static_cast<uint32>(UInt16Prop->GetPropertyValue(ValuePtr)));
            return true;
        }
        if (const FUInt32Property* UInt32Prop = CastField<FUInt32Property>(Property))
        {
            Out += LexToString(UInt32Prop->GetPropertyValue(ValuePtr));
            return true;
        }
        if (const FUInt64Property* UInt64Prop = CastField<FUInt64Property>(Property))
        {
            Out += LexToString(UInt64Prop->GetPropertyValue(ValuePtr));
            return true;
        }

        if (const FFloatProperty* FloatProp = CastField<FFloatProperty>(Property))
        {
            const float Value = FloatProp->GetPropertyValue(ValuePtr);
            uint32 Bits = 0;
            FMemory::Memcpy(&Bits, &Value, sizeof(Bits));
            AppendJsonString(Out, FString::Printf(TEXT("%08x"), Bits));
            return true;
        }
        if (const FDoubleProperty* DoubleProp = CastField<FDoubleProperty>(Property))
        {
            const double Value = DoubleProp->GetPropertyValue(ValuePtr);
            uint64 Bits = 0;
            FMemory::Memcpy(&Bits, &Value, sizeof(Bits));
            AppendJsonString(Out, FString::Printf(TEXT("%08x%08x"),
                static_cast<uint32>(Bits >> 32),
                static_cast<uint32>(Bits & 0xFFFFFFFFu)));
            return true;
        }

        if (const FStrProperty* StrProp = CastField<FStrProperty>(Property))
        {
            AppendJsonString(Out, StrProp->GetPropertyValue(ValuePtr));
            return true;
        }
        if (const FNameProperty* NameProp = CastField<FNameProperty>(Property))
        {
            AppendJsonString(Out, NameProp->GetPropertyValue(ValuePtr).ToString());
            return true;
        }

        if (const FStructProperty* StructProp = CastField<FStructProperty>(Property))
        {
            return AppendStructValue(StructProp->Struct, ValuePtr, Out, Error);
        }
        if (const FArrayProperty* ArrayProp = CastField<FArrayProperty>(Property))
        {
            FScriptArrayHelper Helper(ArrayProp, ValuePtr);
            FProperty* Inner = ArrayProp->Inner;
            Out.AppendChar(TEXT('['));
            for (int32 Index = 0; Index < Helper.Num(); ++Index)
            {
                if (Index > 0)
                {
                    Out.AppendChar(TEXT(','));
                }
                if (!AppendValue(Inner, Helper.GetRawPtr(Index), Out, Error))
                {
                    return false;
                }
            }
            Out.AppendChar(TEXT(']'));
            return true;
        }
        if (const FMapProperty* MapProp = CastField<FMapProperty>(Property))
        {
            FScriptMapHelper Helper(MapProp, ValuePtr);
            FProperty* KeyProp = MapProp->KeyProp;
            FProperty* ValueProp = MapProp->ValueProp;

            TArray<TPair<FString, int32>> Entries;
            Entries.Reserve(Helper.Num());
            for (int32 Index = 0; Index < Helper.Num(); ++Index)
            {
                FString KeyJson;
                FString KeyError;
                if (!AppendValue(KeyProp, Helper.GetKeyPtr(Index), KeyJson, KeyError))
                {
                    Error = KeyError;
                    return false;
                }
                Entries.Emplace(MoveTemp(KeyJson), Index);
            }
            Entries.Sort([](const TPair<FString, int32>& A, const TPair<FString, int32>& B)
            {
                return A.Key.Compare(B.Key, ESearchCase::CaseSensitive) < 0;
            });

            Out.AppendChar(TEXT('{'));
            for (int32 EntryIndex = 0; EntryIndex < Entries.Num(); ++EntryIndex)
            {
                if (EntryIndex > 0)
                {
                    Out.AppendChar(TEXT(','));
                }
                Out += Entries[EntryIndex].Key;
                Out.AppendChar(TEXT(':'));
                if (!AppendValue(ValueProp, Helper.GetValuePtr(Entries[EntryIndex].Value), Out, Error))
                {
                    return false;
                }
            }
            Out.AppendChar(TEXT('}'));
            return true;
        }

        Error = FString::Printf(TEXT("Unsupported property '%s' for deterministic serialization."), *Property->GetName());
        return false;
    }

    /**
     * 按字段名字典序输出结构体对象。GUID 特殊处理为紧凑字符串形式。
     */
    bool AppendStructValue(UScriptStruct* Struct, const void* StructPtr, FString& Out, FString& Error)
    {
        if (Struct->GetFName() == FName(TEXT("Guid")))
        {
            const FGuid* Guid = static_cast<const FGuid*>(StructPtr);
            AppendJsonString(Out, Guid->ToString(EGuidFormats::DigitsWithHyphensLower));
            return true;
        }

        TArray<FProperty*> Fields;
        for (TFieldIterator<FProperty> It(Struct); It; ++It)
        {
            Fields.Add(*It);
        }
        Fields.Sort([](const FProperty& A, const FProperty& B)
        {
            return A.GetName().Compare(B.GetName(), ESearchCase::CaseSensitive) < 0;
        });

        Out.AppendChar(TEXT('{'));
        for (int32 Index = 0; Index < Fields.Num(); ++Index)
        {
            if (Index > 0)
            {
                Out.AppendChar(TEXT(','));
            }
            FProperty* Field = Fields[Index];
            AppendJsonString(Out, Field->GetName());
            Out.AppendChar(TEXT(':'));
            if (!AppendValue(Field, Field->ContainerPtrToValuePtr<void>(StructPtr), Out, Error))
            {
                return false;
            }
        }
        Out.AppendChar(TEXT('}'));
        return true;
    }
}

FString FAkUGCLogicPackHasher::Sha256Hex(const FString& Utf8)
{
    FTCHARToUTF8 Converter(*Utf8, Utf8.Len());
    return Sha256HexDigest(reinterpret_cast<const uint8*>(Converter.Get()), Converter.Length());
}

bool FAkUGCLogicPackHasher::DeterministicSerialize(const FAkUGCProjectDocument& Document, FString& OutJson, FString* OutError)
{
    FString Error;
    FString Json;
    if (!AppendStructValue(FAkUGCProjectDocument::StaticStruct(), &Document, Json, Error))
    {
        if (OutError)
        {
            *OutError = MoveTemp(Error);
        }
        return false;
    }
    OutJson = MoveTemp(Json);
    return true;
}

bool FAkUGCLogicPackHasher::DeterministicSerializeProgram(const FAkUGCLogicProgram& Program, FString& OutJson, FString* OutError)
{
    FString Error;
    FString Json;
    if (!AppendStructValue(FAkUGCLogicProgram::StaticStruct(), &Program, Json, Error))
    {
        if (OutError)
        {
            *OutError = MoveTemp(Error);
        }
        return false;
    }
    OutJson = MoveTemp(Json);
    return true;
}

bool FAkUGCLogicPackHasher::DeterministicSerializeManifest(const FAkUGCLogicPackManifest& Manifest, FString& OutJson, FString* OutError)
{
    FString Error;
    FString Json;
    if (!AppendStructValue(FAkUGCLogicPackManifest::StaticStruct(), &Manifest, Json, Error))
    {
        if (OutError)
        {
            *OutError = MoveTemp(Error);
        }
        return false;
    }
    OutJson = MoveTemp(Json);
    return true;
}

FString FAkUGCLogicPackHasher::HashDocument(const FAkUGCProjectDocument& Document, FString* OutError)
{
    FString Json;
    FString Error;
    if (!DeterministicSerialize(Document, Json, &Error))
    {
        if (OutError)
        {
            *OutError = MoveTemp(Error);
        }
        return FString{};
    }
    return Sha256Hex(Json);
}

FString FAkUGCLogicPackHasher::HashLogicProgram(const FAkUGCLogicProgram& Program, FString* OutError)
{
    FString Json;
    FString Error;
    if (!DeterministicSerializeProgram(Program, Json, &Error))
    {
        if (OutError)
        {
            *OutError = MoveTemp(Error);
        }
        return FString{};
    }
    return Sha256Hex(Json);
}

FString FAkUGCLogicPackHasher::HashManifest(const FAkUGCLogicPackManifest& Manifest, FString* OutError)
{
    FString Json;
    FString Error;
    if (!DeterministicSerializeManifest(Manifest, Json, &Error))
    {
        if (OutError)
        {
            *OutError = MoveTemp(Error);
        }
        return FString{};
    }
    return Sha256Hex(Json);
}
