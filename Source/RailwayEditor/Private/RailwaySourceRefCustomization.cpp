#include "RailwaySourceRefCustomization.h"

#include "DetailWidgetRow.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "PropertyHandle.h"
#include "RailwayDataActor.h"
#include "RailwayTrain.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "RailwaySourceRef"

namespace
{
const FRailwayNetworkData* NetworkFor(const IPropertyHandle& Handle)
{
    TArray<UObject*> Owners;
    Handle.GetOuterObjects(Owners);
    const ARailwayDataActor* Common = nullptr;
    for (UObject* Object : Owners)
    {
        const ARailwayTrain* Train = Cast<ARailwayTrain>(Object);
        const ARailwayDataActor* Data = Train ? Train->GetRailwayData() : nullptr;
        if (!Data || (Common && Common != Data)) return nullptr;
        Common = Data;
    }
    return Common ? &Common->Network : nullptr;
}

FString SourceLabel(const FRailwayNetworkData& Data, FGuid Id)
{
    if (const int32* I = Data.TrackById.Find(Id))
        return FString::Printf(TEXT("Track / %s"), *Data.Tracks[*I].Name.ToString());
    if (const int32* I = Data.StationById.Find(Id))
    {
        const FRailwayBakedStation& S = Data.Stations[*I];
        const auto& Track = Data.Tracks[Data.Edges[S.EdgeIndex].TrackIndex];
        return FString::Printf(TEXT("Station / %s  (%s, %.1f cm)"), *S.Definition.Name.ToString(),
            *Track.Name.ToString(), S.Definition.TrackDistance);
    }
    return FString::Printf(TEXT("Missing source (%s)"), *Id.ToString());
}
}

TSharedRef<IPropertyTypeCustomization> FRailwaySourceRefCustomization::MakeInstance()
{
    return MakeShared<FRailwaySourceRefCustomization>();
}

void FRailwaySourceRefCustomization::CustomizeHeader(TSharedRef<IPropertyHandle> Handle, FDetailWidgetRow& Row,
    IPropertyTypeCustomizationUtils&)
{
    const TSharedRef<IPropertyHandle> Id = Handle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FRailwaySourceRef, Id)).ToSharedRef();
    Row.NameContent()[Handle->CreatePropertyNameWidget()];
    Row.ValueContent().MinDesiredWidth(240)
    [
        SNew(SComboButton)
        .OnGetMenuContent_Lambda([Handle, Id]()
        {
            FMenuBuilder Menu(true, nullptr);
            auto Add = [&](FGuid Value, const FText& Label)
            {
                Menu.AddMenuEntry(Label, FText::FromString(Value.ToString()), FSlateIcon(),
                    FUIAction(FExecuteAction::CreateLambda([Id, Value]
                    {
                        Id->SetValueFromFormattedString(Value.ToString(EGuidFormats::Digits));
                    })));
            };
            Add(FGuid(), LOCTEXT("Clear", "None"));
            if (const auto* Data = NetworkFor(*Handle); Data && Data->NetworkId.IsValid())
            {
                TArray<FGuid> Options;
                Data->TrackById.GenerateKeyArray(Options);
                if (!Handle->HasMetaData(TEXT("RailwayTracksOnly")))
                    for (const auto& Pair : Data->StationById) Options.Add(Pair.Key);
                TArray<TPair<FString, FGuid>> Labels;
                for (FGuid Value : Options) Labels.Emplace(SourceLabel(*Data, Value), Value);
                Labels.Sort([](const auto& A, const auto& B)
                {
                    return A.Key == B.Key ? A.Value < B.Value : A.Key < B.Key;
                });
                for (int32 I = 0; I < Labels.Num(); ++I)
                {
                    const auto& Item = Labels[I];
                    const bool Duplicate = (I && Labels[I - 1].Key == Item.Key) ||
                        (I + 1 < Labels.Num() && Labels[I + 1].Key == Item.Key);
                    Add(Item.Value, FText::FromString(Duplicate ? Item.Key + TEXT(" / ") + Item.Value.ToString() : Item.Key));
                }
            }
            else
            {
                Menu.AddWidget(SNew(STextBlock).Text(LOCTEXT("NoNetwork", "Select a train with a built railway network.")), FText::GetEmpty());
            }
            return Menu.MakeWidget();
        })
        .ButtonContent()
        [
            SNew(STextBlock).Text_Lambda([Handle, Id]()
            {
                FString Raw;
                if (Id->GetValueAsFormattedString(Raw) == FPropertyAccess::MultipleValues) return LOCTEXT("Multiple", "Multiple values");
                FGuid Value;
                if (!FGuid::Parse(Raw, Value) || !Value.IsValid()) return LOCTEXT("Choose", "Select a railway source");
                const auto* Data = NetworkFor(*Handle);
                return FText::FromString(Data ? SourceLabel(*Data, Value) : Value.ToString());
            })
        ]
    ];
}
#undef LOCTEXT_NAMESPACE
