#pragma once

#include <cstring>
#include <string_view>

#include "runtime/vmdefines.h"

namespace simlang
{

enum class BlockType : u8
{
    cFree,
    cObject,
    cList,
    cMap,
    cString
};

struct BlockHeader
{
    static constexpr u32 cElementInfoWordCountMask = 0xFFFFU;
    static constexpr u32 cElementInfoTypeIDShift = 16;

    u32 mSize = 0;                           // 4b (Header AND data.)
    TypeID mTypeID = 0;                      // 2b (For map<K, V> this is K.)
    BlockType mBlockType = BlockType::cFree; // 1b
    bool mMarked = false;                    // 1b (Could also be flags, but we only need one as of now.)
    u32 mLength = 0;                         // 4b (List/map length OR string length.)
    u32 mElementInfo = 0;                    // 4b (Element/value size (2b) and V type (2b) for maps.)

    void clearElementInfo() { mElementInfo = 0; }

    // Lists only use the element word count.

    u32 getListElementWords() const { return mElementInfo & cElementInfoWordCountMask; }
    void setListElementInfo(u32 elementWords) { mElementInfo = elementWords & cElementInfoWordCountMask; }

    // Maps use the element word count and value type ID.

    u32 getMapValueWords() const { return mElementInfo & cElementInfoWordCountMask; }
    TypeID getMapValueTypeID() const { return static_cast<TypeID>(mElementInfo >> cElementInfoTypeIDShift); }
    void setMapValueInfo(u32 valueWords, TypeID valueTypeID)
    {
        mElementInfo =
            (static_cast<u32>(valueTypeID) << cElementInfoTypeIDShift) | (valueWords & cElementInfoWordCountMask);
    }
};

static_assert(sizeof(BlockHeader) % sizeof(VMWord) == 0, "Heap block header is not aligned with VM word size!");
static_assert(sizeof(TypeID) <= sizeof(u16), "Packed heap map value type ID expects a 16 bit TypeID.");

inline constexpr u32 cBlockHeaderWords = sizeof(BlockHeader) / sizeof(VMWord);

struct Block
{
    BlockHeader* mHeader = nullptr;
    VMWord* mPayload = nullptr;
};

struct StringBlock
{
    BlockHeader* mHeader = nullptr;
    VMWord* mPayload = nullptr;

    // Convert byte length to payload word count.
    static u64 getPayloadFromBytes(u32 byteLen)
    {
        // Add null.
        u64 byteLenWithNull = static_cast<u64>(byteLen) + 1U;
        // Then ceil to the next VM word and figure out how many words we need to fit the bytes.
        return (byteLenWithNull + sizeof(VMWord) - 1U) / sizeof(VMWord);
    }

    static char* getBytes(VMWord* payload) { return reinterpret_cast<char*>(payload); }

    std::string_view getView() const { return std::string_view{getBytes(mPayload), mHeader->mLength}; }
};

struct ListBlock
{
    BlockHeader* mHeader = nullptr;
    VMWord* mPayload = nullptr;

    // List structure: [elem0Word0, elem0Word1, ..., elem0WordN, elem1Word0, ...]

    u32 getElementWords() const { return mHeader->getListElementWords(); }
    // The capacity is the number of elements we can fit in the payload.
    u32 getCapacity() const { return (mHeader->mSize - cBlockHeaderWords) / getElementWords(); }

    VMWord* getElement(u32 index) { return mPayload + static_cast<usize>(index) * getElementWords(); }
    const VMWord* getElement(u32 index) const { return mPayload + static_cast<usize>(index) * getElementWords(); }

    // The append slot is the slot after the last element (which is the current length).
    VMWord* getAppendSlot() { return getElement(mHeader->mLength); }

    // Makes a new slot at the given index, shifting all elements after it to the right.
    // Assumes you checked space before!
    VMWord* insertSlot(u32 index)
    {
        u32 elementWords = getElementWords();
        VMWord* slot = getElement(index);

        // The tail words start at the element we're targeting.
        u32 tailWords = (mHeader->mLength - index) * elementWords;
        if (tailWords != 0)
        {
            std::memmove(slot + elementWords, slot, static_cast<usize>(tailWords) * sizeof(VMWord));
        }

        ++mHeader->mLength;

        return slot;
    }

