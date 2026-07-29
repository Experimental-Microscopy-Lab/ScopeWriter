#pragma once

#include "scopewriter/ScopeWriter.h"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace scopewriter::internal
{
    class ZarrWriter
    {
    public:
        ZarrWriter();
        ~ZarrWriter();

        ZarrWriter(const ZarrWriter&) = delete;
        ZarrWriter& operator=(const ZarrWriter&) = delete;

        bool open(const WriterSettings& settings,
                  const std::vector<std::string>& seriesNames,
                  const std::vector<std::string>& seriesMetadata,
                  std::string& error);
        bool append(int positionIndex,
                    std::int64_t t,
                    int c,
                    int z,
                    const void* data,
                    std::size_t byteCount,
                    std::string& error);
        bool close(std::string& error);

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
}
