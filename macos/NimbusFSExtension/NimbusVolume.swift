// NimbusOS - the NimbusFS volume: lookups, directory listing, reading, writing, create, rename, delete.
// NOT COMPILED YET (see NimbusFileSystem.swift).  Methods use FSKit's Swift form (async throws).
import FSKit
import Foundation

/// An item is just its NimbusFS inode number.
final class NimbusItem: FSItem {
    let inode: UInt32
    let type: FSItem.ItemType
    init(inode: UInt32, type: FSItem.ItemType) { self.inode = inode; self.type = type; super.init() }
}

private func posix(_ nb: Int32) -> any Error {
    let e: POSIXError.Code
    switch nb {
    case NBFS_ENOENT: e = .ENOENT
    case NBFS_EEXIST: e = .EEXIST
    case NBFS_ENOTDIR: e = .ENOTDIR
    case NBFS_EISDIR: e = .EISDIR
    case NBFS_ENOSPC: e = .ENOSPC
    case NBFS_ENOTEMPTY: e = .ENOTEMPTY
    case NBFS_ENAMETOOLONG: e = .ENAMETOOLONG
    case NBFS_EINVAL: e = .EINVAL
    default: e = .EIO
    }
    return fs_errorForPOSIXError(e.rawValue)
}
private func unsupported() -> any Error { fs_errorForPOSIXError(POSIXError.ENOTSUP.rawValue) }

final class NimbusVolume: FSVolume, FSVolume.Operations, FSVolume.ReadWriteOperations, FSVolume.PathConfOperations {
    private let fs: UnsafeMutablePointer<nbfs_t>
    private let lock = NSLock()                          // NimbusFS is not thread safe; never held across a suspension point

    init(fs: UnsafeMutablePointer<nbfs_t>, name: String) {
        self.fs = fs
        super.init(volumeID: FSVolume.Identifier(uuid: UUID()), volumeName: FSFileName(string: name))
    }

    private func locked<T>(_ body: () throws -> T) rethrows -> T { try lock.withLock(body) }
    func close() { locked { nbfs_shim_close(fs) } }

    // MARK: helpers
    private func item(_ st: nbfs_stat) -> NimbusItem {
        NimbusItem(inode: st.inode, type: st.type == UInt32(NBFS_TYPE_DIR) ? .directory : .file)
    }

    private func attrs(_ st: nbfs_stat) -> FSItem.Attributes {
        let a = FSItem.Attributes()
        let dir = st.type == UInt32(NBFS_TYPE_DIR)
        a.type = dir ? .directory : .file
        a.mode = UInt32(dir ? 0o40755 : 0o100644)
        a.linkCount = dir ? 2 : 1
        a.uid = getuid()
        a.gid = getgid()
        a.size = UInt64(st.size)
        a.allocSize = UInt64((st.size + 4095) / 4096 * 4096)
        a.fileID = FSItem.Identifier(rawValue: UInt64(st.inode) + 1) ?? .invalid      // NimbusFS root is inode 1, which becomes 2 = FSKit's root
        a.parentID = st.inode == UInt32(NBFS_ROOT_INO) ? .parentOfRoot : (FSItem.Identifier(rawValue: UInt64(st.parent) + 1) ?? .invalid)
        a.modifyTime = timespec(tv_sec: Int(st.mtime), tv_nsec: 0)
        a.changeTime = a.modifyTime
        a.birthTime = timespec(tv_sec: Int(st.ctime), tv_nsec: 0)
        a.accessTime = a.modifyTime
        return a
    }

    private func stat(_ ino: UInt32) -> nbfs_stat? {
        var st = nbfs_stat()
        return nbfs_stat_inode(fs, ino, &st) == 0 ? st : nil
    }

    private func nb(_ item: FSItem) throws -> NimbusItem {
        guard let it = item as? NimbusItem else { throw posix(NBFS_ENOENT) }
        return it
    }

    // MARK: FSVolume.Operations
    var supportedVolumeCapabilities: FSVolume.SupportedCapabilities {
        let c = FSVolume.SupportedCapabilities()
        c.supportsPersistentObjectIDs = true
        c.supportsHardLinks = false
        c.supportsSymbolicLinks = false
        c.caseFormat = .insensitiveCasePreserving
        return c
    }

    var volumeStatistics: FSStatFSResult {
        let r = FSStatFSResult(fileSystemTypeName: "nimbusfs")
        var total: UInt32 = 0, free: UInt32 = 0
        locked { nbfs_statfs(fs, &total, &free) }
        r.blockSize = 4096
        r.ioSize = 4096
        r.totalBlocks = UInt64(total / 4096)
        r.freeBlocks = UInt64(free / 4096)
        r.availableBlocks = r.freeBlocks
        return r
    }

    func mount(options: FSTaskOptions) async throws {}
    func unmount() async { close() }
    func synchronize(flags: FSSyncFlags) async throws { locked { _ = nbfs_sync(fs) } }

    func activate(options: FSTaskOptions) async throws -> FSItem {
        try locked {
            guard let st = stat(UInt32(NBFS_ROOT_INO)) else { throw posix(NBFS_EIO) }
            return item(st)
        }
    }
    func deactivate(options: FSDeactivateOptions) async throws {}

    func attributes(_ desiredAttributes: FSItem.GetAttributesRequest, of fsItem: FSItem) async throws -> FSItem.Attributes {
        let it = try nb(fsItem)
        return try locked {
            guard let st = stat(it.inode) else { throw posix(NBFS_ENOENT) }
            return attrs(st)
        }
    }

