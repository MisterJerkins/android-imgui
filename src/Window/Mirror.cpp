#include "Window/Mirror.h"

#include "Utils/Utils.h"

#include <algorithm>
#include <optional>

std::optional<std::string_view> Mirror::Field(std::string_view device, std::string_view name) {
    while (!device.empty()) {
        const size_t end = device.find('\n');
        const std::string_view line = Utils::Trim(device.substr(0, end));
        if (line.starts_with(name)) {
            return line.substr(name.size());
        }
        if (end == std::string_view::npos) {
            break;
        }
        device.remove_prefix(end + 1);
    }
    return std::nullopt;
}

std::optional<std::array<float, 2>> Mirror::RectSize(std::string_view rect) {
    std::vector<float> numbers;
    size_t start = 0;
    const auto digit = [](char letter) { return Utils::IsDigit(letter) || letter == '-'; };
    for (size_t index = 0; index <= rect.size(); index++) {
        if (index < rect.size() && digit(rect[index])) {
            continue;
        }
        const std::string_view part = rect.substr(start, index - start);
        start = index + 1;
        if (part.empty() || part == "-") {
            continue;
        }
        if (const auto number = Utils::ParseNumber<int64_t>(part)) {
            numbers.push_back(static_cast<float>(*number));
        }
    }
    if (numbers.size() != 4) {
        return std::nullopt;
    }
    const std::array<float, 2> size{numbers[2] - numbers[0], numbers[3] - numbers[1]};
    if (size[0] <= 0.0f || size[1] <= 0.0f) {
        return std::nullopt;
    }
    return size;
}

std::vector<ShownDisplay> Mirror::ParseDisplays(std::string_view dump) {
    constexpr std::string_view DeviceMark = "DisplayDeviceInfo{";
    std::vector<ShownDisplay> shown;
    size_t at = dump.find(DeviceMark);
    while (at != std::string_view::npos) {
        const size_t next = dump.find(DeviceMark, at + DeviceMark.size());
        const std::string_view device = dump.substr(at + DeviceMark.size(), next == std::string_view::npos ? next : next - at - DeviceMark.size());
        const auto stack = Field(device, "mCurrentLayerStack=").and_then([](std::string_view text) { return Utils::ParseNumber<uint32_t>(Utils::Trim(text)); });
        const auto size = Field(device, "mCurrentLayerStackRect=Rect(").and_then(RectSize);
        if (stack && size) {
            shown.push_back({*stack, *size});
        }
        at = next;
    }
    return shown;
}

MirrorPlacement Mirror::Fit(std::array<float, 2> source, std::array<float, 2> target) {
    const float scale = std::min(target[0] / source[0], target[1] / source[1]);
    return {scale, {(target[0] - source[0] * scale) / 2.0f, (target[1] - source[1] * scale) / 2.0f}};
}
