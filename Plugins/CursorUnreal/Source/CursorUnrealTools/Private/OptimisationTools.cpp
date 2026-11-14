#include "CursorToolPacks.h"

#include "Algo/Count.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/LightComponent.h"
#include "Components/MeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "CursorJson.h"
#include "CursorTool.h"
#include "CursorToolHelpers.h"
#include "CursorToolRegistry.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Materials/MaterialInterface.h"
#include "RHIStats.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "StaticMeshResources.h"

/*
 * Optimisation pack, read-only. It answers "what is expensive in this level" from what the editor already knows: the open
 * level's meshes, materials and lights, and the render data of the meshes themselves. Triangle and draw call figures are
 * estimates from LOD 0 (the worst case); they do not replace the profiler, and the pack says so in its results. Nothing here
 * changes an asset or an actor. Acting on the advice (building LODs, enabling Nanite) goes through python_exec or the editor.
 */

namespace
{
	const TCHAR* OptimisationPack = TEXT("optimisation");

	// Helpers carry an Opt prefix so a unity build that merges every tool file does not collide on names.
	TSharedPtr<FJsonValue> OptJson(const TSharedRef<FJsonObject>& Object)
	{
		return MakeShared<FJsonValueObject>(Object);
	}

	/** What one actor or the whole level costs, counted the same way for both. */
	struct FOptCost
	{
		int64 Triangles = 0;
		int64 NaniteTriangles = 0;
		int32 DrawCalls = 0;
		int32 MeshComponents = 0;
		int32 SkeletalComponents = 0;
		int32 InstancedComponents = 0;
		/** Instances across instanced components. Non-instanced meshes count as one each in MeshComponents only. */
		int32 Instances = 0;
		int32 Lights = 0;
		int32 MovableLights = 0;
		int32 ShadowCastingLights = 0;
		int32 NiagaraComponents = 0;
		TSet<const UObject*> Meshes;
		TSet<const UMaterialInterface*> Materials;
	};

	bool OptIsNiagara(const UActorComponent* Component)
	{
		// Matched by name so this module does not link Niagara: the pack stays usable in projects without it.
		return Component->GetClass()->GetName() == TEXT("NiagaraComponent");
	}

	void OptCountMaterials(const UMeshComponent* Component, FOptCost& Cost)
	{
		for (int32 i = 0; i < Component->GetNumMaterials(); ++i)
		{
			if (const UMaterialInterface* Material = Component->GetMaterial(i))
			{
				Cost.Materials.Add(Material);
			}
		}
	}

	void OptAccumulateActor(const AActor* Actor, FOptCost& Cost)
	{
		TInlineComponentArray<const UActorComponent*> Components;
		Actor->GetComponents(Components);
		for (const UActorComponent* Component : Components)
		{
			if (const UStaticMeshComponent* StaticComponent = Cast<UStaticMeshComponent>(Component))
			{
				const UStaticMesh* Mesh = StaticComponent->GetStaticMesh();
				if (!Mesh || !StaticComponent->IsVisible() || !Mesh->GetRenderData())
				{
					continue;
				}
				const UInstancedStaticMeshComponent* Instanced = Cast<UInstancedStaticMeshComponent>(StaticComponent);
				const int32 Count = Instanced ? Instanced->GetInstanceCount() : 1;
				++Cost.MeshComponents;
				if (Instanced)
				{
					++Cost.InstancedComponents;
					Cost.Instances += Count;
				}
				Cost.Meshes.Add(Mesh);
				OptCountMaterials(StaticComponent, Cost);
				if (Mesh->IsNaniteEnabled())
				{
					// Nanite draws in a handful of passes whatever the section count, so it adds no per-section draw calls.
					Cost.NaniteTriangles += static_cast<int64>(Mesh->GetNumNaniteTriangles()) * Count;
				}
				else
				{
					Cost.Triangles += static_cast<int64>(Mesh->GetNumTriangles(0)) * Count;
					// One draw per section; an instanced component batches all its instances into that draw.
					Cost.DrawCalls += Instanced ? Mesh->GetNumSections(0) : Mesh->GetNumSections(0) * Count;
				}
			}
			else if (const USkeletalMeshComponent* SkeletalComponent = Cast<USkeletalMeshComponent>(Component))
			{
				const USkeletalMesh* Mesh = SkeletalComponent->GetSkeletalMeshAsset();
				const FSkeletalMeshRenderData* RenderData = Mesh ? Mesh->GetResourceForRendering() : nullptr;
				if (!RenderData || RenderData->LODRenderData.Num() == 0 || !SkeletalComponent->IsVisible())
				{
					continue;
				}
				++Cost.SkeletalComponents;
				Cost.Meshes.Add(Mesh);
				OptCountMaterials(SkeletalComponent, Cost);
				Cost.Triangles += RenderData->LODRenderData[0].GetTotalFaces();
				Cost.DrawCalls += RenderData->LODRenderData[0].RenderSections.Num();
			}
			else if (const ULightComponent* Light = Cast<ULightComponent>(Component))
			{
				if (!Light->IsVisible())
				{
					continue;
				}
				++Cost.Lights;
				Cost.MovableLights += Light->Mobility == EComponentMobility::Movable ? 1 : 0;
				Cost.ShadowCastingLights += Light->CastShadows ? 1 : 0;
			}
			else if (OptIsNiagara(Component))
			{
				++Cost.NiagaraComponents;
			}
		}
	}

