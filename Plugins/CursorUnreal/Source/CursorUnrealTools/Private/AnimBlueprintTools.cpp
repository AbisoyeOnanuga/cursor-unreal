#include "CursorToolPacks.h"

#include "Animation/AnimBlueprint.h"
#include "Animation/AnimInstance.h"
#include "Animation/Skeleton.h"
#include "CursorJson.h"
#include "CursorPackSupport.h"
#include "CursorTool.h"
#include "CursorToolHelpers.h"
#include "CursorToolRegistry.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphSchema.h"
#include "Engine/SkeletalMesh.h"
#include "Factories/AnimBlueprintFactory.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Logging/TokenizedMessage.h"

/*
 * Animation Blueprint pack: create an AnimBP for a skeleton, read its structure, list graphs, and list the nodes of any
 * graph including state machines and their states. Reading is generic over UEdGraph, so it does not depend on the
 * AnimGraph module's node classes. Adding or wiring animation nodes is out of scope: the AnimGraph node classes are
 * not exported for editing, so python_exec covers that. Edits stay in memory until asset_save.
 */

namespace
{
	const TCHAR* AnimBpPack = TEXT("animation");

	// Helpers carry an AnimBp prefix so a unity build that merges every tool file does not collide on names.
	TSharedPtr<FJsonValue> AnimBpJson(const TSharedRef<FJsonObject>& Object)
	{
		return MakeShared<FJsonValueObject>(Object);
	}

	UAnimBlueprint* AnimBpLoad(const FString& Path, bool bForEditing, FString& OutError)
	{
		if (Path.IsEmpty())
		{
			OutError = TEXT("'blueprint' is required: the asset path of an Animation Blueprint, for example /Game/Characters/ABP_Hero.");
			return nullptr;
		}
		UObject* Asset = CursorToolHelpers::LoadAsset(Path, OutError);
		if (!Asset)
		{
			return nullptr;
		}
		UAnimBlueprint* AnimBlueprint = Cast<UAnimBlueprint>(Asset);
		if (!AnimBlueprint)
		{
			OutError = FString::Printf(TEXT("'%s' is a %s, not an Animation Blueprint. Use the blueprint_* tools for other Blueprints."), *Path, *Asset->GetClass()->GetName());
			return nullptr;
		}
		if (bForEditing && !CursorPackSupport::IsWritableContentPath(AnimBlueprint->GetOutermost()->GetName(), OutError))
		{
			return nullptr;
		}
		return AnimBlueprint;
	}

	const TCHAR* AnimBpStatusName(EBlueprintStatus Status)
	{
		switch (Status)
		{
		case BS_UpToDate: return TEXT("up_to_date");
		case BS_UpToDateWithWarnings: return TEXT("up_to_date_with_warnings");
		case BS_Error: return TEXT("error");
		case BS_Dirty: return TEXT("needs_compile");
		case BS_BeingCreated: return TEXT("being_created");
		default: return TEXT("unknown");
		}
	}

	/** "AnimationGraphSchema" -> "animation_graph"; the schema is what tells an anim graph from a state graph. */
	FString AnimBpGraphKind(const UBlueprint* Blueprint, const UEdGraph* Graph)
	{
		const FString Schema = Graph->Schema ? Graph->Schema->GetName() : FString();
		if (Schema.Contains(TEXT("AnimationStateMachine"))) { return TEXT("state_machine"); }
		if (Schema.Contains(TEXT("AnimationTransition"))) { return TEXT("transition_rule"); }
		if (Schema.Contains(TEXT("AnimationStateGraph"))) { return TEXT("state"); }
		if (Schema.Contains(TEXT("AnimationConduit"))) { return TEXT("conduit"); }
		if (Schema.Contains(TEXT("AnimationGraph"))) { return TEXT("animation_graph"); }
		if (Blueprint->UbergraphPages.Contains(Graph)) { return TEXT("event_graph"); }
		if (Blueprint->FunctionGraphs.Contains(Graph)) { return TEXT("function"); }
		if (Blueprint->MacroGraphs.Contains(Graph)) { return TEXT("macro"); }
		return TEXT("sub_graph");
	}

