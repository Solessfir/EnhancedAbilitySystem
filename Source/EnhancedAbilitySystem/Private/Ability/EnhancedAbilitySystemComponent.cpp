// Copyright Solessfir. All Rights Reserved.

#include "Ability/EnhancedAbilitySystemComponent.h"
#include "Ability/EnhancedGameplayAbility.h"
#include "EnhancedInputComponent.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Logging/StructuredLog.h"
#include "TimerManager.h"
#include UE_INLINE_GENERATED_CPP_BY_NAME(EnhancedAbilitySystemComponent)

DEFINE_LOG_CATEGORY_STATIC(LogEnhancedAbilitySystemComponent, Log, All);

// ----------------------------------------------------------------------------------------------------------------
// Ability Grant / Lifecycle
// ----------------------------------------------------------------------------------------------------------------

void UEnhancedAbilitySystemComponent::InitAbilityActorInfo(AActor* InOwnerActor, AActor* InAvatarActor)
{
	const AActor* PreviousAvatar = GetAvatarActor();
	Super::InitAbilityActorInfo(InOwnerActor, InAvatarActor);

	// A reentrant initialization can already have handled the control transition.
	const bool bWasLocallyControlled = bActorInfoLocallyControlled;
	bActorInfoLocallyControlled = AbilityActorInfo.IsValid() && AbilityActorInfo->OwnerActor.IsValid() && AbilityActorInfo->IsLocallyControlled();
	const APlayerController* Controller = AbilityActorInfo.IsValid() ? AbilityActorInfo->PlayerController.Get() : nullptr;
	if (PreviousAvatar != InAvatarActor || InAvatarActor != GetAvatarActor() || bWasLocallyControlled
		|| !bActorInfoLocallyControlled || !Controller || !Controller->IsLocalController())
	{
		return;
	}

	// An unchanged avatar does not receive OnAvatarSet when its owning controller becomes available.
	ABILITYLIST_SCOPE_LOCK();
	for (const FGameplayAbilitySpec& Spec : ActivatableAbilities.Items)
	{
		const UEnhancedGameplayAbility* Ability = Cast<UEnhancedGameplayAbility>(Spec.GetPrimaryInstance());
		if (!Ability)
		{
			Ability = Cast<UEnhancedGameplayAbility>(Spec.Ability);
		}
		if (Ability && Ability->bActivateOnGranted)
		{
			const EGameplayAbilityNetExecutionPolicy::Type NetPolicy = Ability->GetNetExecutionPolicy();
			if (NetPolicy == EGameplayAbilityNetExecutionPolicy::LocalOnly || NetPolicy == EGameplayAbilityNetExecutionPolicy::LocalPredicted)
			{
				TryActivateAbilityOnGranted(Spec);
			}
		}
	}
}

void UEnhancedAbilitySystemComponent::OnGiveAbility(FGameplayAbilitySpec& AbilitySpec)
{
	PruneAbilityGrantBatchProgress();
	for (const FAbilityListLockActiveChange* Batch : AbilityListLockActiveChanges)
	{
		const int32 GrantIndex = Batch->Adds.IndexOfByPredicate([&AbilitySpec](const FGameplayAbilitySpec& Spec) { return Spec.Handle == AbilitySpec.Handle; });
		if (GrantIndex != INDEX_NONE)
		{
			// Mark the source copy processed before grant callbacks can remove the committed spec.
			AbilityGrantBatchNextIndices.FindOrAdd(Batch->Adds[0].Handle) = GrantIndex + 1;
		}
	}

	Super::OnGiveAbility(AbilitySpec);

	if (AbilitySpec.PendingRemove || bAbilityPendingClearAll)
	{
		return;
	}

	// Grant callbacks can already have installed a binding on the current avatar.
	if (const FAbilityInputBinding* Binding = AbilityInputBindings.Find(AbilitySpec.Handle);
		Binding && IsInputBindingCurrent(AbilitySpec.Handle, Binding->Generation))
	{
		return;
	}

	const UEnhancedGameplayAbility* EnhancedAbility = Cast<UEnhancedGameplayAbility>(AbilitySpec.GetPrimaryInstance());
	if (!EnhancedAbility)
	{
		EnhancedAbility = Cast<UEnhancedGameplayAbility>(AbilitySpec.Ability);
	}
	if (!EnhancedAbility)
	{
		return;
	}

	SetInputBinding(AbilitySpec.Handle, EnhancedAbility->InputAction, EnhancedAbility->ConfirmInputAction, EnhancedAbility->CancelInputAction);
}

