#pragma once
#include <source_location>
#include <string_view>
namespace fatfish::diagnostics {
void initialize() noexcept;
void record(std::string_view category, std::string_view message,
            std::source_location source=std::source_location::current()) noexcept;
void caught(std::source_location source=std::source_location::current()) noexcept;
}
