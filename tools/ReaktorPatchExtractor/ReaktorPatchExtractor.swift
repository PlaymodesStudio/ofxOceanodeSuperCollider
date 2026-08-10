import AppKit
import CryptoKit
import Foundation
import UniformTypeIdentifiers

private let moduleSignatures: Set<UInt32> = [0x00228D00, 0x0022F300]
private let rootSentinelID: UInt32 = 0x75646F4D
private let panelControlKinds: Set<UInt32> = [20, 22, 24, 25]
private let applicationVersion = Bundle.main.object(
    forInfoDictionaryKey: "CFBundleShortVersionString"
) as? String ?? "development"

private func signalType(_ code: UInt32?) -> String {
    switch code {
    case 0: return "unspecified_or_hybrid"
    case 1: return "audio"
    case 2: return "event_table_or_internal"
    case 3: return "event_or_control"
    default: return "unknown"
    }
}

private func loadBundledCatalog() -> [String: Any] {
    var candidates: [URL] = []
    if let bundled = Bundle.main.url(forResource: "module_catalog", withExtension: "json") {
        candidates.append(bundled)
    }
    let executable = URL(fileURLWithPath: CommandLine.arguments[0]).standardizedFileURL
    candidates.append(executable.deletingLastPathComponent().appendingPathComponent("../Resources/module_catalog.json").standardizedFileURL)
    candidates.append(executable.deletingLastPathComponent().appendingPathComponent("module_catalog.json"))
    for url in candidates {
        if let data = try? Data(contentsOf: url),
           let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any] {
            return object
        }
    }
    return [:]
}

enum NRKTError: LocalizedError {
    case truncated(Int)
    case invalid(String)

    var errorDescription: String? {
        switch self {
        case .truncated(let offset): return "Fitxer truncat prop de 0x\(String(offset, radix: 16))"
        case .invalid(let message): return message
        }
    }
}

final class ModuleNode {
    let offset: Int
    let version: UInt32
    let signature: UInt32
    let kind: UInt32
    let objectID: UInt32
    var name: String?
    var detail: String?
    var childCount = 0
    var inputCount: UInt32?
    var outputCount: UInt32?
    var x: Int32?
    var y: Int32?
    var width: UInt32?
    var height: UInt32?
    var typeName: String?
    var parentObjectID: UInt32?
    var hierarchyPath: String?
    var instanceLabel: String?
    var instanceDescription: String?
    var controlProperties: [String: Double]?
    var moduleProperties: [String: Double]?
    var savedControlRawU32: UInt32?
    var savedControlNormalized: Double?
    var savedControlValue: Double?
    var constantValue: String?
    var children: [ModuleNode] = []

    init(offset: Int, version: UInt32, signature: UInt32, kind: UInt32, objectID: UInt32) {
        self.offset = offset
        self.version = version
        self.signature = signature
        self.kind = kind
        self.objectID = objectID
    }

    var label: String { name ?? instanceLabel ?? typeName ?? "kind-\(kind)" }

    func jsonObject() -> [String: Any] {
        var result: [String: Any] = [
            "offset": String(format: "0x%x", offset),
            "version": version,
            "signature": String(format: "0x%08x", signature),
            "kind": kind,
            "object_id": objectID,
            "child_count": childCount,
            "children": children.map { $0.jsonObject() }
        ]
        if let name { result["name"] = name }
        if let detail, !detail.isEmpty { result["description"] = detail }
        if let inputCount { result["input_count"] = inputCount }
        if let outputCount { result["output_count"] = outputCount }
        if let x { result["x"] = x }
        if let y { result["y"] = y }
        if let width { result["width"] = width }
        if let height { result["height"] = height }
        if let typeName { result["type_name"] = typeName }
        if let parentObjectID { result["parent_object_id"] = parentObjectID }
        if let hierarchyPath { result["hierarchy_path"] = hierarchyPath }
        if let instanceLabel { result["instance_label"] = instanceLabel }
        if let instanceDescription { result["instance_description"] = instanceDescription }
        if let controlProperties { result["control_properties"] = controlProperties }
        if let moduleProperties { result["module_properties"] = moduleProperties }
        if let savedControlRawU32 { result["saved_control_raw_u32"] = savedControlRawU32 }
        if let savedControlNormalized { result["saved_control_normalized"] = savedControlNormalized }
        if let savedControlValue { result["saved_control_value"] = savedControlValue }
        if let constantValue { result["constant_value"] = constantValue }
        return result
    }
}

struct ConnectionRecord {
    let targetLocalIndex: UInt32
    let targetInputIndex: UInt32
}

struct PortRecord {
    let offset: Int
    let className: String
    var sourceObjectID: UInt32?
    var sourcePortRecordIndex: Int?
    var signalCode: UInt32?
    var declaredPortIndex: Int32?
    var encoding: UInt32?
    var name: String?
    var detail: String?
    var connections: [ConnectionRecord] = []

    func jsonObject() -> [String: Any] {
        var result: [String: Any] = [
            "offset": String(format: "0x%x", offset),
            "class_name": className,
            "connections": connections.map {
                ["target_local_index": $0.targetLocalIndex,
                 "target_input_index": $0.targetInputIndex]
            }
        ]
        if let sourceObjectID { result["source_object_id"] = sourceObjectID }
        if let sourcePortRecordIndex { result["source_port_record_index"] = sourcePortRecordIndex }
        if let signalCode { result["signal_code"] = signalCode }
        result["signal_type"] = signalType(signalCode)
        if let declaredPortIndex { result["declared_port_index"] = declaredPortIndex }
        if let encoding { result["encoding"] = encoding }
        if let name { result["name"] = name }
        if let detail, !detail.isEmpty { result["description"] = detail }
        return result
    }
}

