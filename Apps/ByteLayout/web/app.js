"use strict";

const wideBytesPerRow = 16;
const narrowBytesPerRow = 8;
const narrowLayoutMaxWidth = 560;
const firstPrintableAscii = 0x20;
const lastPrintableAscii = 0x7e;
const varintContinuationBit = 0x80;
const varintPayloadMask = 0x7f;
const varintBitsPerByte = 7;
const bitsPerByte = 8;

const roleFamilies = [
  { family: "header", roles: ["magic", "keyTableOffset"], name: "Header: magic and key table offset" },
  { family: "tag", roles: ["tag"], name: "Tag byte" },
  { family: "size", roles: ["payloadSize", "elementCount"], name: "u32 payload size / element count" },
  { family: "key", roles: ["keyId", "mapKeyLength", "mapKeyBytes"], name: "Field key id / map key" },
  { family: "integer", roles: ["zigzag", "fixed-int"], name: "Integer: zigzag varint or fixed width" },
  { family: "float", roles: ["float32", "float64"], name: "IEEE-754 float / double" },
  { family: "string", roles: ["stringLength", "stringBytes"], name: "String length and UTF-8 bytes" },
  { family: "key-table", roles: ["keyCount", "keyNameLength", "keyName"], name: "Key table" },
];

const roleTitles = {
  magic: "Magic number",
  keyTableOffset: "Key table offset (u32)",
  tag: "Tag byte",
  payloadSize: "Payload size (u32)",
  elementCount: "Element count (u32)",
  keyId: "Field key id (varint)",
  mapKeyLength: "Map key length (varint)",
  mapKeyBytes: "Map key (UTF-8)",
  zigzag: "Integer (zigzag varint)",
  float32: "Float (4 bytes)",
  float64: "Double (8 bytes)",
  stringLength: "String length (varint)",
  stringBytes: "String bytes (UTF-8)",
  keyCount: "Key count (varint)",
  keyNameLength: "Key name length (varint)",
  keyName: "Key name (UTF-8)",
};

const varintRoles = new Set(["zigzag", "keyId", "mapKeyLength", "stringLength", "keyCount", "keyNameLength"]);
const u32Roles = new Set(["keyTableOffset", "payloadSize", "elementCount"]);
const floatLayouts = {
  float32: { exponentBits: 8, fractionBits: 23, exponentBias: 127 },
  float64: { exponentBits: 11, fractionBits: 52, exponentBias: 1023 },
};

const state = {
  examples: [],
  example: null,
  spanIndexByByte: [],
  byteCells: [],
  treeButtons: [],
  pinnedSpanIndex: null,
  pinnedNodeId: null,
  bytesPerRow: wideBytesPerRow,
};

function findRoleFamily(role) {
  const match = roleFamilies.find((entry) => entry.roles.includes(role));
  return match ? match.family : "header";
}

function formatCount(count, singular, plural) {
  return `${count} ${count === 1 ? singular : plural}`;
}

function formatHexByte(byte) {
  return byte.toString(16).padStart(2, "0");
}

function formatOffset(offset) {
  return "0x" + offset.toString(16).padStart(4, "0");
}

function formatBinaryByte(byte) {
  return byte.toString(2).padStart(bitsPerByte, "0");
}

function createElement(tag, className, text) {
  const element = document.createElement(tag);
  if (className) {
    element.className = className;
  }
  if (text !== undefined) {
    element.textContent = text;
  }
  return element;
}

function getSpanBytes(span) {
  return state.example.bytes.slice(span.offset, span.offset + span.length);
}

function formatBytesHex(bytes) {
  return bytes.map(formatHexByte).join(" ");
}

function buildSpanIndexByByte(example) {
  const indices = new Array(example.bytes.length).fill(-1);
  example.spans.forEach((span, spanIndex) => {
    for (let offset = span.offset; offset < span.offset + span.length; ++offset) {
      indices[offset] = spanIndex;
    }
  });
  return indices;
}

function renderRoleLegend() {
  const legend = document.getElementById("role-legend");
  legend.replaceChildren();
  for (const entry of roleFamilies) {
    const item = createElement("span");
    item.append(createElement("span", "role-swatch role-" + entry.family), entry.name);
    legend.append(item);
  }
}

