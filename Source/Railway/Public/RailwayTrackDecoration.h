#pragma once

#include "RailwayTrack.h"
#include "RailwayTrackDecoration.generated.h"

class UStaticMeshComponent;

/** A streamed world prop positioned from an editor track anchor. */
UCLASS(Blueprintable, meta=(DisplayName="Railway Track Decoration"))
class RAILWAY_API ARailwayTrackDecoration : public ARailwayTrackAnchor
{
    GENERATED_BODY()

public:
    ARailwayTrackDecoration();

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Railway|Decoration")
    TObjectPtr<UStaticMeshComponent> DecorationMesh;

#if WITH_EDITORONLY_DATA
    /** Local offset from the track frame: X forward, Y right, Z up. Scale applies to the prop. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Railway|Decoration")
    FTransform PlacementOffset = FTransform::Identity;
#endif

    /** Editor placement hook. The actor already has its aligned pose and PlacementOffset.
     * Adjust its transform directly here. TrackTransform is the unoffset world frame. */
    UFUNCTION(BlueprintNativeEvent, Category="Railway|Decoration", meta=(DisplayName="On Track Aligned"))
    void OnTrackAligned(const FTransform& TrackTransform);
    virtual void OnTrackAligned_Implementation(const FTransform& TrackTransform);

#if WITH_EDITOR
    UFUNCTION(CallInEditor, Category="Railway|Decoration")
    void RefreshDecorationPlacement();

    virtual void OnConstruction(const FTransform& Transform) override;
protected:
    virtual void RegisterActorTickFunctions(bool bRegister) override;
    virtual void ApplyAnchorPose(const FTransform& TrackPose) override;
    virtual FVector GetAnchorProjectionLocation() const override;

private:
    TWeakObjectPtr<ARailwayTrack> LastHost;
    TOptional<FTransform> LastAlignedPose;
    FTransform LastTrackFrame = FTransform::Identity;
    FTransform LastPlacedPose = FTransform::Identity;
    bool bApplyingPlacement = false;
#endif
};
