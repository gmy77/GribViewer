#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

struct GribField
{
    std::uint64_t offset{};
    std::uint64_t length{};
    int discipline{};
    int parameterCategory{};
    int parameterNumber{};
    int productTemplate{};
    int gridTemplate{};
    std::uint32_t pointCount{};
    std::uint32_t columns{};
    std::uint32_t rows{};
    std::wstring referenceTime;
    std::wstring forecastTime;
    std::wstring parameterName;
};

class GribReader
{
public:
    std::vector<GribField> ReadInventory(const std::filesystem::path& path) const;

private:
    static std::uint32_t ReadU32(const std::vector<std::uint8_t>& bytes, std::size_t offset);
    static std::uint64_t ReadU64(const std::vector<std::uint8_t>& bytes, std::size_t offset);
    static std::wstring ParameterName(int discipline, int category, int number);
};
