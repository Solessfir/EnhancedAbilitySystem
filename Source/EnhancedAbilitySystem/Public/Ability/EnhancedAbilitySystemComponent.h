// Copyright Solessfir. All Rights Reserved.

#pragma once

#include "AbilitySystemComponent.h"
#include "EnhancedAbilitySystemComponent.generated.h"

class UInputAction;
class UEnhancedInputComponent;
class UEnhancedGameplayAbility;
struct FInputActionInstance;

/**
 * How SetActiveGameplayEffectDuration should adjust remaining lifetime.
 */
UENUM(BlueprintType)
enum class EGameplayEffectDurationModification : uint8
{
	// Replace remaining duration with the given value (seconds).
	Override,
	// Add seconds to remaining duration.
	Add,
	// Add Duration fraction of remaining time (0..1 → 0%..100%).
	AddPercent,
	// Subtract Duration fraction of remaining time (0..1 → 0%..100%).
	SubtractPercent
};

/**
 * AbilitySystemComponent that binds UEnhancedGameplayAbility InputActions via Enhanced Input,
 * plus common GAS helpers (cooldown queries, duration tweaks, loose-tag counts, ability active checks).
 * Call BindAbilityInputs() from Pawn::SetupPlayerInputComponent (InputComponent must exist first).
 *
 * Multiplayer notes:
 *   - Grant abilities and mutate active GE durations on the authority only.
 *   - InitAbilityActorInfo on both server (PossessedBy) and owning client (e.g. OnRep_PlayerState / AcknowledgePossession).
 *   - Loose gameplay tags do not automatically replicate; replicate yourself if clients need them.
 */
UCLASS(BlueprintType, ClassGroup = "Ability", Meta = (BlueprintSpawnableComponent))
class ENHANCEDABILITYSYSTEM_API UEnhancedAbilitySystemComponent : public UAbilitySystemComponent
{
	GENERATED_BODY()

public:
	// Retries ActivateOnGranted when an unchanged avatar gains its local controller.
	virtual void InitAbilityActorInfo(AActor* InOwnerActor, AActor* InAvatarActor) override;

	// ----------------------------------------------------------------------------------------------------------------
	// Ability Grant / Query
	// ----------------------------------------------------------------------------------------------------------------

