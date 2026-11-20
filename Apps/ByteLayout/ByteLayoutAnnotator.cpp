#include "ByteLayoutAnnotator.h"

#include <algorithm>
#include <array>
#include <format>
#include <string_view>

namespace ByteLayout
{
namespace
{

using Calder::Detail::Tag;
using Calder::Detail::ByteReader;
using Calder::Detail::Encoding;
using Calder::Detail::ValueCategory;
using Calder::Detail::ValueHeader;

constexpr auto tagByteCount = std::size_t {1};
constexpr auto u32FieldByteCount = sizeof(std::uint32_t);

struct TagDescription
{
    Tag tag;
    std::string_view name;
    std::string_view meaning;
};

constexpr std::array tagDescriptions {
    TagDescription {
        Tag::Null,
        "Null",
        "an empty value, such as a std::optional with nothing in it. The tag is the whole value; nothing follows."},
    TagDescription {Tag::False,
                    "False",
                    "the bool false. The tag is the value itself; nothing follows."},
    TagDescription {Tag::True,
                    "True",
                    "the bool true. The tag is the value itself; nothing follows."},
    TagDescription {
        Tag::Int,
        "Int",
        "a whole number of any C++ integer width. A zigzag varint follows."},
    TagDescription {
        Tag::Double,
        "Double",
        "a double that changes when converted to float, so all 8 IEEE-754 bytes follow."},
    TagDescription {
        Tag::Float32,
        "Float32",
        "a float, or a double that converts to float and back unchanged. 4 IEEE-754 bytes follow."},
    TagDescription {Tag::String,
                    "String",
                    "text. A varint byte length follows, then the UTF-8 bytes."},
    TagDescription {
        Tag::Object,
        "Object",
        "a reflected struct. A u32 payload size follows, then one key id and value per field."},
    TagDescription {
        Tag::Map,
        "Map",
        "a std::map with string keys. A u32 payload size follows, then each entry's key written out in full, then its value."},
    TagDescription {
        Tag::Array,
        "Array",
        "a list whose elements each carry their own tag. A u32 payload size and a u32 element count follow, then the elements."},
    TagDescription {
        Tag::PackedFloat,
        "PackedFloat",
        "a std::vector<float>, or a std::vector<double> whose every element converts to float and back unchanged. A u32 element count follows, then 4 bytes per element and no tags."},
    TagDescription {
        Tag::PackedDouble,
        "PackedDouble",
        "a std::vector<double>. A u32 element count follows, then 8 bytes per element and no tags."},
    TagDescription {
        Tag::PackedInt32,
        "PackedInt32",
        "a std::vector<int32_t> or std::vector<uint32_t>. A u32 payload size and a u32 element count follow, then one zigzag varint per element and no tags."},
    TagDescription {
        Tag::PackedInt64,
        "PackedInt64",
        "a std::vector<int64_t> or std::vector<uint64_t> (stored as its int64 bit pattern). A u32 payload size and a u32 element count follow, then one zigzag varint per element and no tags."},
    TagDescription {
        Tag::PackedInt8,
        "PackedInt8",
        "a std::vector<int8_t>. A u32 element count follows, then 1 byte per element and no tags."},
    TagDescription {
        Tag::PackedUInt8,
        "PackedUInt8",
        "a std::vector<uint8_t>, such as a byte blob. A u32 element count follows, then 1 byte per element and no tags."},
    TagDescription {
        Tag::PackedInt16,
        "PackedInt16",
        "a std::vector<int16_t>. A u32 element count follows, then 2 little-endian bytes per element and no tags."},
    TagDescription {
        Tag::PackedUInt16,
        "PackedUInt16",
        "a std::vector<uint16_t>. A u32 element count follows, then 2 little-endian bytes per element and no tags."},
};

const TagDescription& findTagDescription(Tag tag)
{
    return *std::ranges::find(tagDescriptions, tag, &TagDescription::tag);
}

std::string describePath(const std::string& path)
{
    if (path.empty())
        return "the whole value";

    return path;
}

std::string appendKeyToPath(const std::string& path, std::string_view key)
{
    if (path.empty())
        return std::string {key};

    return std::format("{}.{}", path, key);
}

std::string appendIndexToPath(const std::string& path, std::size_t index)
{
    return std::format("{}[{}]", path, index);
}

std::string formatNumber(double value)
{
    return Miro::Json::print(Miro::Json::Value {value});
}

std::string quoteText(std::string_view text)
{
    return Miro::Json::print(Miro::Json::Value {std::string {text}});
}

std::string explainVarintLength(std::size_t byteCount)
{
    if (byteCount == 1)
        return "It fits in one byte because it is below 128.";

    return std::format("It takes {} bytes: each byte carries 7 bits of the "
                       "number, and its top bit says whether another byte "
                       "follows.",
                       byteCount);
}

Miro::Json::Value createIntegerJson(std::size_t value)
{
    return Miro::Json::Value {static_cast<std::int64_t>(value)};
}

Miro::Json::Value createSpanJson(const AnnotatedSpan& span)
{
    auto object = Miro::Json::Object {};
    object["offset"] = createIntegerJson(span.offset);
    object["length"] = createIntegerJson(span.length);
    object["role"] = span.role;
    object["value"] = span.decodedValue;
    object["explanation"] = span.explanation;
    object["node"] = span.nodeId;
    return object;
}

Miro::Json::Value createNodeJson(const ValueNode& node)
{
    auto object = Miro::Json::Object {};
    object["path"] = node.path;
    object["label"] = node.label;
    object["type"] = node.typeName;
    object["value"] = node.valueText;
    object["start"] = createIntegerJson(node.startOffset);
    object["valueOffset"] = createIntegerJson(node.valueOffset);
    object["end"] = createIntegerJson(node.endOffset);
    object["isContainer"] = node.isContainer;
    object["elementCount"] = createIntegerJson(node.elementCount);
    object["elementBytes"] = createIntegerJson(node.elementBytes);

    auto children = Miro::Json::Array {};

    for (auto childId: node.childIds)
        children.add(childId);

    object["children"] = std::move(children);
    return object;
}

} // namespace

ByteLayoutAnnotator::ByteLayoutAnnotator(
    std::span<const std::uint8_t> bytesToAnnotate)
    : bytes(bytesToAnnotate)
    , document(bytesToAnnotate)
{
}

void ByteLayoutAnnotator::annotateWholeDocument()
{
    annotateHeader();
    rootNodeIds.push_back(annotateValue(ValueSlot {Calder::Detail::headerSize,
                                                   Calder::Detail::headerSize,
                                                   "",
                                                   "value",
                                                   std::nullopt}));
    annotateKeyTable();
    std::ranges::sort(spans, {}, &AnnotatedSpan::offset);
}

int ByteLayoutAnnotator::createNode(const ValueSlot& slot,
                                    std::string typeName,
                                    std::size_t endOffset)
{
    auto node = ValueNode {};
    node.path = slot.path;
    node.label = slot.label;
    node.typeName = std::move(typeName);
    node.startOffset = slot.startOffset;
    node.valueOffset = slot.valueOffset;
    node.endOffset = endOffset;
    node.parentId = slot.parentId;

    auto nodeId = static_cast<int>(nodes.size());
    nodes.push_back(std::move(node));

    if (slot.parentId)
        nodes[static_cast<std::size_t>(*slot.parentId)].childIds.push_back(nodeId);

    return nodeId;
}

void ByteLayoutAnnotator::addSpan(AnnotatedSpan span)
{
    spans.push_back(std::move(span));
}

void ByteLayoutAnnotator::annotateHeader()
{
    auto nodeId = createNode(ValueSlot {0, 0, "", "header", std::nullopt},
                             "Header",
                             Calder::Detail::headerSize);
    rootNodeIds.push_back(nodeId);
    nodes[static_cast<std::size_t>(nodeId)].isContainer = true;

    addSpan({0,
             Calder::Detail::magic.size(),
             "magic",
             "\"CLD1\"",
             "The four ASCII letters \"CLD1\". A reader checks them first to "
             "know this is a Calder document, format version 1, and not "
             "JSON or a journal (\"CLDJ\").",
             nodeId});

    auto tableOffset =
        ByteReader {bytes, Calder::Detail::headerFieldOffset}.readU32();
    addSpan({Calder::Detail::headerFieldOffset,
             u32FieldByteCount,
             "keyTableOffset",
             std::to_string(tableOffset),
             std::format("u32 little-endian key table offset: the field names "
                         "start at byte {} (0x{:02x}). The table is written "
                         "after the value, so the writer leaves this as zero "
                         "and fills it in at the end, once it knows where the "
                         "table lands.",
                         tableOffset,
                         tableOffset),
             nodeId});
}

void ByteLayoutAnnotator::annotateKeyTable()
{
    auto tableOffset = std::size_t {
        ByteReader {bytes, Calder::Detail::headerFieldOffset}.readU32()};
    auto nodeId = createNode(
        ValueSlot {tableOffset, tableOffset, "", "key table", std::nullopt},
        "KeyTable",
        bytes.size());
    rootNodeIds.push_back(nodeId);

    auto reader = ByteReader {bytes, tableOffset};
    auto keyCount = reader.readVarint();
    auto& tableNode = nodes[static_cast<std::size_t>(nodeId)];
    tableNode.isContainer = true;
    tableNode.elementCount = keyCount;
    tableNode.elementBytes = bytes.size() - reader.readOffsetInBytes;

    addSpan({tableOffset,
             reader.readOffsetInBytes - tableOffset,
             "keyCount",
             std::to_string(keyCount),
             std::format("Varint key count: the key table holds {} distinct "
                         "field names. Every struct field in the file refers "
                         "to one of them by its position in this list.",
                         keyCount),
             nodeId});

    for (auto keyId = std::uint64_t {0}; keyId < keyCount; ++keyId)
    {
        auto lengthOffset = reader.readOffsetInBytes;
        auto nameLength = reader.readVarint();
        auto nameOffset = reader.readOffsetInBytes;
        auto name = reader.readString(nameLength);

        addSpan({lengthOffset,
                 nameOffset - lengthOffset,
                 "keyNameLength",
                 std::to_string(nameLength),
                 std::format("Varint length of key {}'s name: {} bytes of "
                             "UTF-8 follow.",
                             keyId,
                             nameLength),
                 nodeId});
        addSpan({nameOffset,
                 nameLength,
                 "keyName",
                 quoteText(name),
                 std::format("Key {} is the field name {}. Each field in the "
                             "file that is called {} stores just the number {} "
                             "instead of these bytes.",
                             keyId,
                             quoteText(name),
                             quoteText(name),
                             keyId),
                 nodeId});
    }
}

std::size_t ByteLayoutAnnotator::findValueEndOffset(std::size_t valueOffset) const
{
    auto reader = ByteReader {bytes, valueOffset};
    Calder::Detail::skipValue(reader);
    return reader.readOffsetInBytes;
}

int ByteLayoutAnnotator::annotateValue(const ValueSlot& slot)
{
    auto header =
        Calder::Detail::parseValueHeader(bytes, slot.valueOffset);
    const auto& tagDescription = findTagDescription(header.tag);
    auto nodeId = createNode(slot,
                             std::string {tagDescription.name},
                             findValueEndOffset(slot.valueOffset));

    addSpan({slot.valueOffset,
             tagByteCount,
             "tag",
             std::string {tagDescription.name},
             std::format("Tag byte {} ({}) for {}: {}",
                         static_cast<int>(header.tag),
                         tagDescription.name,
                         describePath(slot.path),
                         tagDescription.meaning),
             nodeId});

    switch (header.category)
    {
        case ValueCategory::Null:
            nodes[static_cast<std::size_t>(nodeId)].valueText = "null";
            break;
        case ValueCategory::Bool:
            nodes[static_cast<std::size_t>(nodeId)].valueText =
                header.tag == Tag::True ? "true" : "false";
            break;
        case ValueCategory::Number:
            annotateNumber(header.payloadOffset, header.encoding, nodeId, slot.path);
            break;
        case ValueCategory::String:
            annotateString(header, nodeId);
            break;
        case ValueCategory::Object:
            annotateEntries(header, nodeId);
            break;
        case ValueCategory::Array:
            annotateArray(header, nodeId);
            break;
    }

    return nodeId;
}

std::size_t ByteLayoutAnnotator::annotateNumber(std::size_t offset,
                                                Encoding encoding,
                                                int nodeId,
                                                const std::string& path)
{
    auto reader = ByteReader {bytes, offset};
    auto& node = nodes[static_cast<std::size_t>(nodeId)];

    if (encoding == Encoding::Zigzag)
    {
        auto rawVarint = reader.readVarint();
        auto value = Calder::Detail::decodeZigzag(rawVarint);
        auto byteCount = reader.readOffsetInBytes - offset;
        node.valueText = std::to_string(value);
        addSpan({offset,
                 byteCount,
                 "zigzag",
                 node.valueText,
                 std::format("Zigzag varint for {}: the varint reads {}, which "
                             "zigzag decodes to {}. Zigzag interleaves signs "
                             "(0→0, -1→1, 1→2, -2→3, 2→4 …) so small negative "
                             "numbers stay small too. {}",
                             describePath(path),
                             rawVarint,
                             value,
                             explainVarintLength(byteCount)),
                 nodeId});
        return reader.readOffsetInBytes;
    }

    auto value = Calder::Detail::readNumber(reader, encoding);

    if (const auto* integer = std::get_if<std::int64_t>(&value))
    {
        auto byteCount = reader.readOffsetInBytes - offset;
        node.valueText = std::to_string(*integer);
        addSpan({offset,
                 byteCount,
                 "fixed-int",
                 node.valueText,
                 std::format("{} raw byte{} for {}: {}. Small integer types are "
                             "stored at their own width, with no tag.",
                             byteCount,
                             byteCount == 1 ? "" : "s, little-endian,",
                             describePath(path),
                             node.valueText),
                 nodeId});
        return reader.readOffsetInBytes;
    }

    auto decoded = std::get<double>(value);
    node.valueText = formatNumber(decoded);

    if (encoding == Encoding::Float32)
    {
        addSpan({offset,
                 sizeof(float),
                 "float32",
                 node.valueText,
                 std::format("4 IEEE-754 bytes, little-endian, for {}: {}. "
                             "Single precision: 1 sign bit, 8 exponent bits, "
                             "23 fraction bits.",
                             describePath(path),
                             node.valueText),
                 nodeId});
        return reader.readOffsetInBytes;
    }

    addSpan({offset,
             sizeof(double),
             "float64",
             node.valueText,
             std::format("8 IEEE-754 bytes, little-endian, for {}: {}. Kept "
                         "as a double because converting it to float would "
                         "change it.",
                         describePath(path),
                         node.valueText),
             nodeId});
    return reader.readOffsetInBytes;
}

void ByteLayoutAnnotator::annotateString(const ValueHeader& header, int nodeId)
{
    auto reader = ByteReader {bytes, header.payloadOffset};
    auto textLength = reader.readVarint();
    auto textOffset = reader.readOffsetInBytes;
    auto text = reader.readString(textLength);
    auto& node = nodes[static_cast<std::size_t>(nodeId)];
    node.valueText = quoteText(text);

    addSpan({header.payloadOffset,
             textOffset - header.payloadOffset,
             "stringLength",
             std::to_string(textLength),
             std::format("Varint string length for {}: {} bytes of UTF-8 "
                         "follow. {}",
                         describePath(node.path),
                         textLength,
                         explainVarintLength(textOffset - header.payloadOffset)),
             nodeId});

    if (textLength == 0)
        return;

    addSpan({textOffset,
             textLength,
             "stringBytes",
             node.valueText,
             std::format("The UTF-8 text {}, {} bytes. There is no "
                         "terminating zero: the length before it says where "
                         "it ends.",
                         node.valueText,
                         textLength),
             nodeId});
}

void ByteLayoutAnnotator::annotateSizeField(std::size_t offset,
                                            int nodeId,
                                            const std::string& path)
{
    auto payloadSize = ByteReader {bytes, offset}.readU32();
    addSpan({offset,
             u32FieldByteCount,
             "payloadSize",
             std::to_string(payloadSize),
             std::format("u32 little-endian payload size: {} bytes follow this "
                         "field, so a reader can skip all of {} without "
                         "parsing it. The writer puts a zero here and fills "
                         "it in once the contents are written.",
                         payloadSize,
                         describePath(path)),
             nodeId});
}

void ByteLayoutAnnotator::annotateCountField(std::size_t offset,
                                             int nodeId,
                                             const std::string& path,
                                             std::size_t fixedElementWidth)
{
    auto count = ByteReader {bytes, offset}.readU32();
    auto explanation = std::format("u32 little-endian element count: {} holds {} "
                                   "elements.",
                                   describePath(path),
                                   count);

    if (fixedElementWidth != 0)
    {
        explanation += std::format(" Each element is exactly {} bytes, so the "
                                   "elements take {} × {} = {} bytes. That is "
                                   "why this kind of array has no separate "
                                   "payload size: the count gives it.",
                                   fixedElementWidth,
                                   count,
                                   fixedElementWidth,
                                   count * fixedElementWidth);
    }

    addSpan({offset,
             u32FieldByteCount,
             "elementCount",
             std::to_string(count),
             explanation,
             nodeId});
}

void ByteLayoutAnnotator::annotateEntries(const ValueHeader& header, int nodeId)
{
    auto& node = nodes[static_cast<std::size_t>(nodeId)];
    auto path = node.path;
    node.isContainer = true;
    node.elementBytes = header.payloadEndOffset - header.payloadOffset;
    annotateSizeField(header.payloadOffset - u32FieldByteCount, nodeId, path);

    auto hasInlineKeys = header.tag == Tag::Map;
    auto cursor = Calder::Detail::ObjectEntryCursor {
        document, header, header.payloadOffset, header.payloadEndOffset};
    auto entryStartOffset = header.payloadOffset;
    auto entryCount = std::size_t {0};

    while (auto entry = cursor.readNextEntry())
    {
        auto childNodeId =
            annotateValue(ValueSlot {entryStartOffset,
                                     entry->valueOffset,
                                     appendKeyToPath(path, entry->key),
                                     std::string {entry->key},
                                     nodeId});
        annotateEntryKey(
            entryStartOffset, entry->valueOffset, hasInlineKeys, childNodeId);
        entryStartOffset = entry->valueEndOffset;
        ++entryCount;
    }

    nodes[static_cast<std::size_t>(nodeId)].elementCount = entryCount;
}

void ByteLayoutAnnotator::annotateEntryKey(std::size_t entryStartOffset,
                                           std::size_t valueOffset,
                                           bool hasInlineKey,
                                           int childNodeId)
{
    auto reader = ByteReader {bytes, entryStartOffset};
    auto keyNumber = reader.readVarint();
    auto varintByteCount = reader.readOffsetInBytes - entryStartOffset;
    const auto& childPath = nodes[static_cast<std::size_t>(childNodeId)].path;

    if (!hasInlineKey)
    {
        auto name = quoteText(document.findKeyName(keyNumber).value_or(std::string_view {}));
        addSpan({entryStartOffset,
                 valueOffset - entryStartOffset,
                 "keyId",
                 std::format("{} = {}", keyNumber, name),
                 std::format("Varint key id {}: this field's name is entry {} "
                             "of the key table at the end of the file, {}. A "
                             "name is stored once per file, so a field repeated "
                             "in every element of a list costs one small number "
                             "each time instead of its whole name.",
                             keyNumber,
                             keyNumber,
                             name),
                 childNodeId});
        return;
    }

    auto key = quoteText(reader.readString(keyNumber));
    addSpan({entryStartOffset,
             varintByteCount,
             "mapKeyLength",
             std::to_string(keyNumber),
             std::format("Varint map key length: the key of {} is {} bytes of "
                         "UTF-8. Map keys are data, not field names, so they "
                         "are written out in full here instead of going in the "
                         "key table.",
                         childPath,
                         keyNumber),
             childNodeId});
    addSpan({entryStartOffset + varintByteCount,
             keyNumber,
             "mapKeyBytes",
             key,
             std::format("The map key {} as UTF-8, {} bytes. Its value follows "
                         "straight after.",
                         key,
                         keyNumber),
             childNodeId});
}

void ByteLayoutAnnotator::annotateArray(const ValueHeader& header, int nodeId)
{
    auto& node = nodes[static_cast<std::size_t>(nodeId)];
    auto path = node.path;
    node.isContainer = true;
    node.elementCount = header.elementCount;
    node.elementBytes = header.payloadEndOffset - header.payloadOffset;

    auto countFieldOffset = header.payloadOffset - u32FieldByteCount;
    auto fixedElementWidth =
        Calder::Detail::getFixedByteWidth(header.encoding);

    if (fixedElementWidth == 0)
        annotateSizeField(countFieldOffset - u32FieldByteCount, nodeId, path);

    annotateCountField(countFieldOffset, nodeId, path, fixedElementWidth);

    if (header.encoding == Encoding::Tagged)
        annotateTaggedElements(header, nodeId);
    else
        annotatePackedElements(header, nodeId);
}

void ByteLayoutAnnotator::annotateTaggedElements(const ValueHeader& header,
                                                 int nodeId)
{
    auto elementOffset = header.payloadOffset;
    auto path = nodes[static_cast<std::size_t>(nodeId)].path;

    for (auto index = std::size_t {0}; index < header.elementCount; ++index)
    {
        auto childNodeId = annotateValue(ValueSlot {elementOffset,
                                                    elementOffset,
                                                    appendIndexToPath(path, index),
                                                    std::format("[{}]", index),
                                                    nodeId});
        elementOffset = nodes[static_cast<std::size_t>(childNodeId)].endOffset;
    }
}

void ByteLayoutAnnotator::annotatePackedElements(const ValueHeader& header,
                                                 int nodeId)
{
    auto elementOffset = header.payloadOffset;
    auto path = nodes[static_cast<std::size_t>(nodeId)].path;
    auto elementType = std::string {
        findTagDescription(
            Calder::Detail::makePackedElementHeader(header.encoding, 0)
                .tag)
            .name};

    for (auto index = std::size_t {0}; index < header.elementCount; ++index)
    {
        auto elementPath = appendIndexToPath(path, index);
        auto childNodeId =
            createNode(ValueSlot {elementOffset,
                                  elementOffset,
                                  elementPath,
                                  std::format("[{}]", index),
                                  nodeId},
                       std::format("{} (packed, no tag)", elementType),
                       elementOffset);
        auto elementEndOffset =
            annotateNumber(elementOffset, header.encoding, childNodeId, elementPath);
        nodes[static_cast<std::size_t>(childNodeId)].endOffset = elementEndOffset;
        elementOffset = elementEndOffset;
    }
}

std::vector<std::string> ByteLayoutAnnotator::findCoverageProblems() const
{
    auto problems = std::vector<std::string> {};
    auto expectedOffset = std::size_t {0};

    for (const auto& span: spans)
    {
        if (span.offset != expectedOffset)
        {
            problems.push_back(std::format("span at byte {} ({}) but the "
                                           "previous one ended at byte {}",
                                           span.offset,
                                           span.role,
                                           expectedOffset));
        }

        expectedOffset = span.offset + span.length;
    }

    if (expectedOffset != bytes.size())
    {
        problems.push_back(std::format("spans end at byte {} but the document "
                                       "is {} bytes",
                                       expectedOffset,
                                       bytes.size()));
    }

    return problems;
}

Miro::Json::Value ByteLayoutAnnotator::createJson() const
{
    auto spanArray = Miro::Json::Array {};

    for (const auto& span: spans)
        spanArray.add(createSpanJson(span));

    auto nodeArray = Miro::Json::Array {};

    for (const auto& node: nodes)
        nodeArray.add(createNodeJson(node));

    auto rootArray = Miro::Json::Array {};

    for (auto rootNodeId: rootNodeIds)
        rootArray.add(rootNodeId);

    auto byteArray = Miro::Json::Array {};

    for (auto byte: bytes)
        byteArray.add(static_cast<int>(byte));

    auto object = Miro::Json::Object {};
    object["bytes"] = std::move(byteArray);
    object["spans"] = std::move(spanArray);
    object["nodes"] = std::move(nodeArray);
    object["roots"] = std::move(rootArray);
    return object;
}

} // namespace ByteLayout
