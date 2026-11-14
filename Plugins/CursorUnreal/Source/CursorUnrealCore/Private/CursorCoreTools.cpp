#include "CursorCoreTools.h"

#include "CursorJson.h"
#include "CursorSkillLibrary.h"
#include "CursorTool.h"
#include "CursorToolRegistry.h"
#include "CursorUnrealLog.h"
#include "Editor.h"
#include "Editor/Transactor.h"

namespace
{
	const TCHAR* CorePack = TEXT("core");
	const FName BatchName(TEXT("batch"));

	/**
	 * Runs several tool calls in order as one undo step. One model round trip instead of many,
	 * and a failure part-way can put the editor back as it was.
	 */
	class FBatchTool final : public ICursorTool
	{
	public:
		static constexpr int32 MaxCalls = 100;

		virtual FName GetName() const override { return BatchName; }
		virtual FName GetPack() const override { return CorePack; }
		virtual ECursorToolRisk GetRisk() const override { return ECursorToolRisk::Mutating; }
		virtual bool ManagesOwnTransaction() const override { return true; }

		virtual FText GetDescription() const override
		{
			return FText::FromString(TEXT(
				"Run several tool calls in order, in one request and one undo step. Use it whenever you already know the next few calls "
				"(for example: add the nodes of a graph, connect them, set their pin values, compile). "
				"A later call can use what an earlier one returned: give the earlier call a name with 'as', then write \"$name.field\" as an argument value "
				"(\"$tick.id\" is the id of the node the call named tick created; \"$0.id\" is the same for the first call, counting from 0; "
				"\"$list.nodes.0.id\" reaches into arrays). Inside longer text write \"${name.field}\". Start a value with $$ for a literal dollar sign. "
				"By default the batch stops at the first failure and undoes what the earlier calls changed; note that undo does not delete newly created asset files. "
				"Set 'brief' when you do not need to read each result: successful calls are then reported as ok only, failures in full."));
		}

		virtual TSharedRef<FJsonObject> GetInputSchema() const override
		{
			TSharedRef<FJsonObject> CallSchema = FCursorSchema()
				.Str(TEXT("tool"), TEXT("Name of the tool to call."), true)
				.Obj(TEXT("args"), TEXT("Arguments for that tool. A string value \"$name.field\" is replaced by that field of an earlier call's result."))
				.Str(TEXT("as"), TEXT("Optional name for this call's result, so later calls can refer to it as $name.field. Letters, digits and underscores, starting with a letter."))
				.Build();

			TSharedRef<FJsonObject> Calls = MakeShared<FJsonObject>();
			Calls->SetStringField(TEXT("type"), TEXT("array"));
			Calls->SetStringField(TEXT("description"), TEXT("Calls to run, in order."));
			Calls->SetObjectField(TEXT("items"), CallSchema);
			Calls->SetNumberField(TEXT("maxItems"), MaxCalls);

			TSharedRef<FJsonObject> Schema = FCursorSchema()
				.Bool(TEXT("stop_on_error"), TEXT("Stop at the first failing call. Default true."))
				.Bool(TEXT("rollback_on_error"), TEXT("If a call fails, undo the changes made by the calls before it. Default true."))
				.Bool(TEXT("brief"), TEXT("Report successful calls as ok only, without their result text. Failures are always reported in full. Default false."))
				.Build();
			Schema->GetObjectField(TEXT("properties"))->SetObjectField(TEXT("calls"), Calls);
			TArray<TSharedPtr<FJsonValue>> RequiredFields;
			RequiredFields.Add(MakeShared<FJsonValueString>(TEXT("calls")));
			Schema->SetArrayField(TEXT("required"), RequiredFields);
			return Schema;
		}

		virtual void GetSubCalls(const TSharedRef<FJsonObject>& Args, TArray<FCursorSubCall>& OutCalls) const override
		{
			const TArray<TSharedPtr<FJsonValue>>* Calls = nullptr;
			if (!Args->TryGetArrayField(TEXT("calls"), Calls) || !Calls)
			{
				return;
			}
			for (const TSharedPtr<FJsonValue>& Value : *Calls)
			{
				const TSharedPtr<FJsonObject>* Call = nullptr;
				if (!Value.IsValid() || !Value->TryGetObject(Call) || !Call)
				{
					continue;
				}
				FCursorSubCall Sub;
				Sub.ToolName = FName(*(*Call)->GetStringField(TEXT("tool")));
				const TSharedPtr<FJsonObject>* SubArgs = nullptr;
				if ((*Call)->TryGetObjectField(TEXT("args"), SubArgs) && SubArgs)
				{
					Sub.Args = *SubArgs;
				}
				OutCalls.Add(Sub);
			}
		}

