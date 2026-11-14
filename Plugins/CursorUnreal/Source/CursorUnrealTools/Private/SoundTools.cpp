#include "CursorToolPacks.h"

#include "Components/AudioComponent.h"
#include "CursorJson.h"
#include "CursorPackSupport.h"
#include "CursorTool.h"
#include "CursorToolHelpers.h"
#include "CursorToolRegistry.h"
#include "Editor.h"
#include "Engine/World.h"
#include "Factories/SoundCueFactoryNew.h"
#include "Sound/AmbientSound.h"
#include "Sound/DialogueWave.h"
#include "Sound/SoundAttenuation.h"
#include "Sound/SoundBase.h"
#include "Sound/SoundClass.h"
#include "Sound/SoundCue.h"
#include "Sound/SoundNode.h"
#include "Sound/SoundNodeLooping.h"
#include "Sound/SoundNodeWavePlayer.h"
#include "Sound/SoundWave.h"
#include "EdGraph/EdGraphNode.h"

/*
 * Sound pack: build a Sound Cue from sound waves (one wave, or several played at random, optionally looping), create and
 * tune a sound attenuation asset, assign attenuation to a sound, read what a sound asset is, preview a sound in the editor,
 * and place an ambient sound in the level. MetaSound sources can be read, previewed and placed like any other sound, but
 * building a MetaSound graph needs the MetaSound builder API, which is out of scope here (python_exec covers it).
 * Edits stay in memory until asset_save.
 */

namespace
{
	const TCHAR* SoundPack = TEXT("audio");

	// Helpers carry a Sound prefix so a unity build that merges every tool file does not collide on names.
	TSharedPtr<FJsonValue> SoundJson(const TSharedRef<FJsonObject>& Object)
	{
		return MakeShared<FJsonValueObject>(Object);
	}

	USoundBase* SoundLoad(const FString& Path, bool bForEditing, FString& OutError)
	{
		if (Path.IsEmpty())
		{
			OutError = TEXT("'sound' is required: the asset path of a Sound Cue, Sound Wave or MetaSound.");
			return nullptr;
		}
		UObject* Asset = CursorToolHelpers::LoadAsset(Path, OutError);
		if (!Asset)
		{
			return nullptr;
		}
		USoundBase* Sound = Cast<USoundBase>(Asset);
		if (!Sound)
		{
			OutError = FString::Printf(TEXT("'%s' is a %s, not a sound."), *Path, *Asset->GetClass()->GetName());
			return nullptr;
		}
		if (bForEditing && !CursorPackSupport::IsWritableContentPath(Sound->GetOutermost()->GetName(), OutError))
		{
			return nullptr;
		}
		return Sound;
	}

	USoundAttenuation* SoundLoadAttenuation(const FString& Path, bool bForEditing, FString& OutError)
	{
		if (Path.IsEmpty())
		{
			OutError = TEXT("'attenuation' is required: the asset path of a Sound Attenuation.");
			return nullptr;
		}
		UObject* Asset = CursorToolHelpers::LoadAsset(Path, OutError);
		if (!Asset)
		{
			return nullptr;
		}
		USoundAttenuation* Attenuation = Cast<USoundAttenuation>(Asset);
		if (!Attenuation)
		{
			OutError = FString::Printf(TEXT("'%s' is a %s, not a Sound Attenuation."), *Path, *Asset->GetClass()->GetName());
			return nullptr;
		}
		if (bForEditing && !CursorPackSupport::IsWritableContentPath(Attenuation->GetOutermost()->GetName(), OutError))
		{
			return nullptr;
		}
		return Attenuation;
	}

	FString SoundShortNodeName(const USoundNode* Node)
	{
		FString Name = Node->GetClass()->GetName();
		Name.RemoveFromStart(TEXT("SoundNode"));
		return Name;
	}

	// ---- attenuation --------------------------------------------------------------------------------------------------------

