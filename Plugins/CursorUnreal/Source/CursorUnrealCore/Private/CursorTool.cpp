#include "CursorTool.h"

#include "CursorJson.h"
#include "CursorToolRegistry.h"

const TCHAR* LexToString(ECursorToolRisk Risk)
{
	switch (Risk)
	{
	case ECursorToolRisk::ReadOnly: return TEXT("read-only");
	case ECursorToolRisk::Mutating: return TEXT("mutating");
	default: return TEXT("dangerous");
	}
}

// ---------------------------------------------------------------------------------------------
// FCursorToolResult

FCursorToolResult FCursorToolResult::Ok(const FString& InText)
{
	FCursorToolResult R;
	R.Text = InText;
	return R;
}

FCursorToolResult FCursorToolResult::OkJson(const TSharedRef<FJsonObject>& Json)
{
	FCursorToolResult R;
	R.Text = CursorJson::ToString(Json);
	R.Structured = Json;
	return R;
}

FCursorToolResult FCursorToolResult::Error(const FString& Message)
{
	FCursorToolResult R;
	R.bIsError = true;
	R.Text = Message;
	return R;
}

// ---------------------------------------------------------------------------------------------
// FCursorArgs

bool FCursorArgs::Has(const TCHAR* Name) const
{
	const TSharedPtr<FJsonValue> Value = Json->TryGetField(Name);
	return Value.IsValid() && !Value->IsNull();
}

FString FCursorArgs::Str(const TCHAR* Name, const FString& Default) const
{
	FString Out;
	return Json->TryGetStringField(Name, Out) ? Out : Default;
}

int32 FCursorArgs::Int(const TCHAR* Name, int32 Default) const
{
	double Out = 0.0;
	return Json->TryGetNumberField(Name, Out) ? FMath::RoundToInt32(Out) : Default;
}

double FCursorArgs::Num(const TCHAR* Name, double Default) const
{
	double Out = 0.0;
	return Json->TryGetNumberField(Name, Out) ? Out : Default;
}

bool FCursorArgs::Bool(const TCHAR* Name, bool Default) const
{
	bool Out = false;
	return Json->TryGetBoolField(Name, Out) ? Out : Default;
}

TArray<FString> FCursorArgs::StrArray(const TCHAR* Name) const
{
	TArray<FString> Out;
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (Json->TryGetArrayField(Name, Values) && Values)
	{
		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			FString S;
			if (Value.IsValid() && Value->TryGetString(S))
			{
				Out.Add(S);
			}
		}
	}
	else
	{
		// Models sometimes send a single string where a list is expected.
		FString Single;
		if (Json->TryGetStringField(Name, Single) && !Single.IsEmpty())
		{
			Out.Add(Single);
		}
	}
	return Out;
}

TSharedPtr<FJsonObject> FCursorArgs::Obj(const TCHAR* Name) const
{
	const TSharedPtr<FJsonObject>* Out = nullptr;
	if (Json->TryGetObjectField(Name, Out) && Out)
	{
		return *Out;
	}
	return nullptr;
}

const TArray<TSharedPtr<FJsonValue>>* FCursorArgs::Arr(const TCHAR* Name) const
{
	const TArray<TSharedPtr<FJsonValue>>* Out = nullptr;
	Json->TryGetArrayField(Name, Out);
	return Out;
}

static bool ReadTriple(const FJsonObject& Json, const TCHAR* Name, const TCHAR* A, const TCHAR* B, const TCHAR* C, double Out[3])
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (Json.TryGetArrayField(Name, Values) && Values && Values->Num() == 3)
	{
		for (int32 i = 0; i < 3; ++i)
		{
			if (!(*Values)[i].IsValid() || !(*Values)[i]->TryGetNumber(Out[i]))
			{
				return false;
			}
		}
		return true;
	}
	const TSharedPtr<FJsonObject>* Object = nullptr;
	if (Json.TryGetObjectField(Name, Object) && Object && Object->IsValid())
	{
		Out[0] = Out[1] = Out[2] = 0.0;
		const bool bAny =
			(*Object)->TryGetNumberField(A, Out[0]) |
			(*Object)->TryGetNumberField(B, Out[1]) |
			(*Object)->TryGetNumberField(C, Out[2]);
		return bAny;
	}
	return false;
}

