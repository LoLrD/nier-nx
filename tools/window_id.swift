import CoreGraphics

let pid = Int32(CommandLine.arguments[1])!
let windows = CGWindowListCopyWindowInfo([.optionAll], kCGNullWindowID) as! [[String: Any]]
var best: (id: Int, area: Double)? = nil
for window in windows {
    guard window[kCGWindowOwnerPID as String] as? Int32 == pid,
          window[kCGWindowLayer as String] as? Int == 0,
          let bounds = window[kCGWindowBounds as String] as? [String: Double] else { continue }
    let area = (bounds["Width"] ?? 0) * (bounds["Height"] ?? 0)
    if best == nil || area > best!.area {
        best = (window[kCGWindowNumber as String] as! Int, area)
    }
}
if let best { print(best.id) }
