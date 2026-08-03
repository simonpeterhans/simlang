#include "backend/codegen/place.h"

#include "backend/layout/layout.h"

namespace simlang
{

Place Place::makeLocalPlace(Type* type, LocalIdx localIdx)
{
    Place place;
    place.mType = type;
    place.setLocalOffset(localIdx);
    place.mWords = layout::getWordSizeForType(type);
    return place;
}

Place Place::makeGlobalPlace(Type* type, GlobalIdx globalIdx)
{
    Place place;
    place.mType = type;
    place.setGlobalOffset(globalIdx);
    place.mWords = layout::getWordSizeForType(type);
    return place;
}

Place Place::makeAddressOnStackPlace(Type* type)
{
    Place place;
    place.mKind = Kind::cAddressOnStack;
    place.mType = type;
    place.mWords = layout::getWordSizeForType(type);
    return place;
}

Place Place::derive(Type* type, VMAddress offset) const
{
    // Return a copy of this with a different type and offset.
    // That means we keep the kind.
    // Convenient if you have a place of an aggregate and need one for a field.
    Place place = *this;
    place.mType = type;
    place.mWords = layout::getWordSizeForType(type);
    place.addOffset(offset);
    return place;
}

} // namespace simlang