struct WireRecord {
    let sourceObjectID: UInt32
    let sourceName: String?
    let sourceKind: UInt32?
    let sourceOutputIndex: Int?
    let declaredPortIndex: Int32?
    let targetLocalIndex: UInt32
    let targetInputIndex: UInt32
    let targetObjectID: UInt32?
    let targetName: String?
    let targetKind: UInt32?
    let targetInputName: String?

    func jsonObject() -> [String: Any] {
        var result: [String: Any] = [
            "source_object_id": sourceObjectID,
            "target_local_index": targetLocalIndex,
            "target_input_index": targetInputIndex
        ]
        if let sourceName { result["source_name"] = sourceName }
        if let sourceKind { result["source_kind"] = sourceKind }
        if let sourceOutputIndex { result["source_output_index"] = sourceOutputIndex }
        if let declaredPortIndex { result["declared_port_index"] = declaredPortIndex }
        if let targetObjectID { result["target_object_id"] = targetObjectID }
        if let targetName { result["target_name"] = targetName }
        if let targetKind { result["target_kind"] = targetKind }
        if let targetInputName { result["target_input_name"] = targetInputName }
        return result
    }
}

struct ParseReport {
    let sourceURL: URL
    let size: Int
    let nrktHeaders: [Int]
    let modules: [ModuleNode]
    let specialRootCount: Int
    let forest: [ModuleNode]
    let ports: [PortRecord]
    let wires: [WireRecord]
    let snapshots: [[String: Any]]
    let coreCells: [[String: Any]]
    let catalog: [String: Any]
    let coreCatalog: [String: Any]
    let warnings: [String]

    var inputPortCount: Int { ports.filter { $0.className == "KInPort" }.count }
    var outputPortCount: Int { ports.filter { $0.className == "KOutPort" }.count }
    var macroCount: Int { modules.filter { $0.kind == 4 }.count }
    var resolvedWireCount: Int { wires.filter { $0.targetObjectID != nil }.count }
    var decodedCoreCellCount: Int {
        coreCells.filter {
            (($0["core_document"] as? [String: Any])?["parse_status"] as? String) == "decoded"
        }.count
    }
    var coreGraphTotals: (structures: Int, modules: Int, connections: Int) {
        var result = (0, 0, 0)
        for cell in coreCells {
            guard let document = cell["core_document"] as? [String: Any],
                  let counts = document["graph_counts"] as? [String: Any] else { continue }
            result.0 += counts["structures"] as? Int ?? 0
            result.1 += counts["modules"] as? Int ?? 0
            result.2 += counts["connections"] as? Int ?? 0
        }
        return result
    }

    func jsonData() throws -> Data {
        var kinds: [String: Int] = [:]
        for module in modules { kinds[String(module.kind), default: 0] += 1 }
        let coreTotals = coreGraphTotals
        let object: [String: Any] = [
            "source": sourceURL.path,
            "schema": "reaktor-patch-report-v4",
            "size_bytes": size,
            "nrkt_headers": nrktHeaders.map { String(format: "0x%x", $0) },
            "counts": [
                "modules": modules.count,
                "special_root_records": specialRootCount,
                "macros": macroCount,
                "input_ports": inputPortCount,
                "output_ports": outputPortCount,
                "connections": wires.count,
                "resolved_connections": resolvedWireCount,
                "snapshots": snapshots.count,
                "snapshot_parameter_records": snapshots.reduce(0) { $0 + ($1["parameter_count"] as? Int ?? 0) },
                "core_cells": coreCells.count,
                "decoded_core_cells": decodedCoreCellCount,
                "core_structures": coreTotals.structures,
                "core_modules": coreTotals.modules,
                "core_connections": coreTotals.connections,
                "module_kinds": kinds
            ],
            "catalog": catalog,
            "core_catalog": coreCatalog,
            "top_level": forest.map { $0.jsonObject() },
            "ports": ports.map { $0.jsonObject() },
            "connections": wires.map { $0.jsonObject() },
            "snapshots": snapshots,
            "core_cells": coreCells,
            "warnings": warnings
        ]
        return try JSONSerialization.data(withJSONObject: object, options: [.prettyPrinted, .sortedKeys])
    }

    func dotText() -> String {
        func escaped(_ value: String) -> String {
            value.replacingOccurrences(of: "\\", with: "\\\\")
                .replacingOccurrences(of: "\"", with: "\\\"")
                .replacingOccurrences(of: "\n", with: "\\n")
        }
        var lines = [
            "digraph ReaktorPatch {",
            "  graph [rankdir=LR, overlap=false, splines=true];",
            "  node [shape=box, fontname=\"Helvetica\", fontsize=10];",
            "  edge [fontname=\"Helvetica\", fontsize=8];"
        ]
        for module in modules {
            let title = module.name ?? "kind \(module.kind)"
            lines.append("  n\(module.objectID) [label=\"\(escaped(title))\\nid \(module.objectID) · kind \(module.kind)\"];")
        }
        for wire in wires {
            guard let target = wire.targetObjectID else { continue }
            let input = wire.targetInputName ?? "in \(wire.targetInputIndex)"
            let output = wire.sourceOutputIndex.map { "out \($0)" } ?? "out"
            lines.append("  n\(wire.sourceObjectID) -> n\(target) [label=\"\(escaped(output)) → \(escaped(input))\"];")
        }
        lines.append("}")
        return lines.joined(separator: "\n") + "\n"
    }