void UEnhancedAbilitySystemComponent::OnRemoveAbility(FGameplayAbilitySpec& AbilitySpec)
{
	// Immediate removals must also be excluded from callback-time ownership queries.
	AbilitySpec.PendingRemove = true;
	Super::OnRemoveAbility(AbilitySpec);
	ClearInputBinding(AbilitySpec.Handle);
	AbilityInputEventGenerations.Remove(AbilitySpec.Handle);
}

void UEnhancedAbilitySystemComponent::TryActivateAbilityOnGranted(const FGameplayAbilitySpec& AbilitySpec)
{
	if (!AbilitySpec.Ability || AbilitySpec.IsActive() || AbilitySpec.PendingRemove || bAbilityPendingClearAll)
	{
		return;
	}

	const FGameplayAbilityActorInfo* ActorInfo = AbilityActorInfo.Get();
	if (!ActorInfo || !ActorInfo->AvatarActor.IsValid())
	{
		return;
	}

	const AActor* Avatar = ActorInfo->AvatarActor.Get();
	if (!Avatar || Avatar->GetTearOff() || Avatar->GetLifeSpan() > 0.f)
	{
		return;
	}

	const UGameplayAbility* AbilityCDO = AbilitySpec.Ability;
	const EGameplayAbilityNetExecutionPolicy::Type NetPolicy = AbilityCDO->GetNetExecutionPolicy();

	const bool bClientShouldActivate = ActorInfo->IsLocallyControlled()
		&& (NetPolicy == EGameplayAbilityNetExecutionPolicy::LocalPredicted
			|| NetPolicy == EGameplayAbilityNetExecutionPolicy::LocalOnly);

	const bool bServerShouldActivate = ActorInfo->IsNetAuthority()
		&& (NetPolicy == EGameplayAbilityNetExecutionPolicy::ServerOnly
			|| NetPolicy == EGameplayAbilityNetExecutionPolicy::ServerInitiated);

	if (bClientShouldActivate || bServerShouldActivate)
	{
		TryActivateAbility(AbilitySpec.Handle);
	}
}

FGameplayAbilitySpecHandle UEnhancedAbilitySystemComponent::GiveAbilityIfNotOwned(const TSubclassOf<UGameplayAbility> Ability, const int32 Level)
{
	if (!Ability || !IsOwnerActorAuthoritative() || bAbilityPendingClearAll)
	{
		return FGameplayAbilitySpecHandle();
	}

	PruneAbilityGrantBatchProgress();

	for (const FGameplayAbilitySpec& Spec : ActivatableAbilities.Items)
	{
		if (!Spec.PendingRemove && Spec.Ability && Spec.Ability->GetClass() == Ability.Get())
		{
			return Spec.Handle;
		}
	}

	// GiveAbility defers grants made inside ability callbacks until the list lock is released.
	for (const FGameplayAbilitySpec& Spec : AbilityPendingAdds)
	{
		if (!Spec.PendingRemove && Spec.Ability && Spec.Ability->GetClass() == Ability.Get())
		{
			return Spec.Handle;
		}
	}

	// Unlock moves queued grants into batches that retain copies of already processed specs.
	for (const FAbilityListLockActiveChange* Batch : AbilityListLockActiveChanges)
	{
		if (Batch->Adds.IsEmpty())
		{
			continue;
		}

		for (int32 Index = AbilityGrantBatchNextIndices.FindRef(Batch->Adds[0].Handle); Index < Batch->Adds.Num(); ++Index)
		{
			const FGameplayAbilitySpec& Spec = Batch->Adds[Index];
			if (Spec.PendingRemove || !IsValid(Spec.Ability) || Spec.Ability->GetClass() != Ability.Get())
			{
				continue;
			}

			return Spec.Handle;
		}
	}

	return GiveAbility(FGameplayAbilitySpec(Ability, Level));
}

void UEnhancedAbilitySystemComponent::PruneAbilityGrantBatchProgress()
{
	for (auto Iterator = AbilityGrantBatchNextIndices.CreateIterator(); Iterator; ++Iterator)
	{
		const FGameplayAbilitySpecHandle BatchHandle = Iterator.Key();
		if (!AbilityListLockActiveChanges.ContainsByPredicate([BatchHandle](const FAbilityListLockActiveChange* Batch) { return !Batch->Adds.IsEmpty() && Batch->Adds[0].Handle == BatchHandle; }))
		{
			Iterator.RemoveCurrent();
		}
	}
}

bool UEnhancedAbilitySystemComponent::FindAbilitySpecByAssetTag(const FGameplayTag& AbilityTag, const FGameplayAbilitySpec*& OutSpec) const
{
	OutSpec = nullptr;

	if (!AbilityTag.IsValid())
	{
		return false;
	}

	for (const FGameplayAbilitySpec& Spec : ActivatableAbilities.Items)
	{
		if (!Spec.PendingRemove && Spec.Ability && Spec.Ability->GetAssetTags().HasTagExact(AbilityTag))
		{
			OutSpec = &Spec;
			return true;
		}
	}

	return false;
}

