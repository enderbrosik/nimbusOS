// NimbusOS - the NimbusFS volume: lookups, directory listing, reading, writing, create, rename, delete.
// NOT COMPILED YET (see NimbusFileSystem.swift).
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

final class NimbusVolume: FSVolume, FSVolume.Operations, FSVolume.ReadWriteOperations, FSVolume.PathConfOperations {
    private let fs: UnsafeMutablePointer<nbfs_t>
    private let lock = NSLock()                          // NimbusFS is not thread safe

    init(fs: UnsafeMutablePointer<nbfs_t>, name: String) {
        self.fs = fs
        super.init(volumeID: FSVolume.Identifier(uuid: UUID()), volumeName: FSFileName(string: name))
    }

    func close() { lock.lock(); nbfs_shim_close(fs); lock.unlock() }

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

    private var rootInode: UInt32 {
        var ino: UInt32 = 0
        _ = nbfs_lookup(fs, "/", &ino)
        return ino
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
        lock.lock(); nbfs_statfs(fs, &total, &free); lock.unlock()
        r.blockSize = 4096
        r.ioSize = 4096
        r.totalBlocks = UInt64(total / 4096)
        r.freeBlocks = UInt64(free / 4096)
        r.availableBlocks = r.freeBlocks
        return r
    }

    func mount(options: FSTaskOptions, replyHandler: @escaping ((any Error)?) -> Void) { replyHandler(nil) }
    func unmount(replyHandler: @escaping () -> Void) { close(); replyHandler() }
    func synchronize(flags: FSSyncFlags, replyHandler: @escaping ((any Error)?) -> Void) {
        lock.lock(); nbfs_sync(fs); lock.unlock()
        replyHandler(nil)
    }

    func activate(options: FSTaskOptions, replyHandler: @escaping (FSItem?, (any Error)?) -> Void) {
        lock.lock(); defer { lock.unlock() }
        guard let st = stat(rootInode) else { replyHandler(nil, posix(NBFS_EIO)); return }
        replyHandler(item(st), nil)
    }
    func deactivate(options: FSDeactivateOptions, replyHandler: @escaping ((any Error)?) -> Void) { replyHandler(nil) }

    func attributes(_ desiredAttributes: FSItem.GetAttributesRequest, of fsItem: FSItem,
                    replyHandler: @escaping (FSItem.Attributes?, (any Error)?) -> Void) {
        lock.lock(); defer { lock.unlock() }
        guard let it = fsItem as? NimbusItem, let st = stat(it.inode) else { replyHandler(nil, posix(NBFS_ENOENT)); return }
        replyHandler(attrs(st), nil)
    }

    func setAttributes(_ newAttributes: FSItem.SetAttributesRequest, on fsItem: FSItem,
                       replyHandler: @escaping (FSItem.Attributes?, (any Error)?) -> Void) {
        lock.lock(); defer { lock.unlock() }
        guard let it = fsItem as? NimbusItem else { replyHandler(nil, posix(NBFS_ENOENT)); return }
        if newAttributes.isValid(.size) {                    // truncate / extend
            let r = nbfs_truncate(fs, it.inode, UInt32(newAttributes.size))
            if r < 0 { replyHandler(nil, posix(r)); return }
            nbfs_sync(fs)
        }
        guard let st = stat(it.inode) else { replyHandler(nil, posix(NBFS_ENOENT)); return }
        replyHandler(attrs(st), nil)                         // owners, modes and times are not stored by NimbusFS
    }

    func lookupItem(named name: FSFileName, inDirectory directory: FSItem,
                    replyHandler: @escaping (FSItem?, FSFileName?, (any Error)?) -> Void) {
        lock.lock(); defer { lock.unlock() }
        guard let dir = directory as? NimbusItem else { replyHandler(nil, nil, posix(NBFS_ENOTDIR)); return }
        var st = nbfs_stat()
        let r = nbfs_shim_lookup(fs, dir.inode, name.string ?? "", &st)
        if r != 0 { replyHandler(nil, nil, posix(r)); return }
        replyHandler(item(st), name, nil)
    }

    func reclaimItem(_ item: FSItem, replyHandler: @escaping ((any Error)?) -> Void) { replyHandler(nil) }
    func readSymbolicLink(_ item: FSItem, replyHandler: @escaping (FSFileName?, (any Error)?) -> Void) {
        replyHandler(nil, fs_errorForPOSIXError(POSIXError.ENOTSUP.rawValue))
    }