    func summaryText() -> String {
        var lines = [
            "Reaktor Patch Extractor \(applicationVersion)",
            "",
            "Fitxer: \(sourceURL.path)",
            "Mida: \(size) bytes",
            "Mòduls: \(modules.count) (\(macroCount) macros)",
            "Ports: \(inputPortCount) entrades · \(outputPortCount) sortides",
            "Connexions: \(wires.count) · resoltes: \(resolvedWireCount)",
            "Snapshots: \(snapshots.count)",
            "Core Cells: \(decodedCoreCellCount)/\(coreCells.count) descodificades · \(coreGraphTotals.modules) mòduls · \(coreGraphTotals.connections) connexions",
            "Catàleg: \(catalog["documented_module_count"] ?? "?") tipus documentats",
            "Avisos: \(warnings.count)",
            "",
            "Jerarquia de macros:",
        ]
        func visit(_ nodes: [ModuleNode], _ depth: Int) {
            for node in nodes {
                if node.kind == 4 || node.kind == 9 {
                    lines.append(String(repeating: "  ", count: depth) + "• \(node.label) [id \(node.objectID), kind \(node.kind), fills \(node.childCount)]")
                    visit(node.children, depth + 1)
                } else {
                    visit(node.children, depth)
                }
            }
        }
        visit(forest, 0)
        if !warnings.isEmpty {
            lines.append("")
            lines.append("Avisos:")
            lines.append(contentsOf: warnings.map { "• \($0)" })
        }
        return lines.joined(separator: "\n")
    }
}

final class NRKTParser {
    private let data: Data
    private let sourceURL: URL
    private let moduleMarker = Data([0x5b, 0x07, 0, 0, 0] + Array("KSModul".utf8))
    private let inPortMarker = Data([0x5b, 0x07, 0, 0, 0] + Array("KInPort".utf8))
    private let outPortMarker = Data([0x5b, 0x08, 0, 0, 0] + Array("KOutPort".utf8))
    private let catalog = loadBundledCatalog()

    init(url: URL) throws {
        sourceURL = url
        data = try Data(contentsOf: url, options: .mappedIfSafe)
    }

    private func u32(_ offset: Int) throws -> UInt32 {
        guard offset >= 0, offset + 4 <= data.count else { throw NRKTError.truncated(offset) }
        return UInt32(data[offset])
            | (UInt32(data[offset + 1]) << 8)
            | (UInt32(data[offset + 2]) << 16)
            | (UInt32(data[offset + 3]) << 24)
    }

    private func i32(_ value: UInt32) -> Int32 { Int32(bitPattern: value) }

    private func u64(_ offset: Int) throws -> UInt64 {
        UInt64(try u32(offset)) | (UInt64(try u32(offset + 4)) << 32)
    }

    private func f32(_ offset: Int) throws -> Float {
        Float(bitPattern: try u32(offset))
    }

    private func f64(_ offset: Int) throws -> Double {
        Double(bitPattern: try u64(offset))
    }

    private func allOffsets(of marker: Data) -> [Int] {
        guard !marker.isEmpty, data.count >= marker.count else { return [] }
        var result: [Int] = []
        var cursor = 0
        while cursor <= data.count - marker.count,
              let range = data.range(of: marker, options: [], in: cursor..<data.count) {
            result.append(range.lowerBound)
            cursor = range.lowerBound + 1
        }
        return result
    }

    private func nrktString(_ lengthOffset: Int) throws -> (String, Int) {
        let length = Int(try u32(lengthOffset))
        guard length <= 1 << 20 else {
            throw NRKTError.invalid("Longitud de text inversemblant a 0x\(String(lengthOffset, radix: 16))")
        }
        let start = lengthOffset + 4
        let end = start + length
        guard end <= data.count else { throw NRKTError.truncated(start) }
        return (String(decoding: data[start..<end], as: UTF8.self), end)
    }

    private func printableStrings(start: Int, end: Int) -> [(Int, String)] {
        guard start >= 0, end <= data.count, start + 4 < end else { return [] }
        var result: [(Int, String)] = []
        for offset in start..<(end - 4) {
            guard let length = try? u32(offset), length > 0, length <= 16_384 else { continue }
            let textStart = offset + 4
            let textEnd = textStart + Int(length)
            guard textEnd <= end,
                  let value = String(data: data[textStart..<textEnd], encoding: .utf8),
                  !value.isEmpty,
                  value.unicodeScalars.allSatisfy({
                      !CharacterSet.controlCharacters.contains($0) || [9, 10, 13].contains($0.value)
                  }) else { continue }
            result.append((offset, value))
        }
        return result
    }

