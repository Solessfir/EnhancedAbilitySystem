// Copyright Solessfir. All Rights Reserved.

#include "Ability/EnhancedGameplayAbility.h"
#include "Ability/EnhancedAbilitySystemComponent.h"
#include "GameplayEffect.h"
#include "Logging/StructuredLog.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(EnhancedGameplayAbility)

DEFINE_LOG_CATEGORY_STATIC(LogEnhancedGameplayAbility, Log, All);

UEnhancedGameplayAbility::UEnhancedGameplayAbility()
{
	InstancingPolicy = EGameplayAbilityInstancingPolicy::InstancedPerActor;
}

void UEnhancedGameplayAbility::OnAvatarSet(const FGameplayAbilityActorInfo* ActorInfo, const FGameplayAbilitySpec& Spec)
{
	Super::OnAvatarSet(ActorInfo, Spec);

	if (!bActivateOnGranted || !ActorInfo)
	{
		return;
	}

	if (UEnhancedAbilitySystemComponent* ASC = Cast<UEnhancedAbilitySystemComponent>(ActorInfo->AbilitySystemComponent.Get()))
	{
		ASC->TryActivateAbilityOnGranted(Spec);
	}
}

const FGameplayTagContainer& UEnhancedGameplayAbility::GetAbilityTags() const
{
	return GetAssetTags();
}

void UEnhancedGameplayAbility::RemapInputAction(const UInputAction* NewInputAction)
{
	const FGameplayAbilityActorInfo* ActorInfo = GetCurrentActorInfo();
	if (!ActorInfo)
	{
		return;
	}

	UEnhancedAbilitySystemComponent* ASC = Cast<UEnhancedAbilitySystemComponent>(ActorInfo->AbilitySystemComponent.Get());
	const FGameplayAbilitySpec* Spec = GetCurrentAbilitySpec();
	if (!ASC || !Spec)
	{
		return;
	}

	// Guard against mutating the CDO, which would affect all future instances.
	if (UEnhancedGameplayAbility* PrimaryInstance = Cast<UEnhancedGameplayAbility>(Spec->GetPrimaryInstance()))
	{
		PrimaryInstance->InputAction = NewInputAction;
	}
	else if (IsInstantiated())
	{
		InputAction = NewInputAction;
	}
	else
	{
		return;
	}

	// Null primary action clears / rebinds via ASC (all-null → ClearInputBinding).
	ASC->SetInputBinding(Spec->Handle, NewInputAction, ConfirmInputAction, CancelInputAction);
}

void UEnhancedGameplayAbility::SetCooldownDuration(const float DurationSeconds)
{
	if (DurationSeconds <= 0.f)
	{
		UE_LOGFMT(LogEnhancedGameplayAbility, Warning, "SetCooldownDuration ignored - duration must be > 0 ({0}). Use ClearCooldownDuration to unset.", DurationSeconds);
		return;
	}

	// Never mutate the CDO - ApplyCooldown runs on the instance during Commit.
	if (!ensureMsgf(IsInstantiated(), TEXT("SetCooldownDuration must be called on an ability instance, not the CDO.")))
	{
		return;
	}

	CooldownDuration = FScalableFloat(DurationSeconds);
}

void UEnhancedGameplayAbility::ClearCooldownDuration()
{
	if (!ensureMsgf(IsInstantiated(), TEXT("ClearCooldownDuration must be called on an ability instance, not the CDO.")))
	{
		return;
	}

	CooldownDuration.Reset();
}

bool UEnhancedGameplayAbility::HasCooldownDuration() const
{
	return CooldownDuration.IsSet();
}

float UEnhancedGameplayAbility::GetCooldownDuration() const
{
	const float AbilityLevel = GetAbilityLevel();

	if (CooldownDuration.IsSet())
	{
		return CooldownDuration.GetValue().GetValueAtLevel(AbilityLevel);
	}

	const UGameplayEffect* CooldownGE = GetCooldownGameplayEffect();
	if (!CooldownGE || CooldownGE->DurationPolicy != EGameplayEffectDurationType::HasDuration)
	{
		return 0.f;
	}

	// CooldownDuration is not set - try read directly from GE
	if (float OutMagnitude = 0.f; CooldownGE->DurationMagnitude.GetStaticMagnitudeIfPossible(AbilityLevel, OutMagnitude))
	{
		return OutMagnitude;
	}

	return 0.f;
}

const FGameplayTagContainer* UEnhancedGameplayAbility::GetCooldownTags() const
{
	// Scratch container rebuilt every call - do not hold this pointer across nested GetCooldownTags() calls.
	DynamicCooldownTags.Reset();

	if (const FGameplayTagContainer* ParentTags = Super::GetCooldownTags())
	{
		DynamicCooldownTags.AppendTags(*ParentTags);
	}

	if (CooldownTag.IsValid())
	{
		DynamicCooldownTags.AddTag(CooldownTag);
	}

	return &DynamicCooldownTags;
}

void UEnhancedGameplayAbility::ApplyCooldown(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo, const FGameplayAbilityActivationInfo ActivationInfo) const
{
	const UGameplayEffect* CooldownGameplayEffect = GetCooldownGameplayEffect();
	if (!CooldownGameplayEffect)
	{
		return;
	}

	// CooldownTag per-ability path
	if (CooldownTag.IsValid())
	{
		const FGameplayEffectSpecHandle SpecHandle = MakeOutgoingGameplayEffectSpec(CooldownGameplayEffect->GetClass(), GetAbilityLevel(Handle, ActorInfo));
		FGameplayEffectSpec* Spec = SpecHandle.Data.Get();
		if (!Spec)
		{
			return;
		}

		// Dynamic asset tag for identification; dynamic granted tag drives CheckCooldown / GetCooldownTimeRemaining.
		Spec->AddDynamicAssetTag(CooldownTag);
		Spec->DynamicGrantedTags.AddTag(CooldownTag);

		if (CooldownDuration.IsSet())
		{
			const FSetByCallerFloat& SetByCaller = CooldownGameplayEffect->DurationMagnitude.GetSetByCallerFloat();
			if (CooldownGameplayEffect->DurationMagnitude.GetMagnitudeCalculationType() == EGameplayEffectMagnitudeCalculation::SetByCaller && SetByCaller.DataTag.IsValid())
			{
				const float EvaluatedDuration = CooldownDuration.GetValue().GetValueAtLevel(GetAbilityLevel(Handle, ActorInfo));
				Spec->SetSetByCallerMagnitude(SetByCaller.DataTag, FMath::Max(EvaluatedDuration, 0.01f));
			}
			#if !UE_BUILD_SHIPPING
			else
			{
				UE_LOGFMT(LogEnhancedGameplayAbility, Warning, "{0}: CooldownDuration is set but Cooldown GE Duration is not SetByCaller with a Data Tag. Applying GE duration as authored; override ignored.", GetName());
			}
			#endif
		}

		ApplyGameplayEffectSpecToOwner(Handle, ActorInfo, ActivationInfo, SpecHandle);
		return;
	}

	Super::ApplyCooldown(Handle, ActorInfo, ActivationInfo);
}