		virtual void Execute(const TSharedRef<FJsonObject>& Args, FCursorToolContext& Ctx, FCursorToolCompletion OnComplete) override
		{
			TArray<FCursorSubCall> Calls;
			GetSubCalls(Args, Calls);
			if (Calls.Num() == 0)
			{
				OnComplete(FCursorToolResult::Error(TEXT("'calls' must be a non-empty array of {tool, args} objects.")));
				return;
			}
			if (Calls.Num() > MaxCalls)
			{
				OnComplete(FCursorToolResult::Error(FString::Printf(TEXT("A batch can hold at most %d calls."), MaxCalls)));
				return;
			}
			for (const FCursorSubCall& Call : Calls)
			{
				if (Call.ToolName == BatchName)
				{
					OnComplete(FCursorToolResult::Error(TEXT("A batch cannot contain another batch.")));
					return;
				}
			}

			TArray<FString> Aliases;
			FString AliasError;
			if (!ReadAliases(Args, Calls.Num(), Aliases, AliasError))
			{
				OnComplete(FCursorToolResult::Error(AliasError));
				return;
			}

			const TSharedRef<FRun> Run = MakeShared<FRun>();
			Run->Calls = MoveTemp(Calls);
			Run->Aliases = MoveTemp(Aliases);
			Run->Outputs.SetNum(Run->Calls.Num());
			Run->Finished.SetNumZeroed(Run->Calls.Num());
			Run->Ctx = Ctx;
			Run->Ctx.bPreApproved = true; // The gate already judged every call in the batch.
			Run->Ctx.bInBatch = true;
			Run->OnComplete = MoveTemp(OnComplete);
			bool bFlag = true;
			Run->bStopOnError = Args->TryGetBoolField(TEXT("stop_on_error"), bFlag) ? bFlag : true;
			Run->bRollbackOnError = Args->TryGetBoolField(TEXT("rollback_on_error"), bFlag) ? bFlag : true;
			Run->bBrief = Args->TryGetBoolField(TEXT("brief"), bFlag) && bFlag;

			if (GEditor && GEditor->CanTransact())
			{
				Run->bNestedInOuterTransaction = GEditor->IsTransactionActive();
				Run->UndoQueueBefore = GEditor->Trans ? GEditor->Trans->GetQueueLength() : 0;
				GEditor->BeginTransaction(TEXT("CursorUnreal"),
					FText::FromString(FString::Printf(TEXT("Cursor Unreal: batch of %d"), Run->Calls.Num())), nullptr);
				Run->bOpenedTransaction = true;
			}

			Run->Step();
		}

	private:
		static bool IsReferenceChar(TCHAR Ch)
		{
			return FChar::IsAlnum(Ch) || Ch == TEXT('_');
		}

		/** One name per call, empty where the call has none. Mirrors the order GetSubCalls reports. */
		static bool ReadAliases(const TSharedRef<FJsonObject>& Args, int32 CallCount, TArray<FString>& OutAliases, FString& OutError)
		{
			const TArray<TSharedPtr<FJsonValue>>* Calls = nullptr;
			Args->TryGetArrayField(TEXT("calls"), Calls);
			for (const TSharedPtr<FJsonValue>& Value : *Calls)
			{
				const TSharedPtr<FJsonObject>* Call = nullptr;
				if (!Value.IsValid() || !Value->TryGetObject(Call) || !Call)
				{
					continue;
				}
				FString Alias;
				(*Call)->TryGetStringField(TEXT("as"), Alias);
				if (!Alias.IsEmpty())
				{
					bool bValid = FChar::IsAlpha(Alias[0]) || Alias[0] == TEXT('_');
					for (const TCHAR Ch : Alias)
					{
						bValid = bValid && IsReferenceChar(Ch);
					}
					if (!bValid)
					{
						OutError = FString::Printf(TEXT("'as' must be letters, digits and underscores, starting with a letter; got '%s'."), *Alias);
						return false;
					}
					if (OutAliases.Contains(Alias))
					{
						OutError = FString::Printf(TEXT("Two calls in this batch are named '%s'. Each 'as' name must be used once."), *Alias);
						return false;
					}
				}
				OutAliases.Add(Alias);
			}
			OutAliases.SetNum(CallCount);
			return true;
		}