UGameplayAbility* UEnhancedAbilitySystemComponent::GetGameplayAbilityByTag(const FGameplayTag& Tag) const
{
	const FGameplayAbilitySpec* Spec = nullptr;
	if (!FindAbilitySpecByAssetTag(Tag, Spec) || !Spec)
	{
		return nullptr;
	}

	// Spec.Ability is the CDO for InstancedPerActor abilities.
	// IsActive()/channel state only live on the per-actor instance, so prefer that when one has been spawned.
	if (UGameplayAbility* Instance = Spec->GetPrimaryInstance())
	{
		return Instance;
	}

	return Spec->Ability;
}

FGameplayAbilitySpecHandle UEnhancedAbilitySystemComponent::GetGameplayAbilitySpecHandleByTag(const FGameplayTag& AbilityTag) const
{
	const FGameplayAbilitySpec* Spec = nullptr;
	if (FindAbilitySpecByAssetTag(AbilityTag, Spec) && Spec)
	{
		return Spec->Handle;
	}

	return FGameplayAbilitySpecHandle();
}

bool UEnhancedAbilitySystemComponent::IsAbilityActiveByClass(TSubclassOf<UGameplayAbility> AbilityClass) const
{
	if (!AbilityClass)
	{
		return false;
	}

	for (const FGameplayAbilitySpec& Spec : ActivatableAbilities.Items)
	{
		if (!Spec.PendingRemove && Spec.Ability && Spec.Ability->GetClass() == AbilityClass.Get() && Spec.IsActive())
		{
			return true;
		}
	}

	return false;
}

bool UEnhancedAbilitySystemComponent::IsAbilityActiveByHandle(const FGameplayAbilitySpecHandle& Handle) const
{
	if (!Handle.IsValid())
	{
		return false;
	}

	if (const FGameplayAbilitySpec* Spec = FindAbilitySpecFromHandle(Handle))
	{
		return !Spec->PendingRemove && Spec->IsActive();
	}

	return false;
}

bool UEnhancedAbilitySystemComponent::IsAbilityActiveByTag(const FGameplayTag& GameplayTag) const
{
	if (!GameplayTag.IsValid())
	{
		return false;
	}

	for (const FGameplayAbilitySpec& Spec : ActivatableAbilities.Items)
	{
		if (!Spec.PendingRemove && Spec.Ability && Spec.Ability->GetAssetTags().HasTagExact(GameplayTag) && Spec.IsActive())
		{
			return true;
		}
	}

	return false;
}

void UEnhancedAbilitySystemComponent::K2_CancelAllAbilities(UGameplayAbility* Ignore)
{
	CancelAllAbilities(Ignore);
}

void UEnhancedAbilitySystemComponent::CancelAbilitiesWithTags(const FGameplayTagContainer& WithTags, const FGameplayTagContainer& WithoutTags, UGameplayAbility* Ignore)
{
	CancelAbilities(&WithTags, &WithoutTags, Ignore);
}

// ----------------------------------------------------------------------------------------------------------------
// Enhanced Input
// ----------------------------------------------------------------------------------------------------------------

void UEnhancedAbilitySystemComponent::Input_AbilityPressed(FGameplayAbilitySpecHandle Handle, uint64 Generation)
{
	if (IsInputBindingCurrent(Handle, Generation))
	{
		HandleAbilityInputPressed(Handle);
	}
}

void UEnhancedAbilitySystemComponent::Input_AbilityReleased(FGameplayAbilitySpecHandle Handle, uint64 Generation)
{
	if (IsInputBindingCurrent(Handle, Generation))
	{
		HandleAbilityInputReleased(Handle);
	}
}

void UEnhancedAbilitySystemComponent::Input_AbilityInputConfirmed(const FInputActionInstance& ActionInstance, FGameplayAbilitySpecHandle Handle, uint64 Generation)
{
	if (ShouldDispatchGenericInput(ActionInstance, Handle, Generation, true))
	{
		HandleAbilityInputConfirmed(Handle);
	}
}

void UEnhancedAbilitySystemComponent::Input_AbilityInputCanceled(const FInputActionInstance& ActionInstance, FGameplayAbilitySpecHandle Handle, uint64 Generation)
{
	if (ShouldDispatchGenericInput(ActionInstance, Handle, Generation, false))
	{
		HandleAbilityInputCanceled(Handle);
	}
}

bool UEnhancedAbilitySystemComponent::IsInputBindingCurrent(FGameplayAbilitySpecHandle Handle, uint64 Generation) const
{
	const FAbilityInputBinding* Binding = AbilityInputBindings.Find(Handle);
	return !bAbilityPendingClearAll && Binding && Binding->Generation == Generation && Binding->InputComponent.IsValid()
		&& Binding->InputComponent.Get() == GetEnhancedInputComponent();
}

