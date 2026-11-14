#include "CursorToolPacks.h"

#include "ContentBrowserModule.h"
#include "CursorJson.h"
#include "CursorLogCapture.h"
#include "CursorTool.h"
#include "CursorToolHelpers.h"
#include "CursorToolRegistry.h"
#include "Editor.h"
#include "Engine/Selection.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/Actor.h"
#include "IContentBrowserSingleton.h"
#include "IPythonScriptPlugin.h"
#include "LevelEditorViewport.h"
#include "Misc/App.h"
#include "Misc/EngineVersion.h"
#include "PythonScriptTypes.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Subsystems/EditorActorSubsystem.h"
#include "UObject/Package.h"

namespace
{
	const TCHAR* EditorPack = TEXT("editor");

	ELogVerbosity::Type ParseVerbosity(const FString& Text)
	{
		if (Text.Equals(TEXT("error"), ESearchCase::IgnoreCase)) { return ELogVerbosity::Error; }
		if (Text.Equals(TEXT("warning"), ESearchCase::IgnoreCase)) { return ELogVerbosity::Warning; }
		if (Text.Equals(TEXT("display"), ESearchCase::IgnoreCase)) { return ELogVerbosity::Display; }
		if (Text.Equals(TEXT("log"), ESearchCase::IgnoreCase)) { return ELogVerbosity::Log; }
		return ELogVerbosity::All;
	}

	FString JoinLogLines(const TArray<FCursorLogLine>& Lines)
	{
		FString Out;
		for (const FCursorLogLine& Line : Lines)
		{
			Out += FCursorLogCapture::Format(Line);
			Out += TEXT("\n");
		}
		return Out;
	}

	UEditorActorSubsystem* ActorSubsystem()
	{
		return GEditor ? GEditor->GetEditorSubsystem<UEditorActorSubsystem>() : nullptr;
	}

	TSharedRef<FJsonObject> CompactActor(const AActor* Actor)
	{
		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetStringField(TEXT("name"), Actor->GetName());
		const FString Label = Actor->GetActorLabel(false);
		if (!Label.IsEmpty() && Label != Actor->GetName())
		{
			Out->SetStringField(TEXT("label"), Label);
		}
		Out->SetStringField(TEXT("class"), Actor->GetClass()->GetName());
		const FVector L = Actor->GetActorLocation();
		Out->SetStringField(TEXT("location"), FString::Printf(TEXT("%.0f, %.0f, %.0f"), L.X, L.Y, L.Z));
		return Out;
	}

