// SPDX-License-Identifier: GPL-3.0-or-later
import Testing
@testable import UnlatchCore

@Test func identifiersDeriveFromBundlePrefix() {
    #expect(Unlatch.appGroup == "group.io.github.ramanan-bala.Unlatch")
    #expect(Unlatch.logSubsystem == "io.github.ramanan-bala.unlatch")
}