bool UEnhancedAbilitySystemComponent::ShouldDispatchGenericInput(const FInputActionInstance& ActionInstance, FGameplayAbilitySpecHandle Handle, uint64 Generation, bool bConfirm)
{
	if (!IsInputBindingCurrent(Handle, Generation) || !IsAbilityActiveByHandle(Handle))
	{
		return false;
	}

	// Shared actions queue one callback per spec, but GAS confirm/cancel is ASC-wide.
	if (GenericInputFrame != GFrameCounter)
	{
		GenericInputFrame = GFrameCounter;
		ConfirmedInputActions.Reset();
		CanceledInputActions.Reset();
	}

	TSet<TWeakObjectPtr<const UInputAction>>& DispatchedActions = bConfirm ? ConfirmedInputActions : CanceledInputActions;
	const TWeakObjectPtr<const UInputAction> Action = ActionInstance.GetSourceAction();
	if (DispatchedActions.Contains(Action))
	{
		return false;
	}
	DispatchedActions.Add(Action);
	return true;
}

void UEnhancedAbilitySystemComponent::HandleAbilityInputPressed(FGameplayAbilitySpecHandle Handle)
{
	ABILITYLIST_SCOPE_LOCK();
	FGameplayAbilitySpec* Spec = FindAbilitySpecFromHandle(Handle);
	if (!Spec || !Spec->Ability || Spec->PendingRemove || bAbilityPendingClearAll)
	{
		return;
	}

	// Triggered can fire every frame while held.
	// Only process its first frame so instant abilities do not reactivate continuously and failed activations are not retried until the action is released.
	if (Spec->InputPressed)
	{
		return;
	}

	Spec->InputPressed = true;

	if (!Spec->IsActive())
	{
		// Scoped batcher groups activation RPCs for better network efficiency.
		FScopedServerAbilityRPCBatcher BatchWindow(this, Handle);
		TryActivateAbility(Handle);
	}
	else
	{
		// Fresh press on a running ability (multi-stage / Wait Input Press).
		if (Spec->Ability->bReplicateInputDirectly && !IsOwnerActorAuthoritative())
		{
			ServerSetInputPressed(Spec->Handle);
		}

		DispatchAbilityInputEvent(*Spec, EAbilityGenericReplicatedEvent::InputPressed);
	}
}

void UEnhancedAbilitySystemComponent::HandleAbilityInputReleased(FGameplayAbilitySpecHandle Handle)
{
	ABILITYLIST_SCOPE_LOCK();
	FGameplayAbilitySpec* Spec = FindAbilitySpecFromHandle(Handle);
	if (!Spec || !Spec->Ability || Spec->PendingRemove || bAbilityPendingClearAll)
	{
		return;
	}

	Spec->InputPressed = false;

	if (!Spec->IsActive())
	{
		return;
	}

	if (Spec->Ability->bReplicateInputDirectly && !IsOwnerActorAuthoritative())
	{
		ServerSetInputReleased(Spec->Handle);
	}

	DispatchAbilityInputEvent(*Spec, EAbilityGenericReplicatedEvent::InputReleased);
}

void UEnhancedAbilitySystemComponent::HandleAbilityInputConfirmed(FGameplayAbilitySpecHandle Handle)
{
	const FGameplayAbilitySpec* Spec = FindAbilitySpecFromHandle(Handle);
	if (!Spec || !Spec->Ability || Spec->PendingRemove || bAbilityPendingClearAll || !Spec->IsActive())
	{
		return;
	}

	// GAS generic confirm is ASC-wide.
	// Restrict each per-ability action to its active owning spec; the waiting ability task/target actor handles any required replicated event itself.
	LocalInputConfirm();
}

void UEnhancedAbilitySystemComponent::HandleAbilityInputCanceled(FGameplayAbilitySpecHandle Handle)
{
	const FGameplayAbilitySpec* Spec = FindAbilitySpecFromHandle(Handle);
	if (!Spec || !Spec->Ability || Spec->PendingRemove || bAbilityPendingClearAll || !Spec->IsActive())
	{
		return;
	}

	// GAS generic cancel is ASC-wide; replication is owned by the waiting task/target actor.
	LocalInputCancel();
}

