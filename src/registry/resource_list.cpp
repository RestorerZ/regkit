// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "registry/resource_list.h"

#include "win32/text_transform.h"
#include "win32/translation.h"

#include <algorithm>
#include <cstring>
#include <span>
#include <utility>

namespace regkit::resource_list
{
namespace
{

using value_decoder::Decoded;

// layouts follow wdm.h; partial descriptors hold a pointer sized affinity, so their size depends on the writer's architecture
constexpr size_t kPartialHeader = 4;
constexpr size_t kRequirementSize = 32;
constexpr size_t kDeviceSpecificLimit = 256;
constexpr USHORT kInterruptLatched = 0x1;
constexpr USHORT kInterruptMessage = 0x2;
constexpr USHORT kDmaV3 = 0x100;
constexpr USHORT kMemoryLarge40 = 0x200;
constexpr USHORT kMemoryLarge48 = 0x400;
constexpr USHORT kMemoryLarge64 = 0x800;

struct Reader
{
    const BYTE* data = nullptr;
    size_t size = 0;
    bool ok = true;

    template <typename T>
    T At(size_t offset) noexcept
    {
        T value{};
        if (offset > size || sizeof(T) > size - offset)
        {
            ok = false;
            return value;
        }
        std::memcpy(&value, data + offset, sizeof(T));
        return value;
    }
    unsigned long long Affinity(size_t offset, size_t width) noexcept
    {
        return width == 8 ? At<unsigned long long>(offset) : At<ULONG>(offset);
    }
};

struct Output
{
    Decoded* decoded;