    private func parseInstanceMetadata(_ modules: [ModuleNode]) {
        for index in modules.indices {
            let module = modules[index]
            let end = index + 1 < modules.count ? modules[index + 1].offset : data.count
            let limit = min(end, module.offset + 0x1000)
            let strings = printableStrings(start: module.offset, end: limit).filter {
                !["KSModul", "KInPort", "KOutPort"].contains($0.1)
            }
            if panelControlKinds.contains(module.kind) {
                if strings.count >= 2 {
                    for pairIndex in 0..<(strings.count - 1) {
                        let first = strings[pairIndex]
                        let second = strings[pairIndex + 1]
                        if first.0 >= module.offset + 0x100,
                           second.0 == first.0 + 4 + first.1.utf8.count,
                           first.1.utf8.count <= 80, second.1.utf8.count >= 8 {
                            module.instanceLabel = first.1
                            module.instanceDescription = second.1
                            break
                        }
                    }
                }
                if module.offset + 0x64 <= end,
                   let step = try? f64(module.offset + 0x48),
                   let endpoint1 = try? f32(module.offset + 0x5c),
                   let endpoint2 = try? f32(module.offset + 0x60),
                   step.isFinite, endpoint1.isFinite, endpoint2.isFinite,
                   abs(step) >= 1e-12, abs(step) < 1e9,
                   abs(endpoint1) < 1e12, abs(endpoint2) < 1e12 {
                    module.controlProperties = [
                        "step_or_resolution": step,
                        "range_endpoint_1": Double(endpoint1),
                        "range_endpoint_2": Double(endpoint2)
                    ]
                }

                // Primary panel controls keep their saved/global state in the
                // module record.  Snapshot-isolated controls do not appear in
                // a snapshot's parameter list, so this state is required to
                // reconstruct the complete effective preset.
                let stateOffset = module.offset + 0x112
                if stateOffset + 4 <= end, let raw = try? u32(stateOffset) {
                    module.savedControlRawU32 = raw

                    var normalized: Double?
                    if module.kind == 20, let value = try? f32(stateOffset), value.isFinite {
                        let candidate = Double(value)
                        if candidate >= -0.000001 && candidate <= 1.000001 {
                            normalized = min(1.0, max(0.0, candidate))
                        }
                    } else if module.kind == 22, raw <= 1 {
                        normalized = Double(raw)
                    } else if module.kind == 24,
                              let properties = module.controlProperties,
                              properties["range_endpoint_1"] == 0,
                              properties["range_endpoint_2"] == 1 {
                        // Two-position Primary switches use a small integer
                        // selection code here (observed as 0, 1 or 2).
                        normalized = raw == 0 ? 0.0 : 1.0
                    }

                    if let normalized {
                        module.savedControlNormalized = normalized
                        if let properties = module.controlProperties,
                           let endpoint1 = properties["range_endpoint_1"],
                           let endpoint2 = properties["range_endpoint_2"] {
                            module.savedControlValue = endpoint1
                                + normalized * (endpoint2 - endpoint1)
                        }
                    }
                }
            } else if module.kind == 50 {
                // Primary Note Pitch stores its Transpose property directly
                // in the module record.  This is part of the DSP graph: the
                // SteamPipe noise and MW-filter key trackers use -60, while
                // its tuning and feedback paths use 0.
                if module.offset + 0x48 <= end,
                   let pitchOffset = try? f32(module.offset + 0x44),
                   pitchOffset.isFinite, abs(pitchOffset) <= 240 {
                    module.moduleProperties = [
                        "pitch_offset_semitones": Double(pitchOffset)
                    ]
                }
            } else if module.kind == 100 {
                for (offset, value) in strings where offset >= module.offset + 0x80 {
                    if Double(value) != nil {
                        module.constantValue = value
                        break
                    }
                }
            }
        }
    }

    private func parseMacroMetadata(_ module: ModuleNode) {
        do {
            let nameOffset = module.offset + (module.signature == 0x00228D00 ? 0xf0 : 0x104)
            let trailerVersion: UInt32 = module.signature == 0x00228D00 ? 2 : 4
            let (name, afterName) = try nrktString(nameOffset)
            let (detail, afterDetail) = try nrktString(afterName)
            let limit = min(data.count, afterDetail + 32)
            guard afterDetail < limit,
                  let close = data[afterDetail..<limit].firstIndex(of: 0x5d),
                  close + 9 <= data.count,
                  try u32(close + 1) == trailerVersion else { return }
            module.name = name
            module.detail = detail.isEmpty ? nil : detail
            module.childCount = Int(try u32(close + 5))
            module.inputCount = try u32(module.offset + 0x20)
            module.outputCount = try u32(module.offset + 0x24)
            module.x = i32(try u32(module.offset + 0x30))
            module.y = i32(try u32(module.offset + 0x34))
            module.width = try u32(module.offset + 0x38)
            module.height = try u32(module.offset + 0x3c)
        } catch { return }
    }

    private func parseModules() throws -> ([ModuleNode], Int) {
        var modules: [ModuleNode] = []
        var specialRoots = 0
        for offset in allOffsets(of: moduleMarker) {
            guard offset + 28 <= data.count else { continue }
            let version = try u32(offset + 12)
            let signature = try u32(offset + 16)
            guard moduleSignatures.contains(signature) else { continue }
            let kind = try u32(offset + 20)
            let objectID = try u32(offset + 24)
            let module = ModuleNode(offset: offset, version: version, signature: signature,
                                    kind: kind, objectID: objectID)
            if kind == 4 { parseMacroMetadata(module) }
            if objectID == rootSentinelID { specialRoots += 1 } else { modules.append(module) }
        }

        for index in modules.indices where modules[index].kind == 9 {
            let module = modules[index]
            let end = index + 1 < modules.count ? modules[index + 1].offset : data.count
            var cursor = module.offset
            while cursor < end, let close = data[cursor..<end].firstIndex(of: 0x5d) {
                if close + 9 <= end, [2, 4].contains(try u32(close + 1)) {
                    let candidate = Int(try u32(close + 5))
                    if candidate > 0 && candidate < 10_000 { module.childCount = candidate }
                }
                cursor = close + 1
            }
        }
        return (modules, specialRoots)
    }

    private func buildForest(_ modules: [ModuleNode], warnings: inout [String]) -> [ModuleNode] {
        var index = 0
        func consume() -> ModuleNode? {
            guard index < modules.count else { return nil }
            let node = modules[index]
            index += 1
            node.children = []
            for childNumber in 0..<node.childCount {
                guard let child = consume() else {
                    warnings.append("\(node.label) esperava \(node.childCount) fills; el flux acaba al fill \(childNumber)")
                    break
                }
                node.children.append(child)
            }
            return node
        }
        var forest: [ModuleNode] = []
        while index < modules.count { if let node = consume() { forest.append(node) } }
        return forest
    }

