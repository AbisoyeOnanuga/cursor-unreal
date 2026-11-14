#include "CursorNiagaraPack.h"

#include "Algo/Reverse.h"
#include "CursorJson.h"
#include "CursorTool.h"
#include "CursorToolHelpers.h"
#include "CursorToolRegistry.h"
#include "Engine/World.h"
#include "FileHelpers.h"
#include "NiagaraActor.h"
#include "NiagaraComponent.h"
#include "NiagaraEditorUtilities.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraGraph.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraNodeOutput.h"
#include "NiagaraRendererProperties.h"
#include "NiagaraScript.h"
#include "NiagaraScriptSource.h"
#include "NiagaraSystem.h"
#include "NiagaraSystemFactoryNew.h"
#include "NiagaraTypes.h"
#include "NiagaraUserRedirectionParameterStore.h"
#include "UObject/Package.h"

/*
 * Niagara pack. Scope is what NiagaraEditor exports reliably in 5.6: create a system (empty, or from template
 * emitters), add / remove / enable emitters, read each emitter's settings, renderers and module stacks, read and
 * set the system's exposed User.* parameters, compile, and drop a system into the level. Building module graphs
 * (adding modules, wiring inputs, rapid-iteration values) goes through NiagaraStackGraphUtilities, which the editor
 * module does not export; use python_exec for that. Edits stay in memory until niagara_compile with save=true, or asset_save.
 */

namespace
{
	const TCHAR* NiagaraPackName = TEXT("niagara");

	/** Emitters named without a path are looked up among the templates that ship with Niagara. */
	const TCHAR* NiagaraTemplateEmitterFolder = TEXT("/Niagara/DefaultAssets/Templates/Emitters/");

	// Helpers carry a Niagara prefix so a unity build that merges every tool file does not collide on names.
	TSharedPtr<FJsonValue> NiagaraJson(const TSharedRef<FJsonObject>& Object)
	{
		return MakeShared<FJsonValueObject>(Object);
	}

	/** A system or emitter asset the caller wants to read. */
	UObject* NiagaraLoadAsset(const FString& Path, FString& OutError)
	{
		if (Path.IsEmpty())
		{
			OutError = TEXT("'system' is required: the asset path of a Niagara system, for example /Game/FX/NS_Sparks.");
			return nullptr;
		}
		UObject* Asset = CursorToolHelpers::LoadAsset(Path, OutError);
		if (Asset && !Asset->IsA<UNiagaraSystem>() && !Asset->IsA<UNiagaraEmitter>())
		{
			OutError = FString::Printf(TEXT("'%s' is a %s, not a Niagara system or emitter."), *Path, *Asset->GetClass()->GetName());
			return nullptr;
		}
		return Asset;
	}

	/** A Niagara system the caller may edit: project content only. */
	UNiagaraSystem* NiagaraLoadEditableSystem(const FString& Path, FString& OutError)
	{
		if (Path.IsEmpty())
		{
			OutError = TEXT("'system' is required: the asset path of a Niagara system, for example /Game/FX/NS_Sparks.");
			return nullptr;
		}
		UObject* Asset = CursorToolHelpers::LoadAsset(Path, OutError);
		if (!Asset)
		{
			return nullptr;
		}
		UNiagaraSystem* System = Cast<UNiagaraSystem>(Asset);
		if (!System)
		{
			OutError = FString::Printf(TEXT("'%s' is a %s, not a Niagara system."), *Path, *Asset->GetClass()->GetName());
			return nullptr;
		}
		if (!CursorToolHelpers::IsWritableContentPath(System->GetOutermost()->GetName(), OutError))
		{
			return nullptr;
		}
		return System;
	}

	UNiagaraEmitter* NiagaraLoadEmitter(const FString& PathOrTemplate, FString& OutError)
	{
		const FString Trimmed = PathOrTemplate.TrimStartAndEnd();
		if (Trimmed.IsEmpty())
		{
			OutError = TEXT("An emitter is required: an asset path, or the name of a template such as Fountain, SimpleSpriteBurst, DirectionalBurst, Minimal or SingleLoopingParticle.");
			return nullptr;
		}
		const FString Path = Trimmed.StartsWith(TEXT("/")) ? Trimmed : FString(NiagaraTemplateEmitterFolder) + Trimmed;
		FString LoadError;
		UObject* Asset = CursorToolHelpers::LoadAsset(Path, LoadError);
		if (!Asset)
		{
			OutError = Trimmed.StartsWith(TEXT("/")) ? LoadError
				: FString::Printf(TEXT("No template emitter '%s' (looked for %s). Pass an emitter asset path instead."), *Trimmed, *Path);
			return nullptr;
		}
		UNiagaraEmitter* Emitter = Cast<UNiagaraEmitter>(Asset);
		if (!Emitter)
		{
			OutError = FString::Printf(TEXT("'%s' is a %s, not a Niagara emitter."), *Path, *Asset->GetClass()->GetName());
		}
		return Emitter;
	}