    func setAttributes(_ newAttributes: FSItem.SetAttributesRequest, on fsItem: FSItem) async throws -> FSItem.Attributes {
        let it = try nb(fsItem)
        return try locked {
            if newAttributes.isValid(.size) {                // truncate / extend
                let r = nbfs_truncate(fs, it.inode, UInt32(newAttributes.size))
                if r < 0 { throw posix(r) }
                nbfs_sync(fs)
            }
            guard let st = stat(it.inode) else { throw posix(NBFS_ENOENT) }
            return attrs(st)                                 // owners, modes and times are not stored by NimbusFS
        }
    }

    func lookupItem(named name: FSFileName, inDirectory directory: FSItem) async throws -> (FSItem, FSFileName) {
        let dir = try nb(directory)
        return try locked {
            var st = nbfs_stat()
            let r = nbfs_shim_lookup(fs, dir.inode, name.string ?? "", &st)
            if r != 0 { throw posix(r) }
            return (item(st), name)
        }
    }

    func reclaimItem(_ item: FSItem) async throws {}
    func readSymbolicLink(_ item: FSItem) async throws -> FSFileName { throw unsupported() }

    func createItem(named name: FSFileName, type: FSItem.ItemType, inDirectory directory: FSItem,
                    attributes newAttributes: FSItem.SetAttributesRequest) async throws -> (FSItem, FSFileName) {
        let dir = try nb(directory)
        guard type == .file || type == .directory else { throw unsupported() }
        return try locked {
            var st = nbfs_stat()
            let r = nbfs_shim_create(fs, dir.inode, name.string ?? "",
                                     Int32(type == .directory ? NBFS_TYPE_DIR : NBFS_TYPE_FILE), &st)
            if r != 0 { throw posix(r) }
            return (item(st), name)
        }
    }

    func createSymbolicLink(named name: FSFileName, inDirectory directory: FSItem, attributes newAttributes: FSItem.SetAttributesRequest,
                            linkContents contents: FSFileName) async throws -> (FSItem, FSFileName) { throw unsupported() }
    func createLink(to item: FSItem, named name: FSFileName, inDirectory directory: FSItem) async throws -> FSFileName { throw unsupported() }

    func removeItem(_ item: FSItem, named name: FSFileName, fromDirectory directory: FSItem) async throws {
        let dir = try nb(directory)
        try locked {
            let r = nbfs_shim_remove(fs, dir.inode, name.string ?? "")
            if r != 0 { throw posix(r) }
        }
    }

    func renameItem(_ item: FSItem, inDirectory sourceDirectory: FSItem, named sourceName: FSFileName,
                    to destinationName: FSFileName, inDirectory destinationDirectory: FSItem, overItem: FSItem?) async throws -> FSFileName {
        let from = try nb(sourceDirectory), to = try nb(destinationDirectory)
        return try locked {
            if overItem != nil { _ = nbfs_shim_remove(fs, to.inode, destinationName.string ?? "") }
            let r = nbfs_shim_rename(fs, from.inode, sourceName.string ?? "", to.inode, destinationName.string ?? "")
            if r != 0 { throw posix(r) }
            return destinationName
        }
    }

    func enumerateDirectory(_ directory: FSItem, startingAt cookie: FSDirectoryCookie, verifier: FSDirectoryVerifier,
                            attributes: FSItem.GetAttributesRequest?, packer: FSDirectoryEntryPacker) async throws -> FSDirectoryVerifier {
        let dir = try nb(directory)
        locked {
            var cur = UInt32(truncatingIfNeeded: cookie.rawValue)         // our cookie is NimbusFS's own readdir cookie
            var e = nbfs_entry()
            while nbfs_readdir(fs, dir.inode, &cur, &e) == 1 {
                let name = withUnsafePointer(to: &e.name) {
                    $0.withMemoryRebound(to: CChar.self, capacity: Int(NBFS_NAME_MAX) + 1) { String(cString: $0) }
                }
                let isDir = e.type == UInt32(NBFS_TYPE_DIR)
                var entryAttrs: FSItem.Attributes? = nil
                if attributes != nil, let st = stat(e.inode) { entryAttrs = attrs(st) }
                let ok = packer.packEntry(name: FSFileName(string: name), itemType: isDir ? .directory : .file,
                                          itemID: FSItem.Identifier(rawValue: UInt64(e.inode) + 1) ?? .invalid,
                                          nextCookie: FSDirectoryCookie(rawValue: UInt64(cur)),
                                          attributes: entryAttrs)
                if !ok { break }
            }
        }
        return FSDirectoryVerifier(rawValue: 1)
    }

    // MARK: FSVolume.ReadWriteOperations
    func read(from fsItem: FSItem, at offset: off_t, length: Int, into buffer: FSMutableFileDataBuffer) async throws -> Int {
        let it = try nb(fsItem)
        return try locked {
            guard let st = stat(it.inode) else { throw posix(NBFS_ENOENT) }
            if offset >= off_t(st.size) { return 0 }
            let n = min(length, Int(st.size) - Int(offset))
            var got = 0
            buffer.withUnsafeMutableBytes { raw in
                let r = nbfs_read(fs, it.inode, UInt32(offset), raw.baseAddress, UInt32(n))
                got = Int(max(0, r))
            }
            return got
        }
    }

    func write(contents: Data, to fsItem: FSItem, at offset: off_t) async throws -> Int {
        let it = try nb(fsItem)
        return try locked {
            let r = contents.withUnsafeBytes { nbfs_write(fs, it.inode, UInt32(offset), $0.baseAddress, UInt32(contents.count)) }
            if r < 0 { throw posix(r) }
            nbfs_sync(fs)
            return Int(r)
        }
    }

    // MARK: FSVolume.PathConfOperations
    var maximumLinkCount: Int { 1 }
    var maximumNameLength: Int { Int(NBFS_NAME_MAX) }
    var restrictsOwnershipChanges: Bool { false }
    var truncatesLongNames: Bool { false }
}
