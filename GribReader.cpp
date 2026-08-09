#include "GribReader.h"

#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <stdexcept>

namespace
{
    bool HasBytes(const std::vector<std::uint8_t>& bytes, std::size_t offset, std::size_t count)
    {
        return offset <= bytes.size() && count <= bytes.size() - offset;
    }

    std::wstring FormatReferenceTime(const std::vector<std::uint8_t>& bytes, std::size_t section)
    {
        if (!HasBytes(bytes, section, 21))
            return L"n/d";

        const auto year = static_cast<unsigned>(bytes[section + 12]) << 8 | bytes[section + 13];
        std::wstringstream stream;
        stream << std::setfill(L'0') << std::setw(4) << year << L'-'
               << std::setw(2) << static_cast<unsigned>(bytes[section + 14]) << L'-'
               << std::setw(2) << static_cast<unsigned>(bytes[section + 15]) << L' '
               << std::setw(2) << static_cast<unsigned>(bytes[section + 16]) << L':'
               << std::setw(2) << static_cast<unsigned>(bytes[section + 17]) << L" UTC";
        return stream.str();
    }

    std::wstring FormatForecastTime(const std::vector<std::uint8_t>& bytes, std::size_t section)
    {
        if (!HasBytes(bytes, section, 22))
            return L"n/d";

        const auto value = (static_cast<std::uint32_t>(bytes[section + 18]) << 24)
            | (static_cast<std::uint32_t>(bytes[section + 19]) << 16)
            | (static_cast<std::uint32_t>(bytes[section + 20]) << 8)
            | bytes[section + 21];
        static constexpr const wchar_t* units[] = { L"minuti", L"ore", L"giorni", L"mesi", L"anni", L"decenni", L"normali 30 anni", L"secoli", L"3 ore", L"6 ore", L"12 ore", L"secondi" };
        const auto unit = bytes[section + 17];
        std::wstringstream stream;
        stream << value << L' ' << (unit < std::size(units) ? units[unit] : L"unita sconosciute");
        return stream.str();
    }
}

std::vector<GribField> GribReader::ReadInventory(const std::filesystem::path& path) const
{
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input)
        throw std::runtime_error("Impossibile aprire il file GRIB.");

    const auto size = input.tellg();
    if (size <= 0)
        throw std::runtime_error("Il file GRIB e vuoto.");

    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    input.seekg(0);
    input.read(reinterpret_cast<char*>(bytes.data()), size);
    if (!input)
        throw std::runtime_error("Impossibile leggere il file GRIB.");

    std::vector<GribField> fields;
    for (std::size_t message = 0; message + 16 <= bytes.size();)
    {
        if (bytes[message] != 'G' || bytes[message + 1] != 'R' || bytes[message + 2] != 'I' || bytes[message + 3] != 'B')
        {
            ++message;
            continue;
        }

        if (bytes[message + 7] != 2)
        {
            ++message;
            continue;
        }

        const auto messageLength = ReadU64(bytes, message + 8);
        if (messageLength < 20 || messageLength > bytes.size() - message)
        {
            ++message;
            continue;
        }

        GribField field{};
        field.offset = message;
        field.length = messageLength;
        field.discipline = bytes[message + 6];
        for (std::size_t section = message + 16; section + 5 <= message + messageLength - 4;)
        {
            const auto sectionLength = ReadU32(bytes, section);
            if (sectionLength < 5 || sectionLength > message + messageLength - section)
                break;

            switch (bytes[section + 4])
            {
            case 1:
                field.referenceTime = FormatReferenceTime(bytes, section);
                break;
            case 3:
                if (sectionLength >= 38)
                {
                    field.pointCount = ReadU32(bytes, section + 6);
                    field.gridTemplate = static_cast<int>((bytes[section + 12] << 8) | bytes[section + 13]);
                    if (field.gridTemplate == 0 && sectionLength >= 40)
                    {
                        field.columns = ReadU32(bytes, section + 30);
                        field.rows = ReadU32(bytes, section + 34);
                    }
                }
                break;
            case 4:
                if (sectionLength >= 22)
                {
                    field.productTemplate = static_cast<int>((bytes[section + 7] << 8) | bytes[section + 8]);
                    field.parameterCategory = bytes[section + 9];
                    field.parameterNumber = bytes[section + 10];
                    field.forecastTime = FormatForecastTime(bytes, section);
                }
                break;
            default:
                break;
            }
            section += sectionLength;
        }

        field.parameterName = ParameterName(field.discipline, field.parameterCategory, field.parameterNumber);
        fields.push_back(std::move(field));
        message += static_cast<std::size_t>(messageLength);
    }

    if (fields.empty())
        throw std::runtime_error("Nessun messaggio GRIB2 valido trovato.");
    return fields;
}

std::uint32_t GribReader::ReadU32(const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
    if (!HasBytes(bytes, offset, 4))
        throw std::runtime_error("Sezione GRIB troncata.");
    return (static_cast<std::uint32_t>(bytes[offset]) << 24)
        | (static_cast<std::uint32_t>(bytes[offset + 1]) << 16)
        | (static_cast<std::uint32_t>(bytes[offset + 2]) << 8)
        | bytes[offset + 3];
}

std::uint64_t GribReader::ReadU64(const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
    if (!HasBytes(bytes, offset, 8))
        throw std::runtime_error("Messaggio GRIB troncato.");
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < 8; ++index)
        value = (value << 8) | bytes[offset + index];
    return value;
}

std::wstring GribReader::ParameterName(int discipline, int category, int number)
{
    if (discipline == 0 && category == 7 && number == 6)
        return L"CAPE - energia potenziale convettiva";
    if (discipline == 0 && category == 7 && number == 7)
        return L"CIN - inibizione convettiva";
    if (discipline == 0 && category == 1 && number == 8)
        return L"Precipitazione totale";
    if (discipline == 0 && category == 2 && number == 2)
        return L"Vento meridionale";
    if (discipline == 0 && category == 2 && number == 3)
        return L"Vento zonale";
    if (discipline == 0 && category == 0 && number == 0)
        return L"Temperatura";
    std::wstringstream stream;
    stream << L"Parametro D" << discipline << L"-C" << category << L"-N" << number;
    return stream.str();
}
