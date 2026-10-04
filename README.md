# Enhanced Ability System

Gameplay Ability System base for Unreal Engine with **Enhanced Input** ability binding. Assign `InputAction` / Confirm / Cancel on abilities; the ASC binds press, release, and targeting confirm/cancel for you.

## Installation

Get `EnhancedAbilitySystem.zip` from the [releases](https://github.com/Solessfir/EnhancedAbilitySystem/releases) and extract it into your project's `Plugins` folder.

Requires the **Gameplay Abilities** and **Enhanced Input** plugins (enabled automatically by this plugin).

## Quick start

For C++ projects, add `EnhancedAbilitySystem` to your game module's `Build.cs` dependencies:

```csharp
PublicDependencyModuleNames.Add("EnhancedAbilitySystem");
```

### 1. Add an ASC to the Character or PlayerState

Add `EnhancedAbilitySystemComponent` to your Character or PlayerState (or create it as a default subobject in C++). Implement `IAbilitySystemInterface` on the owner and return that component from `GetAbilitySystemComponent`. With PlayerState ownership, the Character should also expose the same ASC through its interface.

```cpp
// Header
UPROPERTY(VisibleDefaultsOnly, BlueprintReadOnly, Category = "Abilities")
TObjectPtr<UEnhancedAbilitySystemComponent> AbilitySystemComponent;

// Constructor
AbilitySystemComponent = CreateDefaultSubobject<UEnhancedAbilitySystemComponent>(TEXT("AbilitySystemComponent"));
AbilitySystemComponent->SetIsReplicated(true);
AbilitySystemComponent->SetReplicationMode(EGameplayEffectReplicationMode::Mixed);
```

### 2. Init actor info (server **and** owning client)

Grant abilities only on the **server**. Init actor info on **both** authority and the locally controlled client.

The example below uses a Character-owned ASC. For PlayerState ownership, retrieve the ASC from the PlayerState and use `InitAbilityActorInfo(GetPlayerState(), this)` instead. Reinitialize it for each new avatar, but grant default abilities once per ASC owner so they persist across respawns.

```cpp
void AMyCharacter::PossessedBy(AController* NewController)
{
	Super::PossessedBy(NewController);

	if (AbilitySystemComponent)
	{
		// Owner and avatar are the same actor when the ASC lives on the Character
		// (no PlayerState indirection).
		AbilitySystemComponent->InitAbilityActorInfo(this, this);
		GrantDefaultAbilities(); // authority only inside
	}
}

// Owning client - pick the hook that matches your setup (common options):
void AMyCharacter::OnRep_PlayerState()
{
	Super::OnRep_PlayerState();
	if (AbilitySystemComponent && IsLocallyControlled())
	{
		AbilitySystemComponent->InitAbilityActorInfo(this, this);
	}
}
// or AcknowledgePossession / OnRep_Controller, etc.
```

```cpp
void AMyCharacter::GrantDefaultAbilities()
{
	if (bDefaultAbilitiesGranted || !HasAuthority() || !AbilitySystemComponent)
	{
		return;
	}

	bDefaultAbilitiesGranted = true;

	for (const TSubclassOf<UEnhancedGameplayAbility> AbilityClass : DefaultAbilities)
	{
		if (AbilityClass)
		{
			AbilitySystemComponent->GiveAbility(FGameplayAbilitySpec(AbilityClass, 1, INDEX_NONE, this));
		}
	}
}
```

### 3. Bind ability inputs after InputComponent exists

Abilities can be granted before input is ready. Use a `UEnhancedInputComponent` and call `BindAbilityInputs()` from `SetupPlayerInputComponent`:

```cpp
void AMyCharacter::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);

	if (AbilitySystemComponent)
	{
		AbilitySystemComponent->BindAbilityInputs();
	}
}
```

### 4. Create abilities and assign Input Actions

1. Create a Blueprint child of **Enhanced Gameplay Ability**.
2. Set **Input Action** (and optionally **Confirm Input Action** / **Cancel Input Action**) on the class defaults.
3. Add the ability class to your character's default grant list (or call `GiveAbility` / `GiveAbilityIfNotOwned` at runtime - **server only**).
4. Ensure those Input Actions are in an **Input Mapping Context** applied to the player (via `UEnhancedInputLocalPlayerSubsystem`), same as any other Enhanced Input setup.

Activation uses **Triggered** after the action's configured triggers succeed, once per press until **Completed** / **Canceled** releases it. This supports `Wait Input Press` / `Wait Input Release`. Confirm/Cancel use **Started** while the ability is active; GAS generic confirm/cancel reaches all listeners on the ASC.

## Classes

### Enhanced Ability System Component

Extends `UAbilitySystemComponent` with Enhanced Input bindings and Blueprint-accessible GAS helpers.

| Feature | Main helpers |
|---|---|
| Input | Bind, remap, and clear ability Input Actions |
| Abilities | Grant if not owned, find by asset tag, check activity, and cancel |
| Gameplay Effects | Query cooldowns, levels, tags, and SetByCaller values; modify remaining duration |
| Loose tags | Add, remove, or override tag counts |

Granting abilities and modifying effect duration require authority. Ability tag queries use **exact asset tags**. `GetGameplayAbilityByTag` returns the primary instance when available, otherwise the CDO. Input remapping requires a primary instance on the owning client, survives rebinding, and releases held input when bindings change.

See [the component header](Source/EnhancedAbilitySystem/Public/Ability/EnhancedAbilitySystemComponent.h) for the full API.

### Enhanced Gameplay Ability

Abstract `UGameplayAbility` base with Input Actions, **Name / Description / Icon** metadata, and optional **Activate On Granted**. Defaults to **Instanced Per Actor**.

Auto-activation follows the ability's net execution policy: local policies on locally controlled avatars, server policies on authority. It retries when the avatar or local control becomes available.

#### Cooldowns

1. Assign a duration Cooldown GE. For per-ability lengths, use a **SetByCaller** duration with a valid Data Tag, e.g. `Data.CooldownDuration`.
2. Set **Cooldown Tag**, e.g. `Cooldown.Dash`, to grant that tag while the effect is active.
3. Optionally set **Cooldown Duration** to override the GE's duration. The override requires the SetByCaller setup above and is clamped to at least **0.01 seconds**.

The Data Tag identifies the duration magnitude; the Cooldown Tag identifies the granted cooldown state. An unset override uses the GE's authored duration.

Without a Cooldown Tag, cooldown application uses stock GAS behavior. Runtime `SetCooldownDuration` / `ClearCooldownDuration` require an ability instance; the override persists until cleared. Use GAS **Get Cooldown Time Remaining** for UI timers.

See [the ability header](Source/EnhancedAbilitySystem/Public/Ability/EnhancedGameplayAbility.h) for properties and helpers.

## Notes

- Only abilities that subclass **Enhanced Gameplay Ability** get input binding. Plain `UGameplayAbility` still works with GAS; they just won't auto-bind Input Actions.
- Local player only: bindings attach to the avatar pawn's `UEnhancedInputComponent` when it is locally controlled.
- Effect-duration helpers modify remaining time, with a **0.01-second** minimum. Percentages use **0–1** fractions.
- Loose tags are not automatically replicated. Their count overrides affect GAS's shared tag counts, including effect and ability grants.
- Attribute sets, project gameplay tags, and locomotion Input Actions stay in your game module - this plugin is the ability ↔ Enhanced Input bridge, not a full gameplay framework.

## License

Licensed under the [MIT License](LICENSE).
