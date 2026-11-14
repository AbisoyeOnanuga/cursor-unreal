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
		static constexpr int32 MaxCalls = 50;

		virtual FName GetName() const override { return BatchName; }
		virtual FName GetPack() const override { return CorePack; }
		virtual ECursorToolRisk GetRisk() const override { return ECursorToolRisk::Mutating; }
		virtual bool ManagesOwnTransaction() const override { return true; }

		virtual FText GetDescription() const override
		{
			return FText::FromString(TEXT(
				"Run several tool calls in order, in one request and one undo step. Use it whenever you already know the next few calls "
				"(for example: add three nodes, connect them, compile). Each call's result is returned. By default the batch stops at the first "
				"failure and undoes what the earlier calls changed; note that undo does not delete newly created asset files. "
				"Calls cannot reference each other's results, so use it for steps whose arguments you already know."));
		}

		virtual TSharedRef<FJsonObject> GetInputSchema() const override
		{
			TSharedRef<FJsonObject> CallSchema = FCursorSchema()
				.Str(TEXT("tool"), TEXT("Name of the tool to call."), true)
				.Obj(TEXT("args"), TEXT("Arguments for that tool."))
				.Build();

			TSharedRef<FJsonObject> Calls = MakeShared<FJsonObject>();
			Calls->SetStringField(TEXT("type"), TEXT("array"));
			Calls->SetStringField(TEXT("description"), TEXT("Calls to run, in order."));
			Calls->SetObjectField(TEXT("items"), CallSchema);
			Calls->SetNumberField(TEXT("maxItems"), MaxCalls);

			TSharedRef<FJsonObject> Schema = FCursorSchema()
				.Bool(TEXT("stop_on_error"), TEXT("Stop at the first failing call. Default true."))
				.Bool(TEXT("rollback_on_error"), TEXT("If a call fails, undo the changes made by the calls before it. Default true."))
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

			const TSharedRef<FRun> Run = MakeShared<FRun>();
			Run->Calls = MoveTemp(Calls);
			Run->Ctx = Ctx;
			Run->Ctx.bPreApproved = true; // The gate already judged every call in the batch.
			Run->Ctx.bInBatch = true;
			Run->OnComplete = MoveTemp(OnComplete);
			bool bFlag = true;
			Run->bStopOnError = Args->TryGetBoolField(TEXT("stop_on_error"), bFlag) ? bFlag : true;
			Run->bRollbackOnError = Args->TryGetBoolField(TEXT("rollback_on_error"), bFlag) ? bFlag : true;

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
		struct FRun : public TSharedFromThis<FRun>
		{
			TArray<FCursorSubCall> Calls;
			FCursorToolContext Ctx;
			FCursorToolCompletion OnComplete;
			TArray<TSharedPtr<FJsonValue>> Results;
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

					const TSharedRef<FRun> Self = AsShared();
					FCursorToolRegistry::Get().Invoke(Call.ToolName, Call.Args, CallCtx, [Self, ThisIndex](const FCursorToolResult& Result)
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

			void Record(int32 CallIndex, const FCursorToolResult& Result)
			{
				TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetStringField(TEXT("tool"), Calls[CallIndex].ToolName.ToString());
				Entry->SetBoolField(TEXT("ok"), !Result.bIsError);
				Entry->SetStringField(TEXT("result"), CursorJson::Truncate(Result.Text, 8000));
				Results.Add(MakeShared<FJsonValueObject>(Entry));
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