		struct FRun : public TSharedFromThis<FRun>
		{
			TArray<FCursorSubCall> Calls;
			TArray<FString> Aliases;
			/** What each finished call returned, for later calls to refer to. */
			TArray<FCursorToolResult> Outputs;
			TArray<bool> Finished;
			bool bBrief = false;
			FCursorToolContext Ctx;
			FCursorToolCompletion OnComplete;
			TArray<TSharedPtr<FJsonValue>> Results;
			/** Pictures the calls returned, handed on with the batch's result. */
			TArray<FCursorImage> Images;
			int32 Index = 0;
			int32 FailedIndex = INDEX_NONE;
			bool bStopOnError = true;
			bool bRollbackOnError = true;
			bool bOpenedTransaction = false;
			bool bNestedInOuterTransaction = false;
			int32 UndoQueueBefore = 0;

			// Synchronous tools complete inside Invoke. Loop instead of recursing so a long batch
			// of them does not build a deep stack.
			bool bInLoop = false;
			bool bCompletedInline = false;

			void Step()
			{
				bInLoop = true;
				while (Index < Calls.Num() && !(bStopOnError && FailedIndex != INDEX_NONE))
				{
					bCompletedInline = false;
					const int32 ThisIndex = Index;
					const FCursorSubCall& Call = Calls[ThisIndex];
					FCursorToolContext CallCtx = Ctx;
					CallCtx.CallId.Reset();

					TSharedPtr<FJsonObject> CallArgs = Call.Args;
					if (CallArgs.IsValid())
					{
						FString ReferenceError;
						CallArgs = ResolveObject(CallArgs.ToSharedRef(), ThisIndex, ReferenceError);
						if (!CallArgs.IsValid())
						{
							// The call never ran: there was nothing to pass it.
							Record(ThisIndex, FCursorToolResult::Error(ReferenceError));
							Index = ThisIndex + 1;
							continue;
						}
					}

					const TSharedRef<FRun> Self = AsShared();
					FCursorToolRegistry::Get().Invoke(Call.ToolName, CallArgs, CallCtx, [Self, ThisIndex](const FCursorToolResult& Result)
					{
						Self->Record(ThisIndex, Result);
						Self->Index = ThisIndex + 1;
						if (Self->bInLoop)
						{
							Self->bCompletedInline = true;
						}
						else
						{
							Self->Step(); // A deferred tool finished; carry on.
						}
					});

					if (!bCompletedInline)
					{
						bInLoop = false;
						return; // Waiting on a deferred tool.
					}
				}
				bInLoop = false;
				Finish();
			}

			/**
			 * Which call a reference names: its 'as' name, or its position counting from 0. INDEX_NONE when the text names
			 * nothing in this batch, in which case it is ordinary text (a price, a shell variable) and is left alone.
			 */
			int32 FindCall(const FString& Name) const
			{
				const int32 Named = Aliases.IndexOfByKey(Name);
				if (Named != INDEX_NONE)
				{
					return Named;
				}
				if (Name.IsNumeric() && !Name.Contains(TEXT(".")) && !Name.Contains(TEXT("-")))
				{
					const int32 Position = FCString::Atoi(*Name);
					return Position < Calls.Num() ? Position : INDEX_NONE;
				}
				return INDEX_NONE;
			}

			/** Follows "name.field.0.field" into an earlier result. Null with OutError set when it cannot be followed. */
			TSharedPtr<FJsonValue> Lookup(const FString& Reference, int32 ForIndex, FString& OutError) const
			{
				TArray<FString> Parts;
				Reference.ParseIntoArray(Parts, TEXT("."));
				const int32 Source = FindCall(Parts[0]);
				const FString Shown = TEXT("$") + Reference;
				if (Source >= ForIndex)
				{
					OutError = FString::Printf(TEXT("%s refers to call %d, which runs %s. A call can only use results of calls before it."),
						*Shown, Source, Source == ForIndex ? TEXT("now") : TEXT("later"));
					return nullptr;
				}
				if (!Finished[Source] || Outputs[Source].bIsError)
				{
					OutError = FString::Printf(TEXT("%s refers to call %d (%s), which failed, so it has no result to use."), *Shown, Source, *Calls[Source].ToolName.ToString());
					return nullptr;
				}

				const FCursorToolResult& Output = Outputs[Source];
				TSharedPtr<FJsonObject> Root = Output.Structured;
				if (!Root.IsValid() && Output.Text.TrimStart().StartsWith(TEXT("{")))
				{
					Root = CursorJson::Parse(Output.Text);
				}
				if (Parts.Num() == 1)
				{
					return Root.IsValid() ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueObject>(Root)) : TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Output.Text));
				}
				if (!Root.IsValid())
				{
					OutError = FString::Printf(TEXT("%s: %s returned plain text, not fields, so there is no '%s' to read. Its result was: %s"),
						*Shown, *Calls[Source].ToolName.ToString(), *Parts[1], *CursorJson::Truncate(Output.Text, 200));
					return nullptr;
				}

