#include "CursorToolHelpers.h"

#include "Components/ActorComponent.h"
#include "CursorJson.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "GameFramework/WorldSettings.h"
#include "JsonObjectConverter.h"
#include "Misc/PackageName.h"
#include "UObject/UnrealType.h"

namespace CursorToolHelpers
{
	UWorld* GetEditorWorld()
	{
		return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	}

	AActor* FindActor(const FString& NameOrLabel, FString& OutError)
	{
		UWorld* World = GetEditorWorld();
		if (!World)
		{
			OutError = TEXT("No level is open in the editor.");
			return nullptr;
		}
		if (NameOrLabel.IsEmpty())
		{
			OutError = TEXT("Actor name is empty.");
			return nullptr;
		}

		TArray<AActor*> LabelMatches;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			// The object name is unique in the level, so an exact hit on it wins outright.
			if (Actor->GetName().Equals(NameOrLabel, ESearchCase::IgnoreCase) || Actor->GetPathName() == NameOrLabel)
			{
				return Actor;
			}
			if (Actor->GetActorLabel(false).Equals(NameOrLabel, ESearchCase::IgnoreCase))
			{
				LabelMatches.Add(Actor);
			}
		}

		if (LabelMatches.Num() == 1)
		{
			return LabelMatches[0];
		}
		if (LabelMatches.Num() > 1)
		{
			TArray<FString> Names;
			for (int32 i = 0; i < LabelMatches.Num() && i < 10; ++i)
			{
				Names.Add(LabelMatches[i]->GetName());
			}
			OutError = FString::Printf(TEXT("%d actors are labelled '%s'. Use one of their unique names instead: %s"),
				LabelMatches.Num(), *NameOrLabel, *FString::Join(Names, TEXT(", ")));
			return nullptr;
		}
		// The level's settings actor is not in the Outliner and is named differently from level to level
		// (WorldSettings, WorldSettings1, WorldInfo_0 in older template maps), so it gets a fixed name here.
		if (NameOrLabel.Equals(TEXT("WorldSettings"), ESearchCase::IgnoreCase))
		{
			if (AWorldSettings* Settings = World->GetWorldSettings())
			{
				return Settings;
			}
		}
		OutError = FString::Printf(TEXT("No actor named or labelled '%s' in the open level. Use actor_list to see what is there."), *NameOrLabel);
		return nullptr;
	}

	FString ToPackagePath(const FString& AssetPath)
	{
		FString Path = AssetPath.TrimStartAndEnd();
		// Strip a class prefix such as StaticMesh'/Game/X.X'
		int32 Quote = INDEX_NONE;
		if (Path.FindChar(TEXT('\''), Quote))
		{
			Path = Path.Mid(Quote + 1);
			Path.RemoveFromEnd(TEXT("'"));
		}
		// The package is everything before the first '.' or ':' of the last path segment:
		// /Game/A/BP.BP_C:Mesh_GEN_VARIABLE and /Game/A/BP.Default__BP_C.Mesh are both in /Game/A/BP.
		int32 Slash = INDEX_NONE;
		Path.FindLastChar(TEXT('/'), Slash);
		for (int32 i = Slash + 1; i < Path.Len(); ++i)
		{
			if (Path[i] == TEXT('.') || Path[i] == TEXT(':'))
			{
				Path.LeftInline(i);
				break;
			}
		}
		return Path;
	}

	UObject* LoadAsset(const FString& Path, FString& OutError)
	{
		const FString PackagePath = ToPackagePath(Path);
		if (!PackagePath.StartsWith(TEXT("/")))
		{
			OutError = FString::Printf(TEXT("'%s' is not an asset path. Asset paths look like /Game/Folder/Name."), *Path);
			return nullptr;
		}
		FText Reason;
		if (!FPackageName::IsValidLongPackageName(PackagePath, /*bIncludeReadOnlyRoots*/ true, &Reason))
		{
			OutError = FString::Printf(TEXT("'%s' is not a valid asset path: %s"), *Path, *Reason.ToString());
			return nullptr;
		}
		if (!FPackageName::DoesPackageExist(PackagePath) && !FindPackage(nullptr, *PackagePath))
		{
			OutError = FString::Printf(TEXT("No asset at '%s'. Use asset_search to find it."), *PackagePath);
			return nullptr;
		}

		// An explicit object name ("/Game/BP.BP_C") is honoured; otherwise take the asset named after its package.
		FString ObjectPath = Path.TrimStartAndEnd();
		if (ObjectPath == PackagePath)
		{
			ObjectPath = PackagePath + TEXT(".") + FPackageName::GetShortName(PackagePath);
		}
		UObject* Object = StaticLoadObject(UObject::StaticClass(), nullptr, *ObjectPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
		if (!Object)
		{
			OutError = FString::Printf(TEXT("Could not load '%s'."), *ObjectPath);
		}
		return Object;
	}

	UClass* ResolveClass(const FString& NameOrPath, UClass* RequiredBase, FString& OutError)
	{
		const FString Name = NameOrPath.TrimStartAndEnd();
		UClass* Class = nullptr;

		if (Name.StartsWith(TEXT("/Script/")))
		{
			Class = FindObject<UClass>(nullptr, *Name);
			if (!Class)
			{
				Class = LoadObject<UClass>(nullptr, *Name, nullptr, LOAD_NoWarn | LOAD_Quiet);
			}
		}
		else if (Name.StartsWith(TEXT("/")))
		{
			FString LoadError;
			if (UObject* Asset = LoadAsset(Name, LoadError))
			{
				if (const UBlueprint* Blueprint = Cast<UBlueprint>(Asset))
				{
					Class = Blueprint->GeneratedClass;
				}
				else
				{
					Class = Cast<UClass>(Asset);
				}
			}
			if (!Class)
			{
				OutError = LoadError.IsEmpty() ? FString::Printf(TEXT("'%s' is not a Blueprint or class."), *Name) : LoadError;
				return nullptr;
			}
		}
		else if (!Name.IsEmpty())
		{
			Class = FindFirstObject<UClass>(*Name, EFindFirstObjectOptions::NativeFirst);
			if (!Class && Name.Len() > 1)
			{
				// Tolerate the C++ prefix: AStaticMeshActor, UStaticMeshComponent.
				const TCHAR First = Name[0];
				if ((First == TEXT('A') || First == TEXT('U')) && FChar::IsUpper(Name[1]))
				{
					Class = FindFirstObject<UClass>(*Name.Mid(1), EFindFirstObjectOptions::NativeFirst);
				}
			}
		}

		if (!Class)
		{
			OutError = FString::Printf(TEXT("No class found for '%s'. Use a native class name (StaticMeshActor), a /Script/Module.Class path, or a Blueprint asset path."), *Name);
			return nullptr;
		}
		if (RequiredBase && !Class->IsChildOf(RequiredBase))
		{
			OutError = FString::Printf(TEXT("'%s' is not a kind of %s."), *Class->GetName(), *RequiredBase->GetName());
			return nullptr;
		}
		if (Class->HasAnyClassFlags(CLASS_Deprecated | CLASS_NewerVersionExists))
		{
			OutError = FString::Printf(TEXT("'%s' is deprecated or out of date."), *Class->GetName());
			return nullptr;
		}
		return Class;
	}

	/**
	 * The defaults of a component on a class, addressed by the component's own name. A component a Blueprint adds
	 * keeps its defaults in an object called <Name>_GEN_VARIABLE that hangs off the class, not off the class's
	 * default object, so the natural spellings do not exist as objects:
	 *   /Game/BP_Door.Default__BP_Door_C.Mesh    /Game/BP_Door.Default__BP_Door_C:Mesh    /Game/BP_Door.BP_Door_C:Mesh
	 * All three mean /Game/BP_Door.BP_Door_C:Mesh_GEN_VARIABLE. Components inherited from a parent Blueprint are found too.
	 */
	UObject* FindComponentTemplate(const FString& Path)
	{
		int32 FirstDot = INDEX_NONE;
		int32 Split = INDEX_NONE;
		if (!Path.FindChar(TEXT('.'), FirstDot))
		{
			return nullptr;
		}
		if (!Path.FindLastChar(TEXT(':'), Split))
		{
			Path.FindLastChar(TEXT('.'), Split);
			if (Split == FirstDot)
			{
				return nullptr; // "/Game/Folder/Asset.Asset": an asset, not something inside one.
			}
		}
		const FString OwnerPath = Path.Left(Split);
		const FString Name = Path.Mid(Split + 1);
		if (Name.IsEmpty())
		{
			return nullptr;
		}

		UObject* Owner = StaticFindObject(UObject::StaticClass(), nullptr, *OwnerPath);
		if (!Owner)
		{
			FString Ignored;
			if (LoadAsset(ToPackagePath(OwnerPath), Ignored))
			{
				Owner = StaticFindObject(UObject::StaticClass(), nullptr, *OwnerPath);
			}
		}
		UClass* Class = Cast<UClass>(Owner);
		if (!Class && Owner && Owner->HasAnyFlags(RF_ClassDefaultObject))
		{
			Class = Owner->GetClass();
		}
		if (!Class)
		{
			return nullptr;
		}

		// A component the C++ parent created (CharMoveComp, CollisionCylinder) lives on the default object.
		if (UObject* Native = StaticFindObjectFast(UObject::StaticClass(), Class->GetDefaultObject(), FName(*Name)))
		{
			return Native;
		}
		for (const UClass* Current = Class; Current; Current = Current->GetSuperClass())
		{
			const UBlueprintGeneratedClass* Generated = Cast<UBlueprintGeneratedClass>(Current);
			if (!Generated || !Generated->SimpleConstructionScript)
			{
				continue;
			}
			for (const USCS_Node* Node : Generated->SimpleConstructionScript->GetAllNodes())
			{
				if (Node && Node->ComponentTemplate
					&& (Node->GetVariableName().ToString().Equals(Name, ESearchCase::IgnoreCase) || Node->ComponentTemplate->GetName().Equals(Name, ESearchCase::IgnoreCase)))
				{
					return Node->ComponentTemplate;
				}
			}
		}
		return nullptr;
	}

	UObject* ResolveObject(const FString& InPath, FString& OutError)
	{
		const FString Path = InPath.TrimStartAndEnd();
		if (Path.IsEmpty())
		{
			OutError = TEXT("Object path is empty.");
			return nullptr;
		}

		if (Path.StartsWith(TEXT("/")))
		{
			// Something already in memory (a class, an actor by full path, a loaded asset's subobject)?
			// "/Game/Folder/Asset" also names the package the asset lives in, and a loaded package is found
			// first. Nobody means the package: fall through to the asset inside it.
			UObject* Existing = StaticFindObject(UObject::StaticClass(), nullptr, *Path);
			if (Existing && !Existing->IsA<UPackage>())
			{
				return Existing;
			}
			if (UObject* Template = FindComponentTemplate(Path))
			{
				return Template;
			}
			return LoadAsset(Path, OutError);
		}

		// Actor, or Actor.Component
		FString ActorError;
		if (AActor* Actor = FindActor(Path, ActorError))
		{
			return Actor;
		}
		FString ActorName, ComponentName;
		if (Path.Split(TEXT("."), &ActorName, &ComponentName, ESearchCase::CaseSensitive, ESearchDir::FromEnd))
		{
			FString Ignored;
			if (AActor* Actor = FindActor(ActorName, Ignored))
			{
				TArray<FString> Available;
				for (UActorComponent* Component : Actor->GetComponents())
				{
					if (Component && Component->GetName().Equals(ComponentName, ESearchCase::IgnoreCase))
					{
						return Component;
					}
					if (Component)
					{
						Available.Add(Component->GetName());
					}
				}
				OutError = FString::Printf(TEXT("Actor '%s' has no component '%s'. Components: %s"), *ActorName, *ComponentName, *FString::Join(Available, TEXT(", ")));
				return nullptr;
			}
		}
		OutError = ActorError;
		return nullptr;
	}

	TSharedRef<FJsonObject> DescribeActor(const AActor* Actor, bool bIncludeComponents)
	{
		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetStringField(TEXT("name"), Actor->GetName());
		Out->SetStringField(TEXT("label"), Actor->GetActorLabel(false));
		Out->SetStringField(TEXT("class"), Actor->GetClass()->GetPathName());
		const FName Folder = Actor->GetFolderPath();
		if (!Folder.IsNone())
		{
			Out->SetStringField(TEXT("folder"), Folder.ToString());
		}
		Out->SetObjectField(TEXT("location"), CursorJson::VectorToJson(Actor->GetActorLocation()));
		Out->SetObjectField(TEXT("rotation"), CursorJson::RotatorToJson(Actor->GetActorRotation()));
		Out->SetObjectField(TEXT("scale"), CursorJson::VectorToJson(Actor->GetActorScale3D()));
		if (Actor->IsHiddenEd())
		{
			Out->SetBoolField(TEXT("hidden_in_editor"), true);
		}
		if (Actor->Tags.Num() > 0)
		{
			TArray<FString> Tags;
			for (const FName& Tag : Actor->Tags)
			{
				Tags.Add(Tag.ToString());
			}
			Out->SetArrayField(TEXT("tags"), CursorJson::StringArray(Tags));
		}
		if (const AActor* Parent = Actor->GetAttachParentActor())
		{
			Out->SetStringField(TEXT("attached_to"), Parent->GetName());
		}

		if (bIncludeComponents)
		{
			TArray<TSharedPtr<FJsonValue>> Components;
			for (const UActorComponent* Component : Actor->GetComponents())
			{
				if (!Component)
				{
					continue;
				}
				TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetStringField(TEXT("name"), Component->GetName());
				Entry->SetStringField(TEXT("class"), Component->GetClass()->GetName());
				if (Component == Actor->GetRootComponent())
				{
					Entry->SetBoolField(TEXT("root"), true);
				}
				Components.Add(MakeShared<FJsonValueObject>(Entry));
			}
			Out->SetArrayField(TEXT("components"), Components);
		}
		return Out;
	}

	TSharedRef<FJsonObject> ReadProperties(const UObject* Object, const TArray<FString>& Names, const FString& NameContains, int32 MaxProperties, TArray<FString>& OutMissing)
	{
		// One oversized value (a ten-thousand-element array) should not crowd out everything else.
		constexpr int32 MaxUnrequestedValueChars = 2000;

		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		TSet<FString> Found;
		int32 Count = 0;
		int32 Skipped = 0;

		for (TFieldIterator<FProperty> It(Object->GetClass()); It; ++It)
		{
			FProperty* Property = *It;
			const FString PropertyName = Property->GetName();
			const bool bRequested = Names.ContainsByPredicate([&](const FString& N) { return N.Equals(PropertyName, ESearchCase::IgnoreCase); });

			if (Names.Num() > 0)
			{
				if (!bRequested)
				{
					continue;
				}
			}
			else
			{
				if (!Property->HasAnyPropertyFlags(CPF_Edit) || Property->HasAnyPropertyFlags(CPF_Deprecated))
				{
					continue;
				}
				if (!NameContains.IsEmpty() && !PropertyName.Contains(NameContains, ESearchCase::IgnoreCase))
				{
					continue;
				}
			}

			if (Count >= MaxProperties)
			{
				++Skipped;
				continue;
			}

			const void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Object);
			TSharedPtr<FJsonValue> Value = FJsonObjectConverter::UPropertyToJsonValue(
				Property, ValuePtr, 0, 0, nullptr, nullptr, EJsonObjectConversionFlags::SkipStandardizeCase);
			if (!Value.IsValid())
			{
				FString Text;
				Property->ExportTextItem_Direct(Text, ValuePtr, nullptr, const_cast<UObject*>(Object), PPF_None);
				Value = MakeShared<FJsonValueString>(Text);
			}

			if (!bRequested && (Value->Type == EJson::Array || Value->Type == EJson::Object))
			{
				FString Serialised;
				const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Serialised);
				FJsonSerializer::Serialize(Value, FString(), Writer);
				if (Serialised.Len() > MaxUnrequestedValueChars)
				{
					Value = MakeShared<FJsonValueString>(FString::Printf(TEXT("<%s, %d characters. Ask for this property by name to read it.>"),
						*Property->GetCPPType(), Serialised.Len()));
				}
			}

			Out->SetField(PropertyName, Value);
			Found.Add(PropertyName.ToLower());
			++Count;
		}

		for (const FString& Name : Names)
		{
			if (!Found.Contains(Name.ToLower()))
			{
				OutMissing.Add(Name);
			}
		}
		if (Skipped > 0)
		{
			Out->SetStringField(TEXT("_note"), FString::Printf(TEXT("%d more properties not shown. Narrow with 'filter' or ask for properties by name."), Skipped));
		}
		return Out;
	}

	namespace
	{
		const TCHAR* JsonTypeName(EJson Type)
		{
			switch (Type)
			{
			case EJson::String: return TEXT("a string");
			case EJson::Number: return TEXT("a number");
			case EJson::Boolean: return TEXT("a boolean");
			case EJson::Array: return TEXT("an array");
			case EJson::Object: return TEXT("an object");
			default: return TEXT("null");
			}
		}

		FProperty* FindPropertyByName(const UStruct* Scope, const FString& Name)
		{
			for (TFieldIterator<FProperty> It(Scope); It; ++It)
			{
				if (It->GetName().Equals(Name, ESearchCase::IgnoreCase))
				{
					return *It;
				}
			}
			return nullptr;
		}

		/**
		 * The engine's JSON converter is lenient: a value of the wrong type is logged and replaced by
		 * zero, and unknown struct members are ignored. An agent would take that for success, so the
		 * shape is checked here first and anything that does not fit is reported.
		 */
		bool CheckJsonFits(const FProperty* Property, const TSharedPtr<FJsonValue>& Value, const FString& Where, FString& OutError)
		{
			const EJson Type = Value.IsValid() ? Value->Type : EJson::Null;
			auto Fail = [&](const TCHAR* Expected)
			{
				OutError = FString::Printf(TEXT("'%s' (%s) expects %s but was given %s."), *Where, *Property->GetCPPType(), Expected, JsonTypeName(Type));
				return false;
			};

			if (CastField<FBoolProperty>(Property))
			{
				return (Type == EJson::Boolean || Type == EJson::Number || Type == EJson::String) ? true : Fail(TEXT("true or false"));
			}

			const UEnum* Enum = nullptr;
			if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
			{
				Enum = EnumProperty->GetEnum();
			}
			else if (const FByteProperty* ByteProperty = CastField<FByteProperty>(Property))
			{
				Enum = ByteProperty->Enum;
			}
			if (Enum)
			{
				if (Type == EJson::Number)
				{
					return true;
				}
				if (Type == EJson::String)
				{
					if (Enum->GetValueByNameString(Value->AsString()) != INDEX_NONE)
					{
						return true;
					}
					TArray<FString> Names;
					for (int32 i = 0; i < Enum->NumEnums() - 1 && Names.Num() < 40; ++i)
					{
						Names.Add(Enum->GetNameStringByIndex(i));
					}
					OutError = FString::Printf(TEXT("'%s' is not a value of %s for '%s'. Values: %s"), *Value->AsString(), *Enum->GetName(), *Where, *FString::Join(Names, TEXT(", ")));
					return false;
				}
				return Fail(TEXT("an enum value name"));
			}

			if (CastField<FNumericProperty>(Property))
			{
				if (Type == EJson::Number || (Type == EJson::String && FCString::IsNumeric(*Value->AsString())))
				{
					return true;
				}
				return Fail(TEXT("a number"));
			}
			if (CastField<FStrProperty>(Property) || CastField<FNameProperty>(Property) || CastField<FTextProperty>(Property))
			{
				return (Type == EJson::String || Type == EJson::Number || Type == EJson::Boolean) ? true : Fail(TEXT("a string"));
			}
			if (const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
			{
				if (Type != EJson::Array)
				{
					return Fail(TEXT("an array"));
				}
				const TArray<TSharedPtr<FJsonValue>>& Items = Value->AsArray();
				for (int32 i = 0; i < Items.Num(); ++i)
				{
					if (!CheckJsonFits(ArrayProperty->Inner, Items[i], FString::Printf(TEXT("%s[%d]"), *Where, i), OutError))
					{
						return false;
					}
				}
				return true;
			}
			if (const FSetProperty* SetProperty = CastField<FSetProperty>(Property))
			{
				if (Type != EJson::Array)
				{
					return Fail(TEXT("an array"));
				}
				const TArray<TSharedPtr<FJsonValue>>& Items = Value->AsArray();
				for (int32 i = 0; i < Items.Num(); ++i)
				{
					if (!CheckJsonFits(SetProperty->ElementProp, Items[i], FString::Printf(TEXT("%s[%d]"), *Where, i), OutError))
					{
						return false;
					}
				}
				return true;
			}
			if (const FMapProperty* MapProperty = CastField<FMapProperty>(Property))
			{
				if (Type != EJson::Object)
				{
					return Fail(TEXT("an object of key: value pairs"));
				}
				for (const auto& Pair : Value->AsObject()->Values)
				{
					if (!CheckJsonFits(MapProperty->ValueProp, Pair.Value, Where + TEXT(".") + Pair.Key, OutError))
					{
						return false;
					}
				}
				return true;
			}
			if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
			{
				if (Type == EJson::String)
				{
					return true; // Unreal's text form, "(X=1,Y=2,Z=3)"; checked when it is imported.
				}
				if (Type != EJson::Object)
				{
					return Fail(TEXT("an object with the struct's members"));
				}
				for (const auto& Pair : Value->AsObject()->Values)
				{
					const FProperty* Member = FindPropertyByName(StructProperty->Struct, Pair.Key);
					if (!Member)
					{
						TArray<FString> Members;
						for (TFieldIterator<FProperty> It(StructProperty->Struct); It && Members.Num() < 40; ++It)
						{
							Members.Add(It->GetName());
						}
						OutError = FString::Printf(TEXT("%s has no member '%s' (in '%s'). Members: %s"),
							*StructProperty->Struct->GetName(), *Pair.Key, *Where, *FString::Join(Members, TEXT(", ")));
						return false;
					}
					if (!CheckJsonFits(Member, Pair.Value, Where + TEXT(".") + Member->GetName(), OutError))
					{
						return false;
					}
				}
				return true;
			}
			if (CastField<FObjectPropertyBase>(Property))
			{
				return (Type == EJson::String || Type == EJson::Null || Type == EJson::Object) ? true : Fail(TEXT("an asset path, an actor name, or null"));
			}
			return true;
		}

		/**
		 * Object references given as text are resolved here rather than by the converter, so that a
		 * path that does not exist is an error instead of a silent null, and actors can be named by label.
		 * @return true when the reference was handled (successfully or not; see OutError).
		 */
		bool TrySetObjectReference(FProperty* Leaf, void* ValuePtr, const TSharedPtr<FJsonValue>& Value, const FString& Where, FString& OutError)
		{
			const FObjectProperty* ObjectProperty = CastField<FObjectProperty>(Leaf);
			if (!ObjectProperty)
			{
				return false;
			}
			if (Value->Type == EJson::Null)
			{
				ObjectProperty->SetObjectPropertyValue(ValuePtr, nullptr);
				return true;
			}
			if (Value->Type != EJson::String)
			{
				return false;
			}
			const FString Text = Value->AsString().TrimStartAndEnd();
			if (Text.IsEmpty() || Text.Equals(TEXT("None"), ESearchCase::IgnoreCase))
			{
				ObjectProperty->SetObjectPropertyValue(ValuePtr, nullptr);
				return true;
			}

			UObject* Target = nullptr;
			FString ResolveError;
			if (const FClassProperty* ClassProperty = CastField<FClassProperty>(Leaf))
			{
				Target = ResolveClass(Text, ClassProperty->MetaClass, ResolveError);
			}
			else
			{
				Target = ResolveObject(Text, ResolveError);
				// A Blueprint asset path where a class-typed object is wanted is a common slip; say so.
				if (Target && !Target->IsA(ObjectProperty->PropertyClass))
				{
					ResolveError = FString::Printf(TEXT("'%s' is a %s, but '%s' needs a %s."),
						*Text, *Target->GetClass()->GetName(), *Where, *ObjectProperty->PropertyClass->GetName());
					Target = nullptr;
				}
			}
			if (!Target)
			{
				OutError = ResolveError;
				return true;
			}
			ObjectProperty->SetObjectPropertyValue(ValuePtr, Target);
			return true;
		}
	}

	bool WriteProperty(UObject* Object, const FString& PropertyPath, const TSharedPtr<FJsonValue>& Value, FString& OutError)
	{
		if (!Object)
		{
			OutError = TEXT("No object.");
			return false;
		}
		if (!Value.IsValid())
		{
			OutError = TEXT("No value given.");
			return false;
		}

		TArray<FString> Segments;
		PropertyPath.ParseIntoArray(Segments, TEXT("."), true);
		if (Segments.Num() == 0)
		{
			OutError = TEXT("Property name is empty.");
			return false;
		}

		// Walk from the object into nested structs.
		const UStruct* Scope = Object->GetClass();
		void* Container = Object;
		TArray<FProperty*> Chain;
		for (int32 i = 0; i < Segments.Num(); ++i)
		{
			FProperty* Property = FindPropertyByName(Scope, Segments[i]);
			if (!Property)
			{
				OutError = FString::Printf(TEXT("'%s' has no property '%s'. Use object_get_properties to see the names."), *Scope->GetName(), *Segments[i]);
				return false;
			}
			Chain.Add(Property);

			if (i + 1 < Segments.Num())
			{
				const FStructProperty* StructProperty = CastField<FStructProperty>(Property);
				if (const FObjectProperty* ObjectProperty = CastField<FObjectProperty>(Property))
				{
					// Something this object owns and shows in its own Details panel (a brush's builder, an actor's
					// component) is edited through it there; follow the reference the same way. Anything it merely
					// points at (a mesh, another actor) has to be addressed as itself, so an edit never lands on a
					// shared asset by accident.
					UObject* Inner = ObjectProperty->GetObjectPropertyValue(Property->ContainerPtrToValuePtr<void>(Container));
					TArray<FString> Rest;
					for (int32 j = i + 1; j < Segments.Num(); ++j)
					{
						Rest.Add(Segments[j]);
					}
					if (!Inner)
					{
						OutError = FString::Printf(TEXT("'%s' is empty on this object, so '%s' cannot be reached through it."), *Segments[i], *Segments[i + 1]);
						return false;
					}
					if (!Inner->IsIn(Object) || !Chain[0]->HasAnyPropertyFlags(CPF_Edit))
					{
						OutError = FString::Printf(TEXT("'%s' refers to another object. Set '%s' on that object itself: %s"), *Segments[i], *FString::Join(Rest, TEXT(".")), *Inner->GetPathName());
						return false;
					}
					return WriteProperty(Inner, FString::Join(Rest, TEXT(".")), Value, OutError);
				}
				if (!StructProperty)
				{
					OutError = FString::Printf(TEXT("'%s' is not a struct, so '%s' cannot be reached through it. To change a component's property, address the component (Actor.Component)."),
						*Segments[i], *Segments[i + 1]);
					return false;
				}
				Container = Property->ContainerPtrToValuePtr<void>(Container);
				Scope = StructProperty->Struct;
			}
		}

		FProperty* Top = Chain[0];
		FProperty* Leaf = Chain.Last();
		if (!Top->HasAnyPropertyFlags(CPF_Edit) || Top->HasAnyPropertyFlags(CPF_EditConst))
		{
			OutError = FString::Printf(TEXT("'%s' is not editable in the Details panel, so it is not set here either. If it really must change, use python_exec."), *Top->GetName());
			return false;
		}

		// Check the value before telling the object it is about to change.
		if (!CheckJsonFits(Leaf, Value, PropertyPath, OutError))
		{
			return false;
		}

		FEditPropertyChain EditChain;
		for (FProperty* Property : Chain)
		{
			EditChain.AddTail(Property);
		}
		EditChain.SetActiveMemberPropertyNode(Top);
		EditChain.SetActivePropertyNode(Leaf);

		Object->Modify();
		Object->PreEditChange(EditChain);

		FText FailReason;
		void* ValuePtr = Leaf->ContainerPtrToValuePtr<void>(Container);
		FString ReferenceError;
		bool bOk = false;
		if (TrySetObjectReference(Leaf, ValuePtr, Value, PropertyPath, ReferenceError))
		{
			bOk = ReferenceError.IsEmpty();
			FailReason = FText::FromString(ReferenceError);
		}
		else
		{
			bOk = FJsonObjectConverter::JsonValueToUProperty(Value, Leaf, ValuePtr, 0, 0, /*bStrictMode*/ false, &FailReason);
		}

		// Always pair the notifications, even on failure, so the object is not left mid-edit.
		FPropertyChangedEvent ChangedEvent(Leaf, EPropertyChangeType::ValueSet);
		ChangedEvent.SetActiveMemberProperty(Top);
		FPropertyChangedChainEvent ChainEvent(EditChain, ChangedEvent);
		Object->PostEditChangeChainProperty(ChainEvent);

		if (!bOk)
		{
			OutError = FString::Printf(TEXT("Could not set '%s' (%s): %s"), *PropertyPath, *Leaf->GetCPPType(),
				FailReason.IsEmpty() ? TEXT("the value does not fit the property type.") : *FailReason.ToString());
			return false;
		}
		return true;
	}
}