	const TCHAR* SoundAlgorithmNames[] = { TEXT("linear"), TEXT("logarithmic"), TEXT("inverse"), TEXT("log_reverse"), TEXT("natural_sound") };

	bool SoundParseAlgorithm(const FString& Name, EAttenuationDistanceModel& Out)
	{
		const FString Lower = Name.ToLower();
		for (int32 i = 0; i < UE_ARRAY_COUNT(SoundAlgorithmNames); ++i)
		{
			if (Lower == SoundAlgorithmNames[i])
			{
				Out = static_cast<EAttenuationDistanceModel>(i);
				return true;
			}
		}
		return false;
	}

	/** Checks attenuation arguments without touching anything, so a bad one is reported before an asset is made or changed. */
	bool SoundValidateAttenuationArgs(const FCursorArgs& Args, FString& OutError)
	{
		EAttenuationDistanceModel Algorithm;
		if (Args.Has(TEXT("distance_algorithm")) && !SoundParseAlgorithm(Args.Str(TEXT("distance_algorithm")), Algorithm))
		{
			OutError = FString::Printf(TEXT("Unknown distance_algorithm '%s'. Use linear, logarithmic, inverse, log_reverse or natural_sound."), *Args.Str(TEXT("distance_algorithm")));
			return false;
		}
		if (Args.Has(TEXT("inner_radius")) && Args.Num(TEXT("inner_radius")) < 0.0)
		{
			OutError = TEXT("'inner_radius' cannot be negative.");
			return false;
		}
		if (Args.Has(TEXT("falloff_distance")) && Args.Num(TEXT("falloff_distance")) < 0.0)
		{
			OutError = TEXT("'falloff_distance' cannot be negative.");
			return false;
		}
		return true;
	}

	/** Inner radius and falloff are what a sphere attenuation is really about; everything else keeps the engine's default. Validate first. */
	void SoundApplyAttenuationArgs(USoundAttenuation* Asset, const FCursorArgs& Args)
	{
		FSoundAttenuationSettings& Settings = Asset->Attenuation;
		EAttenuationDistanceModel Algorithm = Settings.DistanceAlgorithm;
		if (Args.Has(TEXT("distance_algorithm")))
		{
			SoundParseAlgorithm(Args.Str(TEXT("distance_algorithm")), Algorithm);
		}

		Asset->Modify();
		Settings.bAttenuate = true;
		Settings.AttenuationShape = EAttenuationShape::Sphere;
		Settings.DistanceAlgorithm = Algorithm;
		if (Args.Has(TEXT("inner_radius")))
		{
			Settings.AttenuationShapeExtents = FVector(Args.Num(TEXT("inner_radius")), 0.0, 0.0);
		}
		if (Args.Has(TEXT("falloff_distance")))
		{
			Settings.FalloffDistance = static_cast<float>(Args.Num(TEXT("falloff_distance")));
		}
		if (Args.Has(TEXT("spatialize")))
		{
			Settings.bSpatialize = Args.Bool(TEXT("spatialize"));
		}
		if (Args.Has(TEXT("low_pass_filter")))
		{
			Settings.bAttenuateWithLPF = Args.Bool(TEXT("low_pass_filter"));
		}
		Asset->MarkPackageDirty();
	}

	TSharedRef<FJsonObject> SoundDescribeAttenuation(const USoundAttenuation* Asset)
	{
		const FSoundAttenuationSettings& Settings = Asset->Attenuation;
		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetStringField(TEXT("path"), Asset->GetPathName());
		Out->SetBoolField(TEXT("attenuate"), Settings.bAttenuate);
		Out->SetBoolField(TEXT("spatialize"), Settings.bSpatialize);
		Out->SetBoolField(TEXT("low_pass_filter"), Settings.bAttenuateWithLPF);
		Out->SetStringField(TEXT("shape"), StaticEnum<EAttenuationShape::Type>()->GetNameStringByValue(Settings.AttenuationShape.GetValue()));
		const int32 Algorithm = static_cast<int32>(Settings.DistanceAlgorithm);
		Out->SetStringField(TEXT("distance_algorithm"), Algorithm >= 0 && Algorithm < UE_ARRAY_COUNT(SoundAlgorithmNames) ? SoundAlgorithmNames[Algorithm] : TEXT("custom"));
		Out->SetNumberField(TEXT("inner_radius"), Settings.AttenuationShapeExtents.X);
		Out->SetNumberField(TEXT("falloff_distance"), Settings.FalloffDistance);
		Out->SetNumberField(TEXT("max_distance"), Settings.GetMaxDimension() + Settings.FalloffDistance);
		return Out;
	}
}

