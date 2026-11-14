#include "CursorToolPacks.h"

#include "CursorJson.h"
#include "CursorPackSupport.h"
#include "CursorTool.h"
#include "CursorToolHelpers.h"
#include "CursorToolRegistry.h"
#include "Engine/Texture.h"
#include "Factories/MaterialFactoryNew.h"
#include "Factories/MaterialInstanceConstantFactoryNew.h"
#include "FileHelpers.h"
#include "MaterialEditingLibrary.h"
#include "MaterialShared.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionTextureBase.h"
#include "Materials/MaterialExpressionTextureSampleParameter.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialInstanceConstant.h"
#include "RHIDefinitions.h"
#include "UObject/UObjectIterator.h"

/*
 * Material pack: create a material or instance, build a small expression graph, connect it to the material's
 * inputs, set parameters, compile. Everything is reached through UMaterialEditingLibrary and the public
 * expression classes. Material functions, layers and custom HLSL are out of scope; python_exec covers them.
 * Edits stay in memory until material_compile with save=true, or asset_save.
 */

namespace
{
	const TCHAR* MaterialPack = TEXT("materials");

	TSharedPtr<FJsonValue> AsValue(const TSharedRef<FJsonObject>& Object)
	{
		return MakeShared<FJsonValueObject>(Object);
	}

	/** Names accepted by material_connect for the material's own inputs. */
	struct FPropertyName
	{
		const TCHAR* Name;
		EMaterialProperty Property;
	};
	const FPropertyName PropertyNames[] =
	{
		{ TEXT("BaseColor"), MP_BaseColor },
		{ TEXT("Metallic"), MP_Metallic },
		{ TEXT("Specular"), MP_Specular },
		{ TEXT("Roughness"), MP_Roughness },
		{ TEXT("Anisotropy"), MP_Anisotropy },
		{ TEXT("EmissiveColor"), MP_EmissiveColor },
		{ TEXT("Opacity"), MP_Opacity },
		{ TEXT("OpacityMask"), MP_OpacityMask },
		{ TEXT("Normal"), MP_Normal },
		{ TEXT("WorldPositionOffset"), MP_WorldPositionOffset },
		{ TEXT("SubsurfaceColor"), MP_SubsurfaceColor },
		{ TEXT("AmbientOcclusion"), MP_AmbientOcclusion },
		{ TEXT("Refraction"), MP_Refraction },
		{ TEXT("PixelDepthOffset"), MP_PixelDepthOffset },
	};

	FString PropertyNameList()
	{
		TArray<FString> Names;
		for (const FPropertyName& Entry : PropertyNames)
		{
			Names.Add(Entry.Name);
		}
		return FString::Join(Names, TEXT(", "));
	}

	bool FindProperty(const FString& Name, EMaterialProperty& Out)
	{
		for (const FPropertyName& Entry : PropertyNames)
		{
			if (Name.Equals(Entry.Name, ESearchCase::IgnoreCase))
			{
				Out = Entry.Property;
				return true;
			}
		}
		return false;
	}

	UMaterialInterface* LoadMaterialInterface(const FString& Path, bool bForEditing, FString& OutError)
	{
		if (Path.IsEmpty())
		{
			OutError = TEXT("'material' is required: the asset path of a material or material instance.");
			return nullptr;
		}
		UObject* Asset = CursorToolHelpers::LoadAsset(Path, OutError);
		if (!Asset)
		{
			return nullptr;
		}
		UMaterialInterface* Material = Cast<UMaterialInterface>(Asset);
		if (!Material)
		{
			OutError = FString::Printf(TEXT("'%s' is a %s, not a material."), *Path, *Asset->GetClass()->GetName());
			return nullptr;
		}
		if (bForEditing && !CursorPackSupport::IsWritableContentPath(Material->GetOutermost()->GetName(), OutError))
		{
			return nullptr;
		}
		return Material;
	}

	/** A base material (not an instance) the caller may edit. */
	UMaterial* LoadEditableBaseMaterial(const FString& Path, FString& OutError)
	{
		UMaterialInterface* Interface = LoadMaterialInterface(Path, true, OutError);
		if (!Interface)
		{
			return nullptr;
		}
		UMaterial* Material = Cast<UMaterial>(Interface);
		if (!Material)
		{
			OutError = FString::Printf(TEXT("'%s' is a material instance. It has parameters but no expression graph; edit its parent or use material_set_parameter."), *Path);
		}
		return Material;
	}

	UMaterialExpression* FindExpression(UMaterial* Material, const FString& Name, FString& OutError)
	{
		if (Name.IsEmpty())
		{
			OutError = TEXT("An expression name is required (the 'name' reported by material_get_info or material_add_expression).");
			return nullptr;
		}
		for (UMaterialExpression* Expression : Material->GetExpressions())
		{
			if (Expression && Expression->GetName().Equals(Name, ESearchCase::IgnoreCase))
			{
				return Expression;
			}
		}
		OutError = FString::Printf(TEXT("No expression named '%s'. Use material_get_info to list them."), *Name);
		return nullptr;
	}

