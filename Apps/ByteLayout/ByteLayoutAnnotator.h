#pragma once

#include <Calder/Format.h>
#include <Miro/Json.h>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ByteLayout
{

// One run of bytes with a single meaning, e.g. "u32 payload size".
struct AnnotatedSpan
{
    std::size_t offset = 0;
    std::size_t length = 0;
    std::string role;
    std::string decodedValue;
    std::string explanation;
    int nodeId = 0;
};

// One value in the encoded tree, or the header / key table. A field's
// bytes start at its key; valueOffset is where its tag sits.
struct ValueNode
{
    std::string path;
    std::string label;
    std::string typeName;
    std::string valueText;
    std::size_t startOffset = 0;
    std::size_t valueOffset = 0;
    std::size_t endOffset = 0;
    std::size_t elementCount = 0;
    std::size_t elementBytes = 0;
    bool isContainer = false;
    std::optional<int> parentId;
    std::vector<int> childIds;
};

struct ValueSlot
{
    std::size_t startOffset = 0;
    std::size_t valueOffset = 0;
    std::string path;
    std::string label;
    std::optional<int> parentId;
};

// Walks an encoded "CLD1" document with emberstore's own decoding helpers
// and labels every byte with what it is and why it is there.
class ByteLayoutAnnotator
{
public:
    explicit ByteLayoutAnnotator(std::span<const std::uint8_t> bytesToAnnotate);

    void annotateWholeDocument();

    // Every byte must belong to exactly one span; this lists where not.
    std::vector<std::string> findCoverageProblems() const;

    Miro::Json::Value createJson() const;

private:
    int createNode(const ValueSlot& slot,
                   std::string typeName,
                   std::size_t endOffset);
    void addSpan(AnnotatedSpan span);

    void annotateHeader();
    void annotateKeyTable();
    int annotateValue(const ValueSlot& slot);
    std::size_t findValueEndOffset(std::size_t valueOffset) const;

    std::size_t annotateNumber(std::size_t offset,
                               Calder::Detail::Encoding encoding,
                               int nodeId,
                               const std::string& path);
    void annotateString(const Calder::Detail::ValueHeader& header,
                        int nodeId);
    void annotateSizeField(std::size_t offset, int nodeId, const std::string& path);
    void annotateCountField(std::size_t offset,
                            int nodeId,
                            const std::string& path,
                            std::size_t fixedElementWidth);
    void annotateEntries(const Calder::Detail::ValueHeader& header,
                         int nodeId);
    void annotateEntryKey(std::size_t entryStartOffset,
                          std::size_t valueOffset,
                          bool hasInlineKey,
                          int childNodeId);
    void annotateArray(const Calder::Detail::ValueHeader& header,
                       int nodeId);
    void
        annotateTaggedElements(const Calder::Detail::ValueHeader& header,
                               int nodeId);
    void
        annotatePackedElements(const Calder::Detail::ValueHeader& header,
                               int nodeId);

    std::span<const std::uint8_t> bytes;
    Calder::Detail::Document document;
    std::vector<ValueNode> nodes;
    std::vector<AnnotatedSpan> spans;
    std::vector<int> rootNodeIds;
};

} // namespace ByteLayout
