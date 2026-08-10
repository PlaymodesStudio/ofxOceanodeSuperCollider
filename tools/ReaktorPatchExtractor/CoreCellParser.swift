import CryptoKit
import Foundation

@_silgen_name("uncompress")
private func zlibUncompress(
    _ destination: UnsafeMutablePointer<UInt8>?,
    _ destinationLength: UnsafeMutablePointer<UInt>,
    _ source: UnsafePointer<UInt8>?,
    _ sourceLength: UInt
) -> Int32

private let coreCellKinds: Set<UInt32> = [524, 525]
private let coreDocumentMarker = Data("#NI#CS#Document##NI#Reaktor#Core#TaggedFile#".utf8)
private let coreDataMarker = Data([0x61, 0x74, 0x61, 0x64, 0x02, 0, 0, 0])

private enum CoreTag {
    static let file: UInt32 = 0x029349BA
    static let fileType: UInt32 = 0x02A26E32
    static let fileTypeValue: UInt32 = 0x03A32928
    static let structure: UInt32 = 0x028F2D33
    static let structureMeta: UInt32 = 0x03A32D33
    static let structureBounds: UInt32 = 0x02C25D33
    static let moduleArray: UInt32 = 0x02CE4BED
    static let arrayCount: UInt32 = 0x03A3392D
    static let arrayValues: UInt32 = 0x028F392D
    static let module: UInt32 = 0x02B24BED
    static let moduleMeta: UInt32 = 0x03A2C92D
    static let modulePosition: UInt32 = 0x02C2592D
    static let positionValue: UInt32 = 0x03A3096D
    static let properties: UInt32 = 0x02CF0CB0
    static let propertyCount: UInt32 = 0x03A33CB0
    static let propertyValues: UInt32 = 0x028F3CB0
    static let property: UInt32 = 0x02E70CB0
    static let propertyMeta: UInt32 = 0x03A39CB0
    static let propertyValue: UInt32 = 0x028B9CB0
    static let stringValue: UInt32 = 0x03A30D33
    static let intValue: UInt32 = 0x03A30BA9
    static let connectionArray: UInt32 = 0x02CEEBE3
    static let connectionCount: UInt32 = 0x03A33BE3
    static let connectionValues: UInt32 = 0x028F3BE3
    static let connection: UInt32 = 0x04BAEBE3
    static let connectionSource: UInt32 = 0x05CEEBE3
    static let connectionTarget: UInt32 = 0x0392EBE3
}

private struct CoreTagRecord {
    let offset: Int
    let tag: UInt32
    let size: Int
    let payloadOffset: Int
    let end: Int
}

private func coreSHA256(_ data: Data) -> String {
    SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined()
}

private func loadBundledCoreCatalog() -> [String: Any] {
    var candidates: [URL] = []
    if let bundled = Bundle.main.url(forResource: "core_module_catalog", withExtension: "json") {
        candidates.append(bundled)
    }
    let executable = URL(fileURLWithPath: CommandLine.arguments[0]).standardizedFileURL
    candidates.append(executable.deletingLastPathComponent()
        .appendingPathComponent("../Resources/core_module_catalog.json").standardizedFileURL)
    candidates.append(executable.deletingLastPathComponent().appendingPathComponent("core_module_catalog.json"))
    for url in candidates {
        if let data = try? Data(contentsOf: url),
           let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any] {
            return object
        }
    }
    return [:]
}

final class CoreCellParser {
    private let source: Data
    private let modules: [ModuleNode]
    private let catalog: [String: Any]

    init(data: Data, modules: [ModuleNode]) {
        source = data
        self.modules = modules
        catalog = loadBundledCoreCatalog()
    }

    func catalogReport() -> [String: Any] {
        var result: [String: Any] = [:]
        for key in ["schema", "source_sha256", "module_count"] {
            if let value = catalog[key] { result[key] = value }
        }
        return result
    }

    private func u32(_ data: Data, _ offset: Int) -> UInt32? {
        guard offset >= 0, offset + 4 <= data.count else { return nil }
        return UInt32(data[offset])
            | (UInt32(data[offset + 1]) << 8)
            | (UInt32(data[offset + 2]) << 16)
            | (UInt32(data[offset + 3]) << 24)
    }

    private func i32(_ value: UInt32) -> Int32 { Int32(bitPattern: value) }

