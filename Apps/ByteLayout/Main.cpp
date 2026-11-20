// bytelayout: encodes example values with Calder::toBinary, labels every
// byte of the result, and writes it as a script the ByteLayout page loads.
// A script rather than a .json fetch so the page also opens from disk.
//
//   bytelayout <data.js>

#include "ByteLayoutAnnotator.h"
#include "Examples.h"

#include <Miro/Reflect.h>

#include <cstdio>
#include <fstream>
#include <string>
#include <string_view>

namespace
{

using namespace ByteLayoutExamples;

struct ExampleText
{
    std::string_view id;
    std::string_view title;
    std::string_view description;
    std::string_view source;
};

template <typename T>
Miro::Json::Value createExampleJson(const ExampleText& text, const T& value)
{
    constexpr auto prettyJsonIndent = 2;
    auto binary = Calder::toBinary(value);
    auto annotator = ByteLayout::ByteLayoutAnnotator {binary};
    annotator.annotateWholeDocument();

    for (const auto& problem: annotator.findCoverageProblems())
        std::fprintf(
            stderr, "%s: %s\n", std::string {text.id}.c_str(), problem.c_str());

    auto compactJson = Miro::toJSONString(value);
    auto layout = annotator.createJson();
    auto& object = layout.toObject();
    object["id"] = std::string {text.id};
    object["title"] = std::string {text.title};
    object["description"] = std::string {text.description};
    object["source"] = std::string {text.source};
    object["json"] = Miro::toJSONString(value, prettyJsonIndent);
    object["jsonByteCount"] = static_cast<std::int64_t>(compactJson.size());
    object["problemCount"] =
        static_cast<std::int64_t>(annotator.findCoverageProblems().size());
    return layout;
}

Miro::Json::Value createAllExamplesJson()
{
    auto examples = Miro::Json::Array {};

    examples.add(createExampleJson(
        {"person",
         "Person with a list of Fish",
         "Nested structs, a list of structs and a list of strings. Every field "
         "name appears once in the key table even though each Fish repeats "
         "id, name and foo.",
         personSource},
        Person {7,
                "Jamie",
                {{1, "Nemo", {"orange", "stripy"}}, {-2, "Dory", {"blue"}}}}));

    examples.add(createExampleJson(
        {"optionals",
         "std::optional, engaged and empty",
         "An engaged optional is stored as its value with no wrapper. An empty "
         "one keeps its key and stores a one-byte Null.",
         optionalSource},
        OptionalFields {}));

    examples.add(createExampleJson(
        {"packed",
         "Same 8 numbers, seven element types",
         "Every vector here goes through visitPacked() and is stored with no "
         "per-element tag. int16_t and uint8_t elements take 2 and 1 raw bytes "
         "each; int and int64_t take one zigzag varint each.",
         sameNumbersSource},
        createSameNumbersSevenWays()));

    examples.add(createExampleJson(
        {"nested",
         "vector<vector<vector<float>>>",
         "The outer two levels are tagged Arrays (one size and count each); "
         "only the innermost vector<float> is packed.",
         nestedSource},
        NestedFloats {{{1.0f, 2.0f}, {3.5f}}, {{4.0f, 5.0f, 6.0f}}}));

    examples.add(createExampleJson(
        {"map",
         "map<string, vector<double>>",
         "Map keys are written inline, not in the key table. A vector<double> "
         "whose values all fit in a float is narrowed to PackedFloat.",
         mapSource},
        NamedCurves {{"attack", {0.5, 0.25, 1.0}}, {"release", {0.1, 0.2}}}));

    examples.add(createExampleJson(
        {"enum",
         "Enums: by name and as integer",
         "By default an enum is stored as its enumerator's name. "
         "MIRO_ENUM_AS_INTEGER stores the underlying number instead.",
         enumSource},
        Oscillator {}));

    examples.add(createExampleJson(
        {"variant",
         "std::variant<int, vector<float>>",
         "A variant is an Object with a single field named after the type it "
         "holds (\"int\" or the vector's type name), so the key table carries "
         "the type names.",
         variantSource},
        VariantFields {}));

    auto root = Miro::Json::Object {};
    root["examples"] = std::move(examples);
    return root;
}

} // namespace

int main(int argc, char** argv)
{
    constexpr auto expectedArgumentCount = 2;

    if (argc != expectedArgumentCount)
    {
        std::fprintf(stderr, "usage: bytelayout <data.js>\n");
        return 1;
    }

    auto output = std::ofstream {argv[1]};
    output << "window.byteLayoutData = "
           << Miro::Json::print(createAllExamplesJson()) << ";\n";
    return output.good() ? 0 : 1;
}