void UEnhancedAbilitySystemComponent::DispatchAbilityInputEvent(FGameplayAbilitySpec& Spec, EAbilityGenericReplicatedEvent::Type EventType)
{
	if (Spec.PendingRemove || bAbilityPendingClearAll)
	{
		return;
	}

	const FGameplayAbilitySpecHandle Handle = Spec.Handle;
	const uint64 Generation = ++AbilityInputEventGenerations.FindOrAdd(Handle);
	const bool bInputPressed = EventType == EAbilityGenericReplicatedEvent::InputPressed;
	const TArray<FPredictionKey> PredictionKeys = GetPredictionKeysFromSpec(Spec);
	struct FInputInstance
	{
		TWeakObjectPtr<UGameplayAbility> Instance;
		FPredictionKey PredictionKey;
	};
	TArray<FInputInstance> OriginalInstances;
	for (UGameplayAbility* Instance : Spec.GetAbilityInstances())
	{
		if (Instance && Instance->IsActive())
		{
			OriginalInstances.Add({ Instance, Instance->GetCurrentActivationInfo().GetActivationPredictionKey() });
		}
	}

	// A per-actor instance can restart with the same prediction key during an input callback.
	TArray<TWeakObjectPtr<UGameplayAbility>> EndedInstances;
	bool bNonInstancedEnded = false;
	const FDelegateHandle EndedCallback = OnAbilityEnded.AddLambda([&](const FAbilityEndedData& EndedData)
	{
		if (EndedData.AbilitySpecHandle == Handle)
		{
			bNonInstancedEnded = OriginalInstances.IsEmpty();
			EndedInstances.AddUnique(EndedData.AbilityThatEnded);
		}
	});

	if (EventType == EAbilityGenericReplicatedEvent::InputPressed)
	{
		AbilitySpecInputPressed(Spec);
	}
	else
	{
		AbilitySpecInputReleased(Spec);
	}

	for (const FPredictionKey& PredictionKey : PredictionKeys)
	{
		// Input callbacks can synchronously release, remap, or press again while the activation stays alive.
		if (bAbilityPendingClearAll || AbilityInputEventGenerations.FindRef(Handle) != Generation || Spec.InputPressed != bInputPressed)
		{
			break;
		}

		if (Spec.PendingRemove || !Spec.IsActive())
		{
			continue;
		}

		bool bOriginalActivationActive = OriginalInstances.IsEmpty() && !bNonInstancedEnded && GetPredictionKeysFromSpec(Spec).Contains(PredictionKey);
		for (const FInputInstance& OriginalInstance : OriginalInstances)
		{
			const UGameplayAbility* Instance = OriginalInstance.Instance.Get();
			if (OriginalInstance.PredictionKey == PredictionKey && Instance && !EndedInstances.Contains(OriginalInstance.Instance) && Instance->IsActive() && Instance->GetCurrentActivationInfo().GetActivationPredictionKey() == PredictionKey)
			{
				bOriginalActivationActive = true;
				break;
			}
		}
		if (bOriginalActivationActive)
		{
			InvokeReplicatedEvent(EventType, Handle, PredictionKey);
		}
	}
	OnAbilityEnded.Remove(EndedCallback);
}

TArray<FPredictionKey> UEnhancedAbilitySystemComponent::GetPredictionKeysFromSpec(const FGameplayAbilitySpec& Spec)
{
	TArray<FPredictionKey> PredictionKeys;
	for (const UGameplayAbility* Instance : Spec.GetAbilityInstances())
	{
		if (Instance && Instance->IsActive())
		{
			PredictionKeys.AddUnique(Instance->GetCurrentActivationInfo().GetActivationPredictionKey());
		}
	}

	PRAGMA_DISABLE_DEPRECATION_WARNINGS
	if (Spec.Ability && Spec.Ability->GetInstancingPolicy() == EGameplayAbilityInstancingPolicy::NonInstanced)
	{
		PredictionKeys.Add(Spec.ActivationInfo.GetActivationPredictionKey());
	}
	PRAGMA_ENABLE_DEPRECATION_WARNINGS
	return PredictionKeys;
}

