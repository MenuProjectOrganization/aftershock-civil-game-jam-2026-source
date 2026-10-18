// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class JamProject : ModuleRules
{
	public JamProject(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		AddEngineThirdPartyPrivateStaticDependencies(Target, "zlib");

		PublicDependencyModuleNames.AddRange(new string[] {
			"Core",
			"CoreUObject",
			"Engine",
			"InputCore",
			"EnhancedInput",
			"AIModule",
			"NavigationSystem",
			"StateTreeModule",
			"GameplayStateTreeModule",
			"UMG",
			"Slate",
			"SlateCore",
			"GeometryCollectionEngine",
			"FieldSystemEngine",
			"ChaosSolverEngine",
			"Niagara"
		});

		PrivateDependencyModuleNames.AddRange(new string[] {
			"Json",
			"RenderCore",
			"RHI",
			"MeshDescription",
			"StaticMeshDescription",
			"PhysicsCore",
			"Chaos"
		});

		if (Target.bBuildEditor)
		{
			PrivateDependencyModuleNames.AddRange(new string[] {
				"AssetRegistry",
				"UnrealEd",
				"NiagaraEditor",
				"SlateNullRenderer",
				"ApplicationCore",
				"PlanarCut",
				"Voronoi"
			});
		}

		PublicIncludePaths.AddRange(new string[] {
			"JamProject",
			"JamProject/Aftershock",
			"JamProject/Aftershock/Public",
			"JamProject/Variant_Platforming",
			"JamProject/Variant_Platforming/Animation",
			"JamProject/Variant_Combat",
			"JamProject/Variant_Combat/AI",
			"JamProject/Variant_Combat/Animation",
			"JamProject/Variant_Combat/Gameplay",
			"JamProject/Variant_Combat/Interfaces",
			"JamProject/Variant_Combat/UI",
			"JamProject/Variant_SideScrolling",
			"JamProject/Variant_SideScrolling/AI",
			"JamProject/Variant_SideScrolling/Gameplay",
			"JamProject/Variant_SideScrolling/Interfaces",
			"JamProject/Variant_SideScrolling/UI"
		});

		// Uncomment if you are using Slate UI
		// PrivateDependencyModuleNames.AddRange(new string[] { "Slate", "SlateCore" });

		// Uncomment if you are using online features
		// PrivateDependencyModuleNames.Add("OnlineSubsystem");

		// To include OnlineSubsystemSteam, add it to the plugins section in your uproject file with the Enabled attribute set to true
	}
}
