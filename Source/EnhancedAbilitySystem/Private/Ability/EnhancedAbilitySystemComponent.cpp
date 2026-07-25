// Copyright Solessfir. All Rights Reserved.

#include "Ability/EnhancedAbilitySystemComponent.h"
#include "Ability/EnhancedGameplayAbility.h"
#include "EnhancedInputComponent.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "Logging/StructuredLog.h"
#include UE_INLINE_GENERATED_CPP_BY_NAME(EnhancedAbilitySystemComponent)

DEFINE_LOG_CATEGORY_STATIC(LogEnhancedAbilitySystemComponent, Log, All);

// ----------------------------------------------------------------------------------------------------------------
// Ability Grant / Lifecycle
// ----------------------------------------------------------------------------------------------------------------

void UEnhancedAbilitySystemComponent::OnGiveAbility(FGameplayAbilitySpec& AbilitySpec)
{
	Super::OnGiveAbility(AbilitySpec);

	const UEnhancedGameplayAbility* EnhancedAbility = Cast<UEnhancedGameplayAbility>(AbilitySpec.Ability);
	if (!EnhancedAbility)
	{
		return;
	}

	SetInputBinding(AbilitySpec.Handle, EnhancedAbility->InputAction, EnhancedAbility->ConfirmInputAction, EnhancedAbility->CancelInputAction);
}

void UEnhancedAbilitySystemComponent::OnRemoveAbility(FGameplayAbilitySpec& AbilitySpec)
{
	Super::OnRemoveAbility(AbilitySpec);
	ClearInputBinding(AbilitySpec.Handle);
}

void UEnhancedAbilitySystemComponent::TryActivateAbilityOnGranted(const FGameplayAbilitySpec& AbilitySpec)
{
	if (!AbilitySpec.Ability || AbilitySpec.IsActive() || AbilitySpec.PendingRemove)
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
	if (!Ability || !IsOwnerActorAuthoritative())
	{
		return FGameplayAbilitySpecHandle();
	}

	if (const FGameplayAbilitySpec* Existing = FindAbilitySpecFromClass(Ability))
	{
		if (!Existing->PendingRemove)
		{
			return Existing->Handle;
		}
	}

	return GiveAbility(FGameplayAbilitySpec(Ability, Level));
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

void UEnhancedAbilitySystemComponent::Input_AbilityPressed(FGameplayAbilitySpecHandle Handle)
{
	HandleAbilityInputPressed(Handle);
}

void UEnhancedAbilitySystemComponent::Input_AbilityReleased(FGameplayAbilitySpecHandle Handle)
{
	HandleAbilityInputReleased(Handle);
}

void UEnhancedAbilitySystemComponent::Input_AbilityInputConfirmed(FGameplayAbilitySpecHandle Handle)
{
	HandleAbilityInputConfirmed(Handle);
}

void UEnhancedAbilitySystemComponent::Input_AbilityInputCanceled(FGameplayAbilitySpecHandle Handle)
{
	HandleAbilityInputCanceled(Handle);
}

void UEnhancedAbilitySystemComponent::HandleAbilityInputPressed(FGameplayAbilitySpecHandle Handle)
{
	FGameplayAbilitySpec* Spec = FindAbilitySpecFromHandle(Handle);
	if (!Spec || !Spec->Ability || Spec->PendingRemove)
	{
		return;
	}

	// Triggered can fire every frame while held. Only process its first frame so instant abilities do not
	// reactivate continuously and failed activations are not retried until the action is released.
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

		AbilitySpecInputPressed(*Spec);
		InvokeReplicatedEvent(EAbilityGenericReplicatedEvent::InputPressed, Spec->Handle, GetPredictionKeyFromSpec(*Spec));
	}
}

void UEnhancedAbilitySystemComponent::HandleAbilityInputReleased(FGameplayAbilitySpecHandle Handle)
{
	FGameplayAbilitySpec* Spec = FindAbilitySpecFromHandle(Handle);
	if (!Spec || !Spec->Ability || Spec->PendingRemove)
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

	AbilitySpecInputReleased(*Spec);
	InvokeReplicatedEvent(EAbilityGenericReplicatedEvent::InputReleased, Spec->Handle, GetPredictionKeyFromSpec(*Spec));
}