void CursorToolPacks::RegisterSoundPack()
{
	FCursorToolBuilder(TEXT("sound_create_cue"), SoundPack)
		.Describe(TEXT("Create a Sound Cue from sound waves. One wave plays as is; several play one at random each time. Optional volume and pitch multipliers, looping, and a Sound Attenuation asset. "
			"It exists in memory until asset_save writes it. Add the waves by asset path (import audio into the project first)."))
		.Risk(ECursorToolRisk::Mutating)
		.Schema(FCursorSchema()
			.Str(TEXT("path"), TEXT("Where to create it, for example /Game/Audio/SC_Footstep."), true)
			.StrArray(TEXT("waves"), TEXT("Sound Wave (or Dialogue Wave) asset paths to play. Default: an empty cue."))
			.Num(TEXT("volume"), TEXT("Volume multiplier. Default 1."))
			.Num(TEXT("pitch"), TEXT("Pitch multiplier. Default 1."))
			.Bool(TEXT("looping"), TEXT("Loop the cue indefinitely. Default false."))
			.Str(TEXT("attenuation"), TEXT("Sound Attenuation asset path to use for distance falloff.")))
		.Run([](const FCursorArgs& Args, const FCursorToolContext&)
		{
			FString Error;
			// Check everything that can be checked before the asset exists, so a bad argument leaves nothing half-made.
			TArray<TWeakObjectPtr<USoundWave>> Waves;
			TArray<TWeakObjectPtr<UDialogueWave>> Dialogues;
			for (const FString& Path : Args.StrArray(TEXT("waves")))
			{
				UObject* Asset = CursorToolHelpers::LoadAsset(Path, Error);
				if (!Asset)
				{
					return FCursorToolResult::Error(Error);
				}
				if (USoundWave* Wave = Cast<USoundWave>(Asset)) { Waves.Add(Wave); }
				else if (UDialogueWave* Dialogue = Cast<UDialogueWave>(Asset)) { Dialogues.Add(Dialogue); }
				else { return FCursorToolResult::Error(FString::Printf(TEXT("'%s' is a %s, not a Sound Wave."), *Path, *Asset->GetClass()->GetName())); }
			}
			if (Waves.Num() + Dialogues.Num() > 32)
			{
				return FCursorToolResult::Error(TEXT("A random node holds at most 32 sounds. Split them over several cues."));
			}
			const double Volume = Args.Has(TEXT("volume")) ? Args.Num(TEXT("volume")) : 1.0;
			const double Pitch = Args.Has(TEXT("pitch")) ? Args.Num(TEXT("pitch")) : 1.0;
			if (Volume < 0.0 || Pitch <= 0.0)
			{
				return FCursorToolResult::Error(TEXT("'volume' cannot be negative and 'pitch' must be above zero."));
			}
			USoundAttenuation* Attenuation = nullptr;
			if (Args.Has(TEXT("attenuation")))
			{
				Attenuation = SoundLoadAttenuation(Args.Str(TEXT("attenuation")), false, Error);
				if (!Attenuation)
				{
					return FCursorToolResult::Error(Error);
				}
			}

			UPackage* Package = nullptr;
			FString AssetName;
			if (!CursorPackSupport::PrepareNewAsset(Args.Str(TEXT("path")), Package, AssetName, Error))
			{
				return FCursorToolResult::Error(Error);
			}

			USoundCueFactoryNew* Factory = NewObject<USoundCueFactoryNew>();
			Factory->InitialSoundWaves = Waves;
			Factory->InitialDialogueWaves = Dialogues;
			USoundCue* Cue = Cast<USoundCue>(Factory->FactoryCreateNew(USoundCue::StaticClass(), Package, FName(*AssetName),
				RF_Public | RF_Standalone | RF_Transactional, nullptr, GWarn));
			if (!Cue)
			{
				return FCursorToolResult::Error(TEXT("The editor could not create the Sound Cue. See editor_log_tail."));
			}
			Cue->VolumeMultiplier = static_cast<float>(Volume);
			Cue->PitchMultiplier = static_cast<float>(Pitch);
			if (Attenuation)
			{
				Cue->AttenuationSettings = Attenuation;
			}

			bool bLooping = false;
			if (Args.Bool(TEXT("looping")) && Cue->FirstNode)
			{
				// Wrap the existing graph in a looping node and make room for it on the canvas.
				USoundNode* Previous = Cue->FirstNode;
				USoundNodeLooping* Loop = Cue->ConstructSoundNode<USoundNodeLooping>();
				Loop->bLoopIndefinitely = true;
				Loop->InsertChildNode(0);
				Loop->ChildNodes[0] = Previous;
				for (USoundNode* Node : Cue->AllNodes)
				{
					if (Node != Loop && Node->GraphNode)
					{
						Node->GraphNode->NodePosX -= 250;
					}
				}
				Cue->FirstNode = Loop;
				Cue->LinkGraphNodesFromSoundNodes();
				if (Loop->GraphNode)
				{
					Loop->GraphNode->NodePosX = -200;
					Loop->GraphNode->NodePosY = 0;
				}
				bLooping = true;
			}
			CursorPackSupport::FinishNewAsset(Cue);

			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetStringField(TEXT("path"), Cue->GetPathName());
			Out->SetNumberField(TEXT("sound_count"), Waves.Num() + Dialogues.Num());
			Out->SetBoolField(TEXT("looping"), bLooping);
			if (Args.Bool(TEXT("looping")) && !bLooping)
			{
				Out->SetStringField(TEXT("looping_note"), TEXT("An empty cue has nothing to loop. Add waves, then set looping."));
			}
			Out->SetStringField(TEXT("note"), TEXT("Created in memory. Call asset_save to write it to disk."));
			return FCursorToolResult::OkJson(Out);
		})
		.Register();

	FCursorToolBuilder(TEXT("sound_get_info"), SoundPack)
		.Describe(TEXT("Inspect a sound asset (Sound Cue, Sound Wave, MetaSound source...): duration, looping, volume and pitch, attenuation, and max audible distance. "
			"A Sound Cue also lists its nodes and the waves it plays. A Sound Wave also reports channels and sample rate."))
		.Risk(ECursorToolRisk::ReadOnly)
		.Schema(FCursorSchema()
			.Str(TEXT("sound"), TEXT("Sound asset path."), true))
		.Run([](const FCursorArgs& Args, const FCursorToolContext&)
		{
			FString Error;
			USoundBase* Sound = SoundLoad(Args.Str(TEXT("sound")), false, Error);
			if (!Sound)
			{
				return FCursorToolResult::Error(Error);
			}

			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetStringField(TEXT("path"), Sound->GetPathName());
			Out->SetStringField(TEXT("class"), Sound->GetClass()->GetName());
			Out->SetBoolField(TEXT("has_unsaved_changes"), Sound->GetOutermost()->IsDirty());
			const float Duration = Sound->GetDuration();
			// Indefinite sounds report a huge duration; say so rather than print it.
			Out->SetStringField(TEXT("duration"), Duration >= INDEFINITELY_LOOPING_DURATION ? FString(TEXT("indefinite")) : FString::SanitizeFloat(Duration));
			Out->SetBoolField(TEXT("looping"), Sound->IsLooping());
			Out->SetNumberField(TEXT("volume"), Sound->GetVolumeMultiplier());
			Out->SetNumberField(TEXT("pitch"), Sound->GetPitchMultiplier());
			Out->SetNumberField(TEXT("max_distance"), Sound->GetMaxDistance());
			if (Sound->AttenuationSettings)
			{
				Out->SetStringField(TEXT("attenuation"), Sound->AttenuationSettings->GetPathName());
			}
			if (const USoundClass* SoundClass = Sound->GetSoundClass())
			{
				Out->SetStringField(TEXT("sound_class"), SoundClass->GetPathName());
			}

			if (USoundCue* Cue = Cast<USoundCue>(Sound))
			{
				Out->SetBoolField(TEXT("overrides_attenuation"), Cue->bOverrideAttenuation);
				TArray<USoundNode*> Nodes;
				Cue->RecursiveFindAllNodes(Cue->FirstNode, Nodes);
				TArray<TSharedPtr<FJsonValue>> NodeList;
				TArray<FString> Waves;
				for (USoundNode* Node : Nodes)
				{
					TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
					Item->SetStringField(TEXT("type"), SoundShortNodeName(Node));
					Item->SetNumberField(TEXT("children"), Node->ChildNodes.Num());
					if (const USoundNodeWavePlayer* Player = Cast<USoundNodeWavePlayer>(Node))
					{
						if (const USoundWave* Wave = Player->GetSoundWave())
						{
							Item->SetStringField(TEXT("wave"), Wave->GetPathName());
							Waves.AddUnique(Wave->GetPathName());
						}
					}
					NodeList.Add(SoundJson(Item));
				}
				Out->SetArrayField(TEXT("nodes"), NodeList);
				Out->SetArrayField(TEXT("waves"), CursorJson::StringArray(Waves));
			}
			else if (const USoundWave* Wave = Cast<USoundWave>(Sound))
			{
				Out->SetNumberField(TEXT("channels"), Wave->NumChannels);
				Out->SetNumberField(TEXT("sample_rate"), Wave->GetSampleRateForCurrentPlatform());
			}
			return FCursorToolResult::OkJson(Out);
		})
		.Register();

	FCursorToolBuilder(TEXT("sound_create_attenuation"), SoundPack)
		.Describe(TEXT("Create a Sound Attenuation asset with a spherical falloff: full volume inside inner_radius, fading to silence over falloff_distance beyond it (centimetres). "
			"Assign it with sound_set_attenuation or the attenuation argument of sound_create_cue. Tune it later with sound_configure_attenuation."))
		.Risk(ECursorToolRisk::Mutating)
		.Schema(FCursorSchema()
			.Str(TEXT("path"), TEXT("Where to create it, for example /Game/Audio/ATT_Footstep."), true)
			.Num(TEXT("inner_radius"), TEXT("Radius with no attenuation, in cm. Default 400."))
			.Num(TEXT("falloff_distance"), TEXT("Distance over which the sound fades out beyond the inner radius, in cm. Default 3600."))
			.Enum(TEXT("distance_algorithm"), TEXT("Falloff curve. Default linear."), { TEXT("linear"), TEXT("logarithmic"), TEXT("inverse"), TEXT("log_reverse"), TEXT("natural_sound") })
			.Bool(TEXT("spatialize"), TEXT("Pan the sound in 3D. Default true."))
			.Bool(TEXT("low_pass_filter"), TEXT("Muffle with distance. Default false.")))
		.Run([](const FCursorArgs& Args, const FCursorToolContext&)
		{
			FString Error;
			if (!SoundValidateAttenuationArgs(Args, Error))
			{
				return FCursorToolResult::Error(Error);
			}
			UPackage* Package = nullptr;
			FString AssetName;
			if (!CursorPackSupport::PrepareNewAsset(Args.Str(TEXT("path")), Package, AssetName, Error))
			{
				return FCursorToolResult::Error(Error);
			}
			USoundAttenuation* Attenuation = NewObject<USoundAttenuation>(Package, FName(*AssetName), RF_Public | RF_Standalone | RF_Transactional);
			if (!Attenuation)
			{
				return FCursorToolResult::Error(TEXT("The editor could not create the Sound Attenuation. See editor_log_tail."));
			}
			// New assets start at the editor's defaults; spell the ones this tool promises so the answer does not drift with the engine.
			Attenuation->Attenuation.AttenuationShapeExtents = FVector(400.0, 0.0, 0.0);
			Attenuation->Attenuation.FalloffDistance = 3600.0f;
			SoundApplyAttenuationArgs(Attenuation, Args);
			CursorPackSupport::FinishNewAsset(Attenuation);
			TSharedRef<FJsonObject> Out = SoundDescribeAttenuation(Attenuation);
			Out->SetStringField(TEXT("note"), TEXT("Created in memory. Call asset_save to write it to disk."));
			return FCursorToolResult::OkJson(Out);
		})
		.Register();

	FCursorToolBuilder(TEXT("sound_configure_attenuation"), SoundPack)
		.Describe(TEXT("Change a Sound Attenuation asset: inner radius and falloff distance (cm), falloff curve, 3D spatialization, low-pass filtering. Only the arguments you pass change. Returns the result."))
		.Risk(ECursorToolRisk::Mutating)
		.Schema(FCursorSchema()
			.Str(TEXT("attenuation"), TEXT("Sound Attenuation asset path."), true)
			.Num(TEXT("inner_radius"), TEXT("Radius with no attenuation, in cm."))
			.Num(TEXT("falloff_distance"), TEXT("Distance over which the sound fades out beyond the inner radius, in cm."))
			.Enum(TEXT("distance_algorithm"), TEXT("Falloff curve."), { TEXT("linear"), TEXT("logarithmic"), TEXT("inverse"), TEXT("log_reverse"), TEXT("natural_sound") })
			.Bool(TEXT("spatialize"), TEXT("Pan the sound in 3D."))
			.Bool(TEXT("low_pass_filter"), TEXT("Muffle with distance.")))
		.Run([](const FCursorArgs& Args, const FCursorToolContext&)
		{
			FString Error;
			USoundAttenuation* Attenuation = SoundLoadAttenuation(Args.Str(TEXT("attenuation")), true, Error);
			if (!Attenuation || !SoundValidateAttenuationArgs(Args, Error))
			{
				return FCursorToolResult::Error(Error);
			}
			SoundApplyAttenuationArgs(Attenuation, Args);
			return FCursorToolResult::OkJson(SoundDescribeAttenuation(Attenuation));
		})
		.Register();

	FCursorToolBuilder(TEXT("sound_set_attenuation"), SoundPack)
		.Describe(TEXT("Use a Sound Attenuation asset on a sound (Sound Cue, Sound Wave...), or pass attenuation=\"none\" to clear it. A Sound Cue that overrides attenuation inline keeps its override; check overrides_attenuation in sound_get_info."))
		.Risk(ECursorToolRisk::Mutating)
		.Schema(FCursorSchema()
			.Str(TEXT("sound"), TEXT("Sound asset path."), true)
			.Str(TEXT("attenuation"), TEXT("Sound Attenuation asset path, or \"none\" to clear."), true))
		.Run([](const FCursorArgs& Args, const FCursorToolContext&)
		{
			FString Error;
			USoundBase* Sound = SoundLoad(Args.Str(TEXT("sound")), true, Error);
			if (!Sound)
			{
				return FCursorToolResult::Error(Error);
			}
			const FString Wanted = Args.Str(TEXT("attenuation"));
			USoundAttenuation* Attenuation = nullptr;
			if (!Wanted.Equals(TEXT("none"), ESearchCase::IgnoreCase))
			{
				Attenuation = SoundLoadAttenuation(Wanted, false, Error);
				if (!Attenuation)
				{
					return FCursorToolResult::Error(Error);
				}
			}
			Sound->Modify();
			Sound->AttenuationSettings = Attenuation;
			Sound->MarkPackageDirty();
			return FCursorToolResult::Ok(Attenuation ? FString::Printf(TEXT("%s now uses attenuation %s."), *Sound->GetName(), *Attenuation->GetName())
				: FString::Printf(TEXT("Cleared the attenuation asset on %s."), *Sound->GetName()));
		})
		.Register();

	FCursorToolBuilder(TEXT("sound_play"), SoundPack)
		.Describe(TEXT("Play a sound once through the editor's preview audio so the user can hear it. Replaces any preview already playing. Works for Sound Cues, Sound Waves and MetaSound sources. "
			"Silent when the editor runs without audio."))
		.Risk(ECursorToolRisk::Mutating)
		.Schema(FCursorSchema()
			.Str(TEXT("sound"), TEXT("Sound asset path."), true))
		.Run([](const FCursorArgs& Args, const FCursorToolContext&)
		{
			FString Error;
			USoundBase* Sound = SoundLoad(Args.Str(TEXT("sound")), false, Error);
			if (!Sound)
			{
				return FCursorToolResult::Error(Error);
			}
			if (!GEditor)
			{
				return FCursorToolResult::Error(TEXT("The editor is not running."));
			}
			UAudioComponent* Preview = GEditor->PlayPreviewSound(Sound);
			if (!Preview)
			{
				return FCursorToolResult::Error(TEXT("The editor has no audio device to play the preview on."));
			}
			return FCursorToolResult::Ok(FString::Printf(TEXT("Playing %s."), *Sound->GetName()));
		})
		.Register();

	FCursorToolBuilder(TEXT("sound_stop"), SoundPack)
		.Describe(TEXT("Stop the sound that sound_play started."))
		.Risk(ECursorToolRisk::Mutating)
		.Schema(FCursorSchema())
		.Run([](const FCursorArgs&, const FCursorToolContext&)
		{
			if (!GEditor)
			{
				return FCursorToolResult::Error(TEXT("The editor is not running."));
			}
			GEditor->ResetPreviewAudioComponent();
			return FCursorToolResult::Ok(TEXT("Stopped the preview sound."));
		})
		.Register();

	FCursorToolBuilder(TEXT("sound_spawn_ambient"), SoundPack)
		.Describe(TEXT("Place an Ambient Sound actor in the open level, playing the given sound from its location. Returns the actor."))
		.Risk(ECursorToolRisk::Mutating)
		.Schema(FCursorSchema()
			.Str(TEXT("sound"), TEXT("Sound asset path."), true)
			.Vec3(TEXT("location"), TEXT("World location [x, y, z] in cm. Default: the origin."))
			.Str(TEXT("label"), TEXT("Actor label in the World Outliner.")))
		.Run([](const FCursorArgs& Args, const FCursorToolContext&)
		{
			FString Error;
			USoundBase* Sound = SoundLoad(Args.Str(TEXT("sound")), false, Error);
			if (!Sound)
			{
				return FCursorToolResult::Error(Error);
			}
			UWorld* World = CursorToolHelpers::GetEditorWorld();
			if (!World)
			{
				return FCursorToolResult::Error(TEXT("There is no editor level open."));
			}
			FVector Location = FVector::ZeroVector;
			Args.Vec(TEXT("location"), Location);
			AAmbientSound* Actor = World->SpawnActor<AAmbientSound>(Location, FRotator::ZeroRotator);
			if (!Actor || !Actor->GetAudioComponent())
			{
				return FCursorToolResult::Error(TEXT("The editor could not spawn an Ambient Sound actor."));
			}
			Actor->GetAudioComponent()->SetSound(Sound);
			if (Args.Has(TEXT("label")))
			{
				Actor->SetActorLabel(Args.Str(TEXT("label")));
			}
			return FCursorToolResult::OkJson(CursorToolHelpers::DescribeActor(Actor, false));
		})
		.Register();
}

void CursorToolPacks::UnregisterSoundPack()
{
	FCursorToolRegistry::Get().UnregisterPack(SoundPack);
}