	FNiagaraEmitterHandle* NiagaraFindHandle(UNiagaraSystem* System, const FString& Name, FString& OutError)
	{
		TArray<FString> Names;
		for (FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
		{
			Names.Add(Handle.GetName().ToString());
			if (Handle.GetName().ToString().Equals(Name, ESearchCase::IgnoreCase))
			{
				return &Handle;
			}
		}
		OutError = FString::Printf(TEXT("No emitter named '%s' in %s. Emitters: %s"), *Name, *System->GetName(), Names.Num() ? *FString::Join(Names, TEXT(", ")) : TEXT("(none)"));
		return nullptr;
	}

	/** "NiagaraSpriteRendererProperties" -> "Sprite". */
	FString NiagaraShortRendererName(const UNiagaraRendererProperties* Renderer)
	{
		FString Name = Renderer->GetClass()->GetName();
		Name.RemoveFromStart(TEXT("Niagara"));
		Name.RemoveFromEnd(TEXT("RendererProperties"));
		return Name;
	}

	// ---- module stacks ------------------------------------------------------------------------------------------------------

	struct FNiagaraStage
	{
		ENiagaraScriptUsage Usage;
		const TCHAR* Key;
	};
	const FNiagaraStage NiagaraEmitterStages[] =
	{
		{ ENiagaraScriptUsage::EmitterSpawnScript, TEXT("emitter_spawn") },
		{ ENiagaraScriptUsage::EmitterUpdateScript, TEXT("emitter_update") },
		{ ENiagaraScriptUsage::ParticleSpawnScript, TEXT("particle_spawn") },
		{ ENiagaraScriptUsage::ParticleUpdateScript, TEXT("particle_update") },
	};
	const FNiagaraStage NiagaraSystemStages[] =
	{
		{ ENiagaraScriptUsage::SystemSpawnScript, TEXT("system_spawn") },
		{ ENiagaraScriptUsage::SystemUpdateScript, TEXT("system_update") },
	};

	UNiagaraGraph* NiagaraGraphOfScript(UNiagaraScript* Script)
	{
		if (!Script)
		{
			return nullptr;
		}
		UNiagaraScriptSource* Source = Cast<UNiagaraScriptSource>(Script->GetLatestSource());
		return Source ? Source->NodeGraph.Get() : nullptr;
	}

	/**
	 * Walks back from a stage's output node along the parameter map wire, which visits the modules last to first,
	 * then reverses so the list reads in execution order. Capped so a malformed graph cannot loop.
	 */
	TArray<UNiagaraNodeFunctionCall*> NiagaraOrderedModules(const UNiagaraNodeOutput* Output)
	{
		TArray<UNiagaraNodeFunctionCall*> Reversed;
		const UEdGraphNode* Current = Output;
		for (int32 Guard = 0; Current && Guard < 512; ++Guard)
		{
			const UEdGraphPin* ParameterMapIn = nullptr;
			for (const UEdGraphPin* Pin : Current->Pins)
			{
				if (Pin->Direction == EGPD_Input && Pin->PinType.PinSubCategoryObject.Get() == FNiagaraTypeDefinition::GetParameterMapStruct())
				{
					ParameterMapIn = Pin;
					break;
				}
			}
			if (!ParameterMapIn || ParameterMapIn->LinkedTo.Num() == 0)
			{
				break;
			}
			Current = ParameterMapIn->LinkedTo[0]->GetOwningNode();
			if (UNiagaraNodeFunctionCall* Call = const_cast<UNiagaraNodeFunctionCall*>(Cast<UNiagaraNodeFunctionCall>(Current)))
			{
				Reversed.Add(Call);
			}
		}
		Algo::Reverse(Reversed);
		return Reversed;
	}

	TSharedRef<FJsonObject> NiagaraDescribeModule(const UNiagaraNodeFunctionCall* Call)
	{
		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetStringField(TEXT("name"), Call->GetFunctionName());
		Out->SetStringField(TEXT("module"), Call->FunctionScript ? Call->FunctionScript->GetOutermost()->GetName() : FString(TEXT("(set parameters)")));
		Out->SetBoolField(TEXT("enabled"), Call->IsNodeEnabled());
		return Out;
	}

	/** {"particle_spawn": [modules in order], ...} for the stages that exist in the graph. */
	TSharedRef<FJsonObject> NiagaraDescribeStacks(UNiagaraGraph* Graph, TArrayView<const FNiagaraStage> Stages)
	{
		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		if (!Graph)
		{
			return Out;
		}
		for (const FNiagaraStage& Stage : Stages)
		{
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				const UNiagaraNodeOutput* Output = Cast<UNiagaraNodeOutput>(Node);
				if (!Output || Output->GetUsage() != Stage.Usage)
				{
					continue;
				}
				TArray<TSharedPtr<FJsonValue>> Modules;
				for (const UNiagaraNodeFunctionCall* Call : NiagaraOrderedModules(Output))
				{
					Modules.Add(NiagaraJson(NiagaraDescribeModule(Call)));
				}
				Out->SetArrayField(Stage.Key, Modules);
				break;
			}
		}
		return Out;
	}

	TSharedRef<FJsonObject> NiagaraDescribeEmitterData(const FVersionedNiagaraEmitterData* Data)
	{
		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		if (!Data)
		{
			return Out;
		}
		const UEnum* SimTargetEnum = StaticEnum<ENiagaraSimTarget>();
		Out->SetStringField(TEXT("sim_target"), SimTargetEnum->GetNameStringByValue(static_cast<int64>(Data->SimTarget)));
		Out->SetBoolField(TEXT("local_space"), Data->bLocalSpace);
		Out->SetBoolField(TEXT("deterministic"), Data->bDeterminism);
		Out->SetNumberField(TEXT("event_handler_count"), Data->EventHandlerScriptProps.Num());

		const FVersionedNiagaraEmitter Parent = Data->GetParent();
		if (Parent.Emitter)
		{
			Out->SetStringField(TEXT("parent"), Parent.Emitter->GetPathName());
		}

		TArray<TSharedPtr<FJsonValue>> Renderers;
		for (const UNiagaraRendererProperties* Renderer : Data->GetRenderers())
		{
			if (Renderer)
			{
				TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetStringField(TEXT("type"), NiagaraShortRendererName(Renderer));
				Item->SetBoolField(TEXT("enabled"), Renderer->GetIsEnabled());
				Renderers.Add(NiagaraJson(Item));
			}
		}
		Out->SetArrayField(TEXT("renderers"), Renderers);

		// The emitter's four stages share one graph, so any of its scripts leads to it.
		UNiagaraGraph* Graph = NiagaraGraphOfScript(Data->GetScript(ENiagaraScriptUsage::ParticleSpawnScript, FGuid()));
		Out->SetObjectField(TEXT("modules"), NiagaraDescribeStacks(Graph, MakeArrayView(NiagaraEmitterStages)));
		return Out;
	}

	// ---- user parameters ----------------------------------------------------------------------------------------------------

	FString NiagaraStripUserPrefix(const FString& Name)
	{
		FString Out = Name;
		Out.RemoveFromStart(TEXT("User."));
		return Out;
	}

	FNiagaraVariable NiagaraFullUserVariable(const FNiagaraTypeDefinition& Type, const FString& ShortName)
	{
		return FNiagaraVariable(Type, FName(*(TEXT("User.") + ShortName)));
	}