				TSharedPtr<FJsonValue> Current = MakeShared<FJsonValueObject>(Root);
				for (int32 i = 1; i < Parts.Num(); ++i)
				{
					const TSharedPtr<FJsonObject>* Object = nullptr;
					const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
					TSharedPtr<FJsonValue> Next;
					FString Available;
					if (Current->TryGetObject(Object) && Object)
					{
						Next = (*Object)->TryGetField(Parts[i]);
						TArray<FString> Keys;
						(*Object)->Values.GetKeys(Keys);
						Available = TEXT("Fields there: ") + FString::Join(Keys, TEXT(", "));
					}
					else if (Current->TryGetArray(Array) && Array)
					{
						if (Parts[i].IsNumeric() && Array->IsValidIndex(FCString::Atoi(*Parts[i])))
						{
							Next = (*Array)[FCString::Atoi(*Parts[i])];
						}
						Available = FString::Printf(TEXT("That is a list of %d; use a position from 0."), Array->Num());
					}
					else
					{
						Available = TEXT("That is a plain value with nothing inside it.");
					}
					if (!Next.IsValid() || Next->IsNull())
					{
						OutError = FString::Printf(TEXT("%s: the result of call %d (%s) has no '%s'. %s"), *Shown, Source, *Calls[Source].ToolName.ToString(), *Parts[i], *Available);
						return nullptr;
					}
					Current = Next;
				}
				return Current;
			}

			static FString ValueAsText(const TSharedPtr<FJsonValue>& Value)
			{
				FString Text;
				if (Value->TryGetString(Text))
				{
					return Text;
				}
				const TSharedPtr<FJsonObject>* Object = nullptr;
				if (Value->TryGetObject(Object) && Object)
				{
					return CursorJson::ToString(Object->ToSharedRef());
				}
				double Number = 0.0;
				if (Value->Type == EJson::Number && Value->TryGetNumber(Number))
				{
					return Number == FMath::RoundToDouble(Number) ? FString::Printf(TEXT("%lld"), static_cast<int64>(Number)) : FString::SanitizeFloat(Number);
				}
				bool bFlag = false;
				if (Value->TryGetBool(bFlag))
				{
					return bFlag ? TEXT("true") : TEXT("false");
				}
				return FString();
			}

			/** Length of the "name.field.field" run at the start of Text; 0 when it does not start with a name this batch knows. */
			int32 ReferenceLength(const TCHAR* Text) const
			{
				int32 Length = 0;
				while (IsReferenceChar(Text[Length]) || (Text[Length] == TEXT('.') && Length > 0 && IsReferenceChar(Text[Length + 1])))
				{
					++Length;
				}
				if (Length == 0)
				{
					return 0;
				}
				FString Name = FString::ConstructFromPtrSize(Text, Length);
				int32 Dot = INDEX_NONE;
				if (Name.FindChar(TEXT('.'), Dot))
				{
					Name.LeftInline(Dot);
				}
				return FindCall(Name) != INDEX_NONE ? Length : 0;
			}

			TSharedPtr<FJsonValue> ResolveString(const FString& Text, int32 ForIndex, FString& OutError) const
			{
				if (Text.StartsWith(TEXT("$$")))
				{
					return MakeShared<FJsonValueString>(Text.Mid(1));
				}
				// The whole value is a reference: hand over the value itself, with its type.
				if (Text.StartsWith(TEXT("$")) && Text.Len() > 1 && ReferenceLength(*Text + 1) == Text.Len() - 1)
				{
					return Lookup(Text.Mid(1), ForIndex, OutError);
				}
				// References inside longer text: ${name.field}.
				FString Out;
				int32 From = 0;
				bool bChanged = false;
				while (true)
				{
					const int32 Open = Text.Find(TEXT("${"), ESearchCase::CaseSensitive, ESearchDir::FromStart, From);
					if (Open == INDEX_NONE)
					{
						break;
					}
					const int32 Length = ReferenceLength(*Text + Open + 2);
					if (Length == 0 || Text[Open + 2 + Length] != TEXT('}'))
					{
						Out += Text.Mid(From, Open + 2 - From);
						From = Open + 2;
						continue;
					}
					const TSharedPtr<FJsonValue> Value = Lookup(Text.Mid(Open + 2, Length), ForIndex, OutError);
					if (!Value.IsValid())
					{
						return nullptr;
					}
					Out += Text.Mid(From, Open - From) + ValueAsText(Value);
					From = Open + 2 + Length + 1;
					bChanged = true;
				}
				if (!bChanged)
				{
					return MakeShared<FJsonValueString>(Text);
				}
				return MakeShared<FJsonValueString>(Out + Text.Mid(From));
			}

			TSharedPtr<FJsonValue> ResolveValue(const TSharedPtr<FJsonValue>& Value, int32 ForIndex, FString& OutError) const
			{
				if (!Value.IsValid())
				{
					return Value;
				}
				const TSharedPtr<FJsonObject>* Object = nullptr;
				const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
				if (Value->Type == EJson::String)
				{
					return ResolveString(Value->AsString(), ForIndex, OutError);
				}
				if (Value->Type == EJson::Object && Value->TryGetObject(Object) && Object && Object->IsValid())
				{
					const TSharedPtr<FJsonObject> Resolved = ResolveObject(Object->ToSharedRef(), ForIndex, OutError);
					return Resolved.IsValid() ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueObject>(Resolved)) : nullptr;
				}
				if (Value->Type == EJson::Array && Value->TryGetArray(Array) && Array)
				{
					TArray<TSharedPtr<FJsonValue>> Items;
					for (const TSharedPtr<FJsonValue>& Item : *Array)
					{
						const TSharedPtr<FJsonValue> Resolved = ResolveValue(Item, ForIndex, OutError);
						if (!Resolved.IsValid() && Item.IsValid())
						{
							return nullptr;
						}
						Items.Add(Resolved);
					}
					return MakeShared<FJsonValueArray>(Items);
				}
				return Value;
			}

			/** A copy of the arguments with every reference replaced. Null with OutError set when one cannot be. */
			TSharedPtr<FJsonObject> ResolveObject(const TSharedRef<FJsonObject>& Object, int32 ForIndex, FString& OutError) const
			{
				TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Object->Values)
				{
					const TSharedPtr<FJsonValue> Resolved = ResolveValue(Pair.Value, ForIndex, OutError);
					if (!Resolved.IsValid() && Pair.Value.IsValid())
					{
						return nullptr;
					}
					Out->SetField(Pair.Key, Resolved);
				}
				return Out;
			}

			void Record(int32 CallIndex, const FCursorToolResult& Result)
			{
				TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetStringField(TEXT("tool"), Calls[CallIndex].ToolName.ToString());
				if (!Aliases[CallIndex].IsEmpty())
				{
					Entry->SetStringField(TEXT("as"), Aliases[CallIndex]);
				}
				Entry->SetBoolField(TEXT("ok"), !Result.bIsError);
				if (Result.bIsError || !bBrief)
				{
					Entry->SetStringField(TEXT("result"), CursorJson::Truncate(Result.Text, 8000));
				}
				if (Result.Images.Num() > 0)
				{
					// Passed on with the batch's own result, in call order.
					Entry->SetNumberField(TEXT("images"), Result.Images.Num());
					Images.Append(Result.Images);
				}
				Results.Add(MakeShared<FJsonValueObject>(Entry));
				Outputs[CallIndex] = Result;
				Finished[CallIndex] = true;
				if (Result.bIsError && FailedIndex == INDEX_NONE)
				{
					FailedIndex = CallIndex;
				}
			}

			void Finish()
			{
				bool bRolledBack = false;
				FString Note;

				if (bOpenedTransaction && GEditor)
				{
					GEditor->EndTransaction();
					const bool bRecordedUndo = !bNestedInOuterTransaction && GEditor->Trans && GEditor->Trans->GetQueueLength() > UndoQueueBefore;
					if (FailedIndex != INDEX_NONE && bRollbackOnError)
					{
						if (bNestedInOuterTransaction)
						{
							Note = TEXT("Could not roll back: the batch ran inside another open transaction.");
						}
						else if (bRecordedUndo)
						{
							// bCanRedo = false: a rolled-back batch should not sit in the redo history.
							bRolledBack = GEditor->UndoTransaction(false);
							Note = bRolledBack
								? TEXT("Changes made before the failure were undone. Newly created asset files are not removed by undo.")
								: TEXT("Rollback was attempted but the editor refused the undo.");
						}
						else
						{
							Note = TEXT("Nothing undoable had been changed before the failure.");
						}
					}
				}

				TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
				Out->SetNumberField(TEXT("ran"), Results.Num());
				Out->SetNumberField(TEXT("total"), Calls.Num());
				if (FailedIndex != INDEX_NONE)
				{
					Out->SetNumberField(TEXT("failed_index"), FailedIndex);
				}
				Out->SetBoolField(TEXT("rolled_back"), bRolledBack);
				if (!Note.IsEmpty())
				{
					Out->SetStringField(TEXT("note"), Note);
				}
				Out->SetArrayField(TEXT("results"), Results);

				FCursorToolResult Result = FCursorToolResult::OkJson(Out);
				Result.bIsError = FailedIndex != INDEX_NONE;
				Result.Images = MoveTemp(Images);

				FCursorToolCompletion Callback = MoveTemp(OnComplete);
				Callback(Result);
			}
		};
	};
}