bool FCursorArgs::Vec(const TCHAR* Name, FVector& Out) const
{
	double V[3];
	if (!ReadTriple(*Json, Name, TEXT("x"), TEXT("y"), TEXT("z"), V))
	{
		return false;
	}
	Out = FVector(V[0], V[1], V[2]);
	return true;
}

bool FCursorArgs::Rot(const TCHAR* Name, FRotator& Out) const
{
	double V[3];
	if (!ReadTriple(*Json, Name, TEXT("pitch"), TEXT("yaw"), TEXT("roll"), V))
	{
		return false;
	}
	Out = FRotator(V[0], V[1], V[2]);
	return true;
}

// ---------------------------------------------------------------------------------------------
// FCursorSchema

FCursorSchema::FCursorSchema()
	: Properties(MakeShared<FJsonObject>())
{
}

FCursorSchema& FCursorSchema::Add(const TCHAR* Name, const TSharedRef<FJsonObject>& Property, const TCHAR* Description, bool bRequired)
{
	Property->SetStringField(TEXT("description"), Description);
	Properties->SetObjectField(Name, Property);
	if (bRequired)
	{
		Required.Add(MakeShared<FJsonValueString>(Name));
	}
	return *this;
}

static TSharedRef<FJsonObject> TypedProperty(const TCHAR* Type)
{
	TSharedRef<FJsonObject> P = MakeShared<FJsonObject>();
	P->SetStringField(TEXT("type"), Type);
	return P;
}

static TSharedRef<FJsonObject> ArrayProperty(const TCHAR* ItemType)
{
	TSharedRef<FJsonObject> P = TypedProperty(TEXT("array"));
	P->SetObjectField(TEXT("items"), TypedProperty(ItemType));
	return P;
}

FCursorSchema& FCursorSchema::Str(const TCHAR* Name, const TCHAR* Description, bool bRequired)
{
	return Add(Name, TypedProperty(TEXT("string")), Description, bRequired);
}

FCursorSchema& FCursorSchema::Int(const TCHAR* Name, const TCHAR* Description, bool bRequired)
{
	return Add(Name, TypedProperty(TEXT("integer")), Description, bRequired);
}

FCursorSchema& FCursorSchema::Num(const TCHAR* Name, const TCHAR* Description, bool bRequired)
{
	return Add(Name, TypedProperty(TEXT("number")), Description, bRequired);
}

FCursorSchema& FCursorSchema::Bool(const TCHAR* Name, const TCHAR* Description, bool bRequired)
{
	return Add(Name, TypedProperty(TEXT("boolean")), Description, bRequired);
}

FCursorSchema& FCursorSchema::Enum(const TCHAR* Name, const TCHAR* Description, std::initializer_list<const TCHAR*> Values, bool bRequired)
{
	TSharedRef<FJsonObject> P = TypedProperty(TEXT("string"));
	TArray<TSharedPtr<FJsonValue>> EnumValues;
	for (const TCHAR* Value : Values)
	{
		EnumValues.Add(MakeShared<FJsonValueString>(Value));
	}
	P->SetArrayField(TEXT("enum"), EnumValues);
	return Add(Name, P, Description, bRequired);
}

FCursorSchema& FCursorSchema::Obj(const TCHAR* Name, const TCHAR* Description, bool bRequired)
{
	return Add(Name, TypedProperty(TEXT("object")), Description, bRequired);
}

FCursorSchema& FCursorSchema::StrArray(const TCHAR* Name, const TCHAR* Description, bool bRequired)
{
	return Add(Name, ArrayProperty(TEXT("string")), Description, bRequired);
}

FCursorSchema& FCursorSchema::ObjArray(const TCHAR* Name, const TCHAR* Description, bool bRequired)
{
	return Add(Name, ArrayProperty(TEXT("object")), Description, bRequired);
}