    func createItem(named name: FSFileName, type: FSItem.ItemType, inDirectory directory: FSItem,
                    attributes newAttributes: FSItem.SetAttributesRequest,
                    replyHandler: @escaping (FSItem?, FSFileName?, (any Error)?) -> Void) {
        lock.lock(); defer { lock.unlock() }
        guard let dir = directory as? NimbusItem else { replyHandler(nil, nil, posix(NBFS_ENOTDIR)); return }
        guard type == .file || type == .directory else {
            replyHandler(nil, nil, fs_errorForPOSIXError(POSIXError.ENOTSUP.rawValue)); return
        }
        var st = nbfs_stat()
        let r = nbfs_shim_create(fs, dir.inode, name.string ?? "",
                                 Int32(type == .directory ? NBFS_TYPE_DIR : NBFS_TYPE_FILE), &st)
        if r != 0 { replyHandler(nil, nil, posix(r)); return }
        replyHandler(item(st), name, nil)
    }

    func createSymbolicLink(named name: FSFileName, inDirectory directory: FSItem, attributes newAttributes: FSItem.SetAttributesRequest,
                            linkContents contents: FSFileName, replyHandler: @escaping (FSItem?, FSFileName?, (any Error)?) -> Void) {
        replyHandler(nil, nil, fs_errorForPOSIXError(POSIXError.ENOTSUP.rawValue))
    }
    func createLink(to item: FSItem, named name: FSFileName, inDirectory directory: FSItem,
                    replyHandler: @escaping (FSFileName?, (any Error)?) -> Void) {
        replyHandler(nil, fs_errorForPOSIXError(POSIXError.ENOTSUP.rawValue))
    }

    func removeItem(_ item: FSItem, named name: FSFileName, fromDirectory directory: FSItem,
                    replyHandler: @escaping ((any Error)?) -> Void) {
        lock.lock(); defer { lock.unlock() }
        guard let dir = directory as? NimbusItem else { replyHandler(posix(NBFS_ENOTDIR)); return }
        let r = nbfs_shim_remove(fs, dir.inode, name.string ?? "")
        replyHandler(r == 0 ? nil : posix(r))
    }

    func renameItem(_ item: FSItem, inDirectory sourceDirectory: FSItem, named sourceName: FSFileName,
                    to destinationName: FSFileName, inDirectory destinationDirectory: FSItem, overItem: FSItem?,
                    replyHandler: @escaping (FSFileName?, (any Error)?) -> Void) {
        lock.lock(); defer { lock.unlock() }
        guard let from = sourceDirectory as? NimbusItem, let to = destinationDirectory as? NimbusItem else {
            replyHandler(nil, posix(NBFS_ENOTDIR)); return
        }
        if overItem != nil { _ = nbfs_shim_remove(fs, to.inode, destinationName.string ?? "") }
        let r = nbfs_shim_rename(fs, from.inode, sourceName.string ?? "", to.inode, destinationName.string ?? "")
        replyHandler(r == 0 ? destinationName : nil, r == 0 ? nil : posix(r))
    }

    func enumerateDirectory(_ directory: FSItem, startingAt cookie: FSDirectoryCookie, verifier: FSDirectoryVerifier,
                            attributes: FSItem.GetAttributesRequest?, packer: FSDirectoryEntryPacker,
                            replyHandler: @escaping (FSDirectoryVerifier, (any Error)?) -> Void) {
        lock.lock(); defer { lock.unlock() }
        guard let dir = directory as? NimbusItem else { replyHandler(verifier, posix(NBFS_ENOTDIR)); return }
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
        replyHandler(FSDirectoryVerifier(rawValue: 1), nil)
    }

    // MARK: FSVolume.ReadWriteOperations
    func read(from fsItem: FSItem, at offset: off_t, length: Int, into buffer: FSMutableFileDataBuffer,
              replyHandler: @escaping (Int, (any Error)?) -> Void) {
        lock.lock(); defer { lock.unlock() }
        guard let it = fsItem as? NimbusItem, let st = stat(it.inode) else { replyHandler(0, posix(NBFS_ENOENT)); return }
        if offset >= off_t(st.size) { replyHandler(0, nil); return }
        let n = min(length, Int(st.size) - Int(offset))
        var got = 0
        buffer.withUnsafeMutableBytes { raw in
            let r = nbfs_read(fs, it.inode, UInt32(offset), raw.baseAddress, UInt32(n))
            got = Int(max(0, r))
        }
        replyHandler(got, nil)
    }

    func write(contents: Data, to fsItem: FSItem, at offset: off_t,
               replyHandler: @escaping (Int, (any Error)?) -> Void) {
        lock.lock(); defer { lock.unlock() }
        guard let it = fsItem as? NimbusItem else { replyHandler(0, posix(NBFS_ENOENT)); return }
        let r = contents.withUnsafeBytes { nbfs_write(fs, it.inode, UInt32(offset), $0.baseAddress, UInt32(contents.count)) }
        if r < 0 { replyHandler(0, posix(r)); return }
        nbfs_sync(fs)
        replyHandler(Int(r), nil)
    }

    // MARK: FSVolume.PathConfOperations
    var maximumLinkCount: Int { 1 }
    var maximumNameLength: Int { Int(NBFS_NAME_MAX) }
    var restrictsOwnershipChanges: Bool { false }
    var truncatesLongNames: Bool { false }
}