	int32 OptCountTranslucent(const FOptCost& Cost)
	{
		int32 Count = 0;
		for (const UMaterialInterface* Material : Cost.Materials)
		{
			const EBlendMode Blend = Material->GetBlendMode();
			Count += (Blend == BLEND_Translucent || Blend == BLEND_Additive || Blend == BLEND_Modulate) ? 1 : 0;
		}
		return Count;
	}

	void OptWriteCost(const FOptCost& Cost, const TSharedRef<FJsonObject>& Out)
	{
		Out->SetNumberField(TEXT("triangles"), static_cast<double>(Cost.Triangles));
		Out->SetNumberField(TEXT("nanite_triangles"), static_cast<double>(Cost.NaniteTriangles));
		Out->SetNumberField(TEXT("draw_calls_estimate"), Cost.DrawCalls);
		Out->SetNumberField(TEXT("mesh_components"), Cost.MeshComponents);
		Out->SetNumberField(TEXT("skeletal_mesh_components"), Cost.SkeletalComponents);
		Out->SetNumberField(TEXT("unique_meshes"), Cost.Meshes.Num());
		Out->SetNumberField(TEXT("unique_materials"), Cost.Materials.Num());
	}

	/** Static mesh assets with how many instances use them; the unit suggest_lods works in. */
	struct FOptMeshUse
	{
		int32 Components = 0;
		int32 Instances = 0;
	};