	/** The type names tools accept and report. Types outside this list are shown by name and cannot be set. */
	const TCHAR* NiagaraTypeName(const FNiagaraTypeDefinition& Type, FString& OutOther)
	{
		if (Type == FNiagaraTypeDefinition::GetFloatDef()) { return TEXT("float"); }
		if (Type == FNiagaraTypeDefinition::GetIntDef()) { return TEXT("int"); }
		if (Type == FNiagaraTypeDefinition::GetBoolDef()) { return TEXT("bool"); }
		if (Type == FNiagaraTypeDefinition::GetVec2Def()) { return TEXT("vector2"); }
		if (Type == FNiagaraTypeDefinition::GetVec3Def()) { return TEXT("vector3"); }
		if (Type == FNiagaraTypeDefinition::GetVec4Def()) { return TEXT("vector4"); }
		if (Type == FNiagaraTypeDefinition::GetColorDef()) { return TEXT("color"); }
		OutOther = Type.GetName();
		return nullptr;
	}

	bool NiagaraTypeFromName(const FString& Name, FNiagaraTypeDefinition& Out)
	{
		const FString Lower = Name.ToLower();
		if (Lower == TEXT("float")) { Out = FNiagaraTypeDefinition::GetFloatDef(); }
		else if (Lower == TEXT("int") || Lower == TEXT("integer")) { Out = FNiagaraTypeDefinition::GetIntDef(); }
		else if (Lower == TEXT("bool") || Lower == TEXT("boolean")) { Out = FNiagaraTypeDefinition::GetBoolDef(); }
		else if (Lower == TEXT("vector2") || Lower == TEXT("vec2")) { Out = FNiagaraTypeDefinition::GetVec2Def(); }
		else if (Lower == TEXT("vector3") || Lower == TEXT("vec3") || Lower == TEXT("vector")) { Out = FNiagaraTypeDefinition::GetVec3Def(); }
		else if (Lower == TEXT("vector4") || Lower == TEXT("vec4")) { Out = FNiagaraTypeDefinition::GetVec4Def(); }
		else if (Lower == TEXT("color")) { Out = FNiagaraTypeDefinition::GetColorDef(); }
		else { return false; }
		return true;
	}

	TSharedPtr<FJsonValue> NiagaraNumberArray(const double* Values, int32 Count)
	{
		TArray<TSharedPtr<FJsonValue>> Array;
		for (int32 i = 0; i < Count; ++i)
		{
			Array.Add(MakeShared<FJsonValueNumber>(Values[i]));
		}
		return MakeShared<FJsonValueArray>(Array);
	}

	/** Reads the current value of a user parameter as JSON, or null for types this pack does not handle. */
	TSharedPtr<FJsonValue> NiagaraReadValue(const FNiagaraParameterStore& Store, const FNiagaraVariable& Var)
	{
		const FNiagaraTypeDefinition& Type = Var.GetType();
		if (Type == FNiagaraTypeDefinition::GetFloatDef())
		{
			return MakeShared<FJsonValueNumber>(Store.GetParameterValue<float>(Var));
		}
		if (Type == FNiagaraTypeDefinition::GetIntDef())
		{
			return MakeShared<FJsonValueNumber>(Store.GetParameterValue<int32>(Var));
		}
		if (Type == FNiagaraTypeDefinition::GetBoolDef())
		{
			return MakeShared<FJsonValueBoolean>(Store.GetParameterValue<FNiagaraBool>(Var).GetValue());
		}
		if (Type == FNiagaraTypeDefinition::GetVec2Def())
		{
			const FVector2f V = Store.GetParameterValue<FVector2f>(Var);
			const double Values[] = { V.X, V.Y };
			return NiagaraNumberArray(Values, 2);
		}
		if (Type == FNiagaraTypeDefinition::GetVec3Def())
		{
			const FVector3f V = Store.GetParameterValue<FVector3f>(Var);
			const double Values[] = { V.X, V.Y, V.Z };
			return NiagaraNumberArray(Values, 3);
		}
		if (Type == FNiagaraTypeDefinition::GetVec4Def())
		{
			const FVector4f V = Store.GetParameterValue<FVector4f>(Var);
			const double Values[] = { V.X, V.Y, V.Z, V.W };
			return NiagaraNumberArray(Values, 4);
		}
		if (Type == FNiagaraTypeDefinition::GetColorDef())
		{
			const FLinearColor C = Store.GetParameterValue<FLinearColor>(Var);
			const double Values[] = { C.R, C.G, C.B, C.A };
			return NiagaraNumberArray(Values, 4);
		}
		return nullptr;
	}

	/** Accepts [x,y,...] with MinCount..MaxCount numbers. Missing trailing values stay at what Out already holds. */
	bool NiagaraParseNumbers(const TSharedPtr<FJsonValue>& Value, int32 MinCount, int32 MaxCount, double* Out)
	{
		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (!Value.IsValid() || !Value->TryGetArray(Array) || Array->Num() < MinCount || Array->Num() > MaxCount)
		{
			return false;
		}
		for (int32 i = 0; i < Array->Num(); ++i)
		{
			if (!(*Array)[i]->TryGetNumber(Out[i]))
			{
				return false;
			}
		}
		return true;
	}