    // Removes the slot at the given index, shifting all elements after it to the left.
    // Assumes you checked space before!
    void removeSlot(u32 index)
    {
        u32 elementWords = getElementWords();
        VMWord* slot = getElement(index);

        // The tail words start after the element we're targeting.
        u32 tailWords = (mHeader->mLength - index - 1U) * elementWords;
        if (tailWords != 0)
        {
            std::memmove(slot, slot + elementWords, static_cast<usize>(tailWords) * sizeof(VMWord));
        }

        --mHeader->mLength;
    }

    void clear() { mHeader->mLength = 0; }
};

inline constexpr VMWord cMapBucketEmpty = 0;
inline constexpr VMWord cMapBucketOccupiedBit = 0x80000000U;
inline constexpr VMWord cMapHashMask = 0x7FFFFFFFU;
inline constexpr u32 cMapControlWords = 1;
inline constexpr u32 cMapKeyWords = 1;

struct MapBlock
{
    BlockHeader* mHeader = nullptr;
    VMWord* mPayload = nullptr;

    // Bucket structure: [control, key, valueWord0, valueWord1, ..., valueWordN]
    // We currently only have 1 word keys.
    // The control word stores the occupied bit plus the stored 31-bit key hash.
    u32 getValueWords() const { return mHeader->getMapValueWords(); }
    u32 getBucketWords() const { return cMapControlWords + cMapKeyWords + getValueWords(); }

    // The payload is the size of the block minus the header.
    u32 getPayloadWords() const { return mHeader->mSize - cBlockHeaderWords; }

    // The bucket capacity is the payload divided by the bucket size.
    u32 getBucketCapacity() const { return getPayloadWords() / getBucketWords(); }
    // We force a resize on reaching half of the bucket count, so the capacity is that.
    u32 getEntryCapacity() const { return getBucketCapacity() / 2U; }

    VMWord* getBucket(u32 index) { return mPayload + static_cast<usize>(index) * getBucketWords(); }
    const VMWord* getBucket(u32 index) const { return mPayload + static_cast<usize>(index) * getBucketWords(); }

    VMWord* getValue(u32 index) { return getBucket(index) + cMapControlWords + cMapKeyWords; }
    const VMWord* getValue(u32 index) const { return getBucket(index) + cMapControlWords + cMapKeyWords; }

    // Insert an entry into the bucket at the given index.
    void insertEntry(u32 index, u32 hash, VMWord key)
    {
        // Get the bucket at the given index.
        VMWord* bucket = getBucket(index);
        // The first word is the control word.
        bucket[0] = makeControl(hash);
        // The second word is the hash (key).
        bucket[1] = key;
        // We added a new entry.
        ++mHeader->mLength;
    }

    // Just zero out everything.
    void clearBuckets() { std::memset(mPayload, 0, static_cast<usize>(getPayloadWords()) * sizeof(VMWord)); }

    // Insert an occupied bucket into the map.
    void insertOccupiedBucket(const VMWord* sourceBucket)
    {
        u32 bucketCapacity = getBucketCapacity();
        // Strip the control bit, then use the hash to find the index of the bucket we're targeting.
        u32 index = getHash(sourceBucket[0]) % bucketCapacity;
        // Get the bucket at the given index.
        VMWord* bucket = getBucket(index);
        // If it's already in use, advance the index and try again.
        while (isOccupied(bucket[0]))
        {
            index = advanceBucketIndex(index, bucketCapacity);
            bucket = getBucket(index);
        }

        // If we're here, we found an empty bucket.
        // Copy the entire bucket (control, key, value) in.
        std::memcpy(bucket, sourceBucket, static_cast<usize>(getBucketWords()) * sizeof(VMWord));
    }

    template <typename Fn>
    void forEachOccupiedBucket(Fn fn) const
    {
        // Simple iterator calling a function for all buckets that have the control bit set.
        u32 bucketCapacity = getBucketCapacity();
        for (u32 i = 0; i < bucketCapacity; ++i)
        {
            const VMWord* bucket = getBucket(i);
            if (isOccupied(bucket[0]))
            {
                fn(i, bucket);
            }
        }
    }

    void copyOccupiedBucketsTo(MapBlock& dst) const
    {
        // Copies all occupied buckets from this map to the destination map.
        forEachOccupiedBucket(
            [&](u32, const VMWord* bucket)
            {
                dst.insertOccupiedBucket(bucket);
            });
    }