	void OptGatherMeshUse(UWorld* World, TMap<UStaticMesh*, FOptMeshUse>& OutUse)
	{
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			TInlineComponentArray<UStaticMeshComponent*> Components;
			It->GetComponents(Components);
			for (UStaticMeshComponent* Component : Components)
			{
				UStaticMesh* Mesh = Component->GetStaticMesh();
				if (!Mesh)
				{
					continue;
				}
				FOptMeshUse& Use = OutUse.FindOrAdd(Mesh);
				++Use.Components;
				const UInstancedStaticMeshComponent* Instanced = Cast<UInstancedStaticMeshComponent>(Component);
				Use.Instances += Instanced ? Instanced->GetInstanceCount() : 1;
			}
		}
	}

	/** Triangle counts of each LOD, or empty when the mesh has no render data in this editor run. */
	TArray<int32> OptLodTriangles(const UStaticMesh* Mesh)
	{
		TArray<int32> Out;
		if (!Mesh->GetRenderData())
		{
			return Out;
		}
		for (int32 Lod = 0; Lod < Mesh->GetNumLODs(); ++Lod)
		{
			Out.Add(Mesh->GetNumTriangles(Lod));
		}
		return Out;
	}

	/** Plain-language advice for one mesh. Empty means nothing worth changing. */
	TArray<FString> OptLodAdvice(const UStaticMesh* Mesh, const TArray<int32>& Triangles, int32 Instances, int32 Threshold)
	{
		TArray<FString> Advice;
		if (Triangles.Num() == 0 || Mesh->IsNaniteEnabled())
		{
			return Advice;
		}
		const int32 Top = Triangles[0];
		if (Triangles.Num() == 1 && Top > Threshold)
		{
			Advice.Add(FString::Printf(TEXT("%d triangles and no LODs. Add LODs, for example LOD1 near %d, LOD2 near %d and LOD3 near %d triangles (screen sizes about 0.5, 0.25, 0.12), or enable Nanite if the target platforms support it."),
				Top, Top / 2, Top / 4, Top / 8));
		}
		for (int32 Lod = 1; Lod < Triangles.Num(); ++Lod)
		{
			if (Triangles[Lod - 1] > Threshold / 2 && Triangles[Lod] > Triangles[Lod - 1] * 0.75)
			{
				Advice.Add(FString::Printf(TEXT("LOD%d keeps %d%% of LOD%d's triangles (%d of %d). Reduce it harder so the LOD earns its switch."),
					Lod, FMath::RoundToInt(100.0 * Triangles[Lod] / FMath::Max(1, Triangles[Lod - 1])), Lod - 1, Triangles[Lod], Triangles[Lod - 1]));
			}
		}
		if (Advice.Num() > 0 && Instances >= 50)
		{
			Advice.Add(FString::Printf(TEXT("%d instances use this mesh, so any saving is multiplied."), Instances));
		}
		return Advice;
	}

	UWorld* OptWorld(FString& OutError)
	{
		UWorld* World = CursorToolHelpers::GetEditorWorld();
		if (!World)
		{
			OutError = TEXT("There is no editor level open.");
		}
		return World;
	}
}

