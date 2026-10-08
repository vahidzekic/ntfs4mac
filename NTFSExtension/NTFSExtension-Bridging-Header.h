//
//  NTFSExtension-Bridging-Header.h
//  Exposes the frozen C bridge API (docs/ARCHITECTURE.md) to Swift.
//  Only NTFSBridge.h is imported: libntfs-3g's own headers (and config.h)
//  must never leak into the Swift module.
//

#import "Bridge/NTFSBridge.h"