void UEnhancedAbilitySystemComponent::SetInputBinding(const FGameplayAbilitySpecHandle& SpecHandle, const UInputAction* InputAction, const UInputAction* ConfirmInputAction, const UInputAction* CancelInputAction)
{
	ABILITYLIST_SCOPE_LOCK();

	if (!SpecHandle.IsValid() || bAbilityPendingClearAll)
	{
		return;
	}

	// No actions = clear (explicit unbind). Callers that want a dedicated API can use ClearInputBinding.
	if (!(InputAction || ConfirmInputAction || CancelInputAction))
	{
		ClearInputBinding(SpecHandle);
		return;
	}

	UEnhancedInputComponent* InputComponent = GetEnhancedInputComponent();
	if (!InputComponent)
	{
		return;
	}

	ClearInputBinding(SpecHandle);

	// Releasing held input can remove the ability, replace its bindings, or change the avatar.
	const FGameplayAbilitySpec* Spec = FindAbilitySpecFromHandle(SpecHandle);
	InputComponent = GetEnhancedInputComponent();
	if (!Spec || Spec->PendingRemove || !InputComponent || AbilityInputBindings.Contains(SpecHandle))
	{
		return;
	}

	FAbilityInputBinding& Binding = AbilityInputBindings.FindOrAdd(SpecHandle);
	Binding.InputComponent = InputComponent;
	Binding.Generation = ++InputBindingGeneration;
	const uint64 Generation = Binding.Generation;
	TArray<uint32>& Handles = Binding.Handles;

	if (InputAction)
	{
		// Use Triggered rather than Started so the Input Action's trigger pipeline (Hold, Tap, Chord, Combo, etc.) must succeed first.
		// Enhanced Input applies modifiers before this callback.
		// Triggered may repeat while held, so HandleAbilityInputPressed edge-detects the first frame.
		Handles.Add(InputComponent->BindAction(InputAction, ETriggerEvent::Triggered, this, &ThisClass::Input_AbilityPressed, SpecHandle, Generation).GetHandle());

		// Completed: release after a successful trigger.
		Handles.Add(InputComponent->BindAction(InputAction, ETriggerEvent::Completed, this, &ThisClass::Input_AbilityReleased, SpecHandle, Generation).GetHandle());

		// Canceled: release if the trigger didn't finish (e.g. let go during a hold trigger).
		Handles.Add(InputComponent->BindAction(InputAction, ETriggerEvent::Canceled, this, &ThisClass::Input_AbilityReleased, SpecHandle, Generation).GetHandle());
	}

	if (ConfirmInputAction)
	{
		Handles.Add(InputComponent->BindAction(ConfirmInputAction, ETriggerEvent::Started, this, &ThisClass::Input_AbilityInputConfirmed, SpecHandle, Generation).GetHandle());
	}

	if (CancelInputAction)
	{
		Handles.Add(InputComponent->BindAction(CancelInputAction, ETriggerEvent::Started, this, &ThisClass::Input_AbilityInputCanceled, SpecHandle, Generation).GetHandle());
	}
}

void UEnhancedAbilitySystemComponent::BindAbilityInputs()
{
	ABILITYLIST_SCOPE_LOCK();

	for (const FGameplayAbilitySpec& Spec : ActivatableAbilities.Items)
	{
		if (Spec.PendingRemove)
		{
			continue;
		}

		const UEnhancedGameplayAbility* EnhancedAbility = Cast<UEnhancedGameplayAbility>(Spec.GetPrimaryInstance());
		if (!EnhancedAbility)
		{
			EnhancedAbility = Cast<UEnhancedGameplayAbility>(Spec.Ability);
		}

		if (EnhancedAbility)
		{
			SetInputBinding(Spec.Handle, EnhancedAbility->InputAction, EnhancedAbility->ConfirmInputAction, EnhancedAbility->CancelInputAction);
		}
	}
}

void UEnhancedAbilitySystemComponent::ClearInputBinding(const FGameplayAbilitySpecHandle& SpecHandle)
{
	ABILITYLIST_SCOPE_LOCK();

	FAbilityInputBinding Binding;
	if (!AbilityInputBindings.RemoveAndCopyValue(SpecHandle, Binding))
	{
		return;
	}

	if (UEnhancedInputComponent* InputComponent = Binding.InputComponent.Get())
	{
		for (const uint32 Handle : Binding.Handles)
		{
			InputComponent->RemoveBindingByHandle(Handle);
		}
	}

	if (FGameplayAbilitySpec* Spec = FindAbilitySpecFromHandle(SpecHandle); Spec && Spec->InputPressed)
	{
		if (Spec->PendingRemove || bAbilityPendingClearAll)
		{
			Spec->InputPressed = false;
		}
		else
		{
			HandleAbilityInputReleased(SpecHandle);
		}
	}
}

void UEnhancedAbilitySystemComponent::ClearAbilityInputBinding(const FGameplayTag& AbilityTag)
{
	const FGameplayAbilitySpec* Spec = nullptr;
	if (FindAbilitySpecByAssetTag(AbilityTag, Spec) && Spec)
	{
		ClearInputBinding(Spec->Handle);
	}
}

