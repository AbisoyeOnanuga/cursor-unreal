#include "CursorToolPacks.h"

#include "CursorJson.h"
#include "CursorPackSupport.h"
#include "CursorTool.h"
#include "CursorToolHelpers.h"
#include "CursorToolRegistry.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "GameFramework/Actor.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_Event.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_Self.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Logging/TokenizedMessage.h"
#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "UObject/UObjectIterator.h"

/*
 * Blueprint pack. Scope is what the public editor APIs do reliably in 5.6: create a Blueprint from a
 * parent class, read its structure, add variables / components / function graphs, add the common K2 nodes
 * (events, function calls, variable get/set, branch, sequence), wire and default pins, and compile with errors
 * returned. Anything fancier (macros, timelines, interfaces, animation graphs) goes through python_exec.
 * Edits stay in memory until asset_save.
 */

namespace
{
	const TCHAR* BlueprintPack = TEXT("blueprint");

	TSharedPtr<FJsonValue> AsValue(const TSharedRef<FJsonObject>& Object)
	{
		return MakeShared<FJsonValueObject>(Object);
	}

	UBlueprint* LoadBlueprint(const FString& Path, FString& OutError)
	{
		if (Path.IsEmpty())
		{
			OutError = TEXT("'blueprint' is required: the asset path of a Blueprint, for example /Game/Blueprints/BP_Door.");
			return nullptr;
		}
		UObject* Asset = CursorToolHelpers::LoadAsset(Path, OutError);
		if (!Asset)
		{
			return nullptr;
		}
		UBlueprint* Blueprint = Cast<UBlueprint>(Asset);
		if (!Blueprint)
		{
			OutError = FString::Printf(TEXT("'%s' is a %s, not a Blueprint."), *Path, *Asset->GetClass()->GetName());
		}
		return Blueprint;
	}

	/** Loads a Blueprint for editing: it must be project content, not the engine's. */
	UBlueprint* LoadEditableBlueprint(const FString& Path, FString& OutError)
	{
		UBlueprint* Blueprint = LoadBlueprint(Path, OutError);
		if (Blueprint && !CursorPackSupport::IsWritableContentPath(Blueprint->GetOutermost()->GetName(), OutError))
		{
			return nullptr;
		}
		return Blueprint;
	}

	FString GraphKind(const UBlueprint* Blueprint, const UEdGraph* Graph)
	{
		if (Blueprint->UbergraphPages.Contains(Graph)) { return TEXT("event_graph"); }
		if (Blueprint->FunctionGraphs.Contains(Graph)) { return TEXT("function"); }
		if (Blueprint->MacroGraphs.Contains(Graph)) { return TEXT("macro"); }
		if (Blueprint->DelegateSignatureGraphs.Contains(Graph)) { return TEXT("delegate_signature"); }
		return TEXT("sub_graph");
	}

	/** An empty name means the main event graph. */
	UEdGraph* FindGraph(UBlueprint* Blueprint, const FString& Name, FString& OutError)
	{
		if (Name.IsEmpty())
		{
			if (Blueprint->UbergraphPages.Num() > 0)
			{
				return Blueprint->UbergraphPages[0];
			}
			OutError = TEXT("This Blueprint has no event graph. Pass 'graph' with a function graph name (see blueprint_list_graphs).");
			return nullptr;
		}
		TArray<UEdGraph*> Graphs;
		Blueprint->GetAllGraphs(Graphs);
		TArray<FString> Names;
		for (UEdGraph* Graph : Graphs)
		{
			Names.Add(Graph->GetName());
			if (Graph->GetName().Equals(Name, ESearchCase::IgnoreCase))
			{
				return Graph;
			}
		}
		OutError = FString::Printf(TEXT("No graph named '%s'. Graphs: %s"), *Name, *FString::Join(Names, TEXT(", ")));
		return nullptr;
	}

