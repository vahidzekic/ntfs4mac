//
//  NTFSItem.swift
//  NTFSExtension
//
//  FSItem subclass representing one NTFS inode (MFT record).
//
//  Identity
//  --------
//  An item *is* its MFT record number (`ino`). Hard links therefore share a
//  single NTFSItem (NTFSVolume caches items by ino). FSKit item identifiers
//  are derived from the ino with a stable, reversible mapping:
//
//    ino 5 (root directory)  <-> FSItem.Identifier.rootDirectory (raw 2)
//    ino 0, 1, 2             <-> raw (1 << 48) | ino
//                                (NTFS metadata files $MFT, $MFTMirr,
//                                 $LogFile — only visible with show_sys_files
//                                 — whose raw values would otherwise collide
//                                 with .invalid / .parentOfRoot / .rootDirectory)
//    every other ino         <-> raw == ino
//
//  MFT record numbers are 48 bits wide, so bit 48 can never be set by a real
//  ino and the escape range cannot collide with ordinary items.
//

import Foundation
import FSKit
import os

final class NTFSItem: FSItem {
    /// MFT record number.
    let ino: UInt64
    /// Item kind, fixed for the lifetime of the inode.
    let kind: NTFSItemKind

    /// Mutable per-item state, guarded by an unfair lock (FSKit may call in
    /// from several threads concurrently).
    private struct State: Sendable {
        /// Set once the last link was removed (or the item was renamed over).
        /// The MFT record may be reused by a new file afterwards, so a deleted
        /// item must never touch the volume by ino again.
        var deleted = false
        /// Last attributes read from disk; served (with linkCount 0) for
        /// fstat() on a file that was unlinked while open.
        var lastStat: NTFSStat?
    }

    private let state = OSAllocatedUnfairLock(initialState: State())

    /// Escape bit for inos whose raw value collides with reserved identifiers.
    private static let escapeBit: UInt64 = 1 << 48

    init(ino: UInt64, kind: NTFSItemKind, stat: NTFSStat? = nil) {
        self.ino = ino
        self.kind = kind
        super.init()
        if let stat {
            state.withLock { $0.lastStat = stat }
        }
    }

    // MARK: - Identity

    /// The FSKit identifier for this item.
    var identifier: FSItem.Identifier {
        Self.identifier(forIno: ino)
    }

    /// True for the volume's root directory.
    var isRoot: Bool {
        ino == ntfsRootIno
    }

    /// Maps an MFT record number to a stable FSKit identifier.
    static func identifier(forIno ino: UInt64) -> FSItem.Identifier {
        if ino == ntfsRootIno {
            return .rootDirectory
        }
        let raw: UInt64
        if ino <= FSItem.Identifier.rootDirectory.rawValue {
            raw = escapeBit | ino
        } else {
            raw = ino
        }
        return FSItem.Identifier(rawValue: raw) ?? .invalid
    }

    /// Inverse of `identifier(forIno:)`. Returns `nil` for `.invalid` and
    /// `.parentOfRoot`, which name no NTFS inode.
    static func ino(for identifier: FSItem.Identifier) -> UInt64? {
        switch identifier {
        case .rootDirectory:
            return ntfsRootIno
        case .invalid, .parentOfRoot:
            return nil
        default:
            let raw = identifier.rawValue
            if raw & escapeBit != 0 {
                return raw & ~escapeBit
            }
            return raw
        }
    }

    // MARK: - State

    /// True once the inode was deleted while this item was still referenced.
    var isDeleted: Bool {
        state.withLock { $0.deleted }
    }

    /// Marks the item deleted (last link removed / renamed over).
    func markDeleted() {
        state.withLock { $0.deleted = true }
    }

    /// Records the most recent on-disk attributes.
    func remember(_ stat: NTFSStat) {
        state.withLock { $0.lastStat = stat }
    }

    /// The most recently recorded attributes, if any.
    var lastStat: NTFSStat? {
        state.withLock { $0.lastStat }
    }
}

extension NTFSItemKind {
    /// FSKit item type. Items the bridge cannot classify (unsupported reparse
    /// points such as cloud placeholders or dedup stubs) are presented as
    /// regular files: the kernel cannot instantiate a vnode for
    /// `FSItem.ItemType.unknown`, and as files they at least appear in
    /// listings and can be deleted or renamed.
    var fsItemType: FSItem.ItemType {
        switch self {
        case .file: return .file
        case .directory: return .directory
        case .symlink: return .symlink
        case .unknown: return .file
        }
    }
}
