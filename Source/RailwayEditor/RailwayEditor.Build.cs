using UnrealBuildTool;

public class RailwayEditor : ModuleRules
{
    public RailwayEditor(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PrivateDependencyModuleNames.AddRange(new[] {
            "Core", "CoreUObject", "Engine", "Railway", "UnrealEd", "ToolMenus", "Slate", "SlateCore"
        });
    }
}