function renderExampleTabs() {
  const tabs = document.getElementById("example-tabs");
  tabs.replaceChildren();
  state.examples.forEach((example) => {
    const button = createElement("button", "", example.title);
    button.dataset.exampleId = example.id;
    button.addEventListener("click", () => selectExample(example.id));
    tabs.append(button);
  });
}

function renderExampleSummary() {
  const example = state.example;
  const summary = document.getElementById("example-summary");
  const ratio = example.jsonByteCount / example.bytes.length;
  const sizeLine = createElement("p", "size-line");
  sizeLine.innerHTML =
    `<b>Binary: ${example.bytes.length} bytes</b> · compact JSON: ${example.jsonByteCount} bytes · ` +
    `JSON is ${ratio.toFixed(2)}× the binary size.`;
  summary.replaceChildren(createElement("p", "", example.description), sizeLine);
  if (example.problemCount > 0) {
    summary.append(createElement("p", "", `Warning: ${example.problemCount} byte ranges were not labelled; the generator printed them.`));
  }
}

function renderHexGrid() {
  const example = state.example;
  const grid = document.getElementById("hex-grid");
  grid.replaceChildren();
  state.byteCells = [];

  for (let rowStart = 0; rowStart < example.bytes.length; rowStart += state.bytesPerRow) {
    const row = createElement("div", "hex-row");
    row.append(createElement("span", "hex-offset", formatOffset(rowStart)));
    let asciiText = "";

    for (let column = 0; column < state.bytesPerRow; ++column) {
      const offset = rowStart + column;
      if (offset >= example.bytes.length) {
        row.append(createElement("span", "hex-byte empty", ""));
        continue;
      }
      const byte = example.bytes[offset];
      const spanIndex = state.spanIndexByByte[offset];
      const span = example.spans[spanIndex];
      const cell = createElement("span", "hex-byte role-" + findRoleFamily(span.role), formatHexByte(byte));
      if (span.offset === offset) {
        cell.classList.add("span-start");
      }
      cell.dataset.offset = offset;
      cell.addEventListener("mouseenter", () => showSpan(spanIndex));
      cell.addEventListener("mouseleave", restorePinnedSelection);
      cell.addEventListener("click", () => pinSpan(spanIndex));
      state.byteCells.push(cell);
      row.append(cell);
      const isPrintable = byte >= firstPrintableAscii && byte <= lastPrintableAscii;
      asciiText += isPrintable ? String.fromCharCode(byte) : ".";
    }

    row.append(createElement("span", "hex-ascii", asciiText));
    grid.append(row);
  }
}

const nodeTypeDescribers = {
  Header: () => "file header",
  KeyTable: (node) => `key table, ${formatCount(node.elementCount, "name", "names")}`,
  Object: (node) => `Object, ${formatCount(node.elementCount, "field", "fields")}`,
  Map: (node) => `Map, ${formatCount(node.elementCount, "entry", "entries")}`,
};

function describeNodeType(node) {
  const describer = nodeTypeDescribers[node.type];
  if (describer) {
    return describer(node);
  }
  if (node.isContainer) {
    return `${node.type}, ${formatCount(node.elementCount, "element", "elements")}`;
  }
  return node.type;
}

function renderTreeNode(nodeId) {
  const node = state.example.nodes[nodeId];
  const item = createElement("li");
  const button = createElement("button", "tree-node");
  button.append(createElement("span", "tree-label", node.label), " ");
  button.append(createElement("span", "tree-type", describeNodeType(node)), " ");
  if (node.value) {
    button.append(createElement("span", "tree-value", node.value), " ");
  }
  button.append(createElement("span", "tree-range", `bytes ${node.start}–${node.end - 1} (${node.end - node.start})`));
  button.addEventListener("click", () => pinNode(nodeId));
  state.treeButtons[nodeId] = button;
  item.append(button);

  if (node.children.length > 0) {
    const list = createElement("ul");
    node.children.forEach((childId) => list.append(renderTreeNode(childId)));
    item.append(list);
  }
  return item;
}

function renderValueTree() {
  const tree = document.getElementById("value-tree");
  state.treeButtons = [];
  const list = createElement("ul");
  state.example.roots.forEach((rootId) => list.append(renderTreeNode(rootId)));
  tree.replaceChildren(list);
}

function isArrayNode(node) {
  return node.isContainer && (node.type === "Array" || node.type.startsWith("Packed"));
}