	UEdGraphNode* FindNode(UBlueprint* Blueprint, const FString& GraphName, const FString& Id, FString& OutError)
	{
		if (Id.IsEmpty())
		{
			OutError = TEXT("A node id is required (the 'id' reported by blueprint_get_nodes or blueprint_add_node).");
			return nullptr;
		}
		TArray<UEdGraph*> Graphs;
		if (!GraphName.IsEmpty())
		{
			UEdGraph* Graph = FindGraph(Blueprint, GraphName, OutError);
			if (!Graph)
			{
				return nullptr;
			}
			Graphs.Add(Graph);
		}
		else
		{
			Blueprint->GetAllGraphs(Graphs);
		}
		FGuid Guid;
		const bool bIsGuid = FGuid::Parse(Id, Guid);
		for (const UEdGraph* Graph : Graphs)
		{
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (Node && (Node->GetName().Equals(Id, ESearchCase::IgnoreCase) || (bIsGuid && Node->NodeGuid == Guid)))
				{
					return Node;
				}
			}
		}
		OutError = FString::Printf(TEXT("No node '%s'. Use blueprint_get_nodes to list node ids."), *Id);
		return nullptr;
	}

	UEdGraphPin* FindPin(UEdGraphNode* Node, const FString& Name, EEdGraphPinDirection Direction, FString& OutError)
	{
		TArray<FString> Candidates;
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin->bHidden || (Direction != EGPD_MAX && Pin->Direction != Direction))
			{
				continue;
			}
			Candidates.Add(Pin->PinName.ToString());
			if (Pin->PinName.ToString().Equals(Name, ESearchCase::IgnoreCase) || Pin->GetDisplayName().ToString().Equals(Name, ESearchCase::IgnoreCase))
			{
				return Pin;
			}
		}
		OutError = FString::Printf(TEXT("Node %s has no %spin '%s'. Pins: %s"), *Node->GetName(),
			Direction == EGPD_Input ? TEXT("input ") : Direction == EGPD_Output ? TEXT("output ") : TEXT(""), *Name, *FString::Join(Candidates, TEXT(", ")));
		return nullptr;
	}

	FString PinTypeToString(const FEdGraphPinType& Type)
	{
		FString Out = Type.PinCategory.ToString();
		if (Type.PinCategory == UEdGraphSchema_K2::PC_Real)
		{
			Out = TEXT("float");
		}
		if (const UObject* Sub = Type.PinSubCategoryObject.Get())
		{
			Out += TEXT(":") + Sub->GetName();
		}
		else if (!Type.PinSubCategory.IsNone() && Type.PinCategory != UEdGraphSchema_K2::PC_Real)
		{
			Out += TEXT(":") + Type.PinSubCategory.ToString();
		}
		switch (Type.ContainerType)
		{
		case EPinContainerType::Array: return TEXT("array of ") + Out;
		case EPinContainerType::Set: return TEXT("set of ") + Out;
		case EPinContainerType::Map: return TEXT("map of ") + Out;
		default: return Out;
		}
	}

	/** "bool", "int", "float", "string", "vector", "struct:/Script/CoreUObject.Vector", "object:Actor", "class:Actor"... */
	bool ParsePinType(const FString& Text, bool bArray, FEdGraphPinType& Out, FString& OutError)
	{
		const FString Type = Text.TrimStartAndEnd();
		const FString Lower = Type.ToLower();
		Out = FEdGraphPinType();
		Out.ContainerType = bArray ? EPinContainerType::Array : EPinContainerType::None;

		auto SetStruct = [&](UScriptStruct* Struct)
		{
			Out.PinCategory = UEdGraphSchema_K2::PC_Struct;
			Out.PinSubCategoryObject = Struct;
		};

		if (Lower == TEXT("bool") || Lower == TEXT("boolean")) { Out.PinCategory = UEdGraphSchema_K2::PC_Boolean; }
		else if (Lower == TEXT("byte")) { Out.PinCategory = UEdGraphSchema_K2::PC_Byte; }
		else if (Lower == TEXT("int") || Lower == TEXT("integer") || Lower == TEXT("int32")) { Out.PinCategory = UEdGraphSchema_K2::PC_Int; }
		else if (Lower == TEXT("int64")) { Out.PinCategory = UEdGraphSchema_K2::PC_Int64; }
		else if (Lower == TEXT("float") || Lower == TEXT("double") || Lower == TEXT("real"))
		{
			Out.PinCategory = UEdGraphSchema_K2::PC_Real;
			Out.PinSubCategory = UEdGraphSchema_K2::PC_Double;
		}
		else if (Lower == TEXT("string")) { Out.PinCategory = UEdGraphSchema_K2::PC_String; }
		else if (Lower == TEXT("name")) { Out.PinCategory = UEdGraphSchema_K2::PC_Name; }
		else if (Lower == TEXT("text")) { Out.PinCategory = UEdGraphSchema_K2::PC_Text; }
		else if (Lower == TEXT("vector")) { SetStruct(TBaseStructure<FVector>::Get()); }
		else if (Lower == TEXT("vector2d")) { SetStruct(TBaseStructure<FVector2D>::Get()); }
		else if (Lower == TEXT("rotator")) { SetStruct(TBaseStructure<FRotator>::Get()); }
		else if (Lower == TEXT("transform")) { SetStruct(TBaseStructure<FTransform>::Get()); }
		else if (Lower == TEXT("color") || Lower == TEXT("linearcolor")) { SetStruct(TBaseStructure<FLinearColor>::Get()); }
		else if (Lower.StartsWith(TEXT("struct:")))
		{
			const FString Name = Type.Mid(7).TrimStartAndEnd();
			UScriptStruct* Struct = Name.StartsWith(TEXT("/")) ? FindObject<UScriptStruct>(nullptr, *Name) : FindFirstObject<UScriptStruct>(*Name, EFindFirstObjectOptions::NativeFirst);
			if (!Struct)
			{
				OutError = FString::Printf(TEXT("No struct '%s'."), *Name);
				return false;
			}
			SetStruct(Struct);
		}
		else if (Lower.StartsWith(TEXT("object:")) || Lower.StartsWith(TEXT("class:")))
		{
			const bool bClassPin = Lower.StartsWith(TEXT("class:"));
			UClass* Class = CursorToolHelpers::ResolveClass(Type.Mid(bClassPin ? 6 : 7), nullptr, OutError);
			if (!Class)
			{
				return false;
			}
			Out.PinCategory = bClassPin ? UEdGraphSchema_K2::PC_Class : UEdGraphSchema_K2::PC_Object;
			Out.PinSubCategoryObject = Class;
		}
		else
		{
			OutError = FString::Printf(TEXT("Unknown type '%s'. Use bool, byte, int, int64, float, string, name, text, vector, vector2d, rotator, transform, color, struct:<name>, object:<class> or class:<class>."), *Type);
			return false;
		}
		return true;
	}

	TSharedRef<FJsonObject> DescribePin(const UEdGraphPin* Pin)
	{
		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetStringField(TEXT("name"), Pin->PinName.ToString());
		Out->SetStringField(TEXT("direction"), Pin->Direction == EGPD_Input ? TEXT("input") : TEXT("output"));
		Out->SetStringField(TEXT("type"), PinTypeToString(Pin->PinType));
		const FString Default = Pin->GetDefaultAsString();
		if (!Default.IsEmpty())
		{
			Out->SetStringField(TEXT("default"), Default);
		}
		if (Pin->LinkedTo.Num() > 0)
		{
			TArray<FString> Links;
			for (const UEdGraphPin* Other : Pin->LinkedTo)
			{
				Links.Add(Other->GetOwningNode()->GetName() + TEXT(".") + Other->PinName.ToString());
			}
			Out->SetArrayField(TEXT("linked_to"), CursorJson::StringArray(Links));
		}
		return Out;
	}

	TSharedRef<FJsonObject> DescribeNode(const UEdGraphNode* Node, bool bIncludePins)
	{
		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetStringField(TEXT("id"), Node->GetName());
		Out->SetStringField(TEXT("guid"), Node->NodeGuid.ToString(EGuidFormats::Digits));
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
		if (bIncludePins)
		{
			TArray<TSharedPtr<FJsonValue>> Pins;
			for (const UEdGraphPin* Pin : Node->Pins)
			{
				if (!Pin->bHidden)
				{
					Pins.Add(AsValue(DescribePin(Pin)));
				}
			}
			Out->SetArrayField(TEXT("pins"), Pins);
		}
		return Out;
	}

	bool IsValidMemberName(const FString& Name, FString& OutError)
	{
		bool bOk = !Name.IsEmpty() && Name.Len() <= 100 && (FChar::IsAlpha(Name[0]) || Name[0] == TEXT('_'));
		for (const TCHAR Ch : Name)
		{
			bOk = bOk && (FChar::IsAlnum(Ch) || Ch == TEXT('_'));
		}
		if (!bOk)
		{
			OutError = FString::Printf(TEXT("'%s' is not a valid name: use letters, digits and underscores, starting with a letter."), *Name);
		}
		return bOk;
	}

	const TCHAR* StatusToString(EBlueprintStatus Status)
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

	TArray<TSharedPtr<FJsonValue>> DescribeGraphs(const UBlueprint* Blueprint)
	{
		TArray<UEdGraph*> Graphs;
		Blueprint->GetAllGraphs(Graphs);
		TArray<TSharedPtr<FJsonValue>> Out;
		for (const UEdGraph* Graph : Graphs)
		{
			TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("name"), Graph->GetName());
			Item->SetStringField(TEXT("kind"), GraphKind(Blueprint, Graph));
			Item->SetNumberField(TEXT("node_count"), Graph->Nodes.Num());
			Out.Add(AsValue(Item));
		}
		return Out;
	}

	/** Which graph a new node goes into, and where. Nodes without a position are staggered so they do not stack. */
	void PlaceNode(UEdGraphNode* Node, const FCursorArgs& Args, const UEdGraph* Graph)
	{
		Node->NodePosX = Args.Has(TEXT("x")) ? Args.Int(TEXT("x")) : 0;
		Node->NodePosY = Args.Has(TEXT("y")) ? Args.Int(TEXT("y")) : Graph->Nodes.Num() * 160;
	}
}

