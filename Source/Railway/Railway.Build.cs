using UnrealBuildTool;

public class Railway : ModuleRules
{
    public Railway(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PrivateDependencyModuleNames.AddRange(new[] { "AIModule", "Slate", "SlateCore", "InputCore" });
        PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine", "UMG" });
    }
}