    private func enrichHierarchy(_ forest: [ModuleNode]) {
        let catalogModules = catalog["modules"] as? [String: Any] ?? [:]
        func visit(_ scope: [ModuleNode], parent: ModuleNode?, prefix: String) {
            for node in scope {
                let entry = catalogModules[String(node.kind)] as? [String: Any]
                node.typeName = entry?["name"] as? String
                node.parentObjectID = parent?.objectID
                let segment = node.name ?? node.instanceLabel ?? node.typeName ?? "kind-\(node.kind)"
                node.hierarchyPath = prefix.isEmpty
                    ? "/\(segment)#\(node.objectID)"
                    : "\(prefix)/\(segment)#\(node.objectID)"
                visit(node.children, parent: node, prefix: node.hierarchyPath!)
            }
        }
        visit(forest, parent: nil, prefix: "")
    }

    private func catalogReport(usedKinds: Set<UInt32>) -> [String: Any] {
        let allModules = catalog["modules"] as? [String: Any] ?? [:]
        var used: [String: Any] = [:]
        for kind in usedKinds {
            used[String(kind)] = allModules[String(kind)] ?? [
                "kind": kind, "status": "unknown"
            ]
        }
        var result: [String: Any] = ["used_module_types": used]
        for key in ["schema", "source_sha256", "source_product_version", "module_count",
                    "named_module_count", "documented_module_count",
                    "initialized_tmoddata_count", "documented_port_count",
                    "audio_function_count"] {
            if let value = catalog[key] { result[key] = value }
        }
        return result
    }

    private func parseSnapshots(modules: [ModuleNode]) -> [[String: Any]] {
        let moduleIDs = Set(modules.map(\.objectID))
        let modulesByID = Dictionary(uniqueKeysWithValues: modules.map { ($0.objectID, $0) })
        let preferredOffsets = modules.filter { $0.kind == 9 }.map {
            Int64($0.objectID) - 1
        }
        var result: [[String: Any]] = []
        func numericValue(_ value: Any?) -> Double? {
            if let value = value as? Double { return value }
            if let value = value as? Float { return Double(value) }
            if let value = value as? Int { return Double(value) }
            if let value = value as? Int32 { return Double(value) }
            if let value = value as? UInt32 { return Double(value) }
            return nil
        }
        guard data.count >= 16 else { return result }
        for offset in 0..<(data.count - 16) {
            guard let nameLengthValue = try? u32(offset) else { continue }
            let nameLength = Int(nameLengthValue)
            guard nameLength >= 1, nameLength <= 512 else { continue }
            let nameEnd = offset + 4 + nameLength
            guard nameEnd + 108 <= data.count,
                  (try? u32(nameEnd)) == 9,
                  (try? u32(nameEnd + 4)) == 2,
                  (try? u32(nameEnd + 8)) == 9,
                  let name = String(data: data[(offset + 4)..<nameEnd], encoding: .utf8),
                  !name.isEmpty,
                  name.unicodeScalars.allSatisfy({ !CharacterSet.controlCharacters.contains($0) }) else { continue }
            var cursor = nameEnd + 108
            var metadataExtensionBytes = 0
            func parameterRecordStarts(_ offset: Int) -> Bool {
                guard offset + 12 <= data.count else { return false }
                return [32, 40, 60].contains(Int((try? u32(offset)) ?? 0))
                    && (try? u32(offset + 4)) == 9
                    && (try? u32(offset + 8)) == 2
            }
            if !parameterRecordStarts(cursor) {
                guard parameterRecordStarts(cursor + 8) else {
                    continue
                }
                cursor += 8
                metadataExtensionBytes = 8
            }
            var parameters: [[String: Any]] = []
            while cursor + 8 <= data.count {
                guard let sizeValue = try? u32(cursor) else { break }
                let size = Int(sizeValue)
                guard [32, 40, 60].contains(size), cursor + size + 8 <= data.count,
                      (try? u32(cursor + 4)) == 9,
                      (try? u32(cursor + 8)) == 2,
                      let typeCode = try? u32(cursor + 12),
                      let localID = try? u32(cursor + 4 + size),
                      localID != 0 else { break }
                var record: [String: Any] = [
                    "module_object_id": localID,
                    "record_size": size,
                    "value_type_code": typeCode
                ]
                if size == 40, let rawValue = try? u32(cursor + 36) {
                    record["raw_value_u32"] = rawValue
                    if typeCode == 1 {
                        record["value_type"] = "integer"
                        record["value"] = Int32(bitPattern: rawValue)
                    } else if typeCode == 2 {
                        record["value_type"] = "normalized_float"
                        record["value"] = Double(Float(bitPattern: rawValue))
                    } else {
                        record["value_type"] = "unknown"
                    }
                } else if size == 32 {
                    record["value_type"] = "empty_or_null_state"
                } else {
                    record["value_type"] = "compound_state"
                    var words: [UInt32] = []
                    for wordOffset in stride(from: cursor + 4, to: cursor + 4 + size, by: 4) {
                        if let word = try? u32(wordOffset) { words.append(word) }
                    }
                    record["payload_words"] = words
                }
                parameters.append(record)
                cursor += size + 8
            }
            guard let firstParameter = parameters.first,
                  let firstLocalID = firstParameter["module_object_id"] as? UInt32 else { continue }
            var orderedOffsets = preferredOffsets
            orderedOffsets.append(0)
            orderedOffsets.append(contentsOf: moduleIDs.map { Int64($0) - Int64(firstLocalID) }.sorted { abs($0) < abs($1) })
            var seenOffsets: Set<Int64> = []
            orderedOffsets = orderedOffsets.filter { seenOffsets.insert($0).inserted }
            var bestOffset = orderedOffsets.first ?? 0
            var bestScore = -1
            for candidate in orderedOffsets {
                let score = parameters.reduce(0) { partial, record in
                    guard let local = record["module_object_id"] as? UInt32 else { return partial }
                    let adjusted = Int64(local) + candidate
                    return partial + (adjusted >= 0 && adjusted <= Int64(UInt32.max)
                        && moduleIDs.contains(UInt32(adjusted)) ? 1 : 0)
                }
                if score > bestScore {
                    bestOffset = candidate
                    bestScore = score
                }
            }
            let mappingConfident = Double(bestScore) / Double(parameters.count) >= 0.9
            guard mappingConfident || (metadataExtensionBytes == 8 && parameters.count >= 3) else {
                continue
            }
            for index in parameters.indices {
                let local = parameters[index]["module_object_id"] as! UInt32
                parameters[index]["local_module_object_id"] = local
                if mappingConfident {
                    let globalID = UInt32(Int64(local) + bestOffset)
                    parameters[index]["module_object_id"] = globalID
                    if let module = modulesByID[globalID] {
                        parameters[index]["module_kind"] = module.kind
                        if let value = module.typeName { parameters[index]["module_type_name"] = value }
                        if let value = module.instanceLabel { parameters[index]["instance_label"] = value }
                        if let value = module.hierarchyPath { parameters[index]["hierarchy_path"] = value }
                    }
                } else {
                    parameters[index].removeValue(forKey: "module_object_id")
                }
            }
            var snapshot: [String: Any] = [
                "offset": String(format: "0x%x", offset),
                "name": name,
                "parameter_count": parameters.count,
                "resolved_parameter_count": bestScore,
                "candidate_object_id_offset": bestOffset,
                "object_id_mapping_status": mappingConfident ? "resolved" : "unresolved",
                "metadata_extension_bytes": metadataExtensionBytes,
                "parameters": parameters
            ]
            if mappingConfident {
                snapshot["object_id_offset"] = bestOffset

                var overrides: [UInt32: Double] = [:]
                for parameter in parameters {
                    guard let objectID = parameter["module_object_id"] as? UInt32,
                          let value = numericValue(parameter["value"]) else { continue }
                    overrides[objectID] = value
                }

                var effectiveControls: [[String: Any]] = []
                for module in modules {
                    guard var normalized = module.savedControlNormalized else { continue }
                    var source = "module_saved_state"
                    if let override = overrides[module.objectID] {
                        normalized = min(1.0, max(0.0, override))
                        source = "snapshot_override"
                    }

                    var control: [String: Any] = [
                        "module_object_id": module.objectID,
                        "module_kind": module.kind,
                        "normalized_value": normalized,
                        "source": source
                    ]
                    if let value = module.typeName { control["module_type_name"] = value }
                    if let value = module.instanceLabel { control["instance_label"] = value }
                    if let value = module.hierarchyPath { control["hierarchy_path"] = value }
                    if let properties = module.controlProperties {
                        control["control_properties"] = properties
                        if let endpoint1 = properties["range_endpoint_1"],
                           let endpoint2 = properties["range_endpoint_2"] {
                            control["value"] = endpoint1
                                + normalized * (endpoint2 - endpoint1)
                        }
                    }
                    effectiveControls.append(control)
                }
                snapshot["effective_control_count"] = effectiveControls.count
                snapshot["effective_controls"] = effectiveControls
            }
            result.append(snapshot)
        }
        return result
    }

