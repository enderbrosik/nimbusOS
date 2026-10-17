// NimbusOS - entry point of the NimbusFS file system extension (the Xcode template generates the same thing).
// NOT COMPILED YET.
import FSKit
import Foundation

@main
struct NimbusFSExtension: UnaryFileSystemExtension {
    var fileSystem: FSUnaryFileSystem & FSUnaryFileSystemOperations {
        NimbusFileSystem()
    }
}