    void Add(std::wstring label, std::wstring value) const
    {
        decoded->fields.push_back({std::move(label), std::move(value)});
    }
    void Gap() const
    {
        decoded->fields.push_back({});
    }
};

std::wstring Hex(unsigned long long value, int digits)
{
    wchar_t text[24] = {};
    swprintf_s(text, L"0x%0*llX", digits, value);
    return text;
}

std::wstring Number(unsigned long long value)
{
    return std::to_wstring(value);
}

const wchar_t* Lookup(std::span<const std::pair<int, const wchar_t*>> names, int value)
{
    for (const auto& [key, name] : names)
    {
        if (key == value)
        {
            return util::Tr(name);
        }
    }
    return nullptr;
}

std::wstring NameOrNumber(std::span<const std::pair<int, const wchar_t*>> names, int value)
{
    const wchar_t* name = Lookup(names, value);
    return name ? std::wstring(name) : Hex(static_cast<unsigned long long>(static_cast<unsigned int>(value)), 2);
}

constexpr std::pair<int, const wchar_t*> kInterfaceTypes[] = {
    {-1, util::TrNoop(L"Undefined")}, {0, util::TrNoop(L"Internal")}, {1, L"ISA"}, {2, L"EISA"}, {3, L"MicroChannel"},
    {4, L"TurboChannel"}, {5, L"PCI"}, {6, L"VME"}, {7, L"NuBus"}, {8, L"PCMCIA"}, {9, L"CBus"}, {10, L"MPI"}, {11, L"MPSA"},
    {12, util::TrNoop(L"Processor internal")}, {13, util::TrNoop(L"Internal power bus")}, {14, L"PnP ISA"}, {15, L"PnP"},
    {16, L"VMCS"}, {17, L"ACPI"},
};

constexpr std::pair<int, const wchar_t*> kResourceTypes[] = {
    {0, util::TrNoop(L"Null")}, {1, util::TrNoop(L"Port")}, {2, util::TrNoop(L"Interrupt")}, {3, util::TrNoop(L"Memory")},
    {4, util::TrNoop(L"DMA")}, {5, util::TrNoop(L"Device specific")}, {6, util::TrNoop(L"Bus number")},
    {7, util::TrNoop(L"Large memory")}, {0x80, util::TrNoop(L"Configuration data")}, {0x81, util::TrNoop(L"Device private")},
    {0x82, util::TrNoop(L"PC card configuration")}, {0x83, util::TrNoop(L"Multifunction card configuration")},
    {0x84, util::TrNoop(L"Connection")},
};

constexpr std::pair<int, const wchar_t*> kShareDispositions[] = {
    {0, util::TrNoop(L"Undetermined")}, {1, util::TrNoop(L"Device exclusive")}, {2, util::TrNoop(L"Driver exclusive")}, {3, util::TrNoop(L"Shared")},
};

std::wstring ResourceType(int type)
{
    return NameOrNumber(kResourceTypes, type);
}

unsigned long long LargeLength(USHORT flags, ULONG length)
{
    const int shift = (flags & kMemoryLarge64) ? 32 : (flags & kMemoryLarge48) ? 16 : (flags & kMemoryLarge40) ? 8 : 0;
    return static_cast<unsigned long long>(length) << shift;
}

// returns the bytes the descriptor occupies, including device specific data behind it
size_t DecodePartial(Reader& reader, size_t offset, size_t width, size_t descriptor, const Output& out)
{
    const BYTE type = reader.At<BYTE>(offset);
    const BYTE share = reader.At<BYTE>(offset + 1);
    const USHORT flags = reader.At<USHORT>(offset + 2);
    const size_t u = offset + kPartialHeader;
    out.Add(L"  " + std::wstring(util::Tr(L"Share")), NameOrNumber(kShareDispositions, share));
    out.Add(L"  " + std::wstring(util::Tr(L"Flags")), Hex(flags, 4));
    switch (type)
    {
    case 1:
    case 3:
        out.Add(L"  " + std::wstring(util::Tr(L"Start")), Hex(reader.At<unsigned long long>(u), 16));
        out.Add(L"  " + std::wstring(util::Tr(L"Length")), Hex(reader.At<ULONG>(u + 8), 8));
        break;
    case 7:
        out.Add(L"  " + std::wstring(util::Tr(L"Start")), Hex(reader.At<unsigned long long>(u), 16));
        out.Add(L"  " + std::wstring(util::Tr(L"Length")), Hex(LargeLength(flags, reader.At<ULONG>(u + 8)), 16));
        break;
    case 2:
        out.Add(L"  " + std::wstring(util::Tr(L"Mode")), (flags & kInterruptLatched) ? util::Tr(L"Latched") : util::Tr(L"Level sensitive"));
        if (flags & kInterruptMessage)
        {
            out.Add(L"  " + std::wstring(util::Tr(L"Message count")), Number(reader.At<USHORT>(u + 2)));
        }
        else
        {
            out.Add(L"  " + std::wstring(util::Tr(L"Level")), Number(reader.At<USHORT>(u)));
            out.Add(L"  " + std::wstring(util::Tr(L"Group")), Number(reader.At<USHORT>(u + 2)));
        }
        out.Add(L"  " + std::wstring(util::Tr(L"Vector")), Number(reader.At<ULONG>(u + 4)));
        out.Add(L"  " + std::wstring(util::Tr(L"Affinity")), Hex(reader.Affinity(u + 8, width), static_cast<int>(width * 2)));
        break;
    case 4:
        out.Add(L"  " + std::wstring(util::Tr(L"Channel")), Number(reader.At<ULONG>(u)));
        out.Add(L"  " + std::wstring((flags & kDmaV3) ? util::Tr(L"Request line") : util::Tr(L"Port")), Number(reader.At<ULONG>(u + 4)));
        break;
    case 6:
        out.Add(L"  " + std::wstring(util::Tr(L"Start")), Number(reader.At<ULONG>(u)));
        out.Add(L"  " + std::wstring(util::Tr(L"Length")), Number(reader.At<ULONG>(u + 4)));
        break;
    case 5:
        {
            const ULONG length = reader.At<ULONG>(u);
            out.Add(L"  " + std::wstring(util::Tr(L"Data size")), Number(length));
            if (length > reader.size || offset + descriptor > reader.size - length)
            {
                reader.ok = false;
                return descriptor;
            }
            out.Add(L"  " + std::wstring(util::Tr(L"Data")), util::ToHex(std::span(reader.data + offset + descriptor, length), L' ', true, kDeviceSpecificLimit));
            return descriptor + length;
        }
    case 0x84:
        out.Add(L"  " + std::wstring(util::Tr(L"Connection ID")), Hex((static_cast<unsigned long long>(reader.At<ULONG>(u + 8)) << 32) | reader.At<ULONG>(u + 4), 16));
        break;
    default:
        out.Add(L"  " + std::wstring(util::Tr(L"Data")), util::ToHex(std::span(reader.data + std::min(u, reader.size), std::min(descriptor - kPartialHeader, reader.size - std::min(u, reader.size))), L' ', true));
        break;
    }
    return descriptor;
}

size_t DecodeFull(Reader& reader, size_t offset, size_t width, int index, const Output& out)
{
    const size_t descriptor = kPartialHeader + std::max<size_t>(12, 8 + width);
    out.Add(index > 0 ? std::wstring(util::Tr(L"Full descriptor")) + L" " + std::to_wstring(index) : util::Tr(L"Full descriptor"),
            NameOrNumber(kInterfaceTypes, reader.At<LONG>(offset)));
    out.Add(util::Tr(L"Bus number"), Number(reader.At<ULONG>(offset + 4)));
    out.Add(util::Tr(L"Version"), Number(reader.At<USHORT>(offset + 8)) + L"." + Number(reader.At<USHORT>(offset + 10)));
    const ULONG count = reader.At<ULONG>(offset + 12);
    size_t position = offset + 16;
    for (ULONG partial = 0; partial < count && reader.ok; ++partial)
    {
        out.Gap();
        out.Add(std::wstring(util::Tr(L"Resource")) + L" " + std::to_wstring(partial + 1), ResourceType(reader.At<BYTE>(position)));
        position += DecodePartial(reader, position, width, descriptor, out);
        if (position > reader.size)
        {
            reader.ok = false;
        }
    }
    return position - offset;
}

size_t DecodeRequirement(Reader& reader, size_t offset, size_t width, const Output& out)
{
    static constexpr std::pair<int, const wchar_t*> kOptions[] = {
        {0, util::TrNoop(L"Required")}, {1, util::TrNoop(L"Preferred")}, {2, util::TrNoop(L"Default")}, {8, util::TrNoop(L"Alternative")},
    };
    const BYTE option = reader.At<BYTE>(offset);
    const BYTE type = reader.At<BYTE>(offset + 1);
    const USHORT flags = reader.At<USHORT>(offset + 4);
    const size_t u = offset + 8;
    out.Add(L"  " + std::wstring(util::Tr(L"Option")), NameOrNumber(kOptions, option));
    out.Add(L"  " + std::wstring(util::Tr(L"Share")), NameOrNumber(kShareDispositions, reader.At<BYTE>(offset + 2)));
    out.Add(L"  " + std::wstring(util::Tr(L"Flags")), Hex(flags, 4));
    switch (type)
    {
    case 1:
    case 3:
    case 7:
        out.Add(L"  " + std::wstring(util::Tr(L"Length")), Hex(type == 7 ? LargeLength(flags, reader.At<ULONG>(u)) : reader.At<ULONG>(u), 8));
        out.Add(L"  " + std::wstring(util::Tr(L"Alignment")), Hex(type == 7 ? LargeLength(flags, reader.At<ULONG>(u + 4)) : reader.At<ULONG>(u + 4), 8));
        out.Add(L"  " + std::wstring(util::Tr(L"Minimum address")), Hex(reader.At<unsigned long long>(u + 8), 16));
        out.Add(L"  " + std::wstring(util::Tr(L"Maximum address")), Hex(reader.At<unsigned long long>(u + 16), 16));
        break;
    case 2:
        out.Add(L"  " + std::wstring(util::Tr(L"Mode")), (flags & kInterruptLatched) ? util::Tr(L"Latched") : util::Tr(L"Level sensitive"));
        out.Add(L"  " + std::wstring(util::Tr(L"Minimum vector")), Number(reader.At<ULONG>(u)));
        out.Add(L"  " + std::wstring(util::Tr(L"Maximum vector")), Number(reader.At<ULONG>(u + 4)));
        out.Add(L"  " + std::wstring(util::Tr(L"Group")), Number(reader.At<USHORT>(u + 10)));
        out.Add(L"  " + std::wstring(util::Tr(L"Targeted processors")), Hex(reader.Affinity(u + 16, width), static_cast<int>(width * 2)));
        break;
    case 4:
        out.Add(L"  " + std::wstring(util::Tr(L"Minimum channel")), Number(reader.At<ULONG>(u)));
        out.Add(L"  " + std::wstring(util::Tr(L"Maximum channel")), Number(reader.At<ULONG>(u + 4)));
        break;
    case 6:
        out.Add(L"  " + std::wstring(util::Tr(L"Length")), Number(reader.At<ULONG>(u)));
        out.Add(L"  " + std::wstring(util::Tr(L"Minimum bus number")), Number(reader.At<ULONG>(u + 4)));
        out.Add(L"  " + std::wstring(util::Tr(L"Maximum bus number")), Number(reader.At<ULONG>(u + 8)));
        break;
    default:
        out.Add(L"  " + std::wstring(util::Tr(L"Data")), util::ToHex(std::span(reader.data + std::min(u, reader.size), std::min<size_t>(24, reader.size - std::min(u, reader.size))), L' ', true));
        break;
    }
    return kRequirementSize;
}

Decoded DecodeLayout(DWORD type, const BYTE* data, size_t size, size_t width, size_t* consumed)
{
    Decoded decoded;
    Reader reader{data, size};
    const Output out{&decoded};
    size_t position = 0;
    if (type == REG_FULL_RESOURCE_DESCRIPTOR)
    {
        position = DecodeFull(reader, 0, width, 0, out);
    }
    else if (type == REG_RESOURCE_LIST)
    {
        const ULONG count = reader.At<ULONG>(0);
        position = 4;
        for (ULONG index = 0; index < count && reader.ok; ++index)
        {
            if (index > 0)
            {
                out.Gap();
            }
            position += DecodeFull(reader, position, width, static_cast<int>(index + 1), out);
        }
    }
    else
    {
        const ULONG list_size = reader.At<ULONG>(0);
        out.Add(util::Tr(L"Interface"), NameOrNumber(kInterfaceTypes, reader.At<LONG>(4)));
        out.Add(util::Tr(L"Bus number"), Number(reader.At<ULONG>(8)));
        out.Add(util::Tr(L"Slot number"), Number(reader.At<ULONG>(12)));
        const ULONG lists = reader.At<ULONG>(28);
        position = 32;
        for (ULONG list = 0; list < lists && reader.ok; ++list)
        {
            out.Gap();
            out.Add(std::wstring(util::Tr(L"Alternative list")) + L" " + std::to_wstring(list + 1), util::Tr(L"Version") + std::wstring(L" ") + Number(reader.At<USHORT>(position)) + L"." + Number(reader.At<USHORT>(position + 2)));
            const ULONG count = reader.At<ULONG>(position + 4);
            position += 8;
            for (ULONG item = 0; item < count && reader.ok; ++item)
            {
                out.Add(std::wstring(util::Tr(L"Resource")) + L" " + std::to_wstring(item + 1), ResourceType(reader.At<BYTE>(position + 1)));
                position += DecodeRequirement(reader, position, width, out);
            }
        }
        reader.ok = reader.ok && position <= size && list_size <= size;
    }
    decoded.ok = reader.ok && position <= size;
    if (!decoded.ok)
    {
        decoded.fields.clear();
        decoded.error = util::Tr(L"The resource data is malformed.");
    }
    *consumed = position;
    return decoded;
}

} // namespace

bool IsResourceType(DWORD type) noexcept
{
    return type == REG_RESOURCE_LIST || type == REG_FULL_RESOURCE_DESCRIPTOR || type == REG_RESOURCE_REQUIREMENTS_LIST;
}

Decoded Decode(DWORD type, const BYTE* data, size_t size)
{
    // prefer the native layout and fall back to the other one when it doesn't account for the data exactly
    SYSTEM_INFO system = {};
    GetNativeSystemInfo(&system);
    const size_t native = system.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_INTEL ? 4 : 8;
    size_t consumed = 0;
    Decoded first = DecodeLayout(type, data, size, native, &consumed);
    if (first.ok && consumed == size)
    {
        return first;
    }
    size_t other_consumed = 0;
    Decoded other = DecodeLayout(type, data, size, native == 8 ? 4 : 8, &other_consumed);
    return other.ok && (other_consumed == size || !first.ok) ? other : first;
}

} // namespace regkit::resource_list