void CursorCoreTools::Register()
{
	FCursorToolRegistry::Get().Register(MakeShared<FBatchTool>());

	FCursorToolBuilder(TEXT("list_skills"), CorePack)
		.Describe(TEXT("List the skills available in this project: short guides on how to do Unreal work well here (Blueprints, C++ gameplay, "
			"materials, Niagara, animation, audio, editor tools, optimisation, project conventions). Load one with load_skill before starting work it covers."))
		.Risk(ECursorToolRisk::ReadOnly)
		.Run([](const FCursorArgs&, const FCursorToolContext&)
		{
			TArray<TSharedPtr<FJsonValue>> Items;
			for (const FCursorSkillInfo& Info : FCursorSkillLibrary::Get().ListEnabled())
			{
				TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetStringField(TEXT("name"), Info.Name);
				Item->SetStringField(TEXT("description"), Info.Description);
				Item->SetStringField(TEXT("source"), LexToString(Info.Layer));
				Items.Add(MakeShared<FJsonValueObject>(Item));
			}
			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetArrayField(TEXT("skills"), Items);
			return FCursorToolResult::OkJson(Out);
		})
		.Register();

	FCursorToolBuilder(TEXT("load_skill"), CorePack)
		.Describe(TEXT("Read a skill's full instructions. Pass 'file' to read one of the reference files the skill mentions instead."))
		.Risk(ECursorToolRisk::ReadOnly)
		.Schema(FCursorSchema()
			.Str(TEXT("name"), TEXT("Skill name, as returned by list_skills."), true)
			.Str(TEXT("file"), TEXT("Optional reference file inside the skill folder, for example 'reference.md'.")))
		.Run([](const FCursorArgs& Args, const FCursorToolContext&)
		{
			const FCursorSkillLibrary& Library = FCursorSkillLibrary::Get();
			const FString Name = Args.Str(TEXT("name"));
			const FCursorSkillInfo* Info = Library.Find(Name);
			if (!Info || !Info->bEnabled)
			{
				TArray<FString> Names;
				for (const FCursorSkillInfo& Each : Library.ListEnabled())
				{
					Names.Add(Each.Name);
				}
				return FCursorToolResult::Error(FString::Printf(TEXT("No skill named '%s'. Available: %s"), *Name, *FString::Join(Names, TEXT(", "))));
			}

			FString Text;
			FString Error;
			const FString File = Args.Str(TEXT("file"));
			if (!File.IsEmpty())
			{
				return Library.ReadFile(Name, File, Text, Error) ? FCursorToolResult::Ok(Text) : FCursorToolResult::Error(Error);
			}
			if (!Library.Load(Name, Text, Error))
			{
				return FCursorToolResult::Error(Error);
			}
			const TArray<FString> Files = Library.ListFiles(Name);
			if (Files.Num() > 0)
			{
				Text += FString::Printf(TEXT("\n\n---\nReference files in this skill (read with load_skill and 'file'): %s"), *FString::Join(Files, TEXT(", ")));
			}
			return FCursorToolResult::Ok(Text);
		})
		.Register();
}

void CursorCoreTools::Unregister()
{
	FCursorToolRegistry::Get().UnregisterPack(CorePack);
}
