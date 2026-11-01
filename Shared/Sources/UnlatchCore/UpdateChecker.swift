// SPDX-License-Identifier: GPL-3.0-or-later
import Foundation

/// Asks GitHub for the newest Unlatch release (plan Step 19, D-70). There is no signed updater:
/// the app shows that an update exists and how to get it (Homebrew or the DMG), nothing more.
/// One unauthenticated request to api.github.com, sending no data about the Mac or its disks.
public struct UpdateChecker: Sendable {
    public struct Release: Sendable, Equatable {
        public let version: String
        public let page: URL
    }

    public static let releasesAPI = URL(string: "https://api.github.com/repos/Ramanan-Bala/unlatch/releases")!

    private let current: String
    private let session: URLSession

    public init(current: String = Unlatch.version, session: URLSession = .shared) {
        self.current = current
        self.session = session
    }

    /// The newest published release newer than the running version, or nil. Pre-releases
    /// count only while the running version is itself a pre-release (a beta tester).
    public func check() async throws -> Release? {
        var req = URLRequest(url: Self.releasesAPI.appending(queryItems: [URLQueryItem(name: "per_page", value: "20")]))
        req.setValue("application/vnd.github+json", forHTTPHeaderField: "Accept")
        req.setValue("Unlatch/\(current)", forHTTPHeaderField: "User-Agent")
        req.timeoutInterval = 15
        let (data, response) = try await session.data(for: req)
        guard (response as? HTTPURLResponse)?.statusCode == 200 else { return nil }
        return Self.newest(in: data, current: current)
    }

    struct GitHubRelease: Decodable {
        let tag_name: String
        let html_url: URL
        let draft: Bool
        let prerelease: Bool
    }

    /// Picks the release to offer from the API's JSON (tests feed it directly).
    static func newest(in json: Data, current: String) -> Release? {
        guard let all = try? JSONDecoder().decode([GitHubRelease].self, from: json) else { return nil }
        let allowPre = Version(current)?.pre != nil
        var best: (Version, Release)?
        for r in all where !r.draft && (allowPre || !r.prerelease) {
            let tag = r.tag_name.hasPrefix("v") ? String(r.tag_name.dropFirst()) : r.tag_name
            guard let v = Version(tag), let cur = Version(current), v > cur else { continue }
            if best == nil || v > best!.0 { best = (v, Release(version: tag, page: r.html_url)) }
        }
        return best?.1
    }
}

/// Semantic version X.Y.Z[-pre]: a pre-release sorts before its release, and pre-release
/// identifiers compare numerically when both are numbers (beta.2 < beta.10).
public struct Version: Comparable, Sendable {
    public let major, minor, patch: Int
    public let pre: [String]?

    public init?(_ s: String) {
        let parts = s.split(separator: "-", maxSplits: 1)
        let nums = parts[0].split(separator: ".").map { Int($0) }
        guard nums.count == 3, let a = nums[0], let b = nums[1], let c = nums[2] else { return nil }
        (major, minor, patch) = (a, b, c)
        pre = parts.count > 1 ? parts[1].split(separator: ".").map(String.init) : nil
    }

    public static func < (l: Version, r: Version) -> Bool {
        if (l.major, l.minor, l.patch) != (r.major, r.minor, r.patch) {
            return (l.major, l.minor, l.patch) < (r.major, r.minor, r.patch)
        }
        switch (l.pre, r.pre) {
        case (nil, nil), (nil, _): return false
        case (_, nil): return true
        case let (a?, b?):
            for (x, y) in zip(a, b) where x != y {
                if let i = Int(x), let j = Int(y) { return i < j }
                return x < y
            }
            return a.count < b.count
        }
    }
}