function renderArrayCosts() {
  const section = document.getElementById("array-costs");
  const arrayNodeIds = state.example.nodes
    .map((node, nodeId) => nodeId)
    .filter((nodeId) => isArrayNode(state.example.nodes[nodeId]));

  section.replaceChildren();
  if (arrayNodeIds.length === 0) {
    return;
  }

  section.append(createElement("h2", "", "What each array costs"));
  section.append(createElement("p", "muted",
    "Overhead is the tag plus the size and count fields. Per element is the element bytes divided by the count. Click a row to see its bytes."));

  const table = createElement("table");
  const head = createElement("thead");
  head.innerHTML = "<tr><th>Path</th><th>Stored as</th><th>Elements</th><th>Total bytes</th><th>Overhead</th><th>Element bytes</th><th>Per element</th></tr>";
  const body = createElement("tbody");

  for (const nodeId of arrayNodeIds) {
    const node = state.example.nodes[nodeId];
    const totalBytes = node.end - node.valueOffset;
    const perElement = node.elementCount > 0 ? (node.elementBytes / node.elementCount).toFixed(2) : "–";
    const row = createElement("tr");
    const cells = [
      [node.path || "(the whole value)", ""],
      [node.type, ""],
      [node.elementCount, "number"],
      [totalBytes, "number"],
      [totalBytes - node.elementBytes, "number"],
      [node.elementBytes, "number"],
      [perElement, "number"],
    ];
    for (const [text, className] of cells) {
      row.append(createElement("td", className, String(text)));
    }
    row.addEventListener("click", () => pinNode(nodeId));
    body.append(row);
  }

  table.append(head, body);
  const scroller = createElement("div", "table-scroller");
  scroller.append(table);
  section.append(scroller);
}

function renderSources() {
  document.getElementById("cpp-source").textContent = state.example.source;
  document.getElementById("json-source").textContent = state.example.json;
}

function clearHighlights() {
  for (const cell of state.byteCells) {
    cell.classList.remove("in-span", "in-node");
  }
  for (const button of state.treeButtons) {
    if (button) {
      button.classList.remove("is-selected", "is-related");
    }
  }
}

function highlightByteRange(start, end, className) {
  for (let offset = start; offset < end; ++offset) {
    state.byteCells[offset].classList.add(className);
  }
}

function showSpan(spanIndex) {
  const span = state.example.spans[spanIndex];
  const node = state.example.nodes[span.node];
  clearHighlights();
  highlightByteRange(node.start, node.end, "in-node");
  highlightByteRange(span.offset, span.offset + span.length, "in-span");
  state.treeButtons[span.node].classList.add("is-related");
  renderSpanDetail(span, node);
}

function showNode(nodeId) {
  const node = state.example.nodes[nodeId];
  clearHighlights();
  highlightByteRange(node.start, node.end, "in-node");
  state.treeButtons[nodeId].classList.add("is-selected");
  renderNodeDetail(node);
}

function pinSpan(spanIndex) {
  state.pinnedSpanIndex = spanIndex;
  state.pinnedNodeId = null;
  showSpan(spanIndex);
  const span = state.example.spans[spanIndex];
  history.replaceState(null, "", `#${state.example.id}@${formatOffset(span.offset)}`);
}

function pinNode(nodeId) {
  state.pinnedNodeId = nodeId;
  state.pinnedSpanIndex = null;
  showNode(nodeId);
  state.byteCells[state.example.nodes[nodeId].start].scrollIntoView({ block: "nearest" });
}

function restorePinnedSelection() {
  if (state.pinnedSpanIndex !== null) {
    showSpan(state.pinnedSpanIndex);
    return;
  }
  if (state.pinnedNodeId !== null) {
    showNode(state.pinnedNodeId);
    return;
  }
  clearHighlights();
  document.getElementById("detail").innerHTML = '<p class="muted">Hover or click a byte, or click a value in the tree.</p>';
}

function describeByteRange(offset, length) {
  if (length === 1) {
    return `byte ${offset} (${formatOffset(offset)})`;
  }
  return `bytes ${offset}–${offset + length - 1} (${formatOffset(offset)}), ${length} long`;
}

function appendDefinitionRow(list, term, description) {
  list.append(createElement("dt", "", term), createElement("dd", "", description));
}