	// Returns a live or queued grant's handle for the class. Invalid if not authority or a clear-all is pending.
	UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, DisplayName = "Give Ability (If not Owned)", Meta = (AdvancedDisplay = 1), Category = "Ability|Gameplay Ability")
	FGameplayAbilitySpecHandle GiveAbilityIfNotOwned(const TSubclassOf<UGameplayAbility> Ability, const int32 Level = 1);

	// Exact match on ability asset tags. Skips PendingRemove. Prefers primary instance over CDO.
	UFUNCTION(BlueprintCallable, Meta = (AutoCreateRefTerm = "Tag"), Category = "Ability|Gameplay Ability")
	UGameplayAbility* GetGameplayAbilityByTag(const FGameplayTag& Tag) const;

	// Exact match on ability asset tags. Skips PendingRemove. Prefer this over copying a full FGameplayAbilitySpec.
	UFUNCTION(BlueprintCallable, Meta = (AutoCreateRefTerm = "AbilityTag"), Category = "Ability|Gameplay Ability")
	FGameplayAbilitySpecHandle GetGameplayAbilitySpecHandleByTag(const FGameplayTag& AbilityTag) const;

	UFUNCTION(BlueprintPure, Category = "Ability|Gameplay Ability")
	bool IsAbilityActiveByClass(TSubclassOf<UGameplayAbility> AbilityClass) const;

	UFUNCTION(BlueprintPure, Category = "Ability|Gameplay Ability")
	bool IsAbilityActiveByHandle(const FGameplayAbilitySpecHandle& Handle) const;

	// True if any non-pending spec with this exact ability asset tag is active.
	UFUNCTION(BlueprintPure, Meta = (AutoCreateRefTerm = "GameplayTag"), Category = "Ability|Gameplay Ability")
	bool IsAbilityActiveByTag(const FGameplayTag& GameplayTag) const;

	UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Meta = (DisplayName = "Cancel All Abilities"), Category = "Ability|Gameplay Ability")
	void K2_CancelAllAbilities(UGameplayAbility* Ignore = nullptr);

	UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Meta = (DisplayName = "Cancel Abilities With Tags", AutoCreateRefTerm = "WithTags,WithoutTags"), Category = "Ability|Gameplay Ability")
	void CancelAbilitiesWithTags(const FGameplayTagContainer& WithTags, const FGameplayTagContainer& WithoutTags, UGameplayAbility* Ignore = nullptr);

	// ----------------------------------------------------------------------------------------------------------------
	// Enhanced Input binding
	// ----------------------------------------------------------------------------------------------------------------

	// Binds activate / confirm / cancel InputActions for a granted ability handle.
	// Activate uses Triggered so configured Enhanced Input triggers complete before the press is delivered;
	// repeated Triggered frames are edge-detected until Completed/Canceled releases the action.
	// Confirm/cancel are GAS-generic (ASC-wide), but only fire while this handle's spec is active.
	// If all three actions are null, clears existing bindings for that handle (same as ClearInputBinding).
	void SetInputBinding(const FGameplayAbilitySpecHandle& SpecHandle, const UInputAction* InputAction, const UInputAction* ConfirmInputAction, const UInputAction* CancelInputAction);

	// Removes bindings and releases held input for this ability handle (does not change ability CDO/instance InputAction refs).
	UFUNCTION(BlueprintCallable, Category = "Ability|Gameplay Ability")
	void ClearInputBinding(const FGameplayAbilitySpecHandle& SpecHandle);

	// Finds ability by exact asset tag and clears its input bindings.
	UFUNCTION(BlueprintCallable, Meta = (AutoCreateRefTerm = "AbilityTag"), Category = "Ability|Gameplay Ability")
	void ClearAbilityInputBinding(const FGameplayTag& AbilityTag);

	// Rebind primary InputAction on the primary instance (never mutates the CDO). Pass null to clear the primary action
	// and rebind confirm/cancel only (or clear entirely if those are also null). The instance remap survives BindAbilityInputs.
	// This changes local input state only; call it on the owning client. Prefer ClearAbilityInputBinding to drop binds without remapping.
	UFUNCTION(BlueprintCallable, Meta = (AutoCreateRefTerm = "AbilityTag"), Category = "Ability|Gameplay Ability")
	void RemapAbilityInputAction(const FGameplayTag& AbilityTag, const UInputAction* InputAction);

	UEnhancedInputComponent* GetEnhancedInputComponent() const;

	// Re-binds every granted ability's input. OnGiveAbility's bind can silently fail if the avatar
	// Pawn's InputComponent doesn't exist yet (abilities are granted from PossessedBy, server-authoritative,
	// which can race PawnClientRestart). Call from Pawn::SetupPlayerInputComponent, which only fires
	// once InputComponent is guaranteed to exist.
	UFUNCTION(BlueprintCallable, Category = "Ability|Gameplay Ability")
	void BindAbilityInputs();

	// ----------------------------------------------------------------------------------------------------------------
	// Cooldowns / active GameplayEffects
	// ----------------------------------------------------------------------------------------------------------------

	// Longest remaining time among active effects that own any of CooldownTags (typically granted
	// cooldown tags). Returns false if nothing matches. TimeRemaining/CooldownDuration are zeroed on failure.
	UFUNCTION(BlueprintCallable, Meta = (ExpandBoolAsExecs = "ReturnValue", AutoCreateRefTerm = "CooldownTags"), Category = "Ability|Gameplay Effects")
	bool GetCooldownRemainingForTag(const FGameplayTagContainer& CooldownTags, float& TimeRemaining, float& CooldownDuration) const;

	// Level of the active effect, or 0 if the handle is invalid / expired.
	UFUNCTION(BlueprintPure, Category = "Ability|Gameplay Effects")
	int32 GetActiveEffectLevel(const FActiveGameplayEffectHandle& ActiveHandle) const;

	// Asset tags from currently active duration effects that match TagFilter (hierarchical match;
	// e.g. filter Cooldown returns Cooldown.Fire).
	// Useful for UI that lists which cooldowns / timed effects are running.
	UFUNCTION(BlueprintCallable, Meta = (AutoCreateRefTerm = "TagFilter"), Category = "Ability|Gameplay Effects")
	void GetActiveGameplayEffectTags(const FGameplayTagContainer& TagFilter, TArray<FGameplayTag>& OutTags) const;

	// Authority only. Adjust remaining duration of every active duration GE whose asset tags overlap GameplayTags.
	UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Meta = (AutoCreateRefTerm = "GameplayTags"), Category = "Ability|Gameplay Effects")
	void SetActiveGameplayEffectsDurationByTag(const FGameplayTagContainer& GameplayTags, float Duration, EGameplayEffectDurationModification DurationModification = EGameplayEffectDurationModification::Override);

	// Authority only. Adjust remaining duration of a single active duration GE (dirties replication).
	// Calls during effect application resynchronize the expiration timer on the next tick.
	UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category = "Ability|Gameplay Effects")
	void SetActiveGameplayEffectDuration(const FActiveGameplayEffectHandle& Handle, float Duration, EGameplayEffectDurationModification DurationModification = EGameplayEffectDurationModification::Override);

	// SetByCaller magnitude from the first active GE whose SetByCaller map contains this exact data tag.
	// The effect does not also need to own/grant MagnitudeTag. Returns 0 if none found / not set.
	UFUNCTION(BlueprintCallable, Meta = (AutoCreateRefTerm = "MagnitudeTag"), Category = "Ability|Gameplay Effects")
	float GetActiveEffectSetByCallerMagnitude(const FGameplayTag& MagnitudeTag) const;

	// ----------------------------------------------------------------------------------------------------------------
	// Loose gameplay tags
	// ----------------------------------------------------------------------------------------------------------------

	// Calls native loose-tag APIs, which share aggregate explicit counts with GE/ability-granted tags.
	// Count: when bOverride is false, +N adds / -N removes; when true, sets the aggregate explicit count.
	// bOverride: true = absolute set, false = delta.
	// Coordinate overrides/removals when other sources grant the same tag; GAS does not isolate these counts.
	// Loose tags are NOT automatically replicated - authority and owning client must stay in sync yourself
	// (or use GE-granted tags when multiplayer visibility matters). Use GetGameplayTagCount for queries.
	UFUNCTION(BlueprintCallable, Meta = (AutoCreateRefTerm = "GameplayTag"), Category = "Ability|Gameplay Tags")
	void UpdateLooseGameplayTagCount(const FGameplayTag& GameplayTag, int32 Count = 1, bool bOverride = false);