void UEnhancedAbilitySystemComponent::HandleAbilityInputConfirmed(FGameplayAbilitySpecHandle Handle)
{
	const FGameplayAbilitySpec* Spec = FindAbilitySpecFromHandle(Handle);
	if (!Spec || !Spec->Ability || Spec->PendingRemove || !Spec->IsActive())
	{
		return;
	}

	// GAS generic confirm is ASC-wide. Restrict each per-ability action to its active owning spec;
	// the waiting ability task/target actor handles any required replicated event itself.
	LocalInputConfirm();
}

void UEnhancedAbilitySystemComponent::HandleAbilityInputCanceled(FGameplayAbilitySpecHandle Handle)
{
	const FGameplayAbilitySpec* Spec = FindAbilitySpecFromHandle(Handle);
	if (!Spec || !Spec->Ability || Spec->PendingRemove || !Spec->IsActive())
	{
		return;
	}

	// GAS generic cancel is ASC-wide; replication is owned by the waiting task/target actor.
	LocalInputCancel();
}

FPredictionKey UEnhancedAbilitySystemComponent::GetPredictionKeyFromSpec(const FGameplayAbilitySpec& Spec)
{
	if (const UGameplayAbility* PrimaryInstance = Spec.GetPrimaryInstance())
	{
		return PrimaryInstance->GetCurrentActivationInfo().GetActivationPredictionKey();
	}

	if (TArray<UGameplayAbility*> Instances = Spec.GetAbilityInstances(); Instances.Num() > 0)
	{
		return Instances.Last()->GetCurrentActivationInfo().GetActivationPredictionKey();
	}

	UE_LOGFMT(LogEnhancedAbilitySystemComponent, Warning, "Falling back to the Deprecated Non-Instanced Activation Prediction Key! | {0}:{1}", __FUNCTION__, __LINE__);
	PRAGMA_DISABLE_DEPRECATION_WARNINGS
	return Spec.ActivationInfo.GetActivationPredictionKey();
	PRAGMA_ENABLE_DEPRECATION_WARNINGS
}

void UEnhancedAbilitySystemComponent::SetInputBinding(const FGameplayAbilitySpecHandle& SpecHandle, const UInputAction* InputAction, const UInputAction* ConfirmInputAction, const UInputAction* CancelInputAction)
{
	if (!SpecHandle.IsValid())
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

	TArray<uint32>& Handles = AbilityInputBindingHandles.FindOrAdd(SpecHandle);

	if (InputAction)
	{
		// Use Triggered rather than Started so the Input Action's trigger pipeline (Hold, Tap, Chord,
		// Combo, etc.) must succeed first. Enhanced Input applies modifiers before this callback.
		// Triggered may repeat while held, so HandleAbilityInputPressed edge-detects the first frame.
		Handles.Add(InputComponent->BindAction(InputAction, ETriggerEvent::Triggered, this, &ThisClass::Input_AbilityPressed, SpecHandle).GetHandle());

		// Completed: release after a successful trigger.
		Handles.Add(InputComponent->BindAction(InputAction, ETriggerEvent::Completed, this, &ThisClass::Input_AbilityReleased, SpecHandle).GetHandle());

		// Canceled: release if the trigger didn't finish (e.g. let go during a hold trigger).
		Handles.Add(InputComponent->BindAction(InputAction, ETriggerEvent::Canceled, this, &ThisClass::Input_AbilityReleased, SpecHandle).GetHandle());
	}

	if (ConfirmInputAction)
	{
		Handles.Add(InputComponent->BindAction(ConfirmInputAction, ETriggerEvent::Started, this, &ThisClass::Input_AbilityInputConfirmed, SpecHandle).GetHandle());
	}

	if (CancelInputAction)
	{
		Handles.Add(InputComponent->BindAction(CancelInputAction, ETriggerEvent::Started, this, &ThisClass::Input_AbilityInputCanceled, SpecHandle).GetHandle());
	}
}

void UEnhancedAbilitySystemComponent::BindAbilityInputs()
{
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
	TArray<uint32>* Handles = AbilityInputBindingHandles.Find(SpecHandle);
	if (!Handles)
	{
		return;
	}

	if (UEnhancedInputComponent* InputComponent = GetEnhancedInputComponent())
	{
		for (const uint32 Handle : *Handles)
		{
			InputComponent->RemoveBindingByHandle(Handle);
		}
	}

	AbilityInputBindingHandles.Remove(SpecHandle);
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
