// Copyright Solessfir. All Rights Reserved.

using UnrealBuildTool;

public class EnhancedAbilitySystem : ModuleRules
{
	public EnhancedAbilitySystem(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(
		[
			"Core",
			"CoreUObject",
			"Engine",
			"GameplayAbilities",
			"GameplayTags",
			"GameplayTasks",
			"EnhancedInput",
		]);

		PrivateDependencyModuleNames.Add("NetCore");
	}
}