protected:
	virtual void OnGiveAbility(FGameplayAbilitySpec& AbilitySpec) override;

	virtual void OnRemoveAbility(FGameplayAbilitySpec& AbilitySpec) override;

	// Net-safe auto-activate for bActivateOnGranted (local predicted/local only on client; server-only/server-initiated on authority).
	virtual void TryActivateAbilityOnGranted(const FGameplayAbilitySpec& AbilitySpec);

	// Input handlers - override to extend without re-binding Enhanced Input yourself.
	virtual void HandleAbilityInputPressed(FGameplayAbilitySpecHandle Handle);
	virtual void HandleAbilityInputReleased(FGameplayAbilitySpecHandle Handle);
	virtual void HandleAbilityInputConfirmed(FGameplayAbilitySpecHandle Handle);
	virtual void HandleAbilityInputCanceled(FGameplayAbilitySpecHandle Handle);

	// Exact asset-tag match, skips PendingRemove. OutSpec is non-null when return is true.
	bool FindAbilitySpecByAssetTag(const FGameplayTag& AbilityTag, const FGameplayAbilitySpec*& OutSpec) const;

	void DispatchAbilityInputEvent(FGameplayAbilitySpec& Spec, EAbilityGenericReplicatedEvent::Type EventType);
	static TArray<FPredictionKey> GetPredictionKeysFromSpec(const FGameplayAbilitySpec& Spec);

private:
	friend class UEnhancedGameplayAbility;

	// Preserve the last initialized state; querying the pawn can already observe a new possession.
	bool bActorInfoLocallyControlled = false;

	void PruneAbilityGrantBatchProgress();
	// First spec handles identify drain batches without depending on reused stack addresses.
	TMap<FGameplayAbilitySpecHandle, int32> AbilityGrantBatchNextIndices;

	// Generations reject callbacks Enhanced Input queued before a binding was replaced.
	void Input_AbilityPressed(FGameplayAbilitySpecHandle Handle, uint64 Generation);
	void Input_AbilityReleased(FGameplayAbilitySpecHandle Handle, uint64 Generation);
	void Input_AbilityInputConfirmed(const FInputActionInstance& ActionInstance, FGameplayAbilitySpecHandle Handle, uint64 Generation);
	void Input_AbilityInputCanceled(const FInputActionInstance& ActionInstance, FGameplayAbilitySpecHandle Handle, uint64 Generation);
	bool IsInputBindingCurrent(FGameplayAbilitySpecHandle Handle, uint64 Generation) const;
	bool ShouldDispatchGenericInput(const FInputActionInstance& ActionInstance, FGameplayAbilitySpecHandle Handle, uint64 Generation, bool bConfirm);

	struct FAbilityInputBinding
	{
		TWeakObjectPtr<UEnhancedInputComponent> InputComponent;
		TArray<uint32> Handles;
		uint64 Generation = 0;
	};

	TMap<FGameplayAbilitySpecHandle, FAbilityInputBinding> AbilityInputBindings;
	TMap<FGameplayAbilitySpecHandle, uint64> AbilityInputEventGenerations;
	uint64 InputBindingGeneration = 0;
	uint64 GenericInputFrame = MAX_uint64;
	TSet<TWeakObjectPtr<const UInputAction>> ConfirmedInputActions;
	TSet<TWeakObjectPtr<const UInputAction>> CanceledInputActions;
};