	TArray<TSharedPtr<FJsonValue>> AnimBpDescribeGraphs(const UBlueprint* Blueprint)
	{
		TArray<UEdGraph*> Graphs;
		Blueprint->GetAllGraphs(Graphs);
		TArray<TSharedPtr<FJsonValue>> Out;
		for (const UEdGraph* Graph : Graphs)
		{
			TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("name"), Graph->GetName());
			Item->SetStringField(TEXT("kind"), AnimBpGraphKind(Blueprint, Graph));
			Item->SetNumberField(TEXT("node_count"), Graph->Nodes.Num());
			if (const UEdGraph* Parent = Cast<UEdGraph>(Graph->GetOuter()))
			{
				Item->SetStringField(TEXT("parent_graph"), Parent->GetName());
			}
			Out.Add(AnimBpJson(Item));
		}
		return Out;
	}

	TSharedRef<FJsonObject> AnimBpDescribeNode(const UEdGraphNode* Node, bool bIncludePins)
	{
		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetStringField(TEXT("id"), Node->GetName());
		Out->SetStringField(TEXT("class"), Node->GetClass()->GetName());
		Out->SetStringField(TEXT("title"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
		Out->SetNumberField(TEXT("x"), Node->NodePosX);
		Out->SetNumberField(TEXT("y"), Node->NodePosY);
		if (!Node->NodeComment.IsEmpty())
		{
			Out->SetStringField(TEXT("comment"), Node->NodeComment);
		}
		if (Node->bHasCompilerMessage)
		{
			Out->SetStringField(TEXT("compiler_message"), Node->ErrorMsg);
		}
		// A node that owns a sub-graph (state machine, state, transition) names it so the caller can ask for that graph next.
		TArray<FString> BoundGraphs;
		for (const UEdGraph* Other : Node->GetSubGraphs())
		{
			BoundGraphs.Add(Other->GetName());
		}
		if (BoundGraphs.Num() > 0)
		{
			Out->SetArrayField(TEXT("sub_graphs"), CursorJson::StringArray(BoundGraphs));
		}

		TArray<TSharedPtr<FJsonValue>> Pins;
		TArray<FString> Links;
		for (const UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin->bHidden)
			{
				continue;
			}
			if (bIncludePins)
			{
				TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetStringField(TEXT("name"), Pin->PinName.ToString());
				Item->SetStringField(TEXT("direction"), Pin->Direction == EGPD_Input ? TEXT("input") : TEXT("output"));
				Item->SetStringField(TEXT("type"), Pin->PinType.PinCategory.ToString() + (Pin->PinType.PinSubCategoryObject.IsValid() ? TEXT(":") + Pin->PinType.PinSubCategoryObject->GetName() : FString()));
				Pins.Add(AnimBpJson(Item));
			}
			// Output wires, written node.pin -> node.pin, are enough to rebuild the graph's shape.
			if (Pin->Direction == EGPD_Output)
			{
				for (const UEdGraphPin* Other : Pin->LinkedTo)
				{
					Links.Add(FString::Printf(TEXT("%s -> %s.%s"), *Pin->PinName.ToString(), *Other->GetOwningNode()->GetName(), *Other->PinName.ToString()));
				}
			}
		}
		if (bIncludePins)
		{
			Out->SetArrayField(TEXT("pins"), Pins);
		}
		if (Links.Num() > 0)
		{
			Out->SetArrayField(TEXT("links"), CursorJson::StringArray(Links));
		}
		return Out;
	}

	/** Describes one graph's nodes and, when asked, every graph below it (state machines contain states contain graphs). */
	void AnimBpDescribeGraphNodes(const UBlueprint* Blueprint, const UEdGraph* Graph, bool bIncludePins, bool bRecurse, int32& InOutBudget, TArray<TSharedPtr<FJsonValue>>& OutGraphs)
	{
		TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("graph"), Graph->GetName());
		Item->SetStringField(TEXT("kind"), AnimBpGraphKind(Blueprint, Graph));
		TArray<TSharedPtr<FJsonValue>> Nodes;
		for (const UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node)
			{
				continue;
			}
			if (InOutBudget <= 0)
			{
				Item->SetBoolField(TEXT("truncated"), true);
				break;
			}
			--InOutBudget;
			Nodes.Add(AnimBpJson(AnimBpDescribeNode(Node, bIncludePins)));
		}
		Item->SetArrayField(TEXT("nodes"), Nodes);
		OutGraphs.Add(AnimBpJson(Item));

		if (bRecurse)
		{
			for (const UEdGraph* Sub : Graph->SubGraphs)
			{
				if (Sub)
				{
					AnimBpDescribeGraphNodes(Blueprint, Sub, bIncludePins, true, InOutBudget, OutGraphs);
				}
			}
		}
	}

	/** An empty name means the animation graph, the one that produces the final pose. */
	UEdGraph* AnimBpFindGraph(UBlueprint* Blueprint, const FString& Name, FString& OutError)
	{
		TArray<UEdGraph*> Graphs;
		Blueprint->GetAllGraphs(Graphs);
		TArray<FString> Names;
		for (UEdGraph* Graph : Graphs)
		{
			Names.Add(Graph->GetName());
			if (Name.IsEmpty() ? AnimBpGraphKind(Blueprint, Graph) == TEXT("animation_graph") : Graph->GetName().Equals(Name, ESearchCase::IgnoreCase))
			{
				return Graph;
			}
		}
		OutError = Name.IsEmpty() ? FString::Printf(TEXT("This Animation Blueprint has no animation graph. Graphs: %s"), *FString::Join(Names, TEXT(", ")))
			: FString::Printf(TEXT("No graph named '%s'. Graphs: %s"), *Name, *FString::Join(Names, TEXT(", ")));
		return nullptr;
	}
}

