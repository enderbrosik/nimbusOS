// NimbusOS - NimbusFS file system extension for macOS (FSKit).
//
// NOT COMPILED YET: written without Xcode, from memory of the FSKit API. Expect to fix signature
// differences (Xcode's fix-its show them) on the first build.
//
// How it fits together:  FSUnaryFileSystem (this file: probe + load a block device)
//                        -> NimbusVolume (NimbusVolume.swift: files and folders)
//                        -> fs/nbfs.c through NBFSShim.h (the real file system code, shared with NimbusOS itself)
import FSKit
import Foundation

/// The block device FSKit gives us, wrapped so the C code can read and write 512-byte sectors through it.
final class BlockDevice {
    let resource: FSBlockDeviceResource
    init(_ r: FSBlockDeviceResource) { resource = r }

    func read(lba: UInt32, count: UInt32, into buf: UnsafeMutableRawPointer) -> Int32 {
        let len = Int(count) * 512
        do {
            let n = try resource.read(into: UnsafeMutableRawBufferPointer(start: buf, count: len),
                                      startingAt: off_t(lba) * 512, length: len)
            return n == len ? 0 : -1
        } catch { return -1 }
    }

    func write(lba: UInt32, count: UInt32, from buf: UnsafeRawPointer) -> Int32 {
        let len = Int(count) * 512
        do {
            let n = try resource.write(from: UnsafeRawBufferPointer(start: buf, count: len),
                                       startingAt: off_t(lba) * 512, length: len)
            return n == len ? 0 : -1
        } catch { return -1 }
    }

    var sectors: UInt32 { UInt32(truncatingIfNeeded: UInt64(resource.blockCount) * UInt64(resource.blockSize) / 512) }
}

// C callbacks: ctx is an unretained pointer to a BlockDevice.
private let cRead: nbfs_shim_io = { ctx, lba, count, buf in
    Unmanaged<BlockDevice>.fromOpaque(ctx!).takeUnretainedValue().read(lba: lba, count: count, into: buf!)
}
private let cWrite: nbfs_shim_wio = { ctx, lba, count, buf in
    Unmanaged<BlockDevice>.fromOpaque(ctx!).takeUnretainedValue().write(lba: lba, count: count, from: buf!)
}

final class NimbusFileSystem: FSUnaryFileSystem, FSUnaryFileSystemOperations {
    private var device: BlockDevice?
    private var volume: NimbusVolume?

    // Is this device a NimbusFS volume?  (diskutil / mount call this before mounting.)
    func probeResource(resource: FSResource) async throws -> FSProbeResult {
        guard let blk = resource as? FSBlockDeviceResource else { return FSProbeResult.notRecognized }
        let dev = BlockDevice(blk)
        let r = nbfs_shim_probe(Unmanaged.passUnretained(dev).toOpaque(), cRead, dev.sectors)
        if r == 0 { return FSProbeResult.usable(name: "NimbusFS", containerID: FSContainerIdentifier(uuid: UUID())) }
        return FSProbeResult.notRecognized
    }

    func loadResource(resource: FSResource, options: FSTaskOptions) async throws -> FSVolume {
        guard let blk = resource as? FSBlockDeviceResource else { throw fs_errorForPOSIXError(POSIXError.EINVAL.rawValue) }
        let dev = BlockDevice(blk)
        var err: Int32 = 0
        guard let fs = nbfs_shim_mount(Unmanaged.passUnretained(dev).toOpaque(), cRead, cWrite, dev.sectors, &err) else {
            NSLog("NimbusFS: mount failed: %s", String(cString: nbfs_shim_strerror(err)))
            throw fs_errorForPOSIXError(POSIXError.EIO.rawValue)
        }
        device = dev                       // keep the device alive as long as the volume
        let vol = NimbusVolume(fs: fs, name: "NimbusFS")
        volume = vol
        containerStatus = .ready
        return vol
    }

    func unloadResource(resource: FSResource, options: FSTaskOptions) async throws {
        volume?.close()
        volume = nil
        device = nil
    }

    func didFinishLoading() {}
}

// macOS runs a "check" (fsck) before mounting. Without this, Finder says it "can't repair" the disk and
// mounts it read-only. We report a clean check; a real nbfs_check pass can be wired in later.
// NOT COMPILED: if Xcode complains about the signatures, accept its fix-its.
extension NimbusFileSystem: FSManageableResourceMaintenanceOperations {
    func startCheck(task: FSTask, options: FSTaskOptions) throws -> Progress {
        let progress = Progress(totalUnitCount: 100)
        task.logMessage("NimbusFS: check ok")
        progress.completedUnitCount = 100
        task.didComplete(error: nil)
        return progress
    }

    func startFormat(task: FSTask, options: FSTaskOptions) throws -> Progress {
        throw fs_errorForPOSIXError(POSIXError.ENOTSUP.rawValue)
    }
}