void CursorToolPacks::RegisterOptimisationPack()
{
	FCursorToolBuilder(TEXT("optim_level_stats"), OptimisationPack)
		.Describe(TEXT("Cost overview of the open level: actor, mesh and light counts, triangles (LOD 0, Nanite counted separately), estimated draw calls, unique meshes and materials, "
			"translucent materials, movable and shadow-casting lights, Niagara components, plus the last frame's RHI draw call and primitive counts when a viewport has rendered. "
			"Figures are estimates from the level's contents, not a profile. Read-only."))
		.Risk(ECursorToolRisk::ReadOnly)
		.Schema(FCursorSchema())
		.Run([](const FCursorArgs&, const FCursorToolContext&)
		{
			FString Error;
			UWorld* World = OptWorld(Error);
			if (!World)
			{
				return FCursorToolResult::Error(Error);
			}
			FOptCost Total;
			int32 Actors = 0;
			for (TActorIterator<AActor> It(World); It; ++It)
			{
				++Actors;
				OptAccumulateActor(*It, Total);
			}

			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetStringField(TEXT("level"), World->GetMapName());
			Out->SetNumberField(TEXT("actors"), Actors);
			OptWriteCost(Total, Out);
			Out->SetNumberField(TEXT("instanced_components"), Total.InstancedComponents);
			Out->SetNumberField(TEXT("instances"), Total.Instances);
			Out->SetNumberField(TEXT("translucent_materials"), OptCountTranslucent(Total));
			Out->SetNumberField(TEXT("niagara_components"), Total.NiagaraComponents);

			TSharedRef<FJsonObject> Lights = MakeShared<FJsonObject>();
			Lights->SetNumberField(TEXT("total"), Total.Lights);
			Lights->SetNumberField(TEXT("movable"), Total.MovableLights);
			Lights->SetNumberField(TEXT("shadow_casting"), Total.ShadowCastingLights);
			Out->SetObjectField(TEXT("lights"), Lights);

			TSharedRef<FJsonObject> Rhi = MakeShared<FJsonObject>();
			Rhi->SetNumberField(TEXT("draw_calls"), GNumDrawCallsRHI[0]);
			Rhi->SetNumberField(TEXT("primitives"), GNumPrimitivesDrawnRHI[0]);
			Out->SetObjectField(TEXT("last_rhi_frame"), Rhi);
			Out->SetStringField(TEXT("note"), TEXT("Triangles and draw calls are counted from LOD 0 of every visible mesh, so they overstate what a distant view draws. "
				"last_rhi_frame is zero until a viewport renders. Use the profiler (stat unit, stat scenerendering) for real frame cost."));
			return FCursorToolResult::OkJson(Out);
		})
		.Register();

	FCursorToolBuilder(TEXT("optim_list_heavy_actors"), OptimisationPack)
		.Describe(TEXT("List the actors in the open level that cost the most, ranked by triangles (LOD 0), estimated draw calls, material count, or mesh component count. "
			"Each entry reports triangles, draw calls, mesh components, instances, materials and whether it uses Nanite. Read-only."))
		.Risk(ECursorToolRisk::ReadOnly)
		.Schema(FCursorSchema()
			.Enum(TEXT("sort_by"), TEXT("What to rank by. Default triangles."), { TEXT("triangles"), TEXT("draw_calls"), TEXT("materials"), TEXT("components") })
			.Int(TEXT("limit"), TEXT("How many actors to return. Default 15, at most 200."))
			.Int(TEXT("min_triangles"), TEXT("Skip actors with fewer triangles than this. Default 0.")))
		.Run([](const FCursorArgs& Args, const FCursorToolContext&)
		{
			FString Error;
			UWorld* World = OptWorld(Error);
			if (!World)
			{
				return FCursorToolResult::Error(Error);
			}
			const FString SortBy = Args.Str(TEXT("sort_by"), TEXT("triangles")).ToLower();
			if (SortBy != TEXT("triangles") && SortBy != TEXT("draw_calls") && SortBy != TEXT("materials") && SortBy != TEXT("components"))
			{
				return FCursorToolResult::Error(FString::Printf(TEXT("Unknown sort_by '%s'. Use triangles, draw_calls, materials or components."), *SortBy));
			}
			const int32 Limit = FMath::Clamp(Args.Has(TEXT("limit")) ? Args.Int(TEXT("limit")) : 15, 1, 200);
			const int64 MinTriangles = Args.Int(TEXT("min_triangles"), 0);

			struct FEntry
			{
				const AActor* Actor;
				FOptCost Cost;
				int64 Key;
			};
			TArray<FEntry> Entries;
			for (TActorIterator<AActor> It(World); It; ++It)
			{
				FEntry Entry;
				Entry.Actor = *It;
				OptAccumulateActor(*It, Entry.Cost);
				if (Entry.Cost.MeshComponents + Entry.Cost.SkeletalComponents == 0 || Entry.Cost.Triangles + Entry.Cost.NaniteTriangles < MinTriangles)
				{
					continue;
				}
				Entry.Key = SortBy == TEXT("triangles") ? Entry.Cost.Triangles + Entry.Cost.NaniteTriangles
					: SortBy == TEXT("draw_calls") ? Entry.Cost.DrawCalls
					: SortBy == TEXT("materials") ? Entry.Cost.Materials.Num()
					: Entry.Cost.MeshComponents + Entry.Cost.SkeletalComponents;
				Entries.Add(MoveTemp(Entry));
			}
			Entries.Sort([](const FEntry& A, const FEntry& B) { return A.Key > B.Key; });

			TArray<TSharedPtr<FJsonValue>> List;
			for (int32 i = 0; i < Entries.Num() && i < Limit; ++i)
			{
				const FEntry& Entry = Entries[i];
				TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetStringField(TEXT("label"), Entry.Actor->GetActorLabel());
				Item->SetStringField(TEXT("name"), Entry.Actor->GetName());
				Item->SetStringField(TEXT("class"), Entry.Actor->GetClass()->GetName());
				Item->SetObjectField(TEXT("location"), CursorJson::VectorToJson(Entry.Actor->GetActorLocation()));
				OptWriteCost(Entry.Cost, Item);
				Item->SetNumberField(TEXT("instances"), Entry.Cost.Instances);
				List.Add(OptJson(Item));
			}
			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetStringField(TEXT("sorted_by"), SortBy);
			Out->SetNumberField(TEXT("matching_actors"), Entries.Num());
			Out->SetArrayField(TEXT("actors"), List);
			return FCursorToolResult::OkJson(Out);
		})
		.Register();

	FCursorToolBuilder(TEXT("optim_mesh_info"), OptimisationPack)
		.Describe(TEXT("Cost facts for one static mesh asset: LOD count, triangles per LOD, sections (draw calls) at LOD 0, material slots, Nanite state, and bounds. "
			"Read-only; the asset is loaded but not changed."))
		.Risk(ECursorToolRisk::ReadOnly)
		.Schema(FCursorSchema()
			.Str(TEXT("mesh"), TEXT("Static mesh asset path, e.g. /Game/Props/SM_Rock."), true))
		.Run([](const FCursorArgs& Args, const FCursorToolContext&)
		{
			FString Error;
			UObject* Asset = CursorToolHelpers::LoadAsset(Args.Str(TEXT("mesh")), Error);
			const UStaticMesh* Mesh = Cast<UStaticMesh>(Asset);
			if (!Mesh)
			{
				return FCursorToolResult::Error(Asset ? FString::Printf(TEXT("'%s' is a %s, not a static mesh."), *Args.Str(TEXT("mesh")), *Asset->GetClass()->GetName()) : Error);
			}
			const TArray<int32> Triangles = OptLodTriangles(Mesh);

			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetStringField(TEXT("mesh"), Mesh->GetPathName());
			Out->SetNumberField(TEXT("lod_count"), Mesh->GetNumLODs());
			Out->SetNumberField(TEXT("triangles_lod0"), Triangles.Num() ? Triangles[0] : 0);
			TArray<TSharedPtr<FJsonValue>> Tris;
			for (const int32 Count : Triangles)
			{
				Tris.Add(MakeShared<FJsonValueNumber>(Count));
			}
			Out->SetArrayField(TEXT("triangles_per_lod"), Tris);
			Out->SetNumberField(TEXT("sections_lod0"), Triangles.Num() ? Mesh->GetNumSections(0) : 0);
			Out->SetNumberField(TEXT("material_slots"), Mesh->GetStaticMaterials().Num());
			Out->SetBoolField(TEXT("nanite"), Mesh->IsNaniteEnabled());
			Out->SetObjectField(TEXT("bounds_extent"), CursorJson::VectorToJson(Mesh->GetBounds().BoxExtent));
			if (Triangles.Num() == 0)
			{
				Out->SetStringField(TEXT("note"), TEXT("The mesh has no render data in this editor run, so triangle counts are unavailable."));
			}
			return FCursorToolResult::OkJson(Out);
		})
		.Register();

	FCursorToolBuilder(TEXT("optim_suggest_lods"), OptimisationPack)
		.Describe(TEXT("Suggest LOD work. For one static mesh asset, or for every static mesh used in the open level when none is given, reports LOD count, triangles per LOD, Nanite state, "
			"how many components and instances use it, and plain-language advice (meshes with no LODs above the triangle threshold, LODs that barely reduce triangles). "
			"Worst offenders first. Read-only: it does not build LODs."))
		.Risk(ECursorToolRisk::ReadOnly)
		.Schema(FCursorSchema()
			.Str(TEXT("mesh"), TEXT("Static mesh asset path. Default: every static mesh in the open level."))
			.Int(TEXT("heavy_threshold"), TEXT("Triangles above which a mesh without LODs is flagged. Default 5000."))
			.Int(TEXT("limit"), TEXT("How many meshes to return when scanning the level. Default 15, at most 100."))
			.Bool(TEXT("only_flagged"), TEXT("When scanning the level, list only meshes that have advice. Default true.")))
		.Run([](const FCursorArgs& Args, const FCursorToolContext&)
		{
			FString Error;
			const int32 Threshold = FMath::Max(100, Args.Has(TEXT("heavy_threshold")) ? Args.Int(TEXT("heavy_threshold")) : 5000);
			const int32 Limit = FMath::Clamp(Args.Has(TEXT("limit")) ? Args.Int(TEXT("limit")) : 15, 1, 100);
			const bool bOnlyFlagged = Args.Has(TEXT("only_flagged")) ? Args.Bool(TEXT("only_flagged")) : true;

			TMap<UStaticMesh*, FOptMeshUse> Use;
			const bool bSingle = Args.Has(TEXT("mesh"));
			if (bSingle)
			{
				UObject* Asset = CursorToolHelpers::LoadAsset(Args.Str(TEXT("mesh")), Error);
				UStaticMesh* Mesh = Cast<UStaticMesh>(Asset);
				if (!Mesh)
				{
					return FCursorToolResult::Error(Asset ? FString::Printf(TEXT("'%s' is a %s, not a static mesh."), *Args.Str(TEXT("mesh")), *Asset->GetClass()->GetName()) : Error);
				}
				Use.Add(Mesh);
				// Usage is a bonus here: with no level open the advice still stands.
				if (UWorld* World = CursorToolHelpers::GetEditorWorld())
				{
					TMap<UStaticMesh*, FOptMeshUse> InLevel;
					OptGatherMeshUse(World, InLevel);
					if (const FOptMeshUse* Found = InLevel.Find(Mesh))
					{
						Use[Mesh] = *Found;
					}
				}
			}
			else
			{
				UWorld* World = OptWorld(Error);
				if (!World)
				{
					return FCursorToolResult::Error(Error);
				}
				OptGatherMeshUse(World, Use);
			}

			struct FEntry
			{
				UStaticMesh* Mesh;
				FOptMeshUse Use;
				TArray<int32> Triangles;
				TArray<FString> Advice;
				int64 Weight;
			};
			TArray<FEntry> Entries;
			for (const TPair<UStaticMesh*, FOptMeshUse>& Pair : Use)
			{
				FEntry Entry;
				Entry.Mesh = Pair.Key;
				Entry.Use = Pair.Value;
				Entry.Triangles = OptLodTriangles(Pair.Key);
				Entry.Advice = OptLodAdvice(Pair.Key, Entry.Triangles, Pair.Value.Instances, Threshold);
				Entry.Weight = Entry.Triangles.Num() ? static_cast<int64>(Entry.Triangles[0]) * FMath::Max(1, Pair.Value.Instances) : 0;
				if (bSingle || !bOnlyFlagged || Entry.Advice.Num() > 0)
				{
					Entries.Add(MoveTemp(Entry));
				}
			}
			Entries.Sort([](const FEntry& A, const FEntry& B) { return A.Weight > B.Weight; });

			TArray<TSharedPtr<FJsonValue>> List;
			for (int32 i = 0; i < Entries.Num() && i < (bSingle ? 1 : Limit); ++i)
			{
				const FEntry& Entry = Entries[i];
				TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetStringField(TEXT("mesh"), Entry.Mesh->GetPathName());
				Item->SetBoolField(TEXT("nanite"), Entry.Mesh->IsNaniteEnabled());
				Item->SetNumberField(TEXT("lod_count"), Entry.Triangles.Num());
				TArray<TSharedPtr<FJsonValue>> Tris;
				for (const int32 Count : Entry.Triangles)
				{
					Tris.Add(MakeShared<FJsonValueNumber>(Count));
				}
				Item->SetArrayField(TEXT("triangles_per_lod"), Tris);
				if (const FStaticMeshRenderData* RenderData = Entry.Mesh->GetRenderData())
				{
					TArray<TSharedPtr<FJsonValue>> Screen;
					for (int32 Lod = 0; Lod < Entry.Triangles.Num() && Lod < MAX_STATIC_MESH_LODS; ++Lod)
					{
						Screen.Add(MakeShared<FJsonValueNumber>(RenderData->ScreenSize[Lod].Default));
					}
					Item->SetArrayField(TEXT("lod_screen_sizes"), Screen);
				}
				Item->SetNumberField(TEXT("components"), Entry.Use.Components);
				Item->SetNumberField(TEXT("instances"), Entry.Use.Instances);
				Item->SetArrayField(TEXT("advice"), CursorJson::StringArray(Entry.Advice));
				List.Add(OptJson(Item));
			}

			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetNumberField(TEXT("meshes_checked"), Use.Num());
			Out->SetNumberField(TEXT("flagged"), Algo::CountIf(Entries, [](const FEntry& E) { return E.Advice.Num() > 0; }));
			Out->SetArrayField(TEXT("suggestions"), List);
			return FCursorToolResult::OkJson(Out);
		})
		.Register();
}

void CursorToolPacks::UnregisterOptimisationPack()
{
	FCursorToolRegistry::Get().UnregisterPack(OptimisationPack);
}