	/** "Multiply" or "MaterialExpressionMultiply". */
	UClass* ResolveExpressionClass(const FString& Type, FString& OutError)
	{
		const FString Wanted = Type.TrimStartAndEnd();
		UClass* Match = nullptr;
		for (TObjectIterator<UClass> It; It; ++It)
		{
			UClass* Class = *It;
			if (!Class->IsChildOf(UMaterialExpression::StaticClass()) || Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
			{
				continue;
			}
			const FString Name = Class->GetName();
			if (Name.Equals(Wanted, ESearchCase::IgnoreCase) || Name.Equals(TEXT("MaterialExpression") + Wanted, ESearchCase::IgnoreCase))
			{
				Match = Class;
				break;
			}
		}
		if (!Match)
		{
			OutError = FString::Printf(TEXT("No material expression '%s'. Common ones: Constant, Constant3Vector, ScalarParameter, VectorParameter, TextureSample, TextureSampleParameter2D, "
				"TextureCoordinate, Multiply, Add, Subtract, Divide, Lerp, OneMinus, Power, Clamp, Saturate, Time, Panner, Fresnel."), *Wanted);
		}
		return Match;
	}

	FString ShortClassName(const UMaterialExpression* Expression)
	{
		return Expression->GetClass()->GetName().Replace(TEXT("MaterialExpression"), TEXT(""));
	}

	TArray<TSharedPtr<FJsonValue>> ColorToJson(const FLinearColor& Color, bool bWithAlpha)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		Out.Add(MakeShared<FJsonValueNumber>(Color.R));
		Out.Add(MakeShared<FJsonValueNumber>(Color.G));
		Out.Add(MakeShared<FJsonValueNumber>(Color.B));
		if (bWithAlpha)
		{
			Out.Add(MakeShared<FJsonValueNumber>(Color.A));
		}
		return Out;
	}

	/** [r,g,b] / [r,g,b,a] / {r,g,b,a}. Alpha defaults to 1. */
	bool ParseColor(const TSharedPtr<FJsonValue>& Value, FLinearColor& Out)
	{
		if (!Value.IsValid())
		{
			return false;
		}
		double C[4] = { 0, 0, 0, 1 };
		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		const TSharedPtr<FJsonObject>* Object = nullptr;
		if (Value->TryGetArray(Array) && Array->Num() >= 3 && Array->Num() <= 4)
		{
			for (int32 i = 0; i < Array->Num(); ++i)
			{
				if (!(*Array)[i]->TryGetNumber(C[i]))
				{
					return false;
				}
			}
		}
		else if (Value->TryGetObject(Object))
		{
			static const TCHAR* Keys[4] = { TEXT("r"), TEXT("g"), TEXT("b"), TEXT("a") };
			for (int32 i = 0; i < 4; ++i)
			{
				if (!(*Object)->TryGetNumberField(Keys[i], C[i]) && !(*Object)->TryGetNumberField(FString(Keys[i]).ToUpper(), C[i]) && i < 3)
				{
					return false;
				}
			}
		}
		else
		{
			return false;
		}
		Out = FLinearColor(static_cast<float>(C[0]), static_cast<float>(C[1]), static_cast<float>(C[2]), static_cast<float>(C[3]));
		return true;
	}

	TSharedRef<FJsonObject> DescribeExpression(const UMaterialExpression* Expression)
	{
		UMaterialExpression* Mutable = const_cast<UMaterialExpression*>(Expression);
		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetStringField(TEXT("name"), Expression->GetName());
		Out->SetStringField(TEXT("type"), ShortClassName(Expression));
		Out->SetNumberField(TEXT("x"), Expression->MaterialExpressionEditorX);
		Out->SetNumberField(TEXT("y"), Expression->MaterialExpressionEditorY);

		if (Expression->HasAParameterName())
		{
			Out->SetStringField(TEXT("parameter_name"), Expression->GetParameterName().ToString());
		}
		if (const UMaterialExpressionScalarParameter* Scalar = Cast<UMaterialExpressionScalarParameter>(Expression))
		{
			Out->SetNumberField(TEXT("default_value"), Scalar->DefaultValue);
		}
		else if (const UMaterialExpressionVectorParameter* Vector = Cast<UMaterialExpressionVectorParameter>(Expression))
		{
			Out->SetArrayField(TEXT("default_value"), ColorToJson(Vector->DefaultValue, true));
		}
		else if (const UMaterialExpressionConstant* Constant = Cast<UMaterialExpressionConstant>(Expression))
		{
			Out->SetNumberField(TEXT("value"), Constant->R);
		}
		else if (const UMaterialExpressionConstant3Vector* Constant3 = Cast<UMaterialExpressionConstant3Vector>(Expression))
		{
			Out->SetArrayField(TEXT("value"), ColorToJson(Constant3->Constant, false));
		}
		if (const UMaterialExpressionTextureBase* TextureBase = Cast<UMaterialExpressionTextureBase>(Expression))
		{
			if (TextureBase->Texture)
			{
				Out->SetStringField(TEXT("texture"), TextureBase->Texture->GetPathName());
			}
		}

		TArray<TSharedPtr<FJsonValue>> Inputs;
		for (int32 i = 0; FExpressionInput* Input = Mutable->GetInput(i); ++i)
		{
			TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("name"), Mutable->GetInputName(i).ToString());
			if (Input->Expression)
			{
				Item->SetStringField(TEXT("from"), Input->Expression->GetName());
				Item->SetNumberField(TEXT("from_output_index"), Input->OutputIndex);
			}
			Inputs.Add(AsValue(Item));
		}
		Out->SetArrayField(TEXT("inputs"), Inputs);