void CursorToolPacks::RegisterAnimBlueprintPack()
{
	FCursorToolBuilder(TEXT("animbp_create"), AnimBpPack)
		.Describe(TEXT("Create an Animation Blueprint for a skeleton. The parent is AnimInstance unless you pass a native or Blueprint AnimInstance subclass. "
			"It exists in memory until asset_save writes it. Returns its path and graphs; inspect them with animbp_list_nodes."))
		.Risk(ECursorToolRisk::Mutating)
		.Schema(FCursorSchema()
			.Str(TEXT("path"), TEXT("Where to create it, for example /Game/Characters/ABP_Hero."), true)
			.Str(TEXT("skeleton"), TEXT("Skeleton asset path the Animation Blueprint targets."), true)
			.Str(TEXT("parent_class"), TEXT("AnimInstance subclass: a native name, a /Script/Module.Class path, or an Animation Blueprint asset path. Default AnimInstance."))
			.Str(TEXT("preview_mesh"), TEXT("Skeletal mesh asset path to preview with. Optional.")))
		.Run([](const FCursorArgs& Args, const FCursorToolContext&)
		{
			FString Error;
			UObject* SkeletonAsset = CursorToolHelpers::LoadAsset(Args.Str(TEXT("skeleton")), Error);
			if (!SkeletonAsset)
			{
				return FCursorToolResult::Error(Args.Has(TEXT("skeleton")) ? Error : FString(TEXT("'skeleton' is required: the asset path of a Skeleton.")));
			}
			USkeleton* Skeleton = Cast<USkeleton>(SkeletonAsset);
			if (!Skeleton)
			{
				return FCursorToolResult::Error(FString::Printf(TEXT("'%s' is a %s, not a Skeleton."), *Args.Str(TEXT("skeleton")), *SkeletonAsset->GetClass()->GetName()));
			}

			USkeletalMesh* PreviewMesh = nullptr;
			if (Args.Has(TEXT("preview_mesh")))
			{
				UObject* MeshAsset = CursorToolHelpers::LoadAsset(Args.Str(TEXT("preview_mesh")), Error);
				PreviewMesh = Cast<USkeletalMesh>(MeshAsset);
				if (!PreviewMesh)
				{
					return FCursorToolResult::Error(MeshAsset ? FString::Printf(TEXT("'%s' is not a skeletal mesh."), *Args.Str(TEXT("preview_mesh"))) : Error);
				}
			}

			UClass* Parent = CursorToolHelpers::ResolveClass(Args.Str(TEXT("parent_class"), TEXT("AnimInstance")), UAnimInstance::StaticClass(), Error);
			if (!Parent)
			{
				return FCursorToolResult::Error(Error);
			}
			// The factory would open a dialog for a bad parent, which a headless editor cannot answer.
			if (!FKismetEditorUtilities::CanCreateBlueprintOfClass(Parent))
			{
				return FCursorToolResult::Error(FString::Printf(TEXT("%s cannot be used as an Animation Blueprint parent (it is not marked Blueprintable)."), *Parent->GetName()));
			}

			UPackage* Package = nullptr;
			FString AssetName;
			if (!CursorPackSupport::PrepareNewAsset(Args.Str(TEXT("path")), Package, AssetName, Error))
			{
				return FCursorToolResult::Error(Error);
			}

			UAnimBlueprintFactory* Factory = NewObject<UAnimBlueprintFactory>();
			Factory->BlueprintType = BPTYPE_Normal;
			Factory->ParentClass = Parent;
			Factory->TargetSkeleton = Skeleton;
			Factory->PreviewSkeletalMesh = PreviewMesh;
			Factory->bTemplate = false;
			UAnimBlueprint* AnimBlueprint = Cast<UAnimBlueprint>(Factory->FactoryCreateNew(UAnimBlueprint::StaticClass(), Package, FName(*AssetName),
				RF_Public | RF_Standalone | RF_Transactional, nullptr, GWarn));
			if (!AnimBlueprint)
			{
				return FCursorToolResult::Error(TEXT("The editor could not create the Animation Blueprint. See editor_log_tail."));
			}
			CursorPackSupport::FinishNewAsset(AnimBlueprint);

			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetStringField(TEXT("path"), AnimBlueprint->GetPathName());
			Out->SetStringField(TEXT("skeleton"), Skeleton->GetPathName());
			Out->SetStringField(TEXT("parent_class"), Parent->GetPathName());
			Out->SetStringField(TEXT("status"), AnimBpStatusName(AnimBlueprint->Status));
			Out->SetArrayField(TEXT("graphs"), AnimBpDescribeGraphs(AnimBlueprint));
			Out->SetStringField(TEXT("note"), TEXT("Created in memory. Call asset_save to write it to disk."));
			return FCursorToolResult::OkJson(Out);
		})
		.Register();

	FCursorToolBuilder(TEXT("animbp_get_info"), AnimBpPack)
		.Describe(TEXT("Structure of an Animation Blueprint: target skeleton, parent class, preview mesh, compile status, variables, implemented interfaces and graphs "
			"(animation graph, state machines, states, transition rules, functions). Use animbp_list_nodes to see what is inside a graph."))
		.Risk(ECursorToolRisk::ReadOnly)
		.Schema(FCursorSchema()
			.Str(TEXT("animbp"), TEXT("Animation Blueprint asset path."), true))
		.Run([](const FCursorArgs& Args, const FCursorToolContext&)
		{
			FString Error;
			UAnimBlueprint* AnimBlueprint = AnimBpLoad(Args.Str(TEXT("animbp")), false, Error);
			if (!AnimBlueprint)
			{
				return FCursorToolResult::Error(Error);
			}

			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetStringField(TEXT("path"), AnimBlueprint->GetPathName());
			Out->SetStringField(TEXT("skeleton"), AnimBlueprint->TargetSkeleton ? AnimBlueprint->TargetSkeleton->GetPathName() : FString());
			Out->SetBoolField(TEXT("is_template"), AnimBlueprint->bIsTemplate);
			Out->SetBoolField(TEXT("is_layer_interface"), AnimBlueprint->BlueprintType == BPTYPE_Interface);
			Out->SetStringField(TEXT("parent_class"), AnimBlueprint->ParentClass ? AnimBlueprint->ParentClass->GetPathName() : FString());
			if (const USkeletalMesh* Mesh = AnimBlueprint->GetPreviewMesh())
			{
				Out->SetStringField(TEXT("preview_mesh"), Mesh->GetPathName());
			}
			Out->SetStringField(TEXT("status"), AnimBpStatusName(AnimBlueprint->Status));
			Out->SetBoolField(TEXT("has_unsaved_changes"), AnimBlueprint->GetOutermost()->IsDirty());

			TArray<TSharedPtr<FJsonValue>> Variables;
			for (const FBPVariableDescription& Var : AnimBlueprint->NewVariables)
			{
				TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetStringField(TEXT("name"), Var.VarName.ToString());
				FString Type = Var.VarType.PinCategory.ToString();
				if (const UObject* Sub = Var.VarType.PinSubCategoryObject.Get())
				{
					Type += TEXT(":") + Sub->GetName();
				}
				Item->SetStringField(TEXT("type"), Type);
				Variables.Add(AnimBpJson(Item));
			}
			Out->SetArrayField(TEXT("variables"), Variables);

			TArray<FString> Interfaces;
			for (const FBPInterfaceDescription& Interface : AnimBlueprint->ImplementedInterfaces)
			{
				if (Interface.Interface)
				{
					Interfaces.Add(Interface.Interface->GetName());
				}
			}
			Out->SetArrayField(TEXT("interfaces"), CursorJson::StringArray(Interfaces));
			Out->SetArrayField(TEXT("graphs"), AnimBpDescribeGraphs(AnimBlueprint));
			return FCursorToolResult::OkJson(Out);
		})
		.Register();

	FCursorToolBuilder(TEXT("animbp_list_graphs"), AnimBpPack)
		.Describe(TEXT("List an Animation Blueprint's graphs with kind (animation_graph, state_machine, state, transition_rule, function, event_graph) and node count."))
		.Risk(ECursorToolRisk::ReadOnly)
		.Schema(FCursorSchema()
			.Str(TEXT("animbp"), TEXT("Animation Blueprint asset path."), true))
		.Run([](const FCursorArgs& Args, const FCursorToolContext&)
		{
			FString Error;
			UAnimBlueprint* AnimBlueprint = AnimBpLoad(Args.Str(TEXT("animbp")), false, Error);
			if (!AnimBlueprint)
			{
				return FCursorToolResult::Error(Error);
			}
			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetArrayField(TEXT("graphs"), AnimBpDescribeGraphs(AnimBlueprint));
			return FCursorToolResult::OkJson(Out);
		})
		.Register();

	FCursorToolBuilder(TEXT("animbp_list_nodes"), AnimBpPack)
		.Describe(TEXT("List the nodes of an Animation Blueprint graph: id, class, title, position, comment, compiler message, the sub-graphs a node owns (a state machine's graph, a state's graph) and output wires. "
			"With no graph it reads the animation graph and, by default, everything below it (state machines, their states and transition rules)."))
		.Risk(ECursorToolRisk::ReadOnly)
		.Schema(FCursorSchema()
			.Str(TEXT("animbp"), TEXT("Animation Blueprint asset path."), true)
			.Str(TEXT("graph"), TEXT("Graph name from animbp_list_graphs. Default: the animation graph."))
			.Bool(TEXT("recursive"), TEXT("Also list the graphs below this one. Default true."))
			.Bool(TEXT("include_pins"), TEXT("Include every pin with its type. Default false: wires are listed without them."))
			.Int(TEXT("max_nodes"), TEXT("Stop after this many nodes in total. Default 300.")))
		.Run([](const FCursorArgs& Args, const FCursorToolContext&)
		{
			FString Error;
			UAnimBlueprint* AnimBlueprint = AnimBpLoad(Args.Str(TEXT("animbp")), false, Error);
			UEdGraph* Graph = AnimBlueprint ? AnimBpFindGraph(AnimBlueprint, Args.Str(TEXT("graph")), Error) : nullptr;
			if (!Graph)
			{
				return FCursorToolResult::Error(Error);
			}
			int32 Budget = FMath::Clamp(Args.Has(TEXT("max_nodes")) ? Args.Int(TEXT("max_nodes")) : 300, 1, 5000);
			const bool bRecurse = Args.Has(TEXT("recursive")) ? Args.Bool(TEXT("recursive")) : true;

			TArray<TSharedPtr<FJsonValue>> Graphs;
			AnimBpDescribeGraphNodes(AnimBlueprint, Graph, Args.Bool(TEXT("include_pins")), bRecurse, Budget, Graphs);
			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetArrayField(TEXT("graphs"), Graphs);
			return FCursorToolResult::OkJson(Out);
		})
		.Register();

	FCursorToolBuilder(TEXT("animbp_compile"), AnimBpPack)
		.Describe(TEXT("Compile an Animation Blueprint and return its status with every error and warning the compiler reported. Does not save: call asset_save."))
		.Risk(ECursorToolRisk::Mutating)
		.Schema(FCursorSchema()
			.Str(TEXT("animbp"), TEXT("Animation Blueprint asset path."), true))
		.Run([](const FCursorArgs& Args, const FCursorToolContext&)
		{
			FString Error;
			UAnimBlueprint* AnimBlueprint = AnimBpLoad(Args.Str(TEXT("animbp")), true, Error);
			if (!AnimBlueprint)
			{
				return FCursorToolResult::Error(Error);
			}

			FCompilerResultsLog Results;
			Results.SetSourcePath(AnimBlueprint->GetPathName());
			Results.SetSilentMode(true);
			FKismetEditorUtilities::CompileBlueprint(AnimBlueprint, EBlueprintCompileOptions::SkipGarbageCollection, &Results);

			TArray<TSharedPtr<FJsonValue>> Errors;
			TArray<TSharedPtr<FJsonValue>> Warnings;
			for (const TSharedRef<FTokenizedMessage>& Message : Results.Messages)
			{
				const FString Text = Message->ToText().ToString();
				if (Message->GetSeverity() == EMessageSeverity::Error)
				{
					Errors.Add(MakeShared<FJsonValueString>(Text));
				}
				else if (Message->GetSeverity() == EMessageSeverity::Warning || Message->GetSeverity() == EMessageSeverity::PerformanceWarning)
				{
					Warnings.Add(MakeShared<FJsonValueString>(Text));
				}
			}

			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetStringField(TEXT("status"), AnimBpStatusName(AnimBlueprint->Status));
			Out->SetBoolField(TEXT("compiled"), AnimBlueprint->Status != BS_Error);
			Out->SetArrayField(TEXT("errors"), Errors);
			Out->SetArrayField(TEXT("warnings"), Warnings);
			FCursorToolResult Result = FCursorToolResult::OkJson(Out);
			Result.bIsError = AnimBlueprint->Status == BS_Error;
			return Result;
		})
		.Register();
}

void CursorToolPacks::UnregisterAnimBlueprintPack()
{
	FCursorToolRegistry::Get().UnregisterPack(AnimBpPack);
}