void CursorToolPacks::RegisterBlueprintPack()
{
	FCursorToolBuilder(TEXT("blueprint_create"), BlueprintPack)
		.Describe(TEXT("Create a new Blueprint asset from a parent class (Actor, Pawn, Character, ActorComponent, a C++ class from cpp_create_class, another Blueprint...). "
			"The Blueprint exists in memory until asset_save writes it. Returns its path and graphs."))
		.Risk(ECursorToolRisk::Mutating)
		.Schema(FCursorSchema()
			.Str(TEXT("path"), TEXT("Where to create it, for example /Game/Blueprints/BP_Door."), true)
			.Str(TEXT("parent_class"), TEXT("Parent class: a native name (Actor), a /Script/Module.Class path, or another Blueprint's asset path. Default Actor.")))
		.Run([](const FCursorArgs& Args, const FCursorToolContext&)
		{
			FString Error;
			UClass* Parent = CursorToolHelpers::ResolveClass(Args.Str(TEXT("parent_class"), TEXT("Actor")), nullptr, Error);
			if (!Parent)
			{
				return FCursorToolResult::Error(Error);
			}
			if (!FKismetEditorUtilities::CanCreateBlueprintOfClass(Parent))
			{
				return FCursorToolResult::Error(FString::Printf(TEXT("%s cannot be used as a Blueprint parent (it is not marked Blueprintable)."), *Parent->GetName()));
			}

			UPackage* Package = nullptr;
			FString AssetName;
			if (!CursorPackSupport::PrepareNewAsset(Args.Str(TEXT("path")), Package, AssetName, Error))
			{
				return FCursorToolResult::Error(Error);
			}

			const EBlueprintType Type = Parent->IsChildOf(UBlueprintFunctionLibrary::StaticClass()) ? BPTYPE_FunctionLibrary : BPTYPE_Normal;
			UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(Parent, Package, FName(*AssetName), Type,
				UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass(), FName(TEXT("CursorUnreal")));
			if (!Blueprint)
			{
				return FCursorToolResult::Error(TEXT("The editor could not create the Blueprint. See editor_log_tail."));
			}
			CursorPackSupport::FinishNewAsset(Blueprint);

			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetStringField(TEXT("path"), Blueprint->GetPathName());
			Out->SetStringField(TEXT("parent_class"), Parent->GetPathName());
			Out->SetStringField(TEXT("status"), StatusToString(Blueprint->Status));
			Out->SetArrayField(TEXT("graphs"), DescribeGraphs(Blueprint));
			Out->SetStringField(TEXT("note"), TEXT("Created in memory. Call asset_save to write it to disk."));
			return FCursorToolResult::OkJson(Out);
		})
		.Register();

	FCursorToolBuilder(TEXT("blueprint_get_info"), BlueprintPack)
		.Describe(TEXT("Structure of a Blueprint: parent class, compile status, variables (with types and defaults), components, and graphs. "
			"Use blueprint_get_nodes to see what is inside a graph."))
		.Risk(ECursorToolRisk::ReadOnly)
		.Schema(FCursorSchema()
			.Str(TEXT("blueprint"), TEXT("Blueprint asset path."), true))
		.Run([](const FCursorArgs& Args, const FCursorToolContext&)
		{
			FString Error;
			UBlueprint* Blueprint = LoadBlueprint(Args.Str(TEXT("blueprint")), Error);
			if (!Blueprint)
			{
				return FCursorToolResult::Error(Error);
			}

			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetStringField(TEXT("path"), Blueprint->GetPathName());
			Out->SetStringField(TEXT("parent_class"), Blueprint->ParentClass ? Blueprint->ParentClass->GetPathName() : FString());
			Out->SetStringField(TEXT("status"), StatusToString(Blueprint->Status));
			Out->SetBoolField(TEXT("has_unsaved_changes"), Blueprint->GetOutermost()->IsDirty());

			TArray<TSharedPtr<FJsonValue>> Variables;
			for (const FBPVariableDescription& Var : Blueprint->NewVariables)
			{
				TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetStringField(TEXT("name"), Var.VarName.ToString());
				Item->SetStringField(TEXT("type"), PinTypeToString(Var.VarType));
				if (!Var.DefaultValue.IsEmpty())
				{
					Item->SetStringField(TEXT("default"), Var.DefaultValue);
				}
				Item->SetBoolField(TEXT("instance_editable"), (Var.PropertyFlags & CPF_DisableEditOnInstance) == 0);
				Variables.Add(AsValue(Item));
			}
			Out->SetArrayField(TEXT("variables"), Variables);

			TArray<TSharedPtr<FJsonValue>> Components;
			if (const USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript)
			{
				for (const USCS_Node* Node : SCS->GetAllNodes())
				{
					TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
					Item->SetStringField(TEXT("name"), Node->GetVariableName().ToString());
					Item->SetStringField(TEXT("class"), Node->ComponentClass ? Node->ComponentClass->GetName() : FString());
					if (const USCS_Node* ParentNode = SCS->FindParentNode(const_cast<USCS_Node*>(Node)))
					{
						Item->SetStringField(TEXT("parent"), ParentNode->GetVariableName().ToString());
					}
					Components.Add(AsValue(Item));
				}
			}
			Out->SetArrayField(TEXT("components"), Components);
			Out->SetArrayField(TEXT("graphs"), DescribeGraphs(Blueprint));
			return FCursorToolResult::OkJson(Out);
		})
		.Register();

	FCursorToolBuilder(TEXT("blueprint_list_graphs"), BlueprintPack)
		.Describe(TEXT("List a Blueprint's graphs (event graph, functions, macros) with kind and node count."))
		.Risk(ECursorToolRisk::ReadOnly)
		.Schema(FCursorSchema()
			.Str(TEXT("blueprint"), TEXT("Blueprint asset path."), true))
		.Run([](const FCursorArgs& Args, const FCursorToolContext&)
		{
			FString Error;
			const UBlueprint* Blueprint = LoadBlueprint(Args.Str(TEXT("blueprint")), Error);
			if (!Blueprint)
			{
				return FCursorToolResult::Error(Error);
			}
			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetArrayField(TEXT("graphs"), DescribeGraphs(Blueprint));
			return FCursorToolResult::OkJson(Out);
		})
		.Register();

	FCursorToolBuilder(TEXT("blueprint_get_nodes"), BlueprintPack)
		.Describe(TEXT("List the nodes in one graph: id, class, title, position, compile message and (by default) every pin with its type, default value and what it is linked to. "
			"Node ids are what blueprint_connect_pins, blueprint_set_pin_default and blueprint_remove_node take."))
		.Risk(ECursorToolRisk::ReadOnly)
		.Schema(FCursorSchema()
			.Str(TEXT("blueprint"), TEXT("Blueprint asset path."), true)
			.Str(TEXT("graph"), TEXT("Graph name from blueprint_list_graphs. Default: the main event graph."))
			.Bool(TEXT("include_pins"), TEXT("Include pins. Default true. Turn off for a quick overview of a large graph."))
			.Int(TEXT("max_nodes"), TEXT("Maximum nodes to return. Default 200.")))
		.Run([](const FCursorArgs& Args, const FCursorToolContext&)
		{
			FString Error;
			UBlueprint* Blueprint = LoadBlueprint(Args.Str(TEXT("blueprint")), Error);
			if (!Blueprint)
			{
				return FCursorToolResult::Error(Error);
			}
			const UEdGraph* Graph = FindGraph(Blueprint, Args.Str(TEXT("graph")), Error);
			if (!Graph)
			{
				return FCursorToolResult::Error(Error);
			}
			const bool bPins = Args.Bool(TEXT("include_pins"), true);
			const int32 Max = FMath::Clamp(Args.Int(TEXT("max_nodes"), 200), 1, 1000);

			TArray<TSharedPtr<FJsonValue>> Nodes;
			for (const UEdGraphNode* Node : Graph->Nodes)
			{
				if (Node && Nodes.Num() < Max)
				{
					Nodes.Add(AsValue(DescribeNode(Node, bPins)));
				}
			}
			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetStringField(TEXT("graph"), Graph->GetName());
			Out->SetNumberField(TEXT("total"), Graph->Nodes.Num());
			Out->SetArrayField(TEXT("nodes"), Nodes);
			return FCursorToolResult::OkJson(Out);
		})
		.Register();

	FCursorToolBuilder(TEXT("blueprint_add_variable"), BlueprintPack)
		.Describe(TEXT("Add a member variable to a Blueprint. Call blueprint_compile afterwards, then asset_save."))
		.Risk(ECursorToolRisk::Mutating)
		.Schema(FCursorSchema()
			.Str(TEXT("blueprint"), TEXT("Blueprint asset path."), true)
			.Str(TEXT("name"), TEXT("Variable name: letters, digits and underscores."), true)
			.Str(TEXT("type"), TEXT("bool, byte, int, int64, float, string, name, text, vector, vector2d, rotator, transform, color, struct:<name>, object:<class>, class:<class>."), true)
			.Bool(TEXT("is_array"), TEXT("Make it an array of that type."))
			.Str(TEXT("default_value"), TEXT("Default value as text, as the editor writes it: true, 42, 1.5, Hello, (X=1,Y=2,Z=3)."))
			.Bool(TEXT("instance_editable"), TEXT("Editable per placed instance in the Details panel. Default false.")))
		.Run([](const FCursorArgs& Args, const FCursorToolContext&)
		{
			FString Error;
			UBlueprint* Blueprint = LoadEditableBlueprint(Args.Str(TEXT("blueprint")), Error);
			if (!Blueprint)
			{
				return FCursorToolResult::Error(Error);
			}
			const FString Name = Args.Str(TEXT("name"));
			FEdGraphPinType Type;
			if (!IsValidMemberName(Name, Error) || !ParsePinType(Args.Str(TEXT("type")), Args.Bool(TEXT("is_array")), Type, Error))
			{
				return FCursorToolResult::Error(Error);
			}
			if (FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, FName(*Name)) != INDEX_NONE)
			{
				return FCursorToolResult::Error(FString::Printf(TEXT("The Blueprint already has a variable named '%s'."), *Name));
			}
			if (!FBlueprintEditorUtils::AddMemberVariable(Blueprint, FName(*Name), Type, Args.Str(TEXT("default_value"))))
			{
				return FCursorToolResult::Error(FString::Printf(TEXT("The editor refused variable '%s' (a name clash with an inherited member, most likely)."), *Name));
			}
			// New variables are not editable per instance unless asked.
			FBlueprintEditorUtils::SetBlueprintOnlyEditableFlag(Blueprint, FName(*Name), !Args.Bool(TEXT("instance_editable")));
			return FCursorToolResult::Ok(FString::Printf(TEXT("Added %s variable '%s'. Call blueprint_compile next."), *PinTypeToString(Type), *Name));
		})
		.Register();

	FCursorToolBuilder(TEXT("blueprint_add_component"), BlueprintPack)
		.Describe(TEXT("Add a component to an Actor Blueprint (StaticMeshComponent, PointLightComponent, BoxComponent...). "
			"Scene components attach to 'parent' or become the root when there is none. Call blueprint_compile afterwards."))
		.Risk(ECursorToolRisk::Mutating)
		.Schema(FCursorSchema()
			.Str(TEXT("blueprint"), TEXT("Actor Blueprint asset path."), true)
			.Str(TEXT("class"), TEXT("Component class, for example StaticMeshComponent."), true)
			.Str(TEXT("name"), TEXT("Component name. Default: derived from the class."))
			.Str(TEXT("parent"), TEXT("Name of an existing scene component to attach to.")))
		.Run([](const FCursorArgs& Args, const FCursorToolContext&)
		{
			FString Error;
			UBlueprint* Blueprint = LoadEditableBlueprint(Args.Str(TEXT("blueprint")), Error);
			if (!Blueprint)
			{
				return FCursorToolResult::Error(Error);
			}
			USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
			if (!SCS)
			{
				return FCursorToolResult::Error(TEXT("Only Actor Blueprints have components."));
			}
			UClass* Class = CursorToolHelpers::ResolveClass(Args.Str(TEXT("class")), UActorComponent::StaticClass(), Error);
			if (!Class)
			{
				return FCursorToolResult::Error(Error);
			}
			if (Class->HasAnyClassFlags(CLASS_Abstract))
			{
				return FCursorToolResult::Error(FString::Printf(TEXT("%s is abstract and cannot be added."), *Class->GetName()));
			}

			FString Name = Args.Str(TEXT("name"));
			if (Name.IsEmpty())
			{
				Name = Class->GetName().Replace(TEXT("Component"), TEXT(""));
				if (Name.IsEmpty())
				{
					Name = Class->GetName();
				}
			}
			if (!IsValidMemberName(Name, Error))
			{
				return FCursorToolResult::Error(Error);
			}
			for (const USCS_Node* Existing : SCS->GetAllNodes())
			{
				if (Existing->GetVariableName().ToString().Equals(Name, ESearchCase::IgnoreCase))
				{
					return FCursorToolResult::Error(FString::Printf(TEXT("A component named '%s' already exists."), *Name));
				}
			}

			USCS_Node* ParentNode = nullptr;
			if (Args.Has(TEXT("parent")))
			{
				const FString ParentName = Args.Str(TEXT("parent"));
				for (USCS_Node* Existing : SCS->GetAllNodes())
				{
					if (Existing->GetVariableName().ToString().Equals(ParentName, ESearchCase::IgnoreCase))
					{
						ParentNode = Existing;
					}
				}
				if (!ParentNode)
				{
					return FCursorToolResult::Error(FString::Printf(TEXT("No component named '%s' to attach to. See blueprint_get_info."), *ParentName));
				}
				if (!Class->IsChildOf(USceneComponent::StaticClass()) || !ParentNode->ComponentClass || !ParentNode->ComponentClass->IsChildOf(USceneComponent::StaticClass()))
				{
					return FCursorToolResult::Error(TEXT("Only scene components can be attached to a parent."));
				}
			}

			Blueprint->Modify();
			SCS->Modify();
			USCS_Node* NewNode = SCS->CreateNode(Class, FName(*Name));
			if (!NewNode)
			{
				return FCursorToolResult::Error(TEXT("The editor could not create the component."));
			}
			if (ParentNode)
			{
				ParentNode->AddChildNode(NewNode);
			}
			else
			{
				SCS->AddNode(NewNode);
			}
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
			return FCursorToolResult::Ok(FString::Printf(TEXT("Added %s '%s'%s. Call blueprint_compile next."), *Class->GetName(), *Name,
				ParentNode ? *FString::Printf(TEXT(" under '%s'"), *ParentNode->GetVariableName().ToString()) : TEXT("")));
		})
		.Register();

	FCursorToolBuilder(TEXT("blueprint_add_function"), BlueprintPack)
		.Describe(TEXT("Add an empty function graph (with its entry node) to a Blueprint. Add nodes to it with blueprint_add_node using graph=<function name>."))
		.Risk(ECursorToolRisk::Mutating)
		.Schema(FCursorSchema()
			.Str(TEXT("blueprint"), TEXT("Blueprint asset path."), true)
			.Str(TEXT("name"), TEXT("Function name."), true))
		.Run([](const FCursorArgs& Args, const FCursorToolContext&)
		{
			FString Error;
			UBlueprint* Blueprint = LoadEditableBlueprint(Args.Str(TEXT("blueprint")), Error);
			if (!Blueprint)
			{
				return FCursorToolResult::Error(Error);
			}
			const FString Name = Args.Str(TEXT("name"));
			if (!IsValidMemberName(Name, Error))
			{
				return FCursorToolResult::Error(Error);
			}
			if (!FBlueprintEditorUtils::IsGraphNameUnique(Blueprint, FName(*Name)))
			{
				return FCursorToolResult::Error(FString::Printf(TEXT("A graph named '%s' already exists."), *Name));
			}
			UEdGraph* Graph = FBlueprintEditorUtils::CreateNewGraph(Blueprint, FName(*Name), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
			if (!Graph)
			{
				return FCursorToolResult::Error(TEXT("The editor could not create the graph."));
			}
			FBlueprintEditorUtils::AddFunctionGraph<UClass>(Blueprint, Graph, /*bIsUserCreated*/ true, nullptr);

			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetStringField(TEXT("graph"), Graph->GetName());
			TArray<TSharedPtr<FJsonValue>> Nodes;
			for (const UEdGraphNode* Node : Graph->Nodes)
			{
				Nodes.Add(AsValue(DescribeNode(Node, true)));
			}
			Out->SetArrayField(TEXT("nodes"), Nodes);
			return FCursorToolResult::OkJson(Out);
		})
		.Register();

	FCursorToolBuilder(TEXT("blueprint_add_node"), BlueprintPack)
		.Describe(TEXT("Add one common node to a graph and return it with its pins. Kinds: "
			"event (override a parent event such as BeginPlay or Tick; event graph only), custom_event (event graph only), "
			"call_function (any BlueprintCallable function: set 'function' and optionally 'class', default the parent class; static library functions such as KismetSystemLibrary.PrintString work), "
			"variable_get / variable_set (a member variable by 'name'), branch, sequence, self. "
			"Then wire it with blueprint_connect_pins and fill pin values with blueprint_set_pin_default."))
		.Risk(ECursorToolRisk::Mutating)
		.Schema(FCursorSchema()
			.Str(TEXT("blueprint"), TEXT("Blueprint asset path."), true)
			.Enum(TEXT("kind"), TEXT("What to add."), { TEXT("event"), TEXT("custom_event"), TEXT("call_function"), TEXT("variable_get"), TEXT("variable_set"), TEXT("branch"), TEXT("sequence"), TEXT("self") }, true)
			.Str(TEXT("graph"), TEXT("Graph name. Default: the main event graph."))
			.Str(TEXT("name"), TEXT("event: parent event name (BeginPlay, Tick, ActorBeginOverlap; 'Receive' is added when needed). custom_event: its name. variable_get/set: the variable."))
			.Str(TEXT("function"), TEXT("call_function: function name, for example PrintString or SetActorLocation."))
			.Str(TEXT("class"), TEXT("call_function: class that owns the function, for example KismetSystemLibrary. Default: the Blueprint's parent class."))
			.Int(TEXT("x"), TEXT("Node X position in the graph."))
			.Int(TEXT("y"), TEXT("Node Y position in the graph.")))
		.Run([](const FCursorArgs& Args, const FCursorToolContext&)
		{
			FString Error;
			UBlueprint* Blueprint = LoadEditableBlueprint(Args.Str(TEXT("blueprint")), Error);
			if (!Blueprint)
			{
				return FCursorToolResult::Error(Error);
			}
			UEdGraph* Graph = FindGraph(Blueprint, Args.Str(TEXT("graph")), Error);
			if (!Graph)
			{
				return FCursorToolResult::Error(Error);
			}
			const FString Kind = Args.Str(TEXT("kind")).ToLower();
			const FString Name = Args.Str(TEXT("name"));
			const bool bEventGraph = Blueprint->UbergraphPages.Contains(Graph);

			Graph->Modify();
			UEdGraphNode* Created = nullptr;
			bool bAlreadyExisted = false;

			if (Kind == TEXT("event"))
			{
				if (!bEventGraph)
				{
					return FCursorToolResult::Error(TEXT("Events can only be added to an event graph."));
				}
				if (Name.IsEmpty() || !Blueprint->ParentClass)
				{
					return FCursorToolResult::Error(TEXT("'name' is required for an event, for example BeginPlay."));
				}
				UFunction* Event = Blueprint->ParentClass->FindFunctionByName(FName(*Name));
				if (!Event)
				{
					Event = Blueprint->ParentClass->FindFunctionByName(FName(*(TEXT("Receive") + Name)));
				}
				if (!Event || !Event->HasAnyFunctionFlags(FUNC_BlueprintEvent))
				{
					return FCursorToolResult::Error(FString::Printf(TEXT("%s has no overridable event '%s'. Common ones on Actor: BeginPlay, Tick, ActorBeginOverlap, EndPlay. Use custom_event for your own."),
						*Blueprint->ParentClass->GetName(), *Name));
				}
				// One node per overridden event: return the existing one instead of making a second.
				if (UK2Node_Event* Existing = FBlueprintEditorUtils::FindOverrideForFunction(Blueprint, Event->GetOwnerClass(), Event->GetFName()))
				{
					Created = Existing;
					bAlreadyExisted = true;
				}
				else
				{
					FGraphNodeCreator<UK2Node_Event> Creator(*Graph);
					UK2Node_Event* Node = Creator.CreateNode();
					Node->EventReference.SetExternalMember(Event->GetFName(), Event->GetOwnerClass());
					Node->bOverrideFunction = true;
					PlaceNode(Node, Args, Graph);
					Creator.Finalize();
					Created = Node;
				}
			}
			else if (Kind == TEXT("custom_event"))
			{
				if (!bEventGraph)
				{
					return FCursorToolResult::Error(TEXT("Custom events can only be added to an event graph."));
				}
				if (!IsValidMemberName(Name, Error))
				{
					return FCursorToolResult::Error(Error);
				}
				FGraphNodeCreator<UK2Node_CustomEvent> Creator(*Graph);
				UK2Node_CustomEvent* Node = Creator.CreateNode();
				Node->CustomFunctionName = FName(*Name);
				PlaceNode(Node, Args, Graph);
				Creator.Finalize();
				Created = Node;
			}
			else if (Kind == TEXT("call_function"))
			{
				const FString FunctionName = Args.Str(TEXT("function"));
				if (FunctionName.IsEmpty())
				{
					return FCursorToolResult::Error(TEXT("'function' is required for call_function."));
				}
				UClass* Owner = Args.Has(TEXT("class")) ? CursorToolHelpers::ResolveClass(Args.Str(TEXT("class")), nullptr, Error) : Blueprint->ParentClass.Get();
				if (!Owner)
				{
					return FCursorToolResult::Error(Error.IsEmpty() ? TEXT("The Blueprint has no parent class.") : Error);
				}
				// Many Actor functions are exposed as K2_Name (K2_SetActorLocation shows as SetActorLocation).
				UFunction* Function = Owner->FindFunctionByName(FName(*FunctionName));
				if (!Function)
				{
					Function = Owner->FindFunctionByName(FName(*(TEXT("K2_") + FunctionName)));
				}
				if (!Function || !Function->HasAnyFunctionFlags(FUNC_BlueprintCallable))
				{
					return FCursorToolResult::Error(FString::Printf(TEXT("%s has no Blueprint-callable function '%s'."), *Owner->GetName(), *FunctionName));
				}
				FGraphNodeCreator<UK2Node_CallFunction> Creator(*Graph);
				UK2Node_CallFunction* Node = Creator.CreateNode();
				Node->SetFromFunction(Function);
				PlaceNode(Node, Args, Graph);
				Creator.Finalize();
				Created = Node;
			}
			else if (Kind == TEXT("variable_get") || Kind == TEXT("variable_set"))
			{
				if (Name.IsEmpty())
				{
					return FCursorToolResult::Error(TEXT("'name' is required: the member variable to read or write."));
				}
				const bool bKnown = FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, FName(*Name)) != INDEX_NONE
					|| (Blueprint->SkeletonGeneratedClass && FindFProperty<FProperty>(Blueprint->SkeletonGeneratedClass, FName(*Name)));
				if (!bKnown)
				{
					return FCursorToolResult::Error(FString::Printf(TEXT("No variable '%s' on this Blueprint. Add it with blueprint_add_variable first."), *Name));
				}
				if (Kind == TEXT("variable_get"))
				{
					FGraphNodeCreator<UK2Node_VariableGet> Creator(*Graph);
					UK2Node_VariableGet* Node = Creator.CreateNode();
					Node->VariableReference.SetSelfMember(FName(*Name));
					PlaceNode(Node, Args, Graph);
					Creator.Finalize();
					Created = Node;
				}
				else
				{
					FGraphNodeCreator<UK2Node_VariableSet> Creator(*Graph);
					UK2Node_VariableSet* Node = Creator.CreateNode();
					Node->VariableReference.SetSelfMember(FName(*Name));
					PlaceNode(Node, Args, Graph);
					Creator.Finalize();
					Created = Node;
				}
			}
			else if (Kind == TEXT("branch"))
			{
				FGraphNodeCreator<UK2Node_IfThenElse> Creator(*Graph);
				UK2Node_IfThenElse* Node = Creator.CreateNode();
				PlaceNode(Node, Args, Graph);
				Creator.Finalize();
				Created = Node;
			}
			else if (Kind == TEXT("sequence"))
			{
				FGraphNodeCreator<UK2Node_ExecutionSequence> Creator(*Graph);
				UK2Node_ExecutionSequence* Node = Creator.CreateNode();
				PlaceNode(Node, Args, Graph);
				Creator.Finalize();
				Created = Node;
			}
			else if (Kind == TEXT("self"))
			{
				FGraphNodeCreator<UK2Node_Self> Creator(*Graph);
				UK2Node_Self* Node = Creator.CreateNode();
				PlaceNode(Node, Args, Graph);
				Creator.Finalize();
				Created = Node;
			}
			else
			{
				return FCursorToolResult::Error(FString::Printf(TEXT("Unknown kind '%s'."), *Kind));
			}

			FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
			TSharedRef<FJsonObject> Out = DescribeNode(Created, true);
			if (bAlreadyExisted)
			{
				Out->SetBoolField(TEXT("already_existed"), true);
			}
			return FCursorToolResult::OkJson(Out);
		})
		.Register();

	FCursorToolBuilder(TEXT("blueprint_remove_node"), BlueprintPack)
		.Describe(TEXT("Delete a node from a graph (its links are removed with it). Entry nodes of functions cannot be deleted."))
		.Risk(ECursorToolRisk::Mutating)
		.Schema(FCursorSchema()
			.Str(TEXT("blueprint"), TEXT("Blueprint asset path."), true)
			.Str(TEXT("node"), TEXT("Node id from blueprint_get_nodes."), true)
			.Str(TEXT("graph"), TEXT("Graph name. Default: search every graph.")))
		.Run([](const FCursorArgs& Args, const FCursorToolContext&)
		{
			FString Error;
			UBlueprint* Blueprint = LoadEditableBlueprint(Args.Str(TEXT("blueprint")), Error);
			if (!Blueprint)
			{
				return FCursorToolResult::Error(Error);
			}
			UEdGraphNode* Node = FindNode(Blueprint, Args.Str(TEXT("graph")), Args.Str(TEXT("node")), Error);
			if (!Node)
			{
				return FCursorToolResult::Error(Error);
			}
			if (!Node->CanUserDeleteNode())
			{
				return FCursorToolResult::Error(FString::Printf(TEXT("Node %s cannot be deleted."), *Node->GetName()));
			}
			const FString Id = Node->GetName();
			FBlueprintEditorUtils::RemoveNode(Blueprint, Node, /*bDontRecompile*/ true);
			return FCursorToolResult::Ok(FString::Printf(TEXT("Removed node %s."), *Id));
		})
		.Register();

	FCursorToolBuilder(TEXT("blueprint_connect_pins"), BlueprintPack)
		.Describe(TEXT("Connect an output pin to an input pin using the Blueprint schema's own rules (execution to execution, matching or convertible data types). "
			"Fails with the schema's reason when the pins are incompatible. Execution pins are usually named 'execute' (input) and 'then' (output)."))
		.Risk(ECursorToolRisk::Mutating)
		.Schema(FCursorSchema()
			.Str(TEXT("blueprint"), TEXT("Blueprint asset path."), true)
			.Str(TEXT("from_node"), TEXT("Node id owning the output pin."), true)
			.Str(TEXT("from_pin"), TEXT("Output pin name."), true)
			.Str(TEXT("to_node"), TEXT("Node id owning the input pin."), true)
			.Str(TEXT("to_pin"), TEXT("Input pin name."), true)
			.Str(TEXT("graph"), TEXT("Graph name. Default: search every graph.")))
		.Run([](const FCursorArgs& Args, const FCursorToolContext&)
		{
			FString Error;
			UBlueprint* Blueprint = LoadEditableBlueprint(Args.Str(TEXT("blueprint")), Error);
			if (!Blueprint)
			{
				return FCursorToolResult::Error(Error);
			}
			const FString GraphName = Args.Str(TEXT("graph"));
			UEdGraphNode* FromNode = FindNode(Blueprint, GraphName, Args.Str(TEXT("from_node")), Error);
			UEdGraphNode* ToNode = FromNode ? FindNode(Blueprint, GraphName, Args.Str(TEXT("to_node")), Error) : nullptr;
			UEdGraphPin* FromPin = ToNode ? FindPin(FromNode, Args.Str(TEXT("from_pin")), EGPD_Output, Error) : nullptr;
			UEdGraphPin* ToPin = FromPin ? FindPin(ToNode, Args.Str(TEXT("to_pin")), EGPD_Input, Error) : nullptr;
			if (!ToPin)
			{
				return FCursorToolResult::Error(Error);
			}

			const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
			const FPinConnectionResponse Response = Schema->CanCreateConnection(FromPin, ToPin);
			if (Response.Response == CONNECT_RESPONSE_DISALLOW)
			{
				return FCursorToolResult::Error(FString::Printf(TEXT("Cannot connect %s.%s to %s.%s: %s"), *FromNode->GetName(), *FromPin->PinName.ToString(),
					*ToNode->GetName(), *ToPin->PinName.ToString(), *Response.Message.ToString()));
			}
			if (!Schema->TryCreateConnection(FromPin, ToPin))
			{
				return FCursorToolResult::Error(TEXT("The schema refused the connection."));
			}
			FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
			return FCursorToolResult::Ok(FString::Printf(TEXT("Connected %s.%s to %s.%s."), *FromNode->GetName(), *FromPin->PinName.ToString(), *ToNode->GetName(), *ToPin->PinName.ToString()));
		})
		.Register();

	FCursorToolBuilder(TEXT("blueprint_set_pin_default"), BlueprintPack)
		.Describe(TEXT("Set the literal value of an unconnected input pin: text for PrintString's InString, 1.5 for a float, true for a bool, "
			"(X=1,Y=2,Z=3) for a vector, an asset path for an object pin."))
		.Risk(ECursorToolRisk::Mutating)
		.Schema(FCursorSchema()
			.Str(TEXT("blueprint"), TEXT("Blueprint asset path."), true)
			.Str(TEXT("node"), TEXT("Node id."), true)
			.Str(TEXT("pin"), TEXT("Input pin name."), true)
			.Str(TEXT("value"), TEXT("The value, as text."), true)
			.Str(TEXT("graph"), TEXT("Graph name. Default: search every graph.")))
		.Run([](const FCursorArgs& Args, const FCursorToolContext&)
		{
			FString Error;
			UBlueprint* Blueprint = LoadEditableBlueprint(Args.Str(TEXT("blueprint")), Error);
			if (!Blueprint)
			{
				return FCursorToolResult::Error(Error);
			}
			UEdGraphNode* Node = FindNode(Blueprint, Args.Str(TEXT("graph")), Args.Str(TEXT("node")), Error);
			UEdGraphPin* Pin = Node ? FindPin(Node, Args.Str(TEXT("pin")), EGPD_Input, Error) : nullptr;
			if (!Pin)
			{
				return FCursorToolResult::Error(Error);
			}
			if (Pin->LinkedTo.Num() > 0)
			{
				return FCursorToolResult::Error(TEXT("That pin is connected, so its literal value is not used. Break the connection first."));
			}

			const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
			const FString Value = Args.Str(TEXT("value"));
			const FName Category = Pin->PinType.PinCategory;
			Node->Modify();
			if (Category == UEdGraphSchema_K2::PC_Object || Category == UEdGraphSchema_K2::PC_Class
				|| Category == UEdGraphSchema_K2::PC_SoftObject || Category == UEdGraphSchema_K2::PC_SoftClass)
			{
				UObject* Object = nullptr;
				if (!Value.IsEmpty())
				{
					Object = (Category == UEdGraphSchema_K2::PC_Class || Category == UEdGraphSchema_K2::PC_SoftClass)
						? static_cast<UObject*>(CursorToolHelpers::ResolveClass(Value, nullptr, Error)) : CursorToolHelpers::LoadAsset(Value, Error);
					if (!Object)
					{
						return FCursorToolResult::Error(Error);
					}
				}
				Schema->TrySetDefaultObject(*Pin, Object);
			}
			else if (Category == UEdGraphSchema_K2::PC_Text)
			{
				Schema->TrySetDefaultText(*Pin, FText::FromString(Value));
			}
			else
			{
				const FString Problem = Schema->IsPinDefaultValid(Pin, Value, nullptr, FText::GetEmpty());
				if (!Problem.IsEmpty())
				{
					return FCursorToolResult::Error(FString::Printf(TEXT("'%s' is not a valid value for a %s pin: %s"), *Value, *PinTypeToString(Pin->PinType), *Problem));
				}
				Schema->TrySetDefaultValue(*Pin, Value);
			}
			FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
			return FCursorToolResult::Ok(FString::Printf(TEXT("Set %s.%s to '%s'."), *Node->GetName(), *Pin->PinName.ToString(), *Pin->GetDefaultAsString()));
		})
		.Register();

	FCursorToolBuilder(TEXT("blueprint_compile"), BlueprintPack)
		.Describe(TEXT("Compile a Blueprint and return its status with every error and warning the compiler reported. "
			"Run after structural edits (variables, components, nodes, connections); fix errors before moving on. Does not save: call asset_save."))
		.Risk(ECursorToolRisk::Mutating)
		.Schema(FCursorSchema()
			.Str(TEXT("blueprint"), TEXT("Blueprint asset path."), true))
		.Run([](const FCursorArgs& Args, const FCursorToolContext&)
		{
			FString Error;
			UBlueprint* Blueprint = LoadEditableBlueprint(Args.Str(TEXT("blueprint")), Error);
			if (!Blueprint)
			{
				return FCursorToolResult::Error(Error);
			}

			FCompilerResultsLog Results;
			Results.SetSourcePath(Blueprint->GetPathName());
			// The messages come back in the result; keep them out of the log so a compile error does not read as an editor fault.
			Results.SetSilentMode(true);
			FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipGarbageCollection, &Results);

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
			Out->SetStringField(TEXT("status"), StatusToString(Blueprint->Status));
			Out->SetBoolField(TEXT("compiled"), Blueprint->Status != BS_Error);
			Out->SetArrayField(TEXT("errors"), Errors);
			Out->SetArrayField(TEXT("warnings"), Warnings);

			FCursorToolResult Result = FCursorToolResult::OkJson(Out);
			if (Blueprint->Status == BS_Error)
			{
				// Errors are the tool's answer, not a failure to run it, but the model must not mistake them for success.
				Result.bIsError = true;
			}
			return Result;
		})
		.Register();
}

void CursorToolPacks::UnregisterBlueprintPack()
{
	FCursorToolRegistry::Get().UnregisterPack(BlueprintPack);
}