		TArray<FString> Outputs;
		for (const FExpressionOutput& Output : Mutable->GetOutputs())
		{
			Outputs.Add(Output.OutputName.ToString());
		}
		Out->SetArrayField(TEXT("outputs"), CursorJson::StringArray(Outputs));
		return Out;
	}

	TArray<FString> InputNames(UMaterialExpression* Expression)
	{
		return UMaterialEditingLibrary::GetMaterialExpressionInputNames(Expression);
	}

	TArray<FString> OutputNames(UMaterialExpression* Expression)
	{
		TArray<FString> Names;
		for (const FExpressionOutput& Output : Expression->GetOutputs())
		{
			Names.Add(Output.OutputName.ToString());
		}
		return Names;
	}

	TArray<FString> NamesToStrings(const TArray<FName>& Names)
	{
		TArray<FString> Out;
		for (const FName& Name : Names)
		{
			Out.Add(Name.ToString());
		}
		return Out;
	}

	/** Applies a convenience 'value' to a freshly made or existing expression. Returns false with a reason when the value does not fit. */
	bool ApplyExpressionValue(UMaterialExpression* Expression, const TSharedPtr<FJsonValue>& Value, FString& OutError)
	{
		if (!Value.IsValid() || Value->IsNull())
		{
			return true;
		}
		FLinearColor Color;
		double Number = 0.0;
		if (UMaterialExpressionScalarParameter* Scalar = Cast<UMaterialExpressionScalarParameter>(Expression))
		{
			if (!Value->TryGetNumber(Number)) { OutError = TEXT("A scalar parameter takes a number."); return false; }
			Scalar->DefaultValue = static_cast<float>(Number);
		}
		else if (UMaterialExpressionVectorParameter* Vector = Cast<UMaterialExpressionVectorParameter>(Expression))
		{
			if (!ParseColor(Value, Color)) { OutError = TEXT("A vector parameter takes [r,g,b] or [r,g,b,a]."); return false; }
			Vector->DefaultValue = Color;
		}
		else if (UMaterialExpressionConstant* Constant = Cast<UMaterialExpressionConstant>(Expression))
		{
			if (!Value->TryGetNumber(Number)) { OutError = TEXT("Constant takes a number."); return false; }
			Constant->R = static_cast<float>(Number);
		}
		else if (UMaterialExpressionConstant3Vector* Constant3 = Cast<UMaterialExpressionConstant3Vector>(Expression))
		{
			if (!ParseColor(Value, Color)) { OutError = TEXT("Constant3Vector takes [r,g,b]."); return false; }
			Constant3->Constant = Color;
		}
		else if (UMaterialExpressionTextureBase* TextureBase = Cast<UMaterialExpressionTextureBase>(Expression))
		{
			FString Path;
			if (!Value->TryGetString(Path)) { OutError = TEXT("A texture expression takes a texture asset path."); return false; }
			UTexture* Texture = Cast<UTexture>(CursorToolHelpers::LoadAsset(Path, OutError));
			if (!Texture)
			{
				if (OutError.IsEmpty()) { OutError = FString::Printf(TEXT("'%s' is not a texture."), *Path); }
				return false;
			}
			TextureBase->Texture = Texture;
		}
		else
		{
			OutError = FString::Printf(TEXT("'value' does not apply to %s. Use 'properties' to set its fields."), *ShortClassName(Expression));
			return false;
		}
		return true;
	}

	TArray<FString> CompileErrors(UMaterial* Material)
	{
		TArray<FString> Errors;
		if (const FMaterialResource* Resource = Material->GetMaterialResource(GMaxRHIFeatureLevel))
		{
			Errors = Resource->GetCompileErrors();
		}
		return Errors;
	}
}

