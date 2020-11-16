#ifndef NONSTUPIDBITFIELD_H
#define NONSTUPIDBITFIELD_H

#include "types.h"

#include <memory.h>

#include <initializer_list>
#include <algorithm>

// like std::bitset but less stupid and optimised for 
// our use case (keeping track of memory invalidations)

template <u32 Size>
class NonStupidBitField
{
    static_assert((Size % 64) == 0);

public:
    static const u32 DataLength = Size >> 6;
    u64 Data[DataLength];

    struct Ref
    {
        NonStupidBitField& BitField;
        u32 Idx;

    public:
        operator bool()
        {
            return BitField.Data[Idx >> 6] & (1ULL << (Idx & 0x3F));
        }

        Ref& operator=(bool set)
        {
            BitField.Data[Idx >> 6] &= ~(1ULL << (Idx & 0x3F));
            BitField.Data[Idx >> 6] |= ((u64)set << (Idx & 0x3F));
            return *this;
        }
    };

    struct Iterator
    {
        NonStupidBitField& BitField;
        u64 RemainingBits;
        u32 DataIdx;
        u32 Idx;

    public: 
        u32 operator*() { return DataIdx * 64 + Idx; }

        Iterator& operator=(bool set)
        {
            BitField.Data[DataIdx] &= ~(1ULL << Idx);
            BitField.Data[DataIdx] |= ((u64)set << Idx);
            return *this;
        }
        
        bool operator==(Iterator other) { return DataIdx == other.DataIdx; }
        bool operator!=(Iterator other) { return DataIdx != other.DataIdx; }

        Iterator operator++(int)
        {
            Iterator prev(*this);
            ++*this;
            return prev;
        }

        Iterator& operator++()
        {
            while (RemainingBits == 0)
            {
                DataIdx++;
                if (DataIdx >= DataLength)
                    break;
                RemainingBits = BitField.Data[DataIdx];
            }

            if (DataIdx < DataLength)
            {
                Idx = __builtin_ctzll(RemainingBits);
                RemainingBits &= ~(1ULL << Idx);
            }

            return *this;
        }
    };

    static const u32 _Size = Size;

    NonStupidBitField()
    {
        memset(Data, 0, sizeof(Data));
    }

    NonStupidBitField(u32 start, u32 size)
    {
        memset(Data, 0, sizeof(Data));
        
        if (size == 0)
            return;

        u32 startIdx = start >> 6;
        u32 endIdx = (start + size - 1) >> 6;
        
        if (startIdx == endIdx)
        {
            Data[startIdx] = ~(0xFFFFFFFFFFFFFFFFULL << (size & 0x3F)) << (start & 0x3F);
        }
        else
        {
            for (u32 i = startIdx; i <= endIdx; i++)
            {
                if (i == startIdx)
                    Data[i] = 0xFFFFFFFFFFFFFFFFULL << (start & 0x3F);
                else if (i == endIdx)
                    Data[i] = (start + size) & 0x3F ? ~(0xFFFFFFFFFFFFFFFFULL << ((start + size) & 0x3F)) : 0xFFFFFFFFFFFFFFFFULL;
                else
                    Data[i] = 0xFFFFFFFFFFFFFFFFULL;
            }
        }
    }

    NonStupidBitField(const std::initializer_list<u64>& values)
    {
        for (u32 i = 0; i < DataLength; i++)
        {
            Data[i] = i < values.size() ? (values.begin() + i) : 0;
        }
    }

    Iterator End()
    {
        return {*this, 0, DataLength, 0};
    }

    Iterator Begin()
    {
        for (u32 i = 0; i < DataLength; i++)
        {
            if (Data[i])
            {
                u32 firstIdx = __builtin_ctzll(Data[i]);
                return {*this, Data[i] & ~(1ULL << firstIdx), i, firstIdx};
            }
        }
        return End();
    }

    Ref operator[](u32 idx)
    {
        return Ref{*this, idx};
    }

    NonStupidBitField& operator|=(const NonStupidBitField<Size>& other)
    {
        for (u32 i = 0; i < DataLength; i++)
        {
            Data[i] |= other.Data[i];
        }
        return *this;
    }
    NonStupidBitField& operator&=(const NonStupidBitField<Size>& other)
    {
        for (u32 i = 0; i < DataLength; i++)
        {
            Data[i] &= other.Data[i];
        }
        return *this;
    }
};


#endif