	/** Writes a JSON value into a parameter that must already exist in the store. */
	bool NiagaraWriteValue(FNiagaraParameterStore& Store, const FNiagaraVariable& Var, const TSharedPtr<FJsonValue>& Value, FString& OutError)
	{
		const FNiagaraTypeDefinition& Type = Var.GetType();
		double N[4] = { 0.0, 0.0, 0.0, 1.0 };
		bool bOk = false;
		if (!Value.IsValid() || Value->IsNull())
		{
			OutError = TEXT("'value' is required.");
			return false;
		}
		if (Type == FNiagaraTypeDefinition::GetFloatDef())
		{
			if (!Value->TryGetNumber(N[0])) { OutError = TEXT("A float parameter takes a number."); return false; }
			bOk = Store.SetParameterValue<float>(static_cast<float>(N[0]), Var);
		}
		else if (Type == FNiagaraTypeDefinition::GetIntDef())
		{
			if (!Value->TryGetNumber(N[0])) { OutError = TEXT("An int parameter takes a number."); return false; }
			bOk = Store.SetParameterValue<int32>(static_cast<int32>(N[0]), Var);
		}
		else if (Type == FNiagaraTypeDefinition::GetBoolDef())
		{
			bool bValue = false;
			if (!Value->TryGetBool(bValue)) { OutError = TEXT("A bool parameter takes true or false."); return false; }
			FNiagaraBool Wrapped;
			Wrapped.SetValue(bValue);
			bOk = Store.SetParameterValue<FNiagaraBool>(Wrapped, Var);
		}
		else if (Type == FNiagaraTypeDefinition::GetVec2Def())
		{
			if (!NiagaraParseNumbers(Value, 2, 2, N)) { OutError = TEXT("A vector2 parameter takes [x, y]."); return false; }
			bOk = Store.SetParameterValue<FVector2f>(FVector2f(static_cast<float>(N[0]), static_cast<float>(N[1])), Var);
		}
		else if (Type == FNiagaraTypeDefinition::GetVec3Def())
		{
			if (!NiagaraParseNumbers(Value, 3, 3, N)) { OutError = TEXT("A vector3 parameter takes [x, y, z]."); return false; }
			bOk = Store.SetParameterValue<FVector3f>(FVector3f(static_cast<float>(N[0]), static_cast<float>(N[1]), static_cast<float>(N[2])), Var);
		}
		else if (Type == FNiagaraTypeDefinition::GetVec4Def())
		{
			if (!NiagaraParseNumbers(Value, 4, 4, N)) { OutError = TEXT("A vector4 parameter takes [x, y, z, w]."); return false; }
			bOk = Store.SetParameterValue<FVector4f>(FVector4f(static_cast<float>(N[0]), static_cast<float>(N[1]), static_cast<float>(N[2]), static_cast<float>(N[3])), Var);
		}
		else if (Type == FNiagaraTypeDefinition::GetColorDef())
		{
			if (!NiagaraParseNumbers(Value, 3, 4, N)) { OutError = TEXT("A color parameter takes [r, g, b] or [r, g, b, a]."); return false; }
			bOk = Store.SetParameterValue<FLinearColor>(FLinearColor(static_cast<float>(N[0]), static_cast<float>(N[1]), static_cast<float>(N[2]), static_cast<float>(N[3])), Var);
		}
		else
		{
			OutError = FString::Printf(TEXT("Parameters of type %s cannot be set by this tool."), *Type.GetName());
			return false;
		}
		if (!bOk)
		{
			OutError = TEXT("The parameter store refused the value.");
		}
		return bOk;
	}

	TArray<FNiagaraVariable> NiagaraUserParameters(UNiagaraSystem* System)
	{
		TArray<FNiagaraVariable> Vars;
		System->GetExposedParameters().GetUserParameters(Vars);
		Vars.Sort([](const FNiagaraVariable& A, const FNiagaraVariable& B) { return A.GetName().LexicalLess(B.GetName()); });
		return Vars;
	}

	TArray<TSharedPtr<FJsonValue>> NiagaraDescribeUserParameters(UNiagaraSystem* System)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		for (const FNiagaraVariable& Var : NiagaraUserParameters(System))
		{
			const FString ShortName = NiagaraStripUserPrefix(Var.GetName().ToString());
			TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("name"), ShortName);
			FString Other;
			if (const TCHAR* TypeName = NiagaraTypeName(Var.GetType(), Other))
			{
				Item->SetStringField(TEXT("type"), TypeName);
				const FNiagaraVariable Full = NiagaraFullUserVariable(Var.GetType(), ShortName);
				if (const TSharedPtr<FJsonValue> Value = NiagaraReadValue(System->GetExposedParameters(), Full))
				{
					Item->SetField(TEXT("value"), Value);
				}
			}
			else
			{
				Item->SetStringField(TEXT("type"), Other);
				Item->SetBoolField(TEXT("settable"), false);
			}
			Out.Add(NiagaraJson(Item));
		}
		return Out;
	}

	FNiagaraVariable NiagaraFindUserParameter(UNiagaraSystem* System, const FString& Name, FString& OutError)
	{
		const FString Wanted = NiagaraStripUserPrefix(Name.TrimStartAndEnd());
		TArray<FString> Names;
		for (const FNiagaraVariable& Var : NiagaraUserParameters(System))
		{
			const FString ShortName = NiagaraStripUserPrefix(Var.GetName().ToString());
			Names.Add(ShortName);
			if (ShortName.Equals(Wanted, ESearchCase::IgnoreCase))
			{
				return NiagaraFullUserVariable(Var.GetType(), ShortName);
			}
		}
		OutError = FString::Printf(TEXT("No user parameter '%s' on %s. Existing: %s. Add one with niagara_add_user_parameter."), *Wanted, *System->GetName(),
			Names.Num() ? *FString::Join(Names, TEXT(", ")) : TEXT("(none)"));
		return FNiagaraVariable();
	}

	// ---- compile ------------------------------------------------------------------------------------------------------------

	const TCHAR* NiagaraStatusName(ENiagaraScriptCompileStatus Status)
	{
		switch (Status)
		{
		case ENiagaraScriptCompileStatus::NCS_UpToDate: return TEXT("up_to_date");
		case ENiagaraScriptCompileStatus::NCS_UpToDateWithWarnings:
		case ENiagaraScriptCompileStatus::NCS_ComputeUpToDateWithWarnings: return TEXT("up_to_date_with_warnings");
		case ENiagaraScriptCompileStatus::NCS_Error: return TEXT("error");
		case ENiagaraScriptCompileStatus::NCS_Dirty: return TEXT("needs_compile");
		case ENiagaraScriptCompileStatus::NCS_BeingCreated: return TEXT("being_created");
		default: return TEXT("unknown");
		}
	}

	/** Every script a system compiles: its own two, plus each enabled emitter's. */
	void NiagaraGatherScripts(UNiagaraSystem* System, TArray<UNiagaraScript*>& OutScripts)
	{
		OutScripts.Add(System->GetSystemSpawnScript());
		OutScripts.Add(System->GetSystemUpdateScript());
		for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
		{
			if (FVersionedNiagaraEmitterData* Data = Handle.GetEmitterData())
			{
				Data->GetScripts(OutScripts, /*bCompilableOnly*/ true, /*bEnabledOnly*/ true);
			}
		}
		OutScripts.RemoveAll([](const UNiagaraScript* Script) { return Script == nullptr; });
	}

	/** Collects compile errors from the scripts' last compile. */
	TArray<FString> NiagaraCompileErrors(UNiagaraSystem* System)
	{
		TArray<UNiagaraScript*> Scripts;
		NiagaraGatherScripts(System, Scripts);
		TArray<FString> Errors;
		for (UNiagaraScript* Script : Scripts)
		{
			const FNiagaraVMExecutableData& Data = Script->GetVMExecutableData();
			const bool bFailed = Script->GetLastCompileStatus() == ENiagaraScriptCompileStatus::NCS_Error;
			if (bFailed && !Data.ErrorMsg.IsEmpty())
			{
				Errors.AddUnique(Data.ErrorMsg);
			}
			for (const FNiagaraCompileEvent& Event : Data.LastCompileEvents)
			{
				if (Event.Severity == FNiagaraCompileEventSeverity::Error)
				{
					Errors.AddUnique(Event.Message);
				}
			}
			if (bFailed && Data.ErrorMsg.IsEmpty() && Errors.Num() == 0)
			{
				Errors.AddUnique(FString::Printf(TEXT("%s failed to compile."), *Script->GetName()));
			}
		}
		return Errors;
	}
}