    private func parseOutputPort(_ offset: Int) throws -> PortRecord {
        var cursor = offset + outPortMarker.count
        let version = try u32(cursor)
        let signal = try u32(cursor + 4)
        let declared = i32(try u32(cursor + 8))
        let encoding = try u32(cursor + 12)
        cursor += 16
        guard version == 1 else { throw NRKTError.invalid("Versió KOutPort \(version) no suportada") }
        var name: String?
        var detail: String?
        // The low three bits select the serialized text layout. Higher bits
        // are independent port flags, as on KInPort.
        switch encoding & 0x7 {
        case 0:
            (name, cursor) = try nrktString(cursor)
            (detail, cursor) = try nrktString(cursor)
        case 2: (detail, cursor) = try nrktString(cursor)
        case 4: (name, cursor) = try nrktString(cursor)
        case 6: break
        default: throw NRKTError.invalid("Encoding KOutPort \(encoding) desconegut a 0x\(String(offset, radix: 16))")
        }
        let count = Int(try u32(cursor))
        cursor += 4
        guard count < 10_000 else { throw NRKTError.invalid("Massa connexions a 0x\(String(offset, radix: 16))") }
        var connections: [ConnectionRecord] = []
        for _ in 0..<count {
            connections.append(ConnectionRecord(targetLocalIndex: try u32(cursor), targetInputIndex: try u32(cursor + 4)))
            cursor += 8
        }
        guard cursor < data.count, data[cursor] == 0x5d else {
            throw NRKTError.invalid("Final KOutPort inesperat a 0x\(String(cursor, radix: 16))")
        }
        return PortRecord(offset: offset, className: "KOutPort", signalCode: signal,
                          declaredPortIndex: declared, encoding: encoding,
                          name: name, detail: detail, connections: connections)
    }

