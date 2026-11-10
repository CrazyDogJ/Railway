#pragma once

#include "IPropertyTypeCustomization.h"

class FRailwaySourceRefCustomization final : public IPropertyTypeCustomization
{
public:
    static TSharedRef<IPropertyTypeCustomization> MakeInstance();

    virtual void CustomizeHeader(TSharedRef<IPropertyHandle> Handle, FDetailWidgetRow& Row,
        IPropertyTypeCustomizationUtils& Utils) override;
    virtual void CustomizeChildren(TSharedRef<IPropertyHandle>, IDetailChildrenBuilder&,
        IPropertyTypeCustomizationUtils&) override {}
};