void CursorNiagaraPack::Register()
{
	FCursorToolBuilder(TEXT("niagara_create_system"), NiagaraPackName)
		.Describe(TEXT("Create a Niagara system asset, optionally with emitters. Name template emitters that ship with Niagara (Fountain, SimpleSpriteBurst, DirectionalBurst, OmnidirectionalBurst, "
			"ConfettiBurst, Minimal, SingleLoopingParticle, HangingParticulates, BlowingParticles, UpwardMeshBurst, DynamicBeam, StaticBeam, LocationBasedRibbon) or pass emitter asset paths. "
			"To start from an existing system instead, use asset_duplicate. Then inspect with niagara_get_info and call niagara_compile."))
		.Risk(ECursorToolRisk::Mutating)
		.Schema(FCursorSchema()
			.Str(TEXT("path"), TEXT("Where to create it, for example /Game/FX/NS_Sparks."), true)
			.StrArray(TEXT("emitters"), TEXT("Template names or emitter asset paths to add. Default: none (an empty system).")))
		.Run([](const FCursorArgs& Args, const FCursorToolContext&)
		{
			// Resolve every emitter first, so a bad name leaves no half-made asset behind.
			FString Error;
			TArray<UNiagaraEmitter*> Emitters;
			for (const FString& Name : Args.StrArray(TEXT("emitters")))
			{
				UNiagaraEmitter* Emitter = NiagaraLoadEmitter(Name, Error);
				if (!Emitter)
				{
					return FCursorToolResult::Error(Error);
				}
				Emitters.Add(Emitter);
			}

			UPackage* Package = nullptr;
			FString AssetName;
			if (!CursorToolHelpers::PrepareNewAsset(Args.Str(TEXT("path")), Package, AssetName, Error))
			{
				return FCursorToolResult::Error(Error);
			}
			UNiagaraSystem* System = NewObject<UNiagaraSystem>(Package, FName(*AssetName), RF_Public | RF_Standalone | RF_Transactional);
			if (!System)
			{
				return FCursorToolResult::Error(TEXT("The editor could not create the Niagara system. See editor_log_tail."));
			}
			UNiagaraSystemFactoryNew::InitializeSystem(System, /*bCreateDefaultNodes*/ true);
			for (UNiagaraEmitter* Emitter : Emitters)
			{
				FNiagaraEditorUtilities::AddEmitterToSystem(*System, *Emitter, Emitter->GetExposedVersion().VersionGuid, /*bCreateCopy*/ true);
			}
			CursorToolHelpers::FinishNewAsset(System);

			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetStringField(TEXT("path"), System->GetPathName());
			TArray<FString> Added;
			for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
			{
				Added.Add(Handle.GetName().ToString());
			}
			Out->SetArrayField(TEXT("emitters"), CursorJson::StringArray(Added));
			Out->SetStringField(TEXT("note"), TEXT("Created in memory. Call niagara_compile (save=true to write it) before previewing."));
			return FCursorToolResult::OkJson(Out);
		})
		.Register();

	FCursorToolBuilder(TEXT("niagara_get_info"), NiagaraPackName)
		.Describe(TEXT("Inspect a Niagara system or emitter asset. For a system: its emitters (enabled, sim target, renderers, parent emitter, and the module stack of each stage in execution order), "
			"the system's own spawn and update modules, its exposed user parameters with current values, and its compile state. For an emitter asset: the same detail for that emitter."))
		.Risk(ECursorToolRisk::ReadOnly)
		.Schema(FCursorSchema()
			.Str(TEXT("system"), TEXT("Niagara system or emitter asset path."), true)
			.Bool(TEXT("include_modules"), TEXT("List module stacks. Default true. Turn off for a short overview.")))
		.Run([](const FCursorArgs& Args, const FCursorToolContext&)
		{
			FString Error;
			UObject* Asset = NiagaraLoadAsset(Args.Str(TEXT("system")), Error);
			if (!Asset)
			{
				return FCursorToolResult::Error(Error);
			}
			const bool bModules = Args.Has(TEXT("include_modules")) ? Args.Bool(TEXT("include_modules")) : true;

			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetStringField(TEXT("path"), Asset->GetPathName());
			Out->SetBoolField(TEXT("has_unsaved_changes"), Asset->GetOutermost()->IsDirty());

			if (UNiagaraEmitter* Emitter = Cast<UNiagaraEmitter>(Asset))
			{
				Out->SetStringField(TEXT("type"), TEXT("emitter"));
				Out->SetObjectField(TEXT("emitter"), NiagaraDescribeEmitterData(Emitter->GetLatestEmitterData()));
				return FCursorToolResult::OkJson(Out);
			}

			UNiagaraSystem* System = CastChecked<UNiagaraSystem>(Asset);
			Out->SetStringField(TEXT("type"), TEXT("system"));
			Out->SetBoolField(TEXT("ready_to_run"), System->IsReadyToRun());
			Out->SetBoolField(TEXT("compiling"), System->HasOutstandingCompilationRequests());
			Out->SetNumberField(TEXT("warmup_time"), System->GetWarmupTime());
			Out->SetBoolField(TEXT("deterministic"), System->NeedsDeterminism());

			TArray<TSharedPtr<FJsonValue>> Emitters;
			for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
			{
				TSharedRef<FJsonObject> Item = NiagaraDescribeEmitterData(Handle.GetEmitterData());
				Item->SetStringField(TEXT("name"), Handle.GetName().ToString());
				Item->SetStringField(TEXT("id"), Handle.GetId().ToString(EGuidFormats::Digits));
				Item->SetBoolField(TEXT("enabled"), Handle.GetIsEnabled());
				if (!bModules)
				{
					Item->RemoveField(TEXT("modules"));
				}
				Emitters.Add(NiagaraJson(Item));
			}
			Out->SetArrayField(TEXT("emitters"), Emitters);

			if (bModules)
			{
				Out->SetObjectField(TEXT("system_modules"), NiagaraDescribeStacks(NiagaraGraphOfScript(System->GetSystemSpawnScript()), MakeArrayView(NiagaraSystemStages)));
			}
			Out->SetArrayField(TEXT("user_parameters"), NiagaraDescribeUserParameters(System));
			return FCursorToolResult::OkJson(Out);
		})
		.Register();

	FCursorToolBuilder(TEXT("niagara_add_emitter"), NiagaraPackName)
		.Describe(TEXT("Add an emitter to a Niagara system. Takes a template name (Fountain, SimpleSpriteBurst, Minimal...) or an emitter asset path. The system gets its own copy of the emitter. "
			"Call niagara_compile afterwards."))
		.Risk(ECursorToolRisk::Mutating)
		.Schema(FCursorSchema()
			.Str(TEXT("system"), TEXT("Niagara system asset path."), true)
			.Str(TEXT("emitter"), TEXT("Template name or emitter asset path."), true)
			.Str(TEXT("name"), TEXT("Name for the emitter inside the system. Default: the emitter asset's name.")))
		.Run([](const FCursorArgs& Args, const FCursorToolContext&)
		{
			FString Error;
			UNiagaraSystem* System = NiagaraLoadEditableSystem(Args.Str(TEXT("system")), Error);
			UNiagaraEmitter* Emitter = System ? NiagaraLoadEmitter(Args.Str(TEXT("emitter")), Error) : nullptr;
			if (!Emitter)
			{
				return FCursorToolResult::Error(Error);
			}
			System->Modify();
			const FGuid NewId = FNiagaraEditorUtilities::AddEmitterToSystem(*System, *Emitter, Emitter->GetExposedVersion().VersionGuid, /*bCreateCopy*/ true);
			FNiagaraEmitterHandle* Added = System->GetEmitterHandles().FindByPredicate([&](const FNiagaraEmitterHandle& Handle) { return Handle.GetId() == NewId; });
			if (!Added)
			{
				return FCursorToolResult::Error(TEXT("The editor did not add the emitter."));
			}
			if (Args.Has(TEXT("name")))
			{
				Added->SetName(FName(*Args.Str(TEXT("name"))), *System);
			}
			System->MarkPackageDirty();

			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetStringField(TEXT("emitter"), Added->GetName().ToString());
			Out->SetStringField(TEXT("id"), NewId.ToString(EGuidFormats::Digits));
			Out->SetNumberField(TEXT("emitter_count"), System->GetEmitterHandles().Num());
			Out->SetStringField(TEXT("note"), TEXT("Call niagara_compile to rebuild the system."));
			return FCursorToolResult::OkJson(Out);
		})
		.Register();

	FCursorToolBuilder(TEXT("niagara_remove_emitter"), NiagaraPackName)
		.Describe(TEXT("Remove an emitter from a Niagara system by its name in the system (see niagara_get_info)."))
		.Risk(ECursorToolRisk::Mutating)
		.Schema(FCursorSchema()
			.Str(TEXT("system"), TEXT("Niagara system asset path."), true)
			.Str(TEXT("emitter"), TEXT("Emitter name in the system."), true))
		.Run([](const FCursorArgs& Args, const FCursorToolContext&)
		{
			FString Error;
			UNiagaraSystem* System = NiagaraLoadEditableSystem(Args.Str(TEXT("system")), Error);
			FNiagaraEmitterHandle* Found = System ? NiagaraFindHandle(System, Args.Str(TEXT("emitter")), Error) : nullptr;
			if (!Found)
			{
				return FCursorToolResult::Error(Error);
			}
			// A copy: removing edits the array the handle lives in.
			const FNiagaraEmitterHandle Handle = *Found;
			const FString Name = Handle.GetName().ToString();
			System->Modify();
			System->RemoveEmitterHandle(Handle);
			System->MarkPackageDirty();
			return FCursorToolResult::Ok(FString::Printf(TEXT("Removed emitter %s. %d left. Call niagara_compile to rebuild the system."), *Name, System->GetEmitterHandles().Num()));
		})
		.Register();

	FCursorToolBuilder(TEXT("niagara_set_emitter_enabled"), NiagaraPackName)
		.Describe(TEXT("Enable or disable an emitter in a Niagara system. Disabled emitters are not simulated or rendered."))
		.Risk(ECursorToolRisk::Mutating)
		.Schema(FCursorSchema()
			.Str(TEXT("system"), TEXT("Niagara system asset path."), true)
			.Str(TEXT("emitter"), TEXT("Emitter name in the system."), true)
			.Bool(TEXT("enabled"), TEXT("True to enable, false to disable."), true))
		.Run([](const FCursorArgs& Args, const FCursorToolContext&)
		{
			FString Error;
			UNiagaraSystem* System = NiagaraLoadEditableSystem(Args.Str(TEXT("system")), Error);
			FNiagaraEmitterHandle* Handle = System ? NiagaraFindHandle(System, Args.Str(TEXT("emitter")), Error) : nullptr;
			if (!Handle)
			{
				return FCursorToolResult::Error(Error);
			}
			if (!Args.Has(TEXT("enabled")))
			{
				return FCursorToolResult::Error(TEXT("'enabled' is required."));
			}
			const bool bEnabled = Args.Bool(TEXT("enabled"));
			System->Modify();
			const bool bChanged = Handle->SetIsEnabled(bEnabled, *System, /*bRecompileIfChanged*/ false);
			System->MarkPackageDirty();
			return FCursorToolResult::Ok(FString::Printf(TEXT("Emitter %s is %s%s."), *Handle->GetName().ToString(), bEnabled ? TEXT("enabled") : TEXT("disabled"),
				bChanged ? TEXT(". Call niagara_compile to rebuild the system") : TEXT(" (no change)")));
		})
		.Register();

	FCursorToolBuilder(TEXT("niagara_list_parameters"), NiagaraPackName)
		.Describe(TEXT("List the user parameters exposed by a Niagara system (the ones Blueprints and the level can override), with type and default value."))
		.Risk(ECursorToolRisk::ReadOnly)
		.Schema(FCursorSchema()
			.Str(TEXT("system"), TEXT("Niagara system asset path."), true))
		.Run([](const FCursorArgs& Args, const FCursorToolContext&)
		{
			FString Error;
			UObject* Asset = NiagaraLoadAsset(Args.Str(TEXT("system")), Error);
			UNiagaraSystem* System = Cast<UNiagaraSystem>(Asset);
			if (!System)
			{
				return FCursorToolResult::Error(Asset ? FString::Printf(TEXT("'%s' is an emitter. Parameters are listed for systems."), *Asset->GetPathName()) : Error);
			}
			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetStringField(TEXT("path"), System->GetPathName());
			Out->SetArrayField(TEXT("parameters"), NiagaraDescribeUserParameters(System));
			return FCursorToolResult::OkJson(Out);
		})
		.Register();

	FCursorToolBuilder(TEXT("niagara_add_user_parameter"), NiagaraPackName)
		.Describe(TEXT("Expose a new user parameter on a Niagara system, so it can be overridden per instance. Types: float, int, bool, vector2, vector3, vector4, color. "
			"Modules that read User.<name> pick it up once wired in the Niagara editor."))
		.Risk(ECursorToolRisk::Mutating)
		.Schema(FCursorSchema()
			.Str(TEXT("system"), TEXT("Niagara system asset path."), true)
			.Str(TEXT("name"), TEXT("Parameter name without the User. prefix."), true)
			.Enum(TEXT("type"), TEXT("Parameter type."), { TEXT("float"), TEXT("int"), TEXT("bool"), TEXT("vector2"), TEXT("vector3"), TEXT("vector4"), TEXT("color") }, true)
			.Any(TEXT("value"), TEXT("Initial value: a number, true/false, [x,y], [x,y,z], [x,y,z,w] or [r,g,b(,a)]. Default: zero.")))
		.Run([](const FCursorArgs& Args, const FCursorToolContext&)
		{
			FString Error;
			UNiagaraSystem* System = NiagaraLoadEditableSystem(Args.Str(TEXT("system")), Error);
			if (!System)
			{
				return FCursorToolResult::Error(Error);
			}
			const FString Name = NiagaraStripUserPrefix(Args.Str(TEXT("name")).TrimStartAndEnd());
			bool bValidName = !Name.IsEmpty() && !Name.Contains(TEXT("."));
			for (const TCHAR Ch : Name)
			{
				bValidName = bValidName && (FChar::IsAlnum(Ch) || Ch == TEXT('_'));
			}
			if (!bValidName)
			{
				return FCursorToolResult::Error(FString::Printf(TEXT("'%s' is not a valid parameter name: use letters, digits and underscores."), *Name));
			}
			FNiagaraTypeDefinition Type;
			if (!NiagaraTypeFromName(Args.Str(TEXT("type")), Type))
			{
				return FCursorToolResult::Error(FString::Printf(TEXT("Unknown type '%s'. Use float, int, bool, vector2, vector3, vector4 or color."), *Args.Str(TEXT("type"))));
			}
			for (const FNiagaraVariable& Var : NiagaraUserParameters(System))
			{
				if (NiagaraStripUserPrefix(Var.GetName().ToString()).Equals(Name, ESearchCase::IgnoreCase))
				{
					return FCursorToolResult::Error(FString::Printf(TEXT("%s already has a user parameter '%s'. Use niagara_set_parameter to change it."), *System->GetName(), *Name));
				}
			}

			const TSharedPtr<FJsonValue> Value = Args.Raw()->TryGetField(TEXT("value"));
			const FNiagaraVariable Var = NiagaraFullUserVariable(Type, Name);
			System->Modify();
			if (!System->GetExposedParameters().AddParameter(Var, /*bInitialize*/ true))
			{
				return FCursorToolResult::Error(TEXT("The editor refused to add the parameter."));
			}
			if (Value.IsValid() && !Value->IsNull() && !NiagaraWriteValue(System->GetExposedParameters(), Var, Value, Error))
			{
				// Do not leave a parameter behind with the wrong value.
				System->GetExposedParameters().RemoveParameter(Var);
				return FCursorToolResult::Error(Error);
			}
			System->MarkPackageDirty();
			return FCursorToolResult::Ok(FString::Printf(TEXT("Added user parameter '%s' (%s) to %s."), *Name, *Args.Str(TEXT("type")).ToLower(), *System->GetName()));
		})
		.Register();

	FCursorToolBuilder(TEXT("niagara_set_parameter"), NiagaraPackName)
		.Describe(TEXT("Set the default value of an existing user parameter on a Niagara system. List them with niagara_list_parameters. "
			"Values: a number, true/false, [x,y], [x,y,z], [x,y,z,w] or [r,g,b(,a)] depending on the parameter's type."))
		.Risk(ECursorToolRisk::Mutating)
		.Schema(FCursorSchema()
			.Str(TEXT("system"), TEXT("Niagara system asset path."), true)
			.Str(TEXT("name"), TEXT("Parameter name, with or without the User. prefix."), true)
			.Any(TEXT("value"), TEXT("The new value."), true))
		.Run([](const FCursorArgs& Args, const FCursorToolContext&)
		{
			FString Error;
			UNiagaraSystem* System = NiagaraLoadEditableSystem(Args.Str(TEXT("system")), Error);
			if (!System)
			{
				return FCursorToolResult::Error(Error);
			}
			const FNiagaraVariable Var = NiagaraFindUserParameter(System, Args.Str(TEXT("name")), Error);
			if (!Var.IsValid())
			{
				return FCursorToolResult::Error(Error);
			}
			System->Modify();
			if (!NiagaraWriteValue(System->GetExposedParameters(), Var, Args.Raw()->TryGetField(TEXT("value")), Error))
			{
				return FCursorToolResult::Error(Error);
			}
			System->MarkPackageDirty();

			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetStringField(TEXT("name"), NiagaraStripUserPrefix(Var.GetName().ToString()));
			if (const TSharedPtr<FJsonValue> Now = NiagaraReadValue(System->GetExposedParameters(), Var))
			{
				Out->SetField(TEXT("value"), Now);
			}
			return FCursorToolResult::OkJson(Out);
		})
		.Register();

	FCursorToolBuilder(TEXT("niagara_compile"), NiagaraPackName)
		.Describe(TEXT("Compile a Niagara system after edits and report compile errors. Waits for the VM scripts (GPU shaders keep compiling in the background). "
			"Set save=true to write the asset afterwards."))
		.Risk(ECursorToolRisk::Mutating)
		.Schema(FCursorSchema()
			.Str(TEXT("system"), TEXT("Niagara system asset path."), true)
			.Bool(TEXT("force"), TEXT("Recompile even if nothing changed. Default false."))
			.Bool(TEXT("save"), TEXT("Save the asset to disk after compiling. Default false.")))
		.Run([](const FCursorArgs& Args, const FCursorToolContext&)
		{
			FString Error;
			UNiagaraSystem* System = NiagaraLoadEditableSystem(Args.Str(TEXT("system")), Error);
			if (!System)
			{
				return FCursorToolResult::Error(Error);
			}
			System->RequestCompile(Args.Bool(TEXT("force")));
			System->WaitForCompilationComplete(/*bIncludingGPUShaders*/ false, /*bShowProgress*/ false);

			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetStringField(TEXT("path"), System->GetPathName());
			const TArray<FString> Errors = NiagaraCompileErrors(System);
			Out->SetBoolField(TEXT("compiled"), Errors.Num() == 0);
			Out->SetBoolField(TEXT("ready_to_run"), System->IsReadyToRun());
			Out->SetArrayField(TEXT("errors"), CursorJson::StringArray(Errors));

			TArray<UNiagaraScript*> Scripts;
			NiagaraGatherScripts(System, Scripts);
			TArray<TSharedPtr<FJsonValue>> Statuses;
			for (const UNiagaraScript* Script : Scripts)
			{
				TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetStringField(TEXT("script"), Script->GetFullName(System->GetOutermost()));
				Item->SetStringField(TEXT("status"), NiagaraStatusName(Script->GetLastCompileStatus()));
				Statuses.Add(NiagaraJson(Item));
			}
			Out->SetArrayField(TEXT("scripts"), Statuses);

			if (Args.Bool(TEXT("save")))
			{
				const bool bSaved = UEditorLoadingAndSavingUtils::SavePackages({ System->GetOutermost() }, /*bOnlyDirty*/ false);
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

	FCursorToolBuilder(TEXT("niagara_spawn_in_level"), NiagaraPackName)
		.Describe(TEXT("Place a Niagara system in the open level as a NiagaraActor, so it can be seen in the viewport. Returns the actor."))
		.Risk(ECursorToolRisk::Mutating)
		.Schema(FCursorSchema()
			.Str(TEXT("system"), TEXT("Niagara system asset path (project or engine content)."), true)
			.Vec3(TEXT("location"), TEXT("World location [x, y, z] in cm. Default: the origin."))
			.Vec3(TEXT("rotation"), TEXT("Rotation [pitch, yaw, roll] in degrees. Default: none."))
			.Str(TEXT("label"), TEXT("Actor label in the World Outliner.")))
		.Run([](const FCursorArgs& Args, const FCursorToolContext&)
		{
			FString Error;
			UObject* Asset = NiagaraLoadAsset(Args.Str(TEXT("system")), Error);
			UNiagaraSystem* System = Cast<UNiagaraSystem>(Asset);
			if (!System)
			{
				return FCursorToolResult::Error(Asset ? FString::Printf(TEXT("'%s' is an emitter. Only systems can be placed in a level."), *Asset->GetPathName()) : Error);
			}
			UWorld* World = CursorToolHelpers::GetEditorWorld();
			if (!World)
			{
				return FCursorToolResult::Error(TEXT("There is no editor level open."));
			}
			FVector Location = FVector::ZeroVector;
			FRotator Rotation = FRotator::ZeroRotator;
			Args.Vec(TEXT("location"), Location);
			Args.Rot(TEXT("rotation"), Rotation);

			ANiagaraActor* Actor = World->SpawnActor<ANiagaraActor>(Location, Rotation);
			if (!Actor || !Actor->GetNiagaraComponent())
			{
				return FCursorToolResult::Error(TEXT("The editor could not spawn a Niagara actor."));
			}
			Actor->GetNiagaraComponent()->SetAsset(System);
			Actor->GetNiagaraComponent()->Activate(true);
			if (Args.Has(TEXT("label")))
			{
				Actor->SetActorLabel(Args.Str(TEXT("label")));
			}
			return FCursorToolResult::OkJson(CursorToolHelpers::DescribeActor(Actor, false));
		})
		.Register();
}

void CursorNiagaraPack::Unregister()
{
	FCursorToolRegistry::Get().UnregisterPack(NiagaraPackName);
}