function renderSpanDetail(span, node) {
  const detail = document.getElementById("detail");
  const bytes = getSpanBytes(span);
  const list = createElement("dl");
  appendDefinitionRow(list, "Where", describeByteRange(span.offset, span.length));
  appendDefinitionRow(list, "Hex", formatBytesHex(bytes));
  appendDefinitionRow(list, "Decoded", span.value);
  appendDefinitionRow(list, "Belongs to", node.path || node.label);
  detail.replaceChildren(
    createElement("h3", "", roleTitles[span.role] || span.role),
    createElement("p", "explanation", span.explanation),
    list);
  const bitsView = createBitsView(span.role, bytes);
  if (bitsView) {
    detail.append(bitsView);
  }
}

function renderNodeDetail(node) {
  const detail = document.getElementById("detail");
  const list = createElement("dl");
  appendDefinitionRow(list, "Path", node.path || node.label);
  appendDefinitionRow(list, "Stored as", describeNodeType(node));
  appendDefinitionRow(list, "Bytes", `${node.start}–${node.end - 1}, ${node.end - node.start} in all`);
  if (node.start !== node.valueOffset) {
    appendDefinitionRow(list, "Key", `${node.valueOffset - node.start} bytes before the tag`);
  }
  if (node.value) {
    appendDefinitionRow(list, "Value", node.value);
  }
  if (isArrayNode(node) && node.elementCount > 0) {
    const perElement = (node.elementBytes / node.elementCount).toFixed(2);
    appendDefinitionRow(list, "Elements", `${node.elementCount}, ${node.elementBytes} bytes, ${perElement} bytes each`);
  }
  detail.replaceChildren(createElement("h3", "", node.label), list,
    createElement("p", "muted", "Hover its bytes in the grid to see each field."));
}

function createVarintView(role, bytes) {
  const container = createElement("div");
  const table = createElement("table", "bit-table");
  table.innerHTML = "<tr><th>Byte</th><th>Bits</th><th>Means</th></tr>";
  let value = 0n;

  bytes.forEach((byte, index) => {
    const row = createElement("tr");
    const hasMore = (byte & varintContinuationBit) !== 0;
    const bits = formatBinaryByte(byte);
    const bitsCell = createElement("td");
    bitsCell.append(createElement("span", "bit-continuation", bits[0]), " " + bits.slice(1));
    const payload = BigInt(byte & varintPayloadMask);
    value |= payload << BigInt(index * varintBitsPerByte);
    const meaning = `${hasMore ? "another byte follows" : "last byte"}; payload ${payload} × 2^${index * varintBitsPerByte}`;
    row.append(createElement("td", "", formatHexByte(byte)), bitsCell, createElement("td", "", meaning));
    table.append(row);
  });

  container.append(createElement("p", "muted",
    "Blue bit: 1 means another byte follows. The other 7 bits are the number, lowest group first."));
  container.append(table);
  let steps = `varint = ${value}`;
  if (role === "zigzag") {
    const decoded = (value >> 1n) ^ -(value & 1n);
    steps += `\nzigzag decode: (${value} >> 1) XOR −(${value} AND 1) = ${decoded}`;
    steps += value % 2n === 0n ? "\neven, so the number is positive: half of it" : "\nodd, so the number is negative: −(half of it, rounded up)";
  }
  container.append(createElement("p", "bit-steps", steps));
  return container;
}

function createU32View(bytes) {
  const reversed = [...bytes].reverse();
  let value = 0;
  for (const byte of reversed) {
    value = value * 256 + byte;
  }
  const text =
    `stored low byte first: ${formatBytesHex(bytes)}\n` +
    `read high byte first:  ${formatBytesHex(reversed)} = 0x${reversed.map(formatHexByte).join("")} = ${value}`;
  return createElement("p", "bit-steps", text);
}

function sumFractionBits(fractionBits) {
  let fraction = 0;
  for (let index = 0; index < fractionBits.length; ++index) {
    if (fractionBits[index] === "1") {
      fraction += Math.pow(2, -(index + 1));
    }
  }
  return fraction;
}

function decodeFloatBytes(role, bytes) {
  const view = new DataView(new Uint8Array(bytes).buffer);
  if (role === "float32") {
    return view.getFloat32(0, true);
  }
  return view.getFloat64(0, true);
}

