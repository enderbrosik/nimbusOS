//
//  NimbusFSExtension.swift
//  NimbusFSExtension
//
//  Created by Aaron Garsia on 17/10/2026.
//

import ExtensionFoundation
import Foundation
import FSKit

@main
struct NimbusFSExtension : UnaryFileSystemExtension {
    var fileSystem : FSUnaryFileSystem & FSUnaryFileSystemOperations {
        NimbusFileSystem()
    }
}
