// Copyright Solessfir. All Rights Reserved.

#pragma once

#include "Abilities/GameplayAbility.h"
#include "ScalableFloat.h"
#include "EnhancedGameplayAbility.generated.h"

class UInputAction;

/**
 * GameplayAbility base with Enhanced Input action slots (activate / confirm / cancel), lightweight UI metadata, and optional dynamic (tag + SetByCaller) cooldowns.
 * Pair with UEnhancedAbilitySystemComponent for input binding.
 *
 * Cooldown setup (this plugin's path):
 *   1. Assign a Cooldown Gameplay Effect Class.
 *   2. Set Cooldown Tag - injected on the applied GE (asset + granted) for CheckCooldown / UI.
 *   3. Optional Cooldown Duration - when set, GE Duration must be SetByCaller; when unset, GE's own duration is used.
 * If Cooldown Tag is not set, falls back to stock UGameplayAbility::ApplyCooldown (no per-ability tag injection).
 */
UCLASS(Abstract)
class ENHANCEDABILITYSYSTEM_API UEnhancedGameplayAbility : public UGameplayAbility
{
	GENERATED_BODY()

public:
	UEnhancedGameplayAbility();

	// Retries ActivateOnGranted when actor info receives a valid avatar after the spec was granted.
	virtual void OnAvatarSet(const FGameplayAbilityActorInfo* ActorInfo, const FGameplayAbilitySpec& Spec) override;

	// Merges Super cooldown tags (from the Cooldown GE) with CooldownTag for CheckCooldown queries.
	// Returns a scratch container rebuilt each call - do not hold the pointer across nested calls.
	virtual const FGameplayTagContainer* GetCooldownTags() const override;

	virtual void ApplyCooldown(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo, const FGameplayAbilityActivationInfo ActivationInfo) const override;

	UFUNCTION(BlueprintCallable, Category = "Ability")
	const FGameplayTagContainer& GetAbilityTags() const;

	// Rebind this ability's primary InputAction at runtime (primary-instance-safe - never mutates the CDO).
	// Null clears/rebinds via the ASC (all-null actions → ClearInputBinding).
	UFUNCTION(BlueprintCallable, Category = "Ability")
	void RemapInputAction(const UInputAction* NewInputAction);

	// Override the cooldown duration for the next Commit (instance only - never mutates the CDO).
	// Must be called before CommitAbility / ApplyCooldown. Value is in seconds (not a curve).
	UFUNCTION(BlueprintCallable, Category = "Ability|Cooldown")
	void SetCooldownDuration(float DurationSeconds);

	// Clears optional CooldownDuration so ApplyCooldown uses the Cooldown GE's authored duration (instance only).
	UFUNCTION(BlueprintCallable, Category = "Ability|Cooldown")
	void ClearCooldownDuration();

	// True if CooldownDuration optional is set (per-ability SetByCaller override), not merely that a Cooldown GE exists.
	UFUNCTION(BlueprintPure, Category = "Ability|Cooldown")
	bool HasCooldownDuration() const;

	// Cooldown length at current ability level: optional duration when the tagged SetByCaller GE can use it, otherwise the GE's static duration magnitude when readable.
	// 0 if missing/infinite/instant/unknown.
	// For remaining time use engine GetCooldownTimeRemaining (respects overridden GetCooldownTags).
	UFUNCTION(BlueprintPure, Category = "Ability|Cooldown")
	float GetCooldownDuration() const;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Ability")
	FText Name;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Ability")
	FText Description;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Ability")
	TSoftObjectPtr<UTexture2D> Icon;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Input")
	TObjectPtr<const UInputAction> InputAction;

	// GAS generic confirm is ASC-wide. This action is accepted only while this ability's spec is active.
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, AdvancedDisplay, Category = "Input")
	TObjectPtr<const UInputAction> ConfirmInputAction;

	// GAS generic cancel is ASC-wide. This action is accepted only while this ability's spec is active.
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, AdvancedDisplay, Category = "Input")
	TObjectPtr<const UInputAction> CancelInputAction;

	// Activation is attempted from OnAvatarSet, including when actor info becomes valid after the grant.
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Advanced")
	bool bActivateOnGranted = false;

	// Required for this plugin's cooldown path. Injected on apply (asset + granted); used by CheckCooldown / GetCooldownTimeRemaining.
	// Without it, ApplyCooldown falls back to stock UGameplayAbility behavior.
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Meta = (Categories = "Cooldown"), Category = "Cooldowns")
	FGameplayTag CooldownTag;

	// Optional per-ability duration override (SetByCaller on the Cooldown GE). Unset = use the GE's authored duration.
	// Supports curves via FScalableFloat at ability level.
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Cooldowns")
	TOptional<FScalableFloat> CooldownDuration;

private:
	// Scratch buffer for GetCooldownTags() - Super tags + CooldownTag (not safe to cache externally).
	UPROPERTY(Transient, DuplicateTransient)
	mutable FGameplayTagContainer DynamicCooldownTags;
};