    private func allOffsets(of marker: Data, in data: Data) -> [Int] {
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

    private func records(_ data: Data, _ start: Int, _ end: Int) -> [CoreTagRecord]? {
        guard start >= 0, end <= data.count, start <= end else { return nil }
        var result: [CoreTagRecord] = []
        var cursor = start
        while cursor + 8 <= end {
            guard let tag = u32(data, cursor), let sizeValue = u32(data, cursor + 4) else { return nil }
            let size = Int(sizeValue)
            let payloadOffset = cursor + 8
            let recordEnd = payloadOffset + size
            guard recordEnd <= end else { return nil }
            result.append(CoreTagRecord(offset: cursor, tag: tag, size: size,
                                        payloadOffset: payloadOffset, end: recordEnd))
            cursor = recordEnd
        }
        return cursor == end ? result : nil
    }

    private func children(_ data: Data, _ record: CoreTagRecord) -> [CoreTagRecord] {
        records(data, record.payloadOffset, record.end) ?? []
    }

    private func child(_ data: Data, _ record: CoreTagRecord, _ tag: UInt32) -> CoreTagRecord? {
        children(data, record).first { $0.tag == tag }
    }

    private func findFirst(_ data: Data, _ records: [CoreTagRecord], _ tag: UInt32) -> CoreTagRecord? {
        for record in records {
            if record.tag == tag { return record }
            let nested = children(data, record)
            if !nested.isEmpty, let found = findFirst(data, nested, tag) { return found }
        }
        return nil
    }

    private func zlibDecode(_ input: Data, expectedSize: Int) -> Data? {
        guard expectedSize >= 0 else { return nil }
        if expectedSize == 0 { return Data() }
        var output = Data(count: expectedSize)
        var decodedSize = UInt(expectedSize)
        let status = output.withUnsafeMutableBytes { outputBytes -> Int32 in
            input.withUnsafeBytes { inputBytes -> Int32 in
                zlibUncompress(
                    outputBytes.bindMemory(to: UInt8.self).baseAddress,
                    &decodedSize,
                    inputBytes.bindMemory(to: UInt8.self).baseAddress,
                    UInt(input.count)
                )
            }
        }
        guard status == 0, decodedSize == UInt(expectedSize) else { return nil }
        return output
    }

    private func decodeString(_ raw: Data) -> String? {
        guard let lengthValue = u32(raw, 0) else { return nil }
        let length = Int(lengthValue)
        guard 5 + length <= raw.count else { return nil }
        return String(decoding: raw[5..<(5 + length)], as: UTF8.self)
    }

    private func propertyValue(_ data: Data, _ record: CoreTagRecord) -> [String: Any] {
        var cursor = record
        var nested = children(data, cursor)
        while nested.count == 1 {
            cursor = nested[0]
            nested = children(data, cursor)
        }
        let raw = Data(data[cursor.payloadOffset..<cursor.end])
        var result: [String: Any] = [
            "value_tag": String(format: "0x%08x", cursor.tag),
            "raw_size": raw.count
        ]
        if cursor.tag == CoreTag.stringValue, let value = decodeString(raw) {
            result["value_type"] = "string"
            result["value"] = value
        } else if cursor.tag == CoreTag.intValue, raw.count == 4, let value = u32(raw, 0) {
            result["value_type"] = "integer"
            result["value"] = i32(value)
        } else if raw.count == 1 {
            result["value_type"] = "byte"
            result["value"] = raw[0]
        } else if raw.count == 4, let value = u32(raw, 0) {
            result["value_type"] = "u32"
            result["value"] = value
        } else if raw.count == 8, let first = u32(raw, 0), let second = u32(raw, 4) {
            result["value_type"] = "raw_8_bytes"
            result["raw_words"] = [first, second]
        } else {
            result["value_type"] = "opaque"
            result["encoding"] = "base64"
            result["raw_value"] = raw.base64EncodedString()
        }
        return result
    }

    private func parseProperties(_ data: Data, _ record: CoreTagRecord?) -> [String: Any] {
        guard let record else { return ["declared_count": 0, "decoded_count": 0, "properties": []] }
        let recordChildren = children(data, record)
        let countRecord = recordChildren.first { $0.tag == CoreTag.propertyCount }
        let valuesRecord = recordChildren.first { $0.tag == CoreTag.propertyValues }
        let declared = countRecord.flatMap { u32(data, $0.payloadOffset) } ?? 0
        var properties: [[String: Any]] = []
        if let valuesRecord {
            for item in children(data, valuesRecord) where item.tag == CoreTag.property {
                let itemChildren = children(data, item)
                guard let meta = itemChildren.first(where: { $0.tag == CoreTag.propertyMeta }),
                      meta.size >= 8,
                      let valueRecord = itemChildren.first(where: { $0.tag == CoreTag.propertyValue }),
                      let rawID = u32(data, meta.payloadOffset),
                      let type = u32(data, meta.payloadOffset + 4) else { continue }
                var decoded = propertyValue(data, valueRecord)
                decoded["raw_property_id"] = rawID
                decoded["property_id"] = rawID & 0x7FFFFFFF
                decoded["property_flags"] = rawID >> 31
                decoded["property_type_code"] = type
                properties.append(decoded)
            }
        }
        var result: [String: Any] = [
            "declared_count": declared,
            "decoded_count": properties.count,
            "properties": properties
        ]
        for property in properties where property["value_type"] as? String == "string" {
            guard let propertyID = property["property_id"] as? UInt32,
                  let value = property["value"] as? String, !value.isEmpty else { continue }
            if propertyID == 1 || propertyID == 3 { result["name"] = value }
            if propertyID == 10 { result["description"] = value }
        }
        return result
    }

    private func endpoint(_ data: Data, _ record: CoreTagRecord,
                          _ modules: [[String: Any]], source: Bool) -> [String: Any] {
        let raw = Data(data[record.payloadOffset..<record.end])
        var words: [UInt32] = []
        for offset in stride(from: 0, through: max(-1, raw.count - 4), by: 4) {
            if offset >= 0, let value = u32(raw, offset) { words.append(value) }
        }
        var result: [String: Any] = ["raw_words": words]
        guard let mode = words.first else {
            result["kind"] = "invalid"
            return result
        }
        if mode == 0, words.count >= 3 {
            let moduleIndex = Int(words[1])
            result["kind"] = "module_pin"
            result["module_index"] = moduleIndex
            result["pin_index"] = words[2]
            if moduleIndex < modules.count {
                let module = modules[moduleIndex]
                for key in ["path", "type_id", "type_name", "name"] {
                    if let value = module[key] { result["module_\(key)"] = value }
                }
            } else {
                result["resolution_status"] = "module_index_out_of_range"
            }
        } else if source, mode == 3, words.count >= 2 {
            result["kind"] = "quick_constant"
            result["signal_type_code"] = words[1]
            if words[1] == 1, raw.count >= 12,
               let low = u32(raw, raw.count - 8), let high = u32(raw, raw.count - 4) {
                result["value_type"] = "float64"
                result["value"] = Double(bitPattern: UInt64(low) | (UInt64(high) << 32))
            } else if words[1] == 2, let value = words.last {
                result["value_type"] = "integer"
                result["value"] = i32(value)
            }
        } else {
            let names: [UInt32: String] = [
                1: "legacy_or_builtin_bus", 2: "scoped_bus",
                3: source ? "quick_constant" : "scoped_bus"
            ]
            result["kind"] = names[mode] ?? "unknown"
            result["mode"] = mode
        }
        return result
    }

    private func parseStructure(_ data: Data, _ record: CoreTagRecord,
                                path: String = "root") -> [String: Any] {
        let recordChildren = children(data, record)
        var metadata: [String: Any] = [:]
        if let meta = recordChildren.first(where: { $0.tag == CoreTag.structureMeta }), meta.size >= 17,
           let moduleCount = u32(data, meta.payloadOffset),
           let quickBusCount = u32(data, meta.payloadOffset + 5),
           let structureID = u32(data, meta.payloadOffset + 9),
           let checksum = u32(data, meta.payloadOffset + 13) {
            metadata["declared_module_count"] = moduleCount
            metadata["has_owner_properties"] = data[meta.payloadOffset + 4] != 0
            metadata["quick_bus_count"] = quickBusCount
            metadata["structure_id"] = structureID
            metadata["contents_checksum"] = checksum
        }
        if let bounds = recordChildren.first(where: { $0.tag == CoreTag.structureBounds }),
           let value = children(data, bounds).first, value.size >= 16,
           let x = u32(data, value.payloadOffset), let y = u32(data, value.payloadOffset + 4),
           let width = u32(data, value.payloadOffset + 8), let height = u32(data, value.payloadOffset + 12) {
            metadata["bounds"] = ["x": i32(x), "y": i32(y),
                                  "width": i32(width), "height": i32(height)]
        }
        let properties = parseProperties(
            data, recordChildren.first { $0.tag == CoreTag.properties })
        let catalogModules = catalog["modules"] as? [String: Any] ?? [:]
        let flavors = ["input", "output", "normal"]
        let arrays = recordChildren.filter { $0.tag == CoreTag.moduleArray }
        var parsedModules: [[String: Any]] = []
        for (flavorIndex, array) in arrays.prefix(3).enumerated() {
            let arrayChildren = children(data, array)
            let declared = arrayChildren.first(where: { $0.tag == CoreTag.arrayCount })
                .flatMap { u32(data, $0.payloadOffset) } ?? 0
            guard let values = arrayChildren.first(where: { $0.tag == CoreTag.arrayValues }) else { continue }
            let moduleRecords = children(data, values).filter { $0.tag == CoreTag.module }
            for (flavorLocalIndex, moduleRecord) in moduleRecords.enumerated() {
                let moduleChildren = children(data, moduleRecord)
                guard let meta = moduleChildren.first(where: { $0.tag == CoreTag.moduleMeta }),
                      meta.size >= 5, let typeID = u32(data, meta.payloadOffset) else { continue }
                let hasInner = data[meta.payloadOffset + 4] != 0
                let moduleIndex = parsedModules.count
                let modulePath = "\(path)/m\(moduleIndex)"
                let entry = catalogModules[String(typeID)] as? [String: Any] ?? [:]
                let moduleProperties = parseProperties(
                    data, moduleChildren.first { $0.tag == CoreTag.properties })
                var module: [String: Any] = [
                    "index": moduleIndex,
                    "flavor_local_index": flavorLocalIndex,
                    "flavor_declared_count": declared,
                    "flavor": flavorIndex < flavors.count ? flavors[flavorIndex] : "unknown_\(flavorIndex)",
                    "path": modulePath,
                    "type_id": typeID,
                    "type_name": entry["name"] ?? "core_type_\(typeID)",
                    "has_inner_structure": hasInner,
                    "properties": moduleProperties,
                    "record_offset": String(format: "0x%x", moduleRecord.offset),
                    "record_size": moduleRecord.size + 8
                ]
                if let implementation = entry["implementation_class"] {
                    module["implementation_class"] = implementation
                }
                if let position = moduleChildren.first(where: { $0.tag == CoreTag.modulePosition }),
                   let value = children(data, position).first, value.tag == CoreTag.positionValue,
                   value.size >= 8, let x = u32(data, value.payloadOffset),
                   let y = u32(data, value.payloadOffset + 4) {
                    module["position"] = ["x": i32(x), "y": i32(y)]
                }
                if let name = moduleProperties["name"] { module["name"] = name }
                if let detail = moduleProperties["description"] { module["description"] = detail }
                if let inner = moduleChildren.first(where: { $0.tag == CoreTag.structure }) {
                    module["inner_structure"] = parseStructure(data, inner, path: modulePath)
                }
                parsedModules.append(module)
            }
        }

        var parsedConnections: [[String: Any]] = []
        var declaredConnections: UInt32 = 0
        if let array = recordChildren.first(where: { $0.tag == CoreTag.connectionArray }) {
            let arrayChildren = children(data, array)
            declaredConnections = arrayChildren.first(where: { $0.tag == CoreTag.connectionCount })
                .flatMap { u32(data, $0.payloadOffset) } ?? 0
            if let values = arrayChildren.first(where: { $0.tag == CoreTag.connectionValues }) {
                for (index, connection) in children(data, values).filter({ $0.tag == CoreTag.connection }).enumerated() {
                    let connectionChildren = children(data, connection)
                    guard let source = connectionChildren.first(where: { $0.tag == CoreTag.connectionSource }),
                          let target = connectionChildren.first(where: { $0.tag == CoreTag.connectionTarget }) else { continue }
                    parsedConnections.append([
                        "index": index,
                        "source": endpoint(data, source, parsedModules, source: true),
                        "target": endpoint(data, target, parsedModules, source: false)
                    ])
                }
            }
        }
        var result: [String: Any] = metadata
        result["path"] = path
        result["module_count"] = parsedModules.count
        result["declared_connection_count"] = declaredConnections
        result["connection_count"] = parsedConnections.count
        result["properties"] = properties
        result["modules"] = parsedModules
        result["connections"] = parsedConnections
        return result
    }

    private func graphCounts(_ structure: [String: Any]) -> (Int, Int, Int) {
        let parsedModules = structure["modules"] as? [[String: Any]] ?? []
        let parsedConnections = structure["connections"] as? [[String: Any]] ?? []
        var counts = (1, parsedModules.count, parsedConnections.count)
        for module in parsedModules {
            if let inner = module["inner_structure"] as? [String: Any] {
                let child = graphCounts(inner)
                counts.0 += child.0
                counts.1 += child.1
                counts.2 += child.2
            }
        }
        return counts
    }

    private func documents() -> [[String: Any]] {
        let markerOffsets = allOffsets(of: coreDocumentMarker, in: source)
        var result: [[String: Any]] = []
        for (documentIndex, markerOffset) in markerOffsets.enumerated() {
            let end = documentIndex + 1 < markerOffsets.count ? markerOffsets[documentIndex + 1] : source.count
            var cursor = markerOffset + coreDocumentMarker.count
            var decoded = Data()
            var segments: [[String: Any]] = []
            while cursor < end,
                  let range = source.range(of: coreDataMarker, options: [], in: cursor..<end) {
                let segmentOffset = range.lowerBound
                guard segmentOffset + 24 <= end,
                      let storedSizeValue = u32(source, segmentOffset + 16),
                      let decodedSizeValue = u32(source, segmentOffset + 20) else { break }
                let storedSize = Int(storedSizeValue)
                let decodedSize = Int(decodedSizeValue)
                let payloadOffset = segmentOffset + 24
                let payloadEnd = payloadOffset + storedSize
                guard payloadEnd <= end else { break }
                let algorithm = Data(source[(segmentOffset + 8)..<(segmentOffset + 16)])
                let stored = Data(source[payloadOffset..<payloadEnd])
                let part: Data?
                let compression: String
                if algorithm == Data("crngbilz".utf8) {
                    part = zlibDecode(stored, expectedSize: decodedSize)
                    compression = "zlib"
                } else if algorithm == Data("crngenon".utf8) {
                    part = stored.count == decodedSize ? stored : nil
                    compression = "none"
                } else {
                    cursor = segmentOffset + 1
                    continue
                }
                if let part {
                    decoded.append(part)
                    segments.append([
                        "offset": String(format: "0x%x", segmentOffset),
                        "compression": compression,
                        "stored_size": storedSize,
                        "decoded_size": decodedSize,
                        "sha256": coreSHA256(part)
                    ])
                }
                cursor = payloadEnd
            }
            let top = records(decoded, 0, decoded.count) ?? []
            var document: [String: Any] = [
                "document_index": documentIndex,
                "marker_offset": String(format: "0x%x", markerOffset),
                "segment_count": segments.count,
                "segments": segments,
                "decoded_size": decoded.count,
                "decoded_sha256": coreSHA256(decoded),
                "tagged_stream_valid": !top.isEmpty
            ]
            if let typeContainer = findFirst(decoded, top, CoreTag.fileType),
               let value = child(decoded, typeContainer, CoreTag.fileTypeValue), value.size >= 4,
               let lengthValue = u32(decoded, value.payloadOffset) {
                let length = Int(lengthValue)
                if value.payloadOffset + 4 + length <= value.end {
                    document["file_type"] = String(decoding: decoded[(value.payloadOffset + 4)..<(value.payloadOffset + 4 + length)], as: UTF8.self)
                }
            }
            if let structureRecord = findFirst(decoded, top, CoreTag.structure) {
                let structure = parseStructure(decoded, structureRecord)
                let counts = graphCounts(structure)
                document["parse_status"] = "decoded"
                document["structure"] = structure
                document["graph_counts"] = [
                    "structures": counts.0, "modules": counts.1, "connections": counts.2
                ]
            } else {
                document["parse_status"] = "no_structure_found"
            }
            result.append(document)
        }
        return result
    }

    func extract() -> ([[String: Any]], [String]) {
        let coreModules = modules.enumerated().filter { coreCellKinds.contains($0.element.kind) }
        let coreDocuments = documents()
        var warnings: [String] = []
        if coreModules.count != coreDocuments.count {
            warnings.append("Core document count (\(coreDocuments.count)) does not match Core module count (\(coreModules.count))")
        }
        var result: [[String: Any]] = []
        for (coreIndex, indexedModule) in coreModules.enumerated() {
            let index = indexedModule.offset
            let module = indexedModule.element
            let end = index + 1 < modules.count ? modules[index + 1].offset : source.count
            let primaryRecord = Data(source[module.offset..<end])
            var item: [String: Any] = [
                "object_id": module.objectID,
                "module_kind": module.kind,
                "offset": String(format: "0x%x", module.offset),
                "record_size": primaryRecord.count,
                "encoding": "base64",
                "scope": "serialized primary KSModul record slice, including its ports",
                "sha256": coreSHA256(primaryRecord),
                "serialized_record": primaryRecord.base64EncodedString()
            ]
            if let typeName = module.typeName { item["module_type_name"] = typeName }
            if let hierarchyPath = module.hierarchyPath { item["hierarchy_path"] = hierarchyPath }
            item["core_document"] = coreIndex < coreDocuments.count
                ? coreDocuments[coreIndex] : ["parse_status": "missing"]
            result.append(item)
        }
        return (result, warnings)
    }
}