    private func parseInputPort(_ offset: Int) throws -> PortRecord {
        var cursor = offset + inPortMarker.count
        let version = try u32(cursor)
        let signal = try u32(cursor + 4)
        let declared = i32(try u32(cursor + 8))
        let encoding = try u32(cursor + 20)
        cursor += 24
        guard version == 1 else { throw NRKTError.invalid("Versió KInPort \(version) no suportada") }
        var name: String?
        var detail: String?
        switch encoding & 0x7 {
        case 0:
            (name, cursor) = try nrktString(cursor)
            (detail, cursor) = try nrktString(cursor)
        case 2: (detail, cursor) = try nrktString(cursor)
        case 4: (name, cursor) = try nrktString(cursor)
        case 6: break
        default: throw NRKTError.invalid("Encoding KInPort \(encoding) desconegut a 0x\(String(offset, radix: 16))")
        }
        guard cursor < data.count, data[cursor] == 0x5d else {
            throw NRKTError.invalid("Final KInPort inesperat a 0x\(String(cursor, radix: 16))")
        }
        return PortRecord(offset: offset, className: "KInPort", signalCode: signal,
                          declaredPortIndex: declared, encoding: encoding,
                          name: name, detail: detail)
    }

    private func precedingModuleIndex(_ offset: Int, in modules: [ModuleNode]) -> Int? {
        var low = 0
        var high = modules.count
        while low < high {
            let middle = (low + high) / 2
            if modules[middle].offset <= offset { low = middle + 1 } else { high = middle }
        }
        return low > 0 ? low - 1 : nil
    }

    private func parsePorts(_ modules: [ModuleNode], warnings: inout [String]) -> [PortRecord] {
        var markers = allOffsets(of: inPortMarker).map { ($0, "KInPort") }
        markers += allOffsets(of: outPortMarker).map { ($0, "KOutPort") }
        markers.sort { $0.0 < $1.0 }
        var inputIndices: [UInt32: Int] = [:]
        var outputIndices: [UInt32: Int] = [:]
        var ports: [PortRecord] = []
        for (offset, className) in markers {
            do {
                var port = try className == "KInPort" ? parseInputPort(offset) : parseOutputPort(offset)
                if let moduleIndex = precedingModuleIndex(offset, in: modules) {
                    let sourceID = modules[moduleIndex].objectID
                    port.sourceObjectID = sourceID
                    if className == "KInPort" {
                        port.sourcePortRecordIndex = inputIndices[sourceID, default: 0]
                        inputIndices[sourceID, default: 0] += 1
                    } else {
                        port.sourcePortRecordIndex = outputIndices[sourceID, default: 0]
                        outputIndices[sourceID, default: 0] += 1
                    }
                }
                ports.append(port)
            } catch {
                warnings.append(error.localizedDescription + " (0x\(String(offset, radix: 16)))")
                ports.append(PortRecord(offset: offset, className: className))
            }
        }
        return ports
    }

    private func resolveWires(forest: [ModuleNode], ports: [PortRecord]) -> [WireRecord] {
        var modulesByID: [UInt32: ModuleNode] = [:]
        var scopesByID: [UInt32: [ModuleNode]] = [:]
        func index(_ scope: [ModuleNode]) {
            for node in scope {
                modulesByID[node.objectID] = node
                scopesByID[node.objectID] = scope
                index(node.children)
            }
        }
        index(forest)
        var inputNames: [String: String] = [:]
        for port in ports where port.className == "KInPort" {
            if let id = port.sourceObjectID, let position = port.sourcePortRecordIndex, let name = port.name {
                inputNames["\(id):\(position)"] = name
            }
        }
        var wires: [WireRecord] = []
        for port in ports where port.className == "KOutPort" {
            guard let sourceID = port.sourceObjectID else { continue }
            let source = modulesByID[sourceID]
            let scope = scopesByID[sourceID] ?? []
            for connection in port.connections {
                let localIndex = Int(connection.targetLocalIndex)
                let target = localIndex < scope.count ? scope[localIndex] : nil
                let inputName = target.flatMap { inputNames["\($0.objectID):\(connection.targetInputIndex)"] }
                wires.append(WireRecord(
                    sourceObjectID: sourceID, sourceName: source?.name, sourceKind: source?.kind,
                    sourceOutputIndex: port.sourcePortRecordIndex, declaredPortIndex: port.declaredPortIndex,
                    targetLocalIndex: connection.targetLocalIndex, targetInputIndex: connection.targetInputIndex,
                    targetObjectID: target?.objectID, targetName: target?.name, targetKind: target?.kind,
                    targetInputName: inputName))
            }
        }
        return wires
    }

    func parse() throws -> ParseReport {
        let (modules, specialRoots) = try parseModules()
        parseInstanceMetadata(modules)
        var warnings: [String] = []
        let forest = buildForest(modules, warnings: &warnings)
        enrichHierarchy(forest)
        let ports = parsePorts(modules, warnings: &warnings)
        let wires = resolveWires(forest: forest, ports: ports)
        let snapshots = parseSnapshots(modules: modules)
        let coreParser = CoreCellParser(data: data, modules: modules)
        let (coreCells, coreWarnings) = coreParser.extract()
        warnings.append(contentsOf: coreWarnings)
        let nrkt = allOffsets(of: Data("NRKT".utf8))
        return ParseReport(sourceURL: sourceURL, size: data.count, nrktHeaders: nrkt,
                           modules: modules, specialRootCount: specialRoots, forest: forest,
                           ports: ports, wires: wires, snapshots: snapshots, coreCells: coreCells,
                           catalog: catalogReport(usedKinds: Set(modules.map(\.kind))),
                           coreCatalog: coreParser.catalogReport(),
                           warnings: warnings)
    }
}

final class AppDelegate: NSObject, NSApplicationDelegate {
    private var window: NSWindow!
    private var textView: NSTextView!
    private var exportJSONButton: NSButton!
    private var exportDOTButton: NSButton!
    private var report: ParseReport?
    private var pendingURL: URL?

    func applicationDidFinishLaunching(_ notification: Notification) {
        buildWindow()
        if let pendingURL { load(pendingURL) }
    }