void UEnhancedAbilitySystemComponent::RemapAbilityInputAction(const FGameplayTag& AbilityTag, const UInputAction* InputAction)
{
	const FGameplayAbilitySpec* SpecConst = nullptr;
	if (!FindAbilitySpecByAssetTag(AbilityTag, SpecConst) || !SpecConst)
	{
		return;
	}

	// Guard against mutating the CDO.
	UEnhancedGameplayAbility* EnhancedAbility = Cast<UEnhancedGameplayAbility>(SpecConst->GetPrimaryInstance());
	if (!EnhancedAbility)
	{
		UE_LOGFMT(LogEnhancedAbilitySystemComponent, Warning, "RemapAbilityInputAction: Ability with tag '{0}' has no primary instance - cannot safely remap input.", AbilityTag.ToString());
		return;
	}

	EnhancedAbility->InputAction = InputAction;
	SetInputBinding(SpecConst->Handle, InputAction, EnhancedAbility->ConfirmInputAction, EnhancedAbility->CancelInputAction);
}

UEnhancedInputComponent* UEnhancedAbilitySystemComponent::GetEnhancedInputComponent() const
{
	if (const APawn* AvatarPawn = Cast<APawn>(GetAvatarActor()))
	{
		if (AvatarPawn->IsLocallyControlled())
		{
			return Cast<UEnhancedInputComponent>(AvatarPawn->InputComponent);
		}
	}
	return nullptr;
}

// ----------------------------------------------------------------------------------------------------------------
// Cooldowns / active GameplayEffects
// ----------------------------------------------------------------------------------------------------------------

bool UEnhancedAbilitySystemComponent::GetCooldownRemainingForTag(const FGameplayTagContainer& CooldownTags, float& TimeRemaining, float& CooldownDuration) const
{
	TimeRemaining = 0.f;
	CooldownDuration = 0.f;

	if (CooldownTags.Num() == 0)
	{
		return false;
	}

	// Same query shape as UGameplayAbility::GetCooldownTimeRemaining.
	const FGameplayEffectQuery Query = FGameplayEffectQuery::MakeQuery_MatchAnyOwningTags(CooldownTags);
	const TArray<TPair<float, float>> DurationAndTimeRemaining = GetActiveEffectsTimeRemainingAndDuration(Query);
	if (DurationAndTimeRemaining.Num() == 0)
	{
		return false;
	}

	int32 BestIndex = 0;
	float LongestTime = DurationAndTimeRemaining[0].Key;
	for (int32 Index = 1; Index < DurationAndTimeRemaining.Num(); ++Index)
	{
		if (DurationAndTimeRemaining[Index].Key > LongestTime)
		{
			LongestTime = DurationAndTimeRemaining[Index].Key;
			BestIndex = Index;
		}
	}

	TimeRemaining = DurationAndTimeRemaining[BestIndex].Key;
	CooldownDuration = DurationAndTimeRemaining[BestIndex].Value;
	return true;
}

int32 UEnhancedAbilitySystemComponent::GetActiveEffectLevel(const FActiveGameplayEffectHandle& ActiveHandle) const
{
	if (const FActiveGameplayEffect* ActiveEffect = GetActiveGameplayEffect(ActiveHandle))
	{
		return static_cast<int32>(ActiveEffect->Spec.GetLevel());
	}

	return 0;
}

void UEnhancedAbilitySystemComponent::GetActiveGameplayEffectTags(const FGameplayTagContainer& TagFilter, TArray<FGameplayTag>& OutTags) const
{
	OutTags.Reset();

	if (TagFilter.Num() == 0)
	{
		return;
	}

	for (const FActiveGameplayEffectHandle& Handle : ActiveGameplayEffects.GetAllActiveEffectHandles())
	{
		if (!Handle.IsValid())
		{
			continue;
		}

		const FActiveGameplayEffect* ActiveEffect = GetActiveGameplayEffect(Handle);
		if (!ActiveEffect || !ActiveEffect->Spec.Def || ActiveEffect->Spec.Def->DurationPolicy != EGameplayEffectDurationType::HasDuration)
		{
			continue;
		}

		FGameplayTagContainer AssetTags;
		ActiveEffect->Spec.GetAllAssetTags(AssetTags);
		if (AssetTags.Num() == 0 || !AssetTags.HasAny(TagFilter))
		{
			continue;
		}

		for (const FGameplayTag& Tag : AssetTags)
		{
			if (Tag.IsValid() && Tag.MatchesAny(TagFilter))
			{
				OutTags.AddUnique(Tag);
			}
		}
	}
}

void UEnhancedAbilitySystemComponent::SetActiveGameplayEffectsDurationByTag(const FGameplayTagContainer& GameplayTags, const float Duration, const EGameplayEffectDurationModification DurationModification)
{
	if (!IsOwnerActorAuthoritative() || GameplayTags.Num() == 0)
	{
		return;
	}

	for (const FActiveGameplayEffectHandle& Handle : ActiveGameplayEffects.GetAllActiveEffectHandles())
	{
		if (!Handle.IsValid())
		{
			continue;
		}

		const FActiveGameplayEffect* ActiveEffect = GetActiveGameplayEffect(Handle);
		if (!ActiveEffect || !ActiveEffect->Spec.Def || ActiveEffect->Spec.Def->DurationPolicy != EGameplayEffectDurationType::HasDuration)
		{
			continue;
		}

		FGameplayTagContainer AssetTags;
		ActiveEffect->Spec.GetAllAssetTags(AssetTags);
		if (AssetTags.HasAny(GameplayTags))
		{
			SetActiveGameplayEffectDuration(Handle, Duration, DurationModification);
		}
	}
}