FCursorSchema& FCursorSchema::Vec3(const TCHAR* Name, const TCHAR* Description, bool bRequired)
{
	TSharedRef<FJsonObject> P = ArrayProperty(TEXT("number"));
	P->SetNumberField(TEXT("minItems"), 3);
	P->SetNumberField(TEXT("maxItems"), 3);
	return Add(Name, P, Description, bRequired);
}

FCursorSchema& FCursorSchema::Any(const TCHAR* Name, const TCHAR* Description, bool bRequired)
{
	return Add(Name, MakeShared<FJsonObject>(), Description, bRequired);
}

TSharedRef<FJsonObject> FCursorSchema::Build() const
{
	TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));
	Schema->SetObjectField(TEXT("properties"), Properties);
	if (Required.Num() > 0)
	{
		Schema->SetArrayField(TEXT("required"), Required);
	}
	return Schema;
}

// ---------------------------------------------------------------------------------------------
// FCursorToolBuilder

namespace
{
	class FLambdaTool final : public ICursorTool
	{
	public:
		FName Name;
		FName Pack;
		FString Description;
		ECursorToolRisk RiskLevel = ECursorToolRisk::ReadOnly;
		ECursorToolAudience AudienceValue = ECursorToolAudience::All;
		TSharedPtr<FJsonObject> SchemaJson;
		FCursorToolBuilder::FSyncFn SyncFn;
		FCursorToolBuilder::FAsyncFn AsyncFn;

		virtual FName GetName() const override { return Name; }
		virtual FName GetPack() const override { return Pack; }
		virtual FText GetDescription() const override { return FText::FromString(Description); }
		virtual TSharedRef<FJsonObject> GetInputSchema() const override { return SchemaJson.ToSharedRef(); }
		virtual ECursorToolRisk GetRisk() const override { return RiskLevel; }
		virtual ECursorToolAudience GetAudience() const override { return AudienceValue; }

		virtual void Execute(const TSharedRef<FJsonObject>& Args, FCursorToolContext& Ctx, FCursorToolCompletion OnComplete) override
		{
			const FCursorArgs Wrapped(Args);
			if (AsyncFn)
			{
				AsyncFn(Wrapped, Ctx, MoveTemp(OnComplete));
			}
			else if (SyncFn)
			{
				OnComplete(SyncFn(Wrapped, Ctx));
			}
			else
			{
				OnComplete(FCursorToolResult::Error(TEXT("Tool has no implementation.")));
			}
		}
	};
}

FCursorToolBuilder::FCursorToolBuilder(const TCHAR* InName, const TCHAR* InPack)
	: Name(InName)
	, Pack(InPack)
	, SchemaJson(FCursorSchema().Build())
{
}

FCursorToolBuilder& FCursorToolBuilder::Describe(const TCHAR* InDescription)
{
	Description = InDescription;
	return *this;
}

FCursorToolBuilder& FCursorToolBuilder::Risk(ECursorToolRisk InRisk)
{
	RiskLevel = InRisk;
	return *this;
}

FCursorToolBuilder& FCursorToolBuilder::Audience(ECursorToolAudience InAudience)
{
	AudienceValue = InAudience;
	return *this;
}

FCursorToolBuilder& FCursorToolBuilder::Schema(const FCursorSchema& InSchema)
{
	SchemaJson = InSchema.Build();
	return *this;
}

FCursorToolBuilder& FCursorToolBuilder::Run(FSyncFn InFn)
{
	SyncFn = MoveTemp(InFn);
	return *this;
}

FCursorToolBuilder& FCursorToolBuilder::RunAsync(FAsyncFn InFn)
{
	AsyncFn = MoveTemp(InFn);
	return *this;
}

TSharedRef<ICursorTool> FCursorToolBuilder::Build() const
{
	TSharedRef<FLambdaTool> Tool = MakeShared<FLambdaTool>();
	Tool->Name = Name;
	Tool->Pack = Pack;
	Tool->Description = Description;
	Tool->RiskLevel = RiskLevel;
	Tool->AudienceValue = AudienceValue;
	Tool->SchemaJson = SchemaJson;
	Tool->SyncFn = SyncFn;
	Tool->AsyncFn = AsyncFn;
	return Tool;
}

void FCursorToolBuilder::Register() const
{
	FCursorToolRegistry::Get().Register(Build());
}
