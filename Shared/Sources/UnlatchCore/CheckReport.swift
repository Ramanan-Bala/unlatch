// SPDX-License-Identifier: GPL-3.0-or-later
import Foundation

/// What Check Disk found on a volume (D-93): the JSON `unlatch-fuse --check` prints, schema 1.
public struct CheckReport: Codable, Equatable, Sendable {
    public struct Problem: Codable, Equatable, Sendable {
        /// record_unmarked, clusters_unmarked, clusters_shared, bad_runlist, bad_record,
        /// index_unreadable, entry_free_record, entry_wrong_seq, entry_wrong_parent, orphan, mft_mirror,
        /// bad_security.
        public let kind: String
        public let repairable: Bool
        public let record: UInt64
        public let other: UInt64?
        public let lcn: UInt64?
        public let count: UInt64?
        public let path: String
        public let detail: String
    }

    public let schema: Int
    public let problems: [Problem]
    public let state: String
    public let records, recordsInUse, directories, entries: UInt64
    public let clusters, clustersInUse: UInt64
    public let lostRecords, lostClusters: UInt64
    public let found, repairable, repaired, remaining: UInt64
    /// Folder entries the repair removed (D-96); absent from reports older than that.
    public let entriesRemoved: UInt64?
    /// Files no folder listed that the repair put back (D-99), and of those the ones put in
    /// /found.000 under a new name; absent from older reports.
    public let orphansRelinked: UInt64?
    public let orphansFound: UInt64?
    /// $MFTMirr was rewritten from $MFT (D-98); absent from older reports.
    public let mirrorRepaired: Bool?
    /// Records and clusters marked used by nothing that the repair freed, and files given a new
    /// security descriptor (D-102); absent from older reports.
    public let recordsFreed, clustersFreed, securityReplaced: UInt64?
    public let repairRan, dirtyCleared: Bool
    public let exitCode: Int32
    public let error: String?

    /// What the user can do next.
    public enum Verdict: Equatable, Sendable {
        case clean              // nothing found (the mark was cleared, if a repair ran)
        case repaired           // everything found was repaired
        case repairable         // everything found can be repaired safely (bitmap bits, dangling folder
                                // entries, files no folder lists, the file table's backup copy)
        case needsWindows       // something only Windows' chkdsk (or a reformat) can fix
    }

    public var verdict: Verdict {
        if remaining == 0 { return found == 0 ? .clean : .repaired }
        return found > 0 && found == repairable && !repairRan ? .repairable : .needsWindows
    }

    /// Counts by kind, in a fixed order, for summaries.
    public var kinds: [(kind: String, count: Int)] {
        let order = ["mft_mirror", "bad_record", "index_unreadable", "entry_free_record", "entry_wrong_seq",
                     "entry_wrong_parent", "orphan", "bad_security", "clusters_shared", "bad_runlist",
                     "record_unmarked", "clusters_unmarked"]
        let counts = Dictionary(grouping: problems, by: \.kind).mapValues(\.count)
        return (order + counts.keys.filter { !order.contains($0) }.sorted()).compactMap { k in counts[k].map { (k, $0) } }
    }

    /// One line per kind, for people.
    public static func describe(_ kind: String, count n: Int) -> String {
        let s = n == 1 ? "" : "s"
        switch kind {
        case "record_unmarked": return "\(n) file record\(s) in use but marked free (repairable)"
        case "clusters_unmarked": return "\(n) file\(s) using space marked free (repairable)"
        case "clusters_shared": return "\(n) file\(s) sharing space with another file"
        case "bad_runlist": return "\(n) file\(s) whose list of clusters is damaged"
        case "bad_record": return "\(n) file record\(s) that cannot be read"
        case "index_unreadable": return "\(n) folder\(s) that cannot be read"
        case "entry_free_record", "entry_wrong_seq", "entry_wrong_parent":
            return "\(n) folder entr\(n == 1 ? "y" : "ies") pointing at a missing file"
        case "orphan": return "\(n) file\(s) no folder lists"
        case "bad_security": return "\(n) file\(s) whose permissions (security descriptor) are damaged or missing"
        case "mft_mirror": return "the backup copy of the file table does not match it, so the disk cannot be opened"
        default: return "\(n) problem\(s) of kind \(kind)"
        }
    }

    /// Plain advice for the verdict.
    public var advice: String {
        switch verdict {
        case .clean where repairRan && (recordsFreed ?? 0) + (clustersFreed ?? 0) > 0:
            return "Nothing was wrong except space marked used by nothing, which was freed."
                + (dirtyCleared ? " The disk is no longer marked as needing a check." : "")
        case .clean:
            return dirtyCleared ? "Nothing is wrong. The disk is no longer marked as needing a check."
                                : "Nothing is wrong."
        case .repaired:
            var done: [String] = []
            if repaired > 0 { done.append("space in use is marked as used again") }
            if let n = entriesRemoved, n > 0 {
                done.append("\(n) folder entr\(n == 1 ? "y" : "ies") pointing at a missing file \(n == 1 ? "was" : "were") removed")
            }
            if mirrorRepaired == true { done.append("the backup copy of the file table was brought up to date") }
            if let n = orphansRelinked, n > 0 {
                let moved = orphansFound ?? 0
                var line = "\(n) file\(n == 1 ? "" : "s") no folder listed \(n == 1 ? "was" : "were") put back"
                if moved > 0 {
                    line += moved == n ? " in the found.000 folder, since \(n == 1 ? "its" : "their") own could not take "
                        + "\(n == 1 ? "it" : "them") back" : " (\(moved) in the found.000 folder)"
                }
                done.append(line)
            }
            if let n = securityReplaced, n > 0 {
                done.append("\(n) file\(n == 1 ? "" : "s") with damaged permissions \(n == 1 ? "was" : "were") given the "
                            + "permissions Unlatch gives new files")
            }
            if (recordsFreed ?? 0) + (clustersFreed ?? 0) > 0 { done.append("space marked used by nothing was freed") }
            if dirtyCleared { done.append("the disk is no longer marked as needing a check") }
            return "Repaired" + (done.isEmpty ? "" : ": " + done.joined(separator: "; ")) + ". No file's contents were "
                + "changed or overwritten."
        case .repairable:
            var how: [String] = []
            if problems.isEmpty || problems.contains(where: { $0.kind == "record_unmarked" || $0.kind == "clusters_unmarked" }) {
                how.append("marks space that files use as used, so nothing gets overwritten")
            }
            if problems.contains(where: { $0.kind.hasPrefix("entry_") }) {
                how.append("removes folder entries that point at a file that was never written (it was lost when the "
                           + "disk was cut off; only its name is left, and it leads nowhere)")
            }
            if problems.contains(where: { $0.kind == "orphan" }) {
                how.append("puts files that no folder lists back in their folder (the disk was cut off before the "
                           + "folder was written), or in a found.000 folder when theirs is gone or the name is taken")
            }
            if problems.contains(where: { $0.kind == "bad_security" }) {
                how.append("gives files whose permissions were damaged the permissions Unlatch gives every new file")
            }
            if problems.contains(where: { $0.kind == "mft_mirror" }) {
                how.append("brings the backup copy of the file table up to date from the file table itself, then "
                           + "checks the rest of the disk")
            }
            return "Unlatch can repair this safely: it " + how.joined(separator: ", and ") + ". No file's contents are "
                + "changed. Space marked used by nothing is freed as well."
        case .needsWindows:
            return "Unlatch cannot repair this safely. Copy anything important off the disk (read-only), then either run chkdsk on it in Windows (chkdsk X: /f) or reformat it."
        }
    }
}