void UEnhancedAbilitySystemComponent::SetActiveGameplayEffectDuration(const FActiveGameplayEffectHandle& Handle, const float Duration, const EGameplayEffectDurationModification DurationModification)
{
	if (!IsOwnerActorAuthoritative())
	{
		return;
	}

	const UWorld* World = GetWorld();
	if (!World || !Handle.IsValid())
	{
		return;
	}

	FScopedActiveGameplayEffectLock ActiveEffectScopeLock(ActiveGameplayEffects);
	FActiveGameplayEffect* ActiveEffect = ActiveGameplayEffects.GetActiveGameplayEffect(Handle);
	if (!ActiveEffect || !ActiveEffect->Spec.Def || ActiveEffect->Spec.Def->DurationPolicy != EGameplayEffectDurationType::HasDuration)
	{
		return;
	}

	const float WorldTime = World->GetTimeSeconds();
	const float CurrentRemaining = ActiveEffect->GetTimeRemaining(WorldTime);

	float NewDuration;
	switch (DurationModification)
	{
	case EGameplayEffectDurationModification::Add:
		NewDuration = CurrentRemaining + Duration;
		break;

	case EGameplayEffectDurationModification::AddPercent:
		{
			const float Clamped = FMath::Clamp(Duration, 0.f, 1.f);
			NewDuration = CurrentRemaining + CurrentRemaining * Clamped;
		}
		break;

	case EGameplayEffectDurationModification::SubtractPercent:
		{
			const float Clamped = FMath::Clamp(Duration, 0.f, 1.f);
			NewDuration = CurrentRemaining - CurrentRemaining * Clamped;
		}
		break;

	default: // Override
		NewDuration = Duration;
		break;
	}

	// Write Spec.Duration directly: SetDuration is a no-op once bDurationLocked (typical after apply).
	ActiveEffect->Spec.Duration = FMath::Max(NewDuration, 0.01f);

	// Restart the clock from "now" so remaining time == Spec.Duration.
	ActiveEffect->StartServerWorldTime = ActiveGameplayEffects.GetServerWorldTime();
	ActiveEffect->CachedStartServerWorldTime = ActiveEffect->StartServerWorldTime;
	ActiveEffect->StartWorldTime = ActiveGameplayEffects.GetWorldTime();

	ActiveGameplayEffects.MarkItemDirty(*ActiveEffect);
	ActiveGameplayEffects.CheckDuration(Handle);
	// Application callbacks can change pending effects before their original timer is finalized.
	World->GetTimerManager().SetTimerForNextTick(FTimerDelegate::CreateUObject(this, &ThisClass::CheckDurationExpired, Handle));

	ActiveEffect->EventSet.OnTimeChanged.Broadcast(ActiveEffect->Handle, ActiveEffect->StartWorldTime, ActiveEffect->GetDuration());
	OnGameplayEffectDurationChange(*ActiveEffect);
}

float UEnhancedAbilitySystemComponent::GetActiveEffectSetByCallerMagnitude(const FGameplayTag& MagnitudeTag) const
{
	if (!MagnitudeTag.IsValid())
	{
		return 0.f;
	}

	for (const FActiveGameplayEffectHandle& Handle : ActiveGameplayEffects.GetAllActiveEffectHandles())
	{
		if (const FActiveGameplayEffect* ActiveEffect = GetActiveGameplayEffect(Handle))
		{
			if (const float* Magnitude = ActiveEffect->Spec.SetByCallerTagMagnitudes.Find(MagnitudeTag))
			{
				return *Magnitude;
			}
		}
	}

	return 0.f;
}

// ----------------------------------------------------------------------------------------------------------------
// Loose gameplay tags
// ----------------------------------------------------------------------------------------------------------------

void UEnhancedAbilitySystemComponent::UpdateLooseGameplayTagCount(const FGameplayTag& GameplayTag, const int32 Count, const bool bOverride)
{
	if (!GameplayTag.IsValid())
	{
		return;
	}

	if (bOverride)
	{
		SetLooseGameplayTagCount(GameplayTag, Count);
		return;
	}

	if (Count > 0)
	{
		AddLooseGameplayTag(GameplayTag, Count);
	}
	else if (Count < 0)
	{
		RemoveLooseGameplayTag(GameplayTag, -Count);
	}
}