    private func buildWindow() {
        window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 900, height: 650),
                          styleMask: [.titled, .closable, .miniaturizable, .resizable],
                          backing: .buffered, defer: false)
        window.title = "Reaktor Patch Extractor \(applicationVersion)"
        window.center()

        let openButton = NSButton(title: "Obrir .ism / .ens…", target: self, action: #selector(openFile))
        exportJSONButton = NSButton(title: "Exportar JSON…", target: self, action: #selector(exportJSON))
        exportDOTButton = NSButton(title: "Exportar DOT…", target: self, action: #selector(exportDOT))
        exportJSONButton.isEnabled = false
        exportDOTButton.isEnabled = false

        let buttons = NSStackView(views: [openButton, exportJSONButton, exportDOTButton])
        buttons.orientation = .horizontal
        buttons.spacing = 10

        textView = NSTextView(frame: NSRect(x: 0, y: 0, width: 860, height: 560))
        textView.isEditable = false
        textView.isRichText = false
        textView.isVerticallyResizable = true
        textView.isHorizontallyResizable = false
        textView.autoresizingMask = [.width]
        textView.textContainer?.widthTracksTextView = true
        textView.textContainerInset = NSSize(width: 8, height: 8)
        textView.font = NSFont.monospacedSystemFont(ofSize: 12, weight: .regular)
        textView.string = "Obre un instrument .ism o un Ensemble .ens de Reaktor.\n\nL’aplicació és només de lectura."
        let scroll = NSScrollView()
        scroll.hasVerticalScroller = true
        scroll.autohidesScrollers = true
        scroll.documentView = textView

        let root = NSView()
        window.contentView = root
        buttons.translatesAutoresizingMaskIntoConstraints = false
        scroll.translatesAutoresizingMaskIntoConstraints = false
        root.addSubview(buttons)
        root.addSubview(scroll)
        NSLayoutConstraint.activate([
            buttons.leadingAnchor.constraint(equalTo: root.leadingAnchor, constant: 16),
            buttons.topAnchor.constraint(equalTo: root.topAnchor, constant: 16),
            scroll.leadingAnchor.constraint(equalTo: root.leadingAnchor, constant: 16),
            scroll.trailingAnchor.constraint(equalTo: root.trailingAnchor, constant: -16),
            scroll.topAnchor.constraint(equalTo: buttons.bottomAnchor, constant: 12),
            scroll.bottomAnchor.constraint(equalTo: root.bottomAnchor, constant: -16)
        ])
        window.makeKeyAndOrderFront(nil)
    }

    @objc private func openFile() {
        let panel = NSOpenPanel()
        panel.canChooseDirectories = false
        panel.allowsMultipleSelection = false
        panel.allowedContentTypes = ["ism", "ens"].compactMap { UTType(filenameExtension: $0) }
        if panel.runModal() == .OK, let url = panel.url { load(url) }
    }

    private func load(_ url: URL) {
        do {
            let parsed = try NRKTParser(url: url).parse()
            report = parsed
            textView.string = parsed.summaryText()
            exportJSONButton.isEnabled = true
            exportDOTButton.isEnabled = true
            window.title = "Reaktor Patch Extractor \(applicationVersion) — \(url.lastPathComponent)"
        } catch {
            report = nil
            textView.string = "No s’ha pogut analitzar el fitxer:\n\n\(error.localizedDescription)"
            exportJSONButton.isEnabled = false
            exportDOTButton.isEnabled = false
        }
    }

    private func savePanel(extension ext: String, source: URL) -> URL? {
        let panel = NSSavePanel()
        panel.allowedContentTypes = [UTType(filenameExtension: ext)].compactMap { $0 }
        panel.nameFieldStringValue = source.deletingPathExtension().lastPathComponent + "-reaktor." + ext
        return panel.runModal() == .OK ? panel.url : nil
    }

    @objc private func exportJSON() {
        guard let report, let url = savePanel(extension: "json", source: report.sourceURL) else { return }
        do { try report.jsonData().write(to: url, options: .atomic) }
        catch { textView.string += "\n\nError exportant JSON: \(error.localizedDescription)" }
    }

    @objc private func exportDOT() {
        guard let report, let url = savePanel(extension: "dot", source: report.sourceURL) else { return }
        do { try report.dotText().write(to: url, atomically: true, encoding: .utf8) }
        catch { textView.string += "\n\nError exportant DOT: \(error.localizedDescription)" }
    }

    func application(_ sender: NSApplication, openFiles filenames: [String]) {
        guard let first = filenames.first else { return }
        let url = URL(fileURLWithPath: first)
        if window == nil { pendingURL = url } else { load(url) }
        sender.reply(toOpenOrPrint: .success)
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }
}

private func writeStdout(_ data: Data) {
    FileHandle.standardOutput.write(data)
    FileHandle.standardOutput.write(Data("\n".utf8))
}

@main
struct ReaktorPatchExtractorMain {
    static func main() {
        let arguments = Array(CommandLine.arguments.dropFirst())
        let fileArgument = arguments.first { !$0.hasPrefix("-") }
        if let fileArgument {
            do {
                let report = try NRKTParser(url: URL(fileURLWithPath: fileArgument)).parse()
                if arguments.contains("--dot") {
                    writeStdout(Data(report.dotText().utf8))
                } else if arguments.contains("--summary") {
                    writeStdout(Data(report.summaryText().utf8))
                } else {
                    writeStdout(try report.jsonData())
                }
                exit(0)
            } catch {
                FileHandle.standardError.write(Data(("Error: \(error.localizedDescription)\n").utf8))
                exit(1)
            }
        } else {
            let app = NSApplication.shared
            let delegate = AppDelegate()
            app.delegate = delegate
            app.setActivationPolicy(.regular)
            app.activate(ignoringOtherApps: true)
            app.run()
        }
    }
}