function describeFloatDecoding(fields, layout, decoded) {
  const isNegative = fields.signBit === "1";
  const signText = isNegative ? "−" : "";
  if (fields.exponent === 0 && /^0*$/.test(fields.fractionBits)) {
    return `exponent and fraction all zero: the value is ${signText}0`;
  }
  const significand = 1 + sumFractionBits(fields.fractionBits);
  const power = fields.exponent - layout.exponentBias;
  return (
    `sign ${fields.signBit} → ${isNegative ? "negative" : "positive"}\n` +
    `exponent ${fields.exponent} − bias ${layout.exponentBias} = ${power}\n` +
    `fraction bits → 1 + ${significand - 1} = ${significand}\n` +
    `value = ${signText}${significand} × 2^${power} = ${decoded}`
  );
}

function createFloatView(role, bytes) {
  const layout = floatLayouts[role];
  const bits = [...bytes].reverse().map(formatBinaryByte).join("");
  const fields = {
    signBit: bits[0],
    exponentBits: bits.slice(1, 1 + layout.exponentBits),
    fractionBits: bits.slice(1 + layout.exponentBits),
  };
  fields.exponent = parseInt(fields.exponentBits, 2);

  const container = createElement("div");
  const bitsLine = createElement("p", "bit-steps");
  bitsLine.append(
    createElement("span", "bit-sign", fields.signBit), " ",
    createElement("span", "bit-exponent", fields.exponentBits), " ",
    fields.fractionBits);
  container.append(createElement("p", "muted",
    "Bytes reversed to read most significant first. Red: sign. Blue: exponent. Rest: fraction."));
  container.append(bitsLine);
  const decoded = decodeFloatBytes(role, bytes);
  container.append(createElement("p", "bit-steps", describeFloatDecoding(fields, layout, decoded)));
  return container;
}

function createBitsView(role, bytes) {
  if (varintRoles.has(role)) {
    return createVarintView(role, bytes);
  }
  if (u32Roles.has(role)) {
    return createU32View(bytes);
  }
  if (floatLayouts[role]) {
    return createFloatView(role, bytes);
  }
  if (role === "tag") {
    return createElement("p", "bit-steps", `${formatHexByte(bytes[0])} = ${bytes[0]} = ${formatBinaryByte(bytes[0])}`);
  }
  return null;
}

function updateTabState() {
  for (const button of document.querySelectorAll("#example-tabs button")) {
    button.setAttribute("aria-pressed", String(button.dataset.exampleId === state.example.id));
  }
}

function renderExample() {
  state.spanIndexByByte = buildSpanIndexByByte(state.example);
  renderExampleSummary();
  renderHexGrid();
  renderValueTree();
  renderArrayCosts();
  renderSources();
  updateTabState();
  restorePinnedSelection();
}

function selectExample(exampleId) {
  state.example = state.examples.find((example) => example.id === exampleId) || state.examples[0];
  state.pinnedSpanIndex = null;
  state.pinnedNodeId = null;
  history.replaceState(null, "", "#" + state.example.id);
  renderExample();
}

function isByteOffsetInExample(offset) {
  return Number.isInteger(offset) && offset >= 0 && offset < state.example.bytes.length;
}

// "#person@0x2f" opens the person example with byte 0x2f selected.
function selectFromLocationHash() {
  const [exampleId, offsetText] = location.hash.slice(1).split("@");
  selectExample(exampleId);
  const offset = Number(offsetText);
  if (offsetText && isByteOffsetInExample(offset)) {
    pinSpan(state.spanIndexByByte[offset]);
    state.byteCells[offset].scrollIntoView({ block: "center" });
  }
}

function chooseBytesPerRow() {
  return window.innerWidth <= narrowLayoutMaxWidth ? narrowBytesPerRow : wideBytesPerRow;
}

function handleResize() {
  const bytesPerRow = chooseBytesPerRow();
  if (bytesPerRow === state.bytesPerRow || !state.example) {
    return;
  }
  state.bytesPerRow = bytesPerRow;
  renderHexGrid();
  restorePinnedSelection();
}

function loadExamples() {
  if (!window.byteLayoutData) {
    document.getElementById("example-summary").textContent =
      "data.js is missing: run Apps/ByteLayout/serve.sh to generate it.";
    return;
  }
  state.examples = window.byteLayoutData.examples;
  state.bytesPerRow = chooseBytesPerRow();
  renderRoleLegend();
  renderExampleTabs();
  if (window.innerWidth <= narrowLayoutMaxWidth) {
    document.getElementById("format-legend").open = false;
  }
  selectFromLocationHash();
  window.addEventListener("resize", handleResize);
}

loadExamples();
