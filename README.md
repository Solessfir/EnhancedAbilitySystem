# Enhanced Ability System

Gameplay Ability System base for Unreal Engine with **Enhanced Input** ability binding. Assign `InputAction` / Confirm / Cancel on abilities; the ASC binds press, release, and targeting confirm/cancel for you.

## Installation

Get `EnhancedAbilitySystem.zip` from the [releases](https://github.com/Solessfir/EnhancedAbilitySystem/releases) and extract it into your project's `Plugins` folder.

Requires the **Gameplay Abilities** and **Enhanced Input** plugins (enabled automatically by this plugin).

## Quick start

### 1. Character owns an ASC

Add `EnhancedAbilitySystemComponent` to your Character (or create it as a default subobject in C++). Implement `IAbilitySystemInterface` and return that component from `GetAbilitySystemComponent`.

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

`OnGiveAbility` can race `PawnClientRestart` - abilities may be granted before the pawn's `InputComponent` exists, so the first bind silently fails. Call `BindAbilityInputs()` from `SetupPlayerInputComponent`, which only runs once input is ready:

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

The activate binding uses **Triggered** so the Input Action's trigger pipeline (Hold, Tap, Chord, Combo, etc.) completes before GAS receives the press. Modifiers are applied by Enhanced Input before the callback. Repeating `Triggered` frames are edge-detected, so holding an action activates/notifies only once until `Completed` / `Canceled` releases it. Releasing fires `Wait Input Release`.

Confirm/Cancel use **Started** (once per press) and are accepted only while their owning ability spec is active. GAS generic confirm/cancel is ASC-wide, so if multiple active abilities are waiting simultaneously, the event reaches all generic listeners.

## Classes

### Enhanced Ability System Component

Subclass of `UAbilitySystemComponent` that binds `UEnhancedGameplayAbility` input via Enhanced Input, plus common GAS helpers that aren't Blueprint-friendly (or missing) on stock UE 5.x.

#### Input binding

| API | Purpose |
|---|---|
| **Bind Ability Inputs** | Re-bind every granted ability's Input Actions. Call from `SetupPlayerInputComponent`. |
| **Remap Ability Input Action** | Change the activate Input Action on the primary instance (never the CDO). The remap survives `BindAbilityInputs`; call it on the owning client. Null primary + null confirm/cancel clears binds. |
| **Clear Input Binding** | Drop Enhanced Input binds for a **spec handle**. |
| **Clear Ability Input Binding** | Same, looked up by **exact ability asset tag**. |
| **Set Input Binding** | Low-level bind (C++). All-null actions → clear. |

Binding details:

- **Activate** - `Triggered` after the Input Action's configured triggers succeed; first frame only until `Completed` / `Canceled` releases it
- **Confirm / Cancel** - `Started` once per press, ignored while that ability spec is inactive; GAS generic confirm/cancel remains ASC-wide
- Subclass hooks: override `HandleAbilityInputPressed` / `Released` / `Confirmed` / `Canceled`

#### Abilities

| API | Purpose |
|---|---|
| **Give Ability If Not Owned** | Authority only. Give ability if not already granted (by class); otherwise returns the existing handle. |
| **Get Gameplay Ability By Tag** | Exact **asset tag** match; skips `PendingRemove`; prefers primary instance. |
| **Get Gameplay Ability Spec Handle By Tag** | Same match policy; returns handle (not a full spec copy). |
| **Is Ability Active By Class / Handle / Tag** | Whether any matching granted ability is currently running (tag = exact asset tag). |
| **Cancel All Abilities** | Blueprint wrapper around `CancelAllAbilities`. |
| **Cancel Abilities With Tags** | Blueprint wrapper around `CancelAbilities` (with/without tag filters). |

#### Cooldowns & GameplayEffects

| API | Purpose |
|---|---|
| **Get Cooldown Remaining For Tag** | Longest remaining time among active effects that own any of the given tags (UI / gating). |
| **Get Active Effect Level** | Level of an active GE, or 0 if invalid. |
| **Get Active Gameplay Effect Tags** | Asset tags from active **duration** GEs that hierarchically match a tag filter (`Cooldown` includes `Cooldown.Fire`). |
| **Set Active Gameplay Effect Duration** | **Authority only.** Override / add / ±% remaining duration on one active duration GE (replicates dirty). |
| **Set Active Gameplay Effects Duration By Tag** | **Authority only.** Same, for every matching active duration GE. |
| **Get Active Effect Set By Caller Magnitude** | SetByCaller value from the first active GE containing that exact SetByCaller data tag; the GE need not also own/grant the tag. |

Duration modification modes (`EGameplayEffectDurationModification`): **Override**, **Add** (seconds), **Add Percent**, **Subtract Percent** (0–1 fractions of remaining time).

#### Loose tags

| API | Purpose |
|---|---|
| **Update Loose Gameplay Tag Count** | Single-tag helper: `Count` as **+N add / -N remove**, or absolute set when **Override** is checked. |

**Loose tags are not automatically networked.** If both server and clients need the same loose tag, update both (or use a Gameplay Effect that grants the tag). For queries use engine `Get Gameplay Tag Count` (loose + GE-granted).

### Enhanced Gameplay Ability

Abstract `UGameplayAbility` base. Defaults to **Instanced Per Actor**.

Class defaults:

| Property | Purpose |
|---|---|
| **Name / Description / Icon** | Lightweight UI metadata |
| **Input Action** | Activates the ability / drives `Wait Input Press` & `Wait Input Release` |
| **Confirm Input Action** | GAS targeting confirm (`Started`) |
| **Cancel Input Action** | GAS targeting cancel (`Started`) |
| **Activate On Granted** | Auto-activate when given, on the net-relevant side only (local predicted/local only on client; server-only/server-initiated on authority); retries when actor info later receives its avatar |
| **Cooldown Tag** | **Required** for this plugin's CD path - granted while on cooldown (drives CheckCooldown). Without it, stock `ApplyCooldown` only. |
| **Cooldown Duration** | Optional `TOptional<FScalableFloat>` SetByCaller override. **Unset** = GE's authored duration (curves by level when set) |
| **Get Cooldown Duration** | Effective total seconds at ability level: optional override if set, else static duration from the Cooldown GE when readable (`0` if infinite/unknown). Remaining time: engine **Get Cooldown Time Remaining** (uses our `GetCooldownTags`). |
```cpp
// Runtime rebind on the active instance (never mutates the CDO)
MyAbility->RemapInputAction(NewInputAction);
// Or via the ASC + ability asset tag:
ASC->RemapAbilityInputAction(AbilityTag, NewInputAction);
ASC->ClearAbilityInputBinding(AbilityTag);

// Optional: override CD seconds on the instance before CommitAbility
MyAbility->SetCooldownDuration(4.f);
MyAbility->ClearCooldownDuration(); // back to unset → stock GE path
```

#### Cooldowns (required tag + optional duration)

| Setup | Behavior |
|---|---|
| **Cooldown Tag** (required) | Apply Cooldown GE and inject the tag (asset + granted) for `CheckCooldown` / remaining time |
| **+ Cooldown Duration** set | GE must be **Set By Caller**; duration from the optional `FScalableFloat` |
| **Cooldown Duration** unset | GE's own duration magnitude (ScalableFloat, etc.) |
| **No Cooldown Tag** | Stock `UGameplayAbility::ApplyCooldown` only (no per-ability tag) |

Shared Cooldown GE, many abilities:

1. Assign a Cooldown GE (SetByCaller duration if you want per-ability lengths).
2. Set **Cooldown Tag** per ability (required), e.g. `Cooldown.Dash`.
3. Optionally set **Cooldown Duration** for a per-ability length override.

## Notes

- Only abilities that subclass **Enhanced Gameplay Ability** get input binding. Plain `UGameplayAbility` still works with GAS; they just won't auto-bind Input Actions.
- Local player only: bindings attach to the avatar pawn's `UEnhancedInputComponent` when it is locally controlled.
- Attribute sets, project gameplay tags, and locomotion Input Actions stay in your game module - this plugin is the ability ↔ Enhanced Input bridge, not a full gameplay framework.
