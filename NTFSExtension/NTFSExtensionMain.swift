//
//  NTFSExtensionMain.swift
//  NTFSExtension
//
//  Entry point of the FSKit module (extension point com.apple.fskit.fsmodule).
//
//  `UnaryFileSystemExtension` (FSKit, macOS 15.4+) is an `AppExtension`
//  whose associated `FileSystem` type must be an `FSUnaryFileSystem` subclass
//  that also conforms to `FSUnaryFileSystemOperations`. FSKit asks for
//  `fileSystem` and drives all probe / load / unload / check / format calls
//  through it.
//

import ExtensionFoundation
import Foundation
import FSKit

@main
struct NTFSExtensionMain: UnaryFileSystemExtension {
    /// The single file-system instance for the lifetime of the extension
    /// process. Stored (not computed) so that every access returns the same
    /// object — NTFSFileSystem keeps the loaded resource and volume state
    /// that later check/format/unload calls rely on.
    let fileSystem = NTFSFileSystem()
}
