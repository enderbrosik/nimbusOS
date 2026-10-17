// NimbusOS - custom disk icon for mounted NimbusFS volumes.
//
// Finder shows `.VolumeIcon.icns` from a volume's root as the disk icon, but only if the root folder carries the
// "has custom icon" Finder flag. NimbusFS stores no extended attributes, so we serve that flag as a virtual
// `com.apple.FinderInfo` attribute on the root folder. The icon file itself must exist in the image:
//     nfstool put <image> macos/icon/NimbusFS.icns /.VolumeIcon.icns
// NOT COMPILED: FSKit's xattr signatures were written from memory; accept Xcode's fix-its.
import FSKit
import Foundation

private let finderInfoName = "com.apple.FinderInfo"

extension NimbusVolume: FSVolume.XattrOperations {
    var xattrOperationsInUse: Bool {
        get { true }
        set { }
    }

    private func isRoot(_ item: FSItem) -> Bool {
        (item as? NimbusItem)?.inode == UInt32(NBFS_ROOT_INO)
    }

    /// 32-byte FinderInfo; bytes 8-9 are the Finder flags, 0x0400 = kHasCustomIcon.
    private var rootFinderInfo: Data {
        var d = Data(count: 32)
        d[8] = 0x04
        return d
    }

    func supportedXattrNames(for item: FSItem) -> [FSFileName] {
        isRoot(item) ? [FSFileName(string: finderInfoName)] : []
    }

    func xattrs(of item: FSItem) async throws -> [FSFileName] {
        supportedXattrNames(for: item)
    }

    func xattr(named name: FSFileName, of item: FSItem) async throws -> Data {
        guard isRoot(item), name.string == finderInfoName else {
            throw fs_errorForPOSIXError(POSIXError.ENOATTR.rawValue)
        }
        return rootFinderInfo
    }

    func setXattr(named name: FSFileName, to value: Data?, on item: FSItem, policy: FSVolume.SetXattrPolicy) async throws {
        // Nothing is stored; accept silently so Finder does not complain.
    }
}
