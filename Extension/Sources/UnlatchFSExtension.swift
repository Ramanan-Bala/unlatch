// SPDX-License-Identifier: GPL-3.0-or-later
import ExtensionFoundation
import FSKit

@main
struct UnlatchFSExtension: UnaryFileSystemExtension {
    var fileSystem: UnlatchFileSystem { UnlatchFileSystem.shared }
}
