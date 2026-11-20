#pragma once

#include <Calder/Calder.h>

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace ByteLayoutExamples
{

struct Fish
{
    int id = 0;
    std::string name;
    std::vector<std::string> foo;

    MIRO_REFLECT(id, name, foo)
};

struct Person
{
    int id = 0;
    std::string name;
    std::vector<Fish> fish;

    MIRO_REFLECT(id, name, fish)
};

inline constexpr std::string_view personSource = R"(struct Fish
{
    int id = 0;
    std::string name;
    std::vector<std::string> foo;
    MIRO_REFLECT(id, name, foo)
};

struct Person
{
    int id = 0;
    std::string name;
    std::vector<Fish> fish;
    MIRO_REFLECT(id, name, fish)
};

Person {7, "Jamie", {{1, "Nemo", {"orange", "stripy"}},
                     {-2, "Dory", {"blue"}}}})";

struct OptionalFields
{
    std::optional<int> engaged = 42;
    std::optional<int> disengaged;
    std::optional<std::vector<float>> engagedSamples =
        std::vector<float> {0.5f, 1.25f};
    std::optional<std::vector<float>> disengagedSamples;

    MIRO_REFLECT(engaged, disengaged, engagedSamples, disengagedSamples)
};

inline constexpr std::string_view optionalSource = R"(struct OptionalFields
{
    std::optional<int> engaged = 42;
    std::optional<int> disengaged;
    std::optional<std::vector<float>> engagedSamples =
        std::vector<float> {0.5f, 1.25f};
    std::optional<std::vector<float>> disengagedSamples;
    MIRO_REFLECT(engaged, disengaged,
                 engagedSamples, disengagedSamples)
};)";

// The same eight numbers in every array, so only the element type differs.
inline constexpr std::array<int, 8> sharedNumbers {3, 14, 15, 92, 65, -35, 89, 300};

struct SameNumbersSevenWays
{
    std::vector<float> floats;
    std::vector<double> doublesThatFitFloat;
    std::vector<double> doubles;
    std::vector<int> ints;
    std::vector<std::int64_t> int64s;
    std::vector<std::int16_t> int16s;
    std::vector<std::uint8_t> uint8s;

    MIRO_REFLECT(floats, doublesThatFitFloat, doubles, ints, int64s, int16s, uint8s)
};

inline constexpr std::string_view sameNumbersSource =
    R"(// n = {3, 14, 15, 92, 65, -35, 89, 300}
struct SameNumbersSevenWays
{
    std::vector<float> floats;               // n
    std::vector<double> doublesThatFitFloat; // n
    std::vector<double> doubles;             // n[i] + 0.1
    std::vector<int> ints;                   // n
    std::vector<std::int64_t> int64s;        // n
    std::vector<std::int16_t> int16s;        // n
    std::vector<std::uint8_t> uint8s;        // n, wrapped to 0..255
    MIRO_REFLECT(floats, doublesThatFitFloat, doubles,
                 ints, int64s, int16s, uint8s)
};)";

inline SameNumbersSevenWays createSameNumbersSevenWays()
{
    constexpr auto offsetThatDoesNotFitFloat = 0.1;
    auto value = SameNumbersSevenWays {};

    for (auto number: sharedNumbers)
    {
        value.floats.push_back(static_cast<float>(number));
        value.doublesThatFitFloat.push_back(static_cast<double>(number));
        value.doubles.push_back(static_cast<double>(number)
                                + offsetThatDoesNotFitFloat);
        value.ints.push_back(number);
        value.int64s.push_back(number);
        value.int16s.push_back(static_cast<std::int16_t>(number));
        value.uint8s.push_back(static_cast<std::uint8_t>(number));
    }

    return value;
}

using NestedFloats = std::vector<std::vector<std::vector<float>>>;

inline constexpr std::string_view nestedSource = R"(using NestedFloats =
    std::vector<std::vector<std::vector<float>>>;

NestedFloats {{{1.0f, 2.0f}, {3.5f}},
              {{4.0f, 5.0f, 6.0f}}}

// Encoded on its own, not inside a struct: there are no
// field names, so the key table is empty.)";

using NamedCurves = std::map<std::string, std::vector<double>>;

inline constexpr std::string_view mapSource = R"(using NamedCurves =
    std::map<std::string, std::vector<double>>;

NamedCurves {{"attack", {0.5, 0.25, 1.0}},
             {"release", {0.1, 0.2}}})";

enum class Waveform
{
    Sine,
    Square,
    Sawtooth,
};

enum class FilterMode
{
    LowPass = 0,
    HighPass = 1,
    BandPass = 2,
};

struct Oscillator
{
    Waveform waveform = Waveform::Sawtooth;
    FilterMode filter = FilterMode::BandPass;

    MIRO_REFLECT(waveform, filter)
};

inline constexpr std::string_view enumSource =
    R"(enum class Waveform { Sine, Square, Sawtooth };
enum class FilterMode { LowPass, HighPass, BandPass };
MIRO_ENUM_AS_INTEGER(FilterMode)

struct Oscillator
{
    Waveform waveform = Waveform::Sawtooth;  // stored by name
    FilterMode filter = FilterMode::BandPass; // stored as 2
    MIRO_REFLECT(waveform, filter)
};)";

using NumberOrSamples = std::variant<int, std::vector<float>>;

struct VariantFields
{
    NumberOrSamples holdsInt = 5;
    NumberOrSamples holdsSamples = std::vector<float> {0.5f, 2.0f};

    MIRO_REFLECT(holdsInt, holdsSamples)
};

inline constexpr std::string_view variantSource = R"(using NumberOrSamples =
    std::variant<int, std::vector<float>>;

struct VariantFields
{
    NumberOrSamples holdsInt = 5;
    NumberOrSamples holdsSamples =
        std::vector<float> {0.5f, 2.0f};
    MIRO_REFLECT(holdsInt, holdsSamples)
};)";

} // namespace ByteLayoutExamples

MIRO_ENUM_AS_INTEGER(ByteLayoutExamples::FilterMode)