	void RegisterContextTools()
	{
		FCursorToolBuilder(TEXT("editor_get_context"), EditorPack)
			.Describe(TEXT("What the user is looking at right now: project, engine version, open level, selected actors, assets selected in the "
				"Content Browser, assets open in editors, and the viewport camera. Call this first when a request says 'this', 'selected' or 'here'."))
			.Risk(ECursorToolRisk::ReadOnly)
			.Run([](const FCursorArgs&, const FCursorToolContext&)
			{
				constexpr int32 MaxListed = 25;
				TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
				Out->SetStringField(TEXT("project"), FApp::GetProjectName());
				Out->SetStringField(TEXT("engine_version"), FEngineVersion::Current().ToString(EVersionComponent::Patch));

				if (const UWorld* World = CursorToolHelpers::GetEditorWorld())
				{
					Out->SetStringField(TEXT("level"), World->GetOutermost()->GetName());
					Out->SetBoolField(TEXT("level_has_unsaved_changes"), World->GetOutermost()->IsDirty());
				}
				Out->SetBoolField(TEXT("play_in_editor_running"), GEditor && GEditor->PlayWorld != nullptr);

				if (GEditor)
				{
					TArray<TSharedPtr<FJsonValue>> Selected;
					USelection* Selection = GEditor->GetSelectedActors();
					int32 Total = 0;
					for (FSelectionIterator It(*Selection); It; ++It)
					{
						if (const AActor* Actor = Cast<AActor>(*It))
						{
							if (++Total <= MaxListed)
							{
								Selected.Add(MakeShared<FJsonValueObject>(CompactActor(Actor)));
							}
						}
					}
					Out->SetArrayField(TEXT("selected_actors"), Selected);
					if (Total > MaxListed)
					{
						Out->SetNumberField(TEXT("selected_actor_count"), Total);
					}

					if (UAssetEditorSubsystem* AssetEditors = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
					{
						TArray<FString> Open;
						for (const UObject* Asset : AssetEditors->GetAllEditedAssets())
						{
							if (Asset)
							{
								Open.Add(Asset->GetPathName());
							}
						}
						Out->SetArrayField(TEXT("open_asset_editors"), CursorJson::StringArray(Open));
					}
				}

				// The Content Browser only exists when the editor has a UI.
				if (FSlateApplication::IsInitialized() && !IsRunningCommandlet() && FApp::CanEverRender())
				{
					TArray<FAssetData> Assets;
					FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser")).Get().GetSelectedAssets(Assets);
					TArray<FString> Paths;
					for (int32 i = 0; i < Assets.Num() && i < MaxListed; ++i)
					{
						Paths.Add(Assets[i].GetObjectPathString());
					}
					Out->SetArrayField(TEXT("selected_assets"), CursorJson::StringArray(Paths));
				}

				if (GCurrentLevelEditingViewportClient)
				{
					TSharedRef<FJsonObject> Camera = MakeShared<FJsonObject>();
					Camera->SetObjectField(TEXT("location"), CursorJson::VectorToJson(GCurrentLevelEditingViewportClient->GetViewLocation()));
					Camera->SetObjectField(TEXT("rotation"), CursorJson::RotatorToJson(GCurrentLevelEditingViewportClient->GetViewRotation()));
					Out->SetObjectField(TEXT("viewport_camera"), Camera);
				}
				return FCursorToolResult::OkJson(Out);
			})
			.Register();

		FCursorToolBuilder(TEXT("editor_log_tail"), EditorPack)
			.Describe(TEXT("Recent lines from the editor's Output Log. Use it after an operation to see warnings and errors it produced. "
				"Pass the 'latest_sequence' from an earlier call as 'after_sequence' to get only what was logged since."))
			.Risk(ECursorToolRisk::ReadOnly)
			.Schema(FCursorSchema()
				.Int(TEXT("lines"), TEXT("How many lines at most. Default 100, maximum 1000."))
				.Enum(TEXT("min_verbosity"), TEXT("Keep lines at least this severe. Default 'all'."), { TEXT("error"), TEXT("warning"), TEXT("display"), TEXT("log"), TEXT("all") })
				.Str(TEXT("category"), TEXT("Keep lines whose log category contains this text, for example 'Blueprint'."))
				.Str(TEXT("contains"), TEXT("Keep lines containing this text."))
				.Int(TEXT("after_sequence"), TEXT("Only lines logged after this sequence number.")))
			.Run([](const FCursorArgs& Args, const FCursorToolContext&)
			{
				const FCursorLogCapture& Capture = FCursorLogCapture::Get();
				const int32 MaxLines = FMath::Clamp(Args.Int(TEXT("lines"), 100), 1, 1000);
				const TArray<FCursorLogLine> Lines = Capture.GetLines(
					static_cast<int64>(Args.Num(TEXT("after_sequence"), 0.0)), MaxLines,
					ParseVerbosity(Args.Str(TEXT("min_verbosity"), TEXT("all"))), Args.Str(TEXT("category")), Args.Str(TEXT("contains")));

				FString Text = FString::Printf(TEXT("latest_sequence: %lld\nlines: %d\n\n"), Capture.GetLatestSequence(), Lines.Num());
				Text += JoinLogLines(Lines);
				return FCursorToolResult::Ok(Text);
			})
			.Register();
	}

	void RegisterActorTools()
	{
		FCursorToolBuilder(TEXT("actor_list"), EditorPack)
			.Describe(TEXT("List actors in the open level, with filters. Returns each actor's unique name (use it in other actor tools), label, class and location."))
			.Risk(ECursorToolRisk::ReadOnly)
			.Schema(FCursorSchema()
				.Str(TEXT("class"), TEXT("Only actors of this class or its subclasses, for example 'StaticMeshActor', 'Light' or a Blueprint asset path."))
				.Str(TEXT("name_contains"), TEXT("Only actors whose name or label contains this text."))
				.Str(TEXT("folder"), TEXT("Only actors in this World Outliner folder or below it."))
				.Str(TEXT("tag"), TEXT("Only actors with this actor tag."))
				.Int(TEXT("limit"), TEXT("Maximum results. Default 100, maximum 1000."))
				.Int(TEXT("offset"), TEXT("Skip this many results, for paging.")))
			.Run([](const FCursorArgs& Args, const FCursorToolContext&)
			{
				UWorld* World = CursorToolHelpers::GetEditorWorld();
				if (!World)
				{
					return FCursorToolResult::Error(TEXT("No level is open in the editor."));
				}

				UClass* ClassFilter = nullptr;
				if (Args.Has(TEXT("class")))
				{
					FString Error;
					ClassFilter = CursorToolHelpers::ResolveClass(Args.Str(TEXT("class")), AActor::StaticClass(), Error);
					if (!ClassFilter)
					{
						return FCursorToolResult::Error(Error);
					}
				}
				const FString NameContains = Args.Str(TEXT("name_contains"));
				const FString Folder = Args.Str(TEXT("folder"));
				const FName Tag = Args.Has(TEXT("tag")) ? FName(*Args.Str(TEXT("tag"))) : NAME_None;
				const int32 Limit = FMath::Clamp(Args.Int(TEXT("limit"), 100), 1, 1000);
				const int32 Offset = FMath::Max(0, Args.Int(TEXT("offset"), 0));

				TArray<const AActor*> Matches;
				for (TActorIterator<AActor> It(World, ClassFilter ? ClassFilter : AActor::StaticClass()); It; ++It)
				{
					const AActor* Actor = *It;
					if (!Actor->IsEditable() || Actor->HasAnyFlags(RF_Transient))
					{
						continue;
					}
					if (!NameContains.IsEmpty() && !Actor->GetName().Contains(NameContains) && !Actor->GetActorLabel(false).Contains(NameContains))
					{
						continue;
					}
					if (!Folder.IsEmpty() && !Actor->GetFolderPath().ToString().StartsWith(Folder, ESearchCase::IgnoreCase))
					{
						continue;
					}
					if (!Tag.IsNone() && !Actor->Tags.Contains(Tag))
					{
						continue;
					}
					Matches.Add(Actor);
				}
				Matches.Sort([](const AActor& A, const AActor& B) { return A.GetName() < B.GetName(); });

				TArray<TSharedPtr<FJsonValue>> Items;
				for (int32 i = Offset; i < Matches.Num() && Items.Num() < Limit; ++i)
				{
					Items.Add(MakeShared<FJsonValueObject>(CompactActor(Matches[i])));
				}
				TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
				Out->SetNumberField(TEXT("total"), Matches.Num());
				Out->SetNumberField(TEXT("offset"), Offset);
				Out->SetArrayField(TEXT("actors"), Items);
				return FCursorToolResult::OkJson(Out);
			})
			.Register();

		FCursorToolBuilder(TEXT("actor_get"), EditorPack)
			.Describe(TEXT("Details of one actor: class, transform, folder, tags and its components. Follow with object_get_properties for property values."))
			.Risk(ECursorToolRisk::ReadOnly)
			.Schema(FCursorSchema().Str(TEXT("name"), TEXT("Actor name or label."), true))
			.Run([](const FCursorArgs& Args, const FCursorToolContext&)
			{
				FString Error;
				const AActor* Actor = CursorToolHelpers::FindActor(Args.Str(TEXT("name")), Error);
				return Actor ? FCursorToolResult::OkJson(CursorToolHelpers::DescribeActor(Actor, true)) : FCursorToolResult::Error(Error);
			})
			.Register();

		FCursorToolBuilder(TEXT("actor_spawn"), EditorPack)
			.Describe(TEXT("Place an actor in the open level. 'class' can be an actor class (PointLight, StaticMeshActor), a Blueprint asset path, "
				"or an asset that the editor knows how to place (a Static Mesh path creates a StaticMeshActor using it). Undoable."))
			.Risk(ECursorToolRisk::Mutating)
			.Schema(FCursorSchema()
				.Str(TEXT("class"), TEXT("Actor class name, /Script path, or asset path."), true)
				.Vec3(TEXT("location"), TEXT("World location [x, y, z] in centimetres. Default origin."))
				.Vec3(TEXT("rotation"), TEXT("Rotation [pitch, yaw, roll] in degrees."))
				.Vec3(TEXT("scale"), TEXT("Scale [x, y, z]. Default 1."))
				.Str(TEXT("label"), TEXT("Label to show in the World Outliner."))
				.Str(TEXT("folder"), TEXT("World Outliner folder to put it in.")))
			.Run([](const FCursorArgs& Args, const FCursorToolContext&)
			{
				UEditorActorSubsystem* Actors = ActorSubsystem();
				if (!Actors || !CursorToolHelpers::GetEditorWorld())
				{
					return FCursorToolResult::Error(TEXT("No level is open in the editor."));
				}

				FVector Location = FVector::ZeroVector;
				FRotator Rotation = FRotator::ZeroRotator;
				Args.Vec(TEXT("location"), Location);
				Args.Rot(TEXT("rotation"), Rotation);

				const FString ClassText = Args.Str(TEXT("class"));
				AActor* Spawned = nullptr;
				FString ClassError;
				if (UClass* Class = CursorToolHelpers::ResolveClass(ClassText, AActor::StaticClass(), ClassError))
				{
					if (Class->HasAnyClassFlags(CLASS_Abstract))
					{
						return FCursorToolResult::Error(FString::Printf(TEXT("'%s' is abstract and cannot be placed."), *Class->GetName()));
					}
					if (GEditor->FindActorFactoryForActorClass(Class))
					{
						Spawned = Actors->SpawnActorFromClass(Class, Location, Rotation);
					}
					else
					{
						// No actor factory: every Blueprint class, and natives such as Brush. For these the editor's
						// placement code asks the level viewport what is under the mouse cursor before it spawns, which
						// renders the viewport's hit proxies on every call and crashes when the viewport cannot render
						// (a headless editor). The location is given here, so add the actor directly, as that code
						// goes on to do itself.
						Spawned = GEditor->AddActor(CursorToolHelpers::GetEditorWorld()->GetCurrentLevel(), Class, FTransform(Rotation, Location),
							/*bSilent*/ true, RF_Transactional, /*bSelectActor*/ false);
					}
				}
				else if (ClassText.StartsWith(TEXT("/")) && !ClassText.StartsWith(TEXT("/Script/")))
				{
					FString LoadError;
					UObject* Asset = CursorToolHelpers::LoadAsset(ClassText, LoadError);
					if (!Asset)
					{
						return FCursorToolResult::Error(LoadError);
					}
					Spawned = Actors->SpawnActorFromObject(Asset, Location, Rotation);
					if (!Spawned)
					{
						return FCursorToolResult::Error(FString::Printf(TEXT("The editor does not know how to place a %s in a level."), *Asset->GetClass()->GetName()));
					}
				}
				else
				{
					return FCursorToolResult::Error(ClassError);
				}

				if (!Spawned)
				{
					return FCursorToolResult::Error(TEXT("The editor refused to spawn the actor. See editor_log_tail for the reason."));
				}

				FVector Scale;
				if (Args.Vec(TEXT("scale"), Scale))
				{
					Spawned->SetActorScale3D(Scale);
				}
				if (Args.Has(TEXT("label")))
				{
					Spawned->SetActorLabel(Args.Str(TEXT("label")));
				}
				if (Args.Has(TEXT("folder")))
				{
					Spawned->SetFolderPath(FName(*Args.Str(TEXT("folder"))));
				}
				return FCursorToolResult::OkJson(CursorToolHelpers::DescribeActor(Spawned, true));
			})
			.Register();

		FCursorToolBuilder(TEXT("actor_update"), EditorPack)
			.Describe(TEXT("Move, rotate, scale, relabel or re-folder an actor. Only the fields you pass change. Undoable. "
				"For other properties use object_set_properties."))
			.Risk(ECursorToolRisk::Mutating)
			.Schema(FCursorSchema()
				.Str(TEXT("name"), TEXT("Actor name or label."), true)
				.Vec3(TEXT("location"), TEXT("New world location [x, y, z]."))
				.Vec3(TEXT("rotation"), TEXT("New rotation [pitch, yaw, roll] in degrees."))
				.Vec3(TEXT("scale"), TEXT("New scale [x, y, z]."))
				.Str(TEXT("label"), TEXT("New World Outliner label."))
				.Str(TEXT("folder"), TEXT("New World Outliner folder. Empty string moves it to the root.")))
			.Run([](const FCursorArgs& Args, const FCursorToolContext&)
			{
				FString Error;
				AActor* Actor = CursorToolHelpers::FindActor(Args.Str(TEXT("name")), Error);
				if (!Actor)
				{
					return FCursorToolResult::Error(Error);
				}

				FVector Location, Scale;
				FRotator Rotation;
				const bool bLocation = Args.Vec(TEXT("location"), Location);
				const bool bRotation = Args.Rot(TEXT("rotation"), Rotation);
				const bool bScale = Args.Vec(TEXT("scale"), Scale);
				const bool bLabel = Args.Has(TEXT("label"));
				const bool bFolder = Args.Raw()->HasField(TEXT("folder"));
				if (!bLocation && !bRotation && !bScale && !bLabel && !bFolder)
				{
					return FCursorToolResult::Error(TEXT("Nothing to change: pass at least one of location, rotation, scale, label, folder."));
				}

				if ((bLocation || bRotation || bScale) && !Actor->GetRootComponent())
				{
					return FCursorToolResult::Error(FString::Printf(TEXT("'%s' has no root component, so it has no transform."), *Actor->GetName()));
				}

				Actor->Modify();
				if (bLocation || bRotation || bScale)
				{
					Actor->GetRootComponent()->Modify();
					if (bLocation) { Actor->SetActorLocation(Location, false, nullptr, ETeleportType::TeleportPhysics); }
					if (bRotation) { Actor->SetActorRotation(Rotation, ETeleportType::TeleportPhysics); }
					if (bScale) { Actor->SetActorScale3D(Scale); }
					Actor->PostEditMove(/*bFinished*/ true);
				}
				if (bLabel) { Actor->SetActorLabel(Args.Str(TEXT("label"))); }
				if (bFolder) { Actor->SetFolderPath(FName(*Args.Str(TEXT("folder")))); }
				Actor->MarkPackageDirty();
				return FCursorToolResult::OkJson(CursorToolHelpers::DescribeActor(Actor, false));
			})
			.Register();

		FCursorToolBuilder(TEXT("actor_duplicate"), EditorPack)
			.Describe(TEXT("Duplicate an actor in the open level, optionally offset from the original. Undoable."))
			.Risk(ECursorToolRisk::Mutating)
			.Schema(FCursorSchema()
				.Str(TEXT("name"), TEXT("Actor name or label."), true)
				.Vec3(TEXT("offset"), TEXT("Offset [x, y, z] from the original. Default none.")))
			.Run([](const FCursorArgs& Args, const FCursorToolContext&)
			{
				FString Error;
				AActor* Actor = CursorToolHelpers::FindActor(Args.Str(TEXT("name")), Error);
				if (!Actor)
				{
					return FCursorToolResult::Error(Error);
				}
				FVector Offset = FVector::ZeroVector;
				Args.Vec(TEXT("offset"), Offset);
				AActor* Copy = ActorSubsystem() ? ActorSubsystem()->DuplicateActor(Actor, nullptr, Offset) : nullptr;
				return Copy ? FCursorToolResult::OkJson(CursorToolHelpers::DescribeActor(Copy, false))
					: FCursorToolResult::Error(TEXT("The editor refused to duplicate the actor."));
			})
			.Register();

		FCursorToolBuilder(TEXT("actor_delete"), EditorPack)
			.Describe(TEXT("Delete actors from the open level. Asks the user for approval. The editor's undo can bring them back."))
			.Risk(ECursorToolRisk::Dangerous)
			.Schema(FCursorSchema().StrArray(TEXT("names"), TEXT("Actor names or labels to delete."), true))
			.Run([](const FCursorArgs& Args, const FCursorToolContext&)
			{
				const TArray<FString> Names = Args.StrArray(TEXT("names"));
				if (Names.Num() == 0)
				{
					return FCursorToolResult::Error(TEXT("'names' must list at least one actor."));
				}
				// Resolve everything first so a typo does not leave the level half-edited.
				TArray<AActor*> Targets;
				for (const FString& Name : Names)
				{
					FString Error;
					AActor* Actor = CursorToolHelpers::FindActor(Name, Error);
					if (!Actor)
					{
						return FCursorToolResult::Error(Error + TEXT(" Nothing was deleted."));
					}
					Targets.AddUnique(Actor);
				}

				UEditorActorSubsystem* Actors = ActorSubsystem();
				TArray<FString> Deleted, Refused;
				for (AActor* Actor : Targets)
				{
					const FString Name = Actor->GetName();
					(Actors && Actors->DestroyActor(Actor) ? Deleted : Refused).Add(Name);
				}
				TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
				Out->SetArrayField(TEXT("deleted"), CursorJson::StringArray(Deleted));
				if (Refused.Num() > 0)
				{
					Out->SetArrayField(TEXT("refused"), CursorJson::StringArray(Refused));
				}
				FCursorToolResult Result = FCursorToolResult::OkJson(Out);
				Result.bIsError = Refused.Num() > 0;
				return Result;
			})
			.Register();

		FCursorToolBuilder(TEXT("selection_set"), EditorPack)
			.Describe(TEXT("Select actors in the level editor so the user can see what you are referring to. Replaces the current selection; "
				"an empty list clears it. Read the selection with editor_get_context."))
			.Risk(ECursorToolRisk::Mutating)
			.Schema(FCursorSchema().StrArray(TEXT("names"), TEXT("Actor names or labels."), true))
			.Run([](const FCursorArgs& Args, const FCursorToolContext&)
			{
				UEditorActorSubsystem* Actors = ActorSubsystem();
				if (!Actors)
				{
					return FCursorToolResult::Error(TEXT("No level is open in the editor."));
				}
				TArray<AActor*> Targets;
				for (const FString& Name : Args.StrArray(TEXT("names")))
				{
					FString Error;
					AActor* Actor = CursorToolHelpers::FindActor(Name, Error);
					if (!Actor)
					{
						return FCursorToolResult::Error(Error);
					}
					Targets.AddUnique(Actor);
				}
				Actors->SetSelectedLevelActors(Targets);
				return FCursorToolResult::Ok(FString::Printf(TEXT("Selected %d actor(s)."), Targets.Num()));
			})
			.Register();
	}

	void RegisterObjectTools()
	{
		FCursorToolBuilder(TEXT("object_get_properties"), EditorPack)
			.Describe(TEXT("Read property values from an actor, a component (Actor.Component), or an asset (/Game/...). With no 'names' it returns "
				"the properties the Details panel shows; narrow with 'filter'. Values come back as JSON in the same shape object_set_properties accepts."))
			.Risk(ECursorToolRisk::ReadOnly)
			.Schema(FCursorSchema()
				.Str(TEXT("object"), TEXT("Actor name or label, 'Actor.Component', or an asset path."), true)
				.StrArray(TEXT("names"), TEXT("Exact property names to read."))
				.Str(TEXT("filter"), TEXT("Only properties whose name contains this text. Ignored when 'names' is given."))
				.Int(TEXT("limit"), TEXT("Maximum properties to return. Default 60.")))
			.Run([](const FCursorArgs& Args, const FCursorToolContext&)
			{
				FString Error;
				const UObject* Object = CursorToolHelpers::ResolveObject(Args.Str(TEXT("object")), Error);
				if (!Object)
				{
					return FCursorToolResult::Error(Error);
				}
				TArray<FString> Missing;
				const TSharedRef<FJsonObject> Properties = CursorToolHelpers::ReadProperties(
					Object, Args.StrArray(TEXT("names")), Args.Str(TEXT("filter")), FMath::Clamp(Args.Int(TEXT("limit"), 60), 1, 500), Missing);

				TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
				Out->SetStringField(TEXT("object"), Object->GetPathName());
				Out->SetStringField(TEXT("class"), Object->GetClass()->GetPathName());
				Out->SetObjectField(TEXT("properties"), Properties);
				if (Missing.Num() > 0)
				{
					Out->SetArrayField(TEXT("not_found"), CursorJson::StringArray(Missing));
				}
				return FCursorToolResult::OkJson(Out);
			})
			.Register();

		FCursorToolBuilder(TEXT("object_set_properties"), EditorPack)
			.Describe(TEXT("Set one or more properties on an actor, a component (Actor.Component) or an asset, as if edited in the Details panel. Undoable. "
				"Values are JSON: numbers, booleans, strings (enum value names, asset paths for object references), arrays, and objects for structs "
				"({\"R\":1,\"G\":0,\"B\":0,\"A\":1}). Reach into a struct with a dot: \"RelativeLocation.Z\". Assets still need asset_save to reach disk."))
			.Risk(ECursorToolRisk::Mutating)
			.Schema(FCursorSchema()
				.Str(TEXT("object"), TEXT("Actor name or label, 'Actor.Component', or an asset path."), true)
				.Obj(TEXT("properties"), TEXT("Map of property name to new value."), true))
			.Run([](const FCursorArgs& Args, const FCursorToolContext&)
			{
				FString Error;
				UObject* Object = CursorToolHelpers::ResolveObject(Args.Str(TEXT("object")), Error);
				if (!Object)
				{
					return FCursorToolResult::Error(Error);
				}
				const TSharedPtr<FJsonObject> Properties = Args.Obj(TEXT("properties"));
				if (!Properties.IsValid() || Properties->Values.Num() == 0)
				{
					return FCursorToolResult::Error(TEXT("'properties' must be an object with at least one entry."));
				}

				TArray<FString> Set, Failures;
				for (const auto& Pair : Properties->Values)
				{
					// Changing a component of a placed Blueprint actor reruns the actor's construction script, which
					// replaces its components. Look the object up again for each property, or every write after the
					// first lands on the discarded component and is lost while still being reported as set.
					FString SetError;
					Object = CursorToolHelpers::ResolveObject(Args.Str(TEXT("object")), SetError);
					if (Object && CursorToolHelpers::WriteProperty(Object, Pair.Key, Pair.Value, SetError))
					{
						Set.Add(Pair.Key);
					}
					else
					{
						Failures.Add(SetError);
					}
				}
				Object = CursorToolHelpers::ResolveObject(Args.Str(TEXT("object")), Error);
				if (!Object)
				{
					return FCursorToolResult::Error(Error);
				}
				if (Set.Num() > 0)
				{
					Object->MarkPackageDirty();
				}

				TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
				Out->SetStringField(TEXT("object"), Object->GetPathName());
				Out->SetArrayField(TEXT("set"), CursorJson::StringArray(Set));
				if (Failures.Num() > 0)
				{
					Out->SetArrayField(TEXT("failed"), CursorJson::StringArray(Failures));
				}
				FCursorToolResult Result = FCursorToolResult::OkJson(Out);
				Result.bIsError = Failures.Num() > 0;
				return Result;
			})
			.Register();
	}

	void RegisterEscapeHatches()
	{
		FCursorToolBuilder(TEXT("console_command"), EditorPack)
			.Describe(TEXT("Run an editor console command (for example 'stat fps', 'r.ScreenPercentage 75', 'obj list class=StaticMesh'). "
				"Asks the user for approval. Returns what the command printed."))
			.Risk(ECursorToolRisk::Dangerous)
			.Schema(FCursorSchema().Str(TEXT("command"), TEXT("The command line to run."), true))
			.Run([](const FCursorArgs& Args, const FCursorToolContext&)
			{
				const FString Command = Args.Str(TEXT("command")).TrimStartAndEnd();
				if (Command.IsEmpty() || !GEditor)
				{
					return FCursorToolResult::Error(TEXT("'command' is empty."));
				}
				// Commands print either to the device they are handed or to the global log. Collect both.
				const int64 Before = FCursorLogCapture::Get().GetLatestSequence();
				FStringOutputDevice Direct;
				Direct.SetAutoEmitLineTerminator(true);
				const bool bHandled = GEditor->Exec(CursorToolHelpers::GetEditorWorld(), *Command, Direct);

				FString Text = Direct;
				const FString Logged = JoinLogLines(FCursorLogCapture::Get().GetLines(Before, 400));
				if (!Logged.IsEmpty())
				{
					Text += (Text.IsEmpty() ? TEXT("") : TEXT("\n")) + Logged;
				}
				if (!bHandled)
				{
					return FCursorToolResult::Error(FString::Printf(TEXT("'%s' was not recognised as a console command.\n%s"), *Command, *Text));
				}
				return FCursorToolResult::Ok(Text.IsEmpty() ? FString(TEXT("Command ran and printed nothing.")) : Text);
			})
			.Register();

		FCursorToolBuilder(TEXT("python_exec"), EditorPack)
			.Describe(TEXT("Run Python in the editor with the 'unreal' module, for anything the other tools do not cover. Asks the user for approval. "
				"mode 'exec' runs a script and returns what it printed; mode 'eval' evaluates one expression and returns its value. "
				"Prefer the dedicated tools when one fits: they are undoable and report errors better."))
			.Risk(ECursorToolRisk::Dangerous)
			.Schema(FCursorSchema()
				.Str(TEXT("code"), TEXT("Python source."), true)
				.Enum(TEXT("mode"), TEXT("Default 'exec'."), { TEXT("exec"), TEXT("eval") }))
			.Run([](const FCursorArgs& Args, const FCursorToolContext&)
			{
				IPythonScriptPlugin* Python = IPythonScriptPlugin::Get();
				if (!Python || !Python->IsPythonAvailable())
				{
					return FCursorToolResult::Error(TEXT("Python is not available. Enable the 'Python Editor Script Plugin' and restart the editor."));
				}
				FPythonCommandEx Command;
				Command.Command = Args.Str(TEXT("code"));
				Command.ExecutionMode = Args.Str(TEXT("mode")).Equals(TEXT("eval"), ESearchCase::IgnoreCase)
					? EPythonCommandExecutionMode::EvaluateStatement
					: EPythonCommandExecutionMode::ExecuteFile;
				// Unattended: a script must not open a dialog the agent cannot answer.
				Command.Flags = EPythonCommandFlags::Unattended;
				if (Command.Command.TrimStartAndEnd().IsEmpty())
				{
					return FCursorToolResult::Error(TEXT("'code' is empty."));
				}

				const bool bOk = Python->ExecPythonCommandEx(Command);

				FString Output;
				for (const FPythonLogOutputEntry& Entry : Command.LogOutput)
				{
					if (Entry.Type != EPythonLogOutputType::Info)
					{
						Output += FString::Printf(TEXT("[%s] "), LexToString(Entry.Type));
					}
					Output += Entry.Output;
					Output += TEXT("\n");
				}
				if (!bOk)
				{
					return FCursorToolResult::Error(FString::Printf(TEXT("Python failed.\n%s\n%s"), *Command.CommandResult, *Output));
				}
				if (Command.ExecutionMode == EPythonCommandExecutionMode::EvaluateStatement)
				{
					Output = FString::Printf(TEXT("result: %s\n%s"), *Command.CommandResult, *Output);
				}
				return FCursorToolResult::Ok(Output.IsEmpty() ? FString(TEXT("Script ran and printed nothing.")) : Output);
			})
			.Register();
	}
}

void CursorToolPacks::RegisterEditorPack()
{
	RegisterContextTools();
	RegisterActorTools();
	RegisterObjectTools();
	RegisterEscapeHatches();
}

void CursorToolPacks::UnregisterEditorPack()
{
	FCursorToolRegistry::Get().UnregisterPack(EditorPack);
}