    void removeEntry(u32 index)
    {
        u32 bucketCapacity = getBucketCapacity();
        if (bucketCapacity == 0)
        {
            return;
        }

        // Otherwise, find the bucket at the given index and remove it.
        // Now in case we have collisions, we need to find the next available bucket to fill the hole.
        // After that, we need to keep going until we find an empty one.
        // Example:
        // A ideally belongs at 2, stored at 2
        // B ideally belongs at 2, stored at 3
        // C ideally belongs at 3, stored at 4
        // Delete 2: We need to move B to 2, then C to 3.
        // Otherwise, subsequent lookups will fail.

        u32 bucketWords = getBucketWords();
        u32 hole = index;
        u32 current = advanceBucketIndex(hole, bucketCapacity);
        VMWord* currentBucket = getBucket(current);

        // While we're occupied, keep going.
        while (isOccupied(currentBucket[0]))
        {
            // Get the target index for the current bucket.
            u32 idealIndex = getHash(currentBucket[0]) % bucketCapacity;
            u32 currentDistance = getProbeDistance(idealIndex, current, bucketCapacity);
            u32 holeDistance = getProbeDistance(idealIndex, hole, bucketCapacity);
            // If the hole is closer to the ideal index than the current bucket, move the current bucket to the hole.
            // That will make it as close to the ideal index as possible.
            if (holeDistance < currentDistance)
            {
                std::memmove(getBucket(hole), currentBucket, static_cast<usize>(bucketWords) * sizeof(VMWord));
                // The hole is now the current bucket.
                hole = current;
            }

            // Then, move to the next bucket
            current = advanceBucketIndex(current, bucketCapacity);
            currentBucket = getBucket(current);
        }

        // If we reach a bucket that is empty, clear whatever the hole is and we're done.
        std::memset(getBucket(hole), 0, static_cast<usize>(bucketWords) * sizeof(VMWord));
        // We removed an entry.
        --mHeader->mLength;
    }

    template <typename KeyEqual>
    bool findEntry(u32 hash, KeyEqual keyEqual, u32& outIndex, bool& outFound) const
    {
        // This one is not so different to the above.

        u32 bucketCapacity = getBucketCapacity();
        outIndex = bucketCapacity;
        outFound = false;
        if (bucketCapacity == 0)
        {
            outIndex = 0;
            return true;
        }

        // Find the index, tag the hash with the control bit.
        u32 index = hash % bucketCapacity;
        VMWord expectedControl = makeControl(hash);

        // We'll need at most bucketCapacity probes (as we can't have more entries in the map).
        for (u32 probes = 0; probes < bucketCapacity; ++probes)
        {
            // Get the bucket and check the control (first word).
            const VMWord* bucket = getBucket(index);
            VMWord control = bucket[0];
            if (control == cMapBucketEmpty)
            {
                // We didn't find the key at the index, but we could do an insert here.
                outIndex = index;
                return true;
            }

            // Otherwise, we found the key.
            // Check whether the hash matches.
            if (control == expectedControl)
            {
                // If it does, compare the key with the comparator.
                bool equal = false;
                if (keyEqual(bucket[1], equal) == false)
                {
                    // If we're here, something went wrong in the comparison and we bail.
                    return false;
                }

                // Check the outcome.
                if (equal)
                {
                    // We found the key.
                    outIndex = index;
                    outFound = true;
                    return true;
                }
            }

            // We either didn't find the expected control or had a hash collision, so try the next one.
            index = advanceBucketIndex(index, bucketCapacity);
        }

        return true;
    }

private:
    static u32 advanceBucketIndex(u32 index, u32 capacity)
    {
        // Wrap around if we've reached the end of the bucket array.
        ++index;
        return index == capacity ? 0U : index;
    }

    static VMWord makeControl(u32 hash) { return cMapBucketOccupiedBit | (hash & cMapHashMask); }

    static bool isOccupied(VMWord control) { return (control & cMapBucketOccupiedBit) != 0; }

    static u32 getHash(VMWord control) { return control & cMapHashMask; }

    static u32 getProbeDistance(u32 idealIndex, u32 index, u32 capacity)
    {
        return index >= idealIndex ? index - idealIndex : capacity - idealIndex + index;
    }
};

} // namespace simlang
