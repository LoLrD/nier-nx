import CoreGraphics

let pid = Int32(CommandLine.arguments[1])!
let windows = CGWindowListCopyWindowInfo([.optionOnScreenOnly], kCGNullWindowID) as! [[String: Any]]
for window in windows {
    let owner = window[kCGWindowOwnerPID as String] as? Int32
    let layer = window[kCGWindowLayer as String] as? Int
    if owner == pid && layer == 0 {
        print(window[kCGWindowNumber as String] as! Int)
        break
    }
}