void CursorToolPacks::RegisterMaterialPack()
{
	FCursorToolBuilder(TEXT("material_create"), MaterialPack)
		.Describe(TEXT("Create an empty material. Add expressions with material_add_expression, connect them with material_connect, then material_compile. "
			"The material exists in memory until material_compile save=true or asset_save writes it."))
		.Risk(ECursorToolRisk::Mutating)
		.Schema(FCursorSchema()
			.Str(TEXT("path"), TEXT("Where to create it, for example /Game/Materials/M_Rock."), true)
			.Enum(TEXT("blend_mode"), TEXT("Default opaque."), { TEXT("opaque"), TEXT("masked"), TEXT("translucent"), TEXT("additive"), TEXT("modulate") })
			.Enum(TEXT("shading_model"), TEXT("Default default_lit."), { TEXT("default_lit"), TEXT("unlit") })
			.Bool(TEXT("two_sided"), TEXT("Render both faces. Default false.")))
		.Run([](const FCursorArgs& Args, const FCursorToolContext&)
		{
			FString Error;
			UPackage* Package = nullptr;
			FString AssetName;
			if (!CursorPackSupport::PrepareNewAsset(Args.Str(TEXT("path")), Package, AssetName, Error))
			{
				return FCursorToolResult::Error(Error);
			}

			EBlendMode Blend = BLEND_Opaque;
			const FString BlendName = Args.Str(TEXT("blend_mode"), TEXT("opaque")).ToLower();
			if (BlendName == TEXT("masked")) { Blend = BLEND_Masked; }
			else if (BlendName == TEXT("translucent")) { Blend = BLEND_Translucent; }
			else if (BlendName == TEXT("additive")) { Blend = BLEND_Additive; }
			else if (BlendName == TEXT("modulate")) { Blend = BLEND_Modulate; }
			else if (BlendName != TEXT("opaque")) { return FCursorToolResult::Error(FString::Printf(TEXT("Unknown blend_mode '%s'."), *BlendName)); }

			const FString Shading = Args.Str(TEXT("shading_model"), TEXT("default_lit")).ToLower();
			if (Shading != TEXT("default_lit") && Shading != TEXT("unlit"))
			{
				return FCursorToolResult::Error(FString::Printf(TEXT("Unknown shading_model '%s'."), *Shading));
			}

			UMaterialFactoryNew* Factory = NewObject<UMaterialFactoryNew>();
			UMaterial* Material = Cast<UMaterial>(Factory->FactoryCreateNew(UMaterial::StaticClass(), Package, FName(*AssetName),
				RF_Public | RF_Standalone | RF_Transactional, nullptr, GWarn));
			if (!Material)
			{
				return FCursorToolResult::Error(TEXT("The editor could not create the material. See editor_log_tail."));
			}
			Material->BlendMode = Blend;
			Material->TwoSided = Args.Bool(TEXT("two_sided"));
			if (Shading == TEXT("unlit"))
			{
				Material->SetShadingModel(MSM_Unlit);
			}
			CursorPackSupport::FinishNewAsset(Material);

			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetStringField(TEXT("path"), Material->GetPathName());
			Out->SetStringField(TEXT("note"), TEXT("Created in memory. Add expressions, connect them, then call material_compile."));
			return FCursorToolResult::OkJson(Out);
		})
		.Register();

	FCursorToolBuilder(TEXT("material_create_instance"), MaterialPack)
		.Describe(TEXT("Create a material instance from a parent material. Change its parameters with material_set_parameter. Cheaper to iterate on than editing the parent."))
		.Risk(ECursorToolRisk::Mutating)
		.Schema(FCursorSchema()
			.Str(TEXT("path"), TEXT("Where to create it, for example /Game/Materials/MI_Rock_Wet."), true)
			.Str(TEXT("parent"), TEXT("Parent material or instance asset path."), true))
		.Run([](const FCursorArgs& Args, const FCursorToolContext&)
		{
			FString Error;
			UMaterialInterface* Parent = LoadMaterialInterface(Args.Str(TEXT("parent")), false, Error);
			if (!Parent)
			{
				return FCursorToolResult::Error(Error);
			}
			UPackage* Package = nullptr;
			FString AssetName;
			if (!CursorPackSupport::PrepareNewAsset(Args.Str(TEXT("path")), Package, AssetName, Error))
			{
				return FCursorToolResult::Error(Error);
			}
			UMaterialInstanceConstantFactoryNew* Factory = NewObject<UMaterialInstanceConstantFactoryNew>();
			Factory->InitialParent = Parent;
			UMaterialInstanceConstant* Instance = Cast<UMaterialInstanceConstant>(Factory->FactoryCreateNew(UMaterialInstanceConstant::StaticClass(), Package, FName(*AssetName),
				RF_Public | RF_Standalone | RF_Transactional, nullptr, GWarn));
			if (!Instance)
			{
				return FCursorToolResult::Error(TEXT("The editor could not create the material instance. See editor_log_tail."));
			}
			CursorPackSupport::FinishNewAsset(Instance);

			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetStringField(TEXT("path"), Instance->GetPathName());
			Out->SetStringField(TEXT("parent"), Parent->GetPathName());
			return FCursorToolResult::OkJson(Out);
		})
		.Register();

	FCursorToolBuilder(TEXT("material_get_info"), MaterialPack)
		.Describe(TEXT("Inspect a material or material instance. A material returns its settings, every expression (type, parameter name and value, inputs and what feeds them, outputs) "
			"and which expression drives each material input. An instance returns its parent and overridden values. Both list the scalar, vector and texture parameter names."))
		.Risk(ECursorToolRisk::ReadOnly)
		.Schema(FCursorSchema()
			.Str(TEXT("material"), TEXT("Material or material instance asset path."), true))
		.Run([](const FCursorArgs& Args, const FCursorToolContext&)
		{
			FString Error;
			UMaterialInterface* Interface = LoadMaterialInterface(Args.Str(TEXT("material")), false, Error);
			if (!Interface)
			{
				return FCursorToolResult::Error(Error);
			}

			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetStringField(TEXT("path"), Interface->GetPathName());
			Out->SetStringField(TEXT("class"), Interface->GetClass()->GetName());
			Out->SetBoolField(TEXT("has_unsaved_changes"), Interface->GetOutermost()->IsDirty());

			if (UMaterial* Material = Cast<UMaterial>(Interface))
			{
				Out->SetStringField(TEXT("blend_mode"), StaticEnum<EBlendMode>()->GetNameStringByValue(Material->BlendMode));
				Out->SetBoolField(TEXT("two_sided"), Material->IsTwoSided());

				TArray<TSharedPtr<FJsonValue>> Expressions;
				for (const UMaterialExpression* Expression : Material->GetExpressions())
				{
					if (Expression)
					{
						Expressions.Add(AsValue(DescribeExpression(Expression)));
					}
				}
				Out->SetArrayField(TEXT("expressions"), Expressions);

				TSharedRef<FJsonObject> Connections = MakeShared<FJsonObject>();
				for (const FPropertyName& Entry : PropertyNames)
				{
					if (const FExpressionInput* Input = Material->GetExpressionInputForProperty(Entry.Property))
					{
						if (Input->Expression)
						{
							Connections->SetStringField(Entry.Name, Input->Expression->GetName());
						}
					}
				}
				Out->SetObjectField(TEXT("connected_inputs"), Connections);
			}
			else if (const UMaterialInstance* Instance = Cast<UMaterialInstance>(Interface))
			{
				Out->SetStringField(TEXT("parent"), Instance->Parent ? Instance->Parent->GetPathName() : FString());

				TSharedRef<FJsonObject> Overrides = MakeShared<FJsonObject>();
				for (const FScalarParameterValue& Value : Instance->ScalarParameterValues)
				{
					Overrides->SetNumberField(Value.ParameterInfo.Name.ToString(), Value.ParameterValue);
				}
				for (const FVectorParameterValue& Value : Instance->VectorParameterValues)
				{
					Overrides->SetArrayField(Value.ParameterInfo.Name.ToString(), ColorToJson(Value.ParameterValue, true));
				}
				for (const FTextureParameterValue& Value : Instance->TextureParameterValues)
				{
					Overrides->SetStringField(Value.ParameterInfo.Name.ToString(), Value.ParameterValue ? Value.ParameterValue->GetPathName() : FString());
				}
				Out->SetObjectField(TEXT("overrides"), Overrides);
			}

			TArray<FName> Names;
			TSharedRef<FJsonObject> Parameters = MakeShared<FJsonObject>();
			UMaterialEditingLibrary::GetScalarParameterNames(Interface, Names);
			Parameters->SetArrayField(TEXT("scalar"), CursorJson::StringArray(NamesToStrings(Names)));
			Names.Reset();
			UMaterialEditingLibrary::GetVectorParameterNames(Interface, Names);
			Parameters->SetArrayField(TEXT("vector"), CursorJson::StringArray(NamesToStrings(Names)));
			Names.Reset();
			UMaterialEditingLibrary::GetTextureParameterNames(Interface, Names);
			Parameters->SetArrayField(TEXT("texture"), CursorJson::StringArray(NamesToStrings(Names)));
			Out->SetObjectField(TEXT("parameters"), Parameters);
			return FCursorToolResult::OkJson(Out);
		})
		.Register();

	FCursorToolBuilder(TEXT("material_add_expression"), MaterialPack)
		.Describe(TEXT("Add an expression node to a material graph and return it with its input and output names. Examples: ScalarParameter (name, value=0.5), VectorParameter (name, value=[1,0,0,1]), "
			"Constant (value), Constant3Vector (value=[r,g,b]), TextureSampleParameter2D (name, value=<texture path>), TextureSample, Multiply, Add, Lerp, TextureCoordinate. "
			"Use 'properties' to set any other field on the node."))
		.Risk(ECursorToolRisk::Mutating)
		.Schema(FCursorSchema()
			.Str(TEXT("material"), TEXT("Material asset path (not an instance)."), true)
			.Str(TEXT("type"), TEXT("Expression type, with or without the MaterialExpression prefix."), true)
			.Str(TEXT("name"), TEXT("Parameter name, for parameter expressions."))
			.Any(TEXT("value"), TEXT("Default value: a number, [r,g,b(,a)], or a texture asset path, depending on the type."))
			.Obj(TEXT("properties"), TEXT("Other properties to set by name, for example {\"Coordinate\": 1}."))
			.Int(TEXT("x"), TEXT("Node X position."))
			.Int(TEXT("y"), TEXT("Node Y position.")))
		.Run([](const FCursorArgs& Args, const FCursorToolContext&)
		{
			FString Error;
			UMaterial* Material = LoadEditableBaseMaterial(Args.Str(TEXT("material")), Error);
			if (!Material)
			{
				return FCursorToolResult::Error(Error);
			}
			UClass* Class = ResolveExpressionClass(Args.Str(TEXT("type")), Error);
			if (!Class)
			{
				return FCursorToolResult::Error(Error);
			}

			// Check what can be checked before touching the material, so a bad argument leaves nothing half-made.
			const FString ParameterName = Args.Str(TEXT("name"));
			if (!ParameterName.IsEmpty())
			{
				const UMaterialExpression* Default = Class->GetDefaultObject<UMaterialExpression>();
				if (!Default->HasAParameterName())
				{
					return FCursorToolResult::Error(FString::Printf(TEXT("%s is not a parameter, so 'name' does not apply."), *Class->GetName()));
				}
			}

			const int32 X = Args.Has(TEXT("x")) ? Args.Int(TEXT("x")) : -400;
			const int32 Y = Args.Has(TEXT("y")) ? Args.Int(TEXT("y")) : Material->GetExpressions().Num() * 160;
			UMaterialExpression* Expression = UMaterialEditingLibrary::CreateMaterialExpression(Material, Class, X, Y);
			if (!Expression)
			{
				return FCursorToolResult::Error(TEXT("The editor could not create the expression."));
			}

			bool bOk = true;
			if (!ParameterName.IsEmpty())
			{
				Expression->SetParameterName(FName(*ParameterName));
			}
			const TSharedPtr<FJsonValue> Value = Args.Raw()->TryGetField(TEXT("value"));
			bOk = ApplyExpressionValue(Expression, Value, Error);
			if (bOk && Args.Has(TEXT("properties")))
			{
				const TSharedPtr<FJsonObject> Properties = Args.Obj(TEXT("properties"));
				for (const auto& Pair : Properties->Values)
				{
					if (!CursorToolHelpers::WriteProperty(Expression, Pair.Key, Pair.Value, Error))
					{
						bOk = false;
						break;
					}
				}
			}
			if (!bOk)
			{
				// Do not leave a half-configured node behind.
				UMaterialEditingLibrary::DeleteMaterialExpression(Material, Expression);
				return FCursorToolResult::Error(Error);
			}
			Material->MarkPackageDirty();
			return FCursorToolResult::OkJson(DescribeExpression(Expression));
		})
		.Register();

	FCursorToolBuilder(TEXT("material_remove_expression"), MaterialPack)
		.Describe(TEXT("Delete an expression node from a material. Connections to and from it are cleared."))
		.Risk(ECursorToolRisk::Mutating)
		.Schema(FCursorSchema()
			.Str(TEXT("material"), TEXT("Material asset path."), true)
			.Str(TEXT("name"), TEXT("Expression name from material_get_info."), true))
		.Run([](const FCursorArgs& Args, const FCursorToolContext&)
		{
			FString Error;
			UMaterial* Material = LoadEditableBaseMaterial(Args.Str(TEXT("material")), Error);
			UMaterialExpression* Expression = Material ? FindExpression(Material, Args.Str(TEXT("name")), Error) : nullptr;
			if (!Expression)
			{
				return FCursorToolResult::Error(Error);
			}
			const FString Name = Expression->GetName();
			UMaterialEditingLibrary::DeleteMaterialExpression(Material, Expression);
			Material->MarkPackageDirty();
			return FCursorToolResult::Ok(FString::Printf(TEXT("Removed %s."), *Name));
		})
		.Register();

	FCursorToolBuilder(TEXT("material_connect"), MaterialPack)
		.Describe(TEXT("Wire an expression's output either into one of the material's own inputs (to_property: BaseColor, Metallic, Specular, Roughness, EmissiveColor, Opacity, OpacityMask, "
			"Normal, WorldPositionOffset, AmbientOcclusion...) or into another expression's input (to_expression + to_input). Names come from material_get_info."))
		.Risk(ECursorToolRisk::Mutating)
		.Schema(FCursorSchema()
			.Str(TEXT("material"), TEXT("Material asset path."), true)
			.Str(TEXT("from"), TEXT("Expression providing the value."), true)
			.Str(TEXT("from_output"), TEXT("Output name, for multi-output nodes such as TextureSample (RGB, R, G, B, A). Default: the first output."))
			.Str(TEXT("to_property"), TEXT("Material input to drive. Use this or to_expression."))
			.Str(TEXT("to_expression"), TEXT("Expression receiving the value. Use this or to_property."))
			.Str(TEXT("to_input"), TEXT("Input name on to_expression (A, B, Alpha...). Default: the first input.")))
		.Run([](const FCursorArgs& Args, const FCursorToolContext&)
		{
			FString Error;
			UMaterial* Material = LoadEditableBaseMaterial(Args.Str(TEXT("material")), Error);
			UMaterialExpression* From = Material ? FindExpression(Material, Args.Str(TEXT("from")), Error) : nullptr;
			if (!From)
			{
				return FCursorToolResult::Error(Error);
			}
			const FString FromOutput = Args.Str(TEXT("from_output"));
			if (!FromOutput.IsEmpty() && CursorPackSupport::IndexOfName(OutputNames(From), FromOutput) == INDEX_NONE)
			{
				return FCursorToolResult::Error(FString::Printf(TEXT("%s has no output '%s'. Outputs: %s"), *From->GetName(), *FromOutput, *FString::Join(OutputNames(From), TEXT(", "))));
			}

			const bool bToProperty = Args.Has(TEXT("to_property"));
			const bool bToExpression = Args.Has(TEXT("to_expression"));
			if (bToProperty == bToExpression)
			{
				return FCursorToolResult::Error(TEXT("Pass exactly one of to_property or to_expression."));
			}

			if (bToProperty)
			{
				EMaterialProperty Property;
				if (!FindProperty(Args.Str(TEXT("to_property")), Property))
				{
					return FCursorToolResult::Error(FString::Printf(TEXT("Unknown material input '%s'. Choose from: %s"), *Args.Str(TEXT("to_property")), *PropertyNameList()));
				}
				Material->Modify();
				if (!UMaterialEditingLibrary::ConnectMaterialProperty(From, FromOutput, Property))
				{
					return FCursorToolResult::Error(FString::Printf(TEXT("Could not connect %s to %s. The input may not apply to this material's blend mode or shading model."),
						*From->GetName(), *Args.Str(TEXT("to_property"))));
				}
				Material->MarkPackageDirty();
				return FCursorToolResult::Ok(FString::Printf(TEXT("Connected %s to %s. Call material_compile to rebuild shaders."), *From->GetName(), *Args.Str(TEXT("to_property"))));
			}

			UMaterialExpression* To = FindExpression(Material, Args.Str(TEXT("to_expression")), Error);
			if (!To)
			{
				return FCursorToolResult::Error(Error);
			}
			const FString ToInput = Args.Str(TEXT("to_input"));
			if (!ToInput.IsEmpty() && CursorPackSupport::IndexOfName(InputNames(To), ToInput) == INDEX_NONE)
			{
				return FCursorToolResult::Error(FString::Printf(TEXT("%s has no input '%s'. Inputs: %s"), *To->GetName(), *ToInput, *FString::Join(InputNames(To), TEXT(", "))));
			}
			if (From == To)
			{
				return FCursorToolResult::Error(TEXT("An expression cannot feed itself."));
			}
			To->Modify();
			if (!UMaterialEditingLibrary::ConnectMaterialExpressions(From, FromOutput, To, ToInput))
			{
				return FCursorToolResult::Error(FString::Printf(TEXT("Could not connect %s to %s."), *From->GetName(), *To->GetName()));
			}
			Material->MarkPackageDirty();
			return FCursorToolResult::Ok(FString::Printf(TEXT("Connected %s to %s.%s. Call material_compile to rebuild shaders."), *From->GetName(), *To->GetName(), *ToInput));
		})
		.Register();

	FCursorToolBuilder(TEXT("material_set_parameter"), MaterialPack)
		.Describe(TEXT("Set a scalar, vector or texture parameter. On a material instance this sets an override. On a base material it changes the default value of the parameter expression "
			"(call material_compile afterwards). The parameter must already exist: see 'parameters' in material_get_info, or add one with material_add_expression."))
		.Risk(ECursorToolRisk::Mutating)
		.Schema(FCursorSchema()
			.Str(TEXT("material"), TEXT("Material or material instance asset path."), true)
			.Str(TEXT("name"), TEXT("Parameter name."), true)
			.Any(TEXT("value"), TEXT("A number (scalar), [r,g,b(,a)] (vector), or a texture asset path (texture)."), true)
			.Enum(TEXT("type"), TEXT("Parameter kind. Default: inferred from the value."), { TEXT("scalar"), TEXT("vector"), TEXT("texture") }))
		.Run([](const FCursorArgs& Args, const FCursorToolContext&)
		{
			FString Error;
			UMaterialInterface* Interface = LoadMaterialInterface(Args.Str(TEXT("material")), true, Error);
			if (!Interface)
			{
				return FCursorToolResult::Error(Error);
			}
			const FString Name = Args.Str(TEXT("name"));
			const TSharedPtr<FJsonValue> Value = Args.Raw()->TryGetField(TEXT("value"));
			if (Name.IsEmpty() || !Value.IsValid() || Value->IsNull())
			{
				return FCursorToolResult::Error(TEXT("'name' and 'value' are required."));
			}

			FString Type = Args.Str(TEXT("type")).ToLower();
			if (Type.IsEmpty())
			{
				Type = Value->Type == EJson::Number ? TEXT("scalar") : Value->Type == EJson::String ? TEXT("texture") : TEXT("vector");
			}

			double Number = 0.0;
			FLinearColor Color;
			UTexture* Texture = nullptr;
			TArray<FName> Existing;
			if (Type == TEXT("scalar"))
			{
				if (!Value->TryGetNumber(Number)) { return FCursorToolResult::Error(TEXT("A scalar parameter takes a number.")); }
				UMaterialEditingLibrary::GetScalarParameterNames(Interface, Existing);
			}
			else if (Type == TEXT("vector"))
			{
				if (!ParseColor(Value, Color)) { return FCursorToolResult::Error(TEXT("A vector parameter takes [r,g,b] or [r,g,b,a].")); }
				UMaterialEditingLibrary::GetVectorParameterNames(Interface, Existing);
			}
			else if (Type == TEXT("texture"))
			{
				FString Path;
				if (!Value->TryGetString(Path)) { return FCursorToolResult::Error(TEXT("A texture parameter takes a texture asset path.")); }
				Texture = Cast<UTexture>(CursorToolHelpers::LoadAsset(Path, Error));
				if (!Texture)
				{
					return FCursorToolResult::Error(Error.IsEmpty() ? FString::Printf(TEXT("'%s' is not a texture."), *Path) : Error);
				}
				UMaterialEditingLibrary::GetTextureParameterNames(Interface, Existing);
			}
			else
			{
				return FCursorToolResult::Error(FString::Printf(TEXT("Unknown type '%s'."), *Type));
			}

			const TArray<FString> ExistingNames = NamesToStrings(Existing);
			const int32 Index = CursorPackSupport::IndexOfName(ExistingNames, Name);
			if (Index == INDEX_NONE)
			{
				return FCursorToolResult::Error(FString::Printf(TEXT("No %s parameter '%s'. Existing %s parameters: %s"), *Type, *Name, *Type,
					ExistingNames.Num() ? *FString::Join(ExistingNames, TEXT(", ")) : TEXT("(none)")));
			}
			const FName ParameterName(*ExistingNames[Index]);

			if (UMaterialInstanceConstant* Instance = Cast<UMaterialInstanceConstant>(Interface))
			{
				Instance->Modify();
				bool bSet = false;
				if (Type == TEXT("scalar")) { bSet = UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(Instance, ParameterName, static_cast<float>(Number)); }
				else if (Type == TEXT("vector")) { bSet = UMaterialEditingLibrary::SetMaterialInstanceVectorParameterValue(Instance, ParameterName, Color); }
				else { bSet = UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(Instance, ParameterName, Texture); }
				if (!bSet)
				{
					return FCursorToolResult::Error(TEXT("The editor refused to set the parameter."));
				}
				Instance->MarkPackageDirty();
				return FCursorToolResult::Ok(FString::Printf(TEXT("Set %s parameter '%s' on %s."), *Type, *ParameterName.ToString(), *Instance->GetName()));
			}

			UMaterial* Material = Cast<UMaterial>(Interface);
			if (!Material)
			{
				return FCursorToolResult::Error(TEXT("Only materials and constant material instances have parameters this tool can set."));
			}
			for (UMaterialExpression* Expression : Material->GetExpressions())
			{
				if (!Expression || !Expression->HasAParameterName() || Expression->GetParameterName() != ParameterName)
				{
					continue;
				}
				Expression->Modify();
				if (UMaterialExpressionScalarParameter* Scalar = Cast<UMaterialExpressionScalarParameter>(Expression); Scalar && Type == TEXT("scalar"))
				{
					Scalar->DefaultValue = static_cast<float>(Number);
				}
				else if (UMaterialExpressionVectorParameter* Vector = Cast<UMaterialExpressionVectorParameter>(Expression); Vector && Type == TEXT("vector"))
				{
					Vector->DefaultValue = Color;
				}
				else if (UMaterialExpressionTextureSampleParameter* TextureParameter = Cast<UMaterialExpressionTextureSampleParameter>(Expression); TextureParameter && Type == TEXT("texture"))
				{
					TextureParameter->Texture = Texture;
				}
				else
				{
					continue;
				}
				Material->MarkPackageDirty();
				return FCursorToolResult::Ok(FString::Printf(TEXT("Set default of %s parameter '%s' on %s. Call material_compile to apply it."), *Type, *ParameterName.ToString(), *Material->GetName()));
			}
			return FCursorToolResult::Error(FString::Printf(TEXT("Parameter '%s' is not driven by a %s parameter expression."), *Name, *Type));
		})
		.Register();

	FCursorToolBuilder(TEXT("material_compile"), MaterialPack)
		.Describe(TEXT("Recompile a material (or refresh an instance) after edits and report compile errors. Shader compilation continues in the background, so errors found only by the shader compiler "
			"can appear later in editor_log_tail. Set save=true to write the asset afterwards."))
		.Risk(ECursorToolRisk::Mutating)
		.Schema(FCursorSchema()
			.Str(TEXT("material"), TEXT("Material or material instance asset path."), true)
			.Bool(TEXT("layout"), TEXT("Tidy the node layout first (materials only). Default false."))
			.Bool(TEXT("save"), TEXT("Save the asset to disk after compiling. Default false.")))
		.Run([](const FCursorArgs& Args, const FCursorToolContext&)
		{
			FString Error;
			UMaterialInterface* Interface = LoadMaterialInterface(Args.Str(TEXT("material")), true, Error);
			if (!Interface)
			{
				return FCursorToolResult::Error(Error);
			}

			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetStringField(TEXT("path"), Interface->GetPathName());
			TArray<FString> Errors;
			if (UMaterial* Material = Cast<UMaterial>(Interface))
			{
				if (Args.Bool(TEXT("layout")))
				{
					UMaterialEditingLibrary::LayoutMaterialExpressions(Material);
				}
				UMaterialEditingLibrary::RecompileMaterial(Material);
				Errors = CompileErrors(Material);
			}
			else if (UMaterialInstanceConstant* Instance = Cast<UMaterialInstanceConstant>(Interface))
			{
				UMaterialEditingLibrary::UpdateMaterialInstance(Instance);
			}
			Out->SetBoolField(TEXT("compiled"), Errors.Num() == 0);
			Out->SetArrayField(TEXT("errors"), CursorJson::StringArray(Errors));

			if (Args.Bool(TEXT("save")))
			{
				UPackage* Package = Interface->GetOutermost();
				const bool bSaved = UEditorLoadingAndSavingUtils::SavePackages({ Package }, /*bOnlyDirty*/ false);
				Out->SetBoolField(TEXT("saved"), bSaved);
				if (!bSaved)
				{
					Out->SetStringField(TEXT("save_note"), TEXT("The package could not be saved (read-only file, source control, or a validation error). See editor_log_tail."));
				}
			}

			FCursorToolResult Result = FCursorToolResult::OkJson(Out);
			Result.bIsError = Errors.Num() > 0;
			return Result;
		})
		.Register();
}

void CursorToolPacks::UnregisterMaterialPack()
{
	FCursorToolRegistry::Get().UnregisterPack(MaterialPack);
}
