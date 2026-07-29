#include <scopewriter/ScopeWriter.h>

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <vector>

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        return 2;
    }

    const std::filesystem::path outputPath = argv[1];
    std::error_code ignored;
    std::filesystem::remove(outputPath, ignored);

    scopewriter::WriterSettings settings;
    settings.format = scopewriter::Format::OmeTiff;
    settings.outputPath = outputPath;
    settings.width = 2;
    settings.height = 2;
    settings.pixelType = scopewriter::PixelType::UInt16;
    settings.significantBits = 12;
    settings.timeCount = 1;
    settings.enableCompression = false;

    std::vector<std::uint16_t> frame{1, 2, 3, 4};
    scopewriter::Writer writer;
    if (!writer.open(settings)
        || !writer.append(frame.data(), frame.size() * sizeof(std::uint16_t))
        || !writer.close())
    {
        std::cerr << writer.lastError() << '\n';
        return 1;
    }
    if (!std::filesystem::is_regular_file(outputPath))
    {
        return 1;
    }
    std::filesystem::remove(outputPath, ignored);
    return 0;
}
