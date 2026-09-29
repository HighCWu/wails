// Minimal input driver for the macOS click-through CI scenario.
//   swift-driver warp  X Y   — move the cursor (no TCC permission needed)
//   swift-driver click X Y   — post a left click (needs Accessibility)
import CoreGraphics
import Foundation

let args = CommandLine.arguments
guard args.count >= 2 else { FileHandle.standardError.write("usage: driver warp|click X Y\n".data(using: .utf8)!); exit(2) }

let x = Double(args[2]) ?? -1
let y = Double(args[3]) ?? -1
guard x >= 0, y >= 0 else { FileHandle.standardError.write("bad coordinates\n".data(using: .utf8)!); exit(2) }

switch args[1] {
case "warp":
    let r = CGWarpMouseCursorPosition(CGPoint(x: x, y: y))
    if r != .success { FileHandle.standardError.write("warp failed: \(r)\n".data(using: .utf8)!); exit(1) }
case "click":
    let pt = CGPoint(x: x, y: y)
    guard let down = CGEvent(mouseEventSource: nil, mouseType: .leftMouseDown, mouseCursorPosition: pt, mouseButton: .left),
          let up = CGEvent(mouseEventSource: nil, mouseType: .leftMouseUp, mouseCursorPosition: pt, mouseButton: .left) else {
        FileHandle.standardError.write("event creation failed\n".data(using: .utf8)!); exit(1)
    }
    down.post(tap: CGEventTapLocation.cghidEventTap)
    usleep(60_000)
    up.post(tap: CGEventTapLocation.cghidEventTap)
case "locate":
    // dump every on-screen window covering (X,Y): owner, layer, bounds
    guard let list = CGWindowListCopyWindowInfo([.optionOnScreenOnly, .excludeDesktopElements], kCGNullWindowID) as? [[String: Any]] else { exit(0) }
    for w in list {
        guard let b = w[kCGWindowBounds as String] as? [String: Any],
              let bx = b["X"] as? Double, let by = b["Y"] as? Double,
              let bw = b["Width"] as? Double, let bh = b["Height"] as? Double else { continue }
        if x >= bx, x <= bx + bw, y >= by, y <= by + bh {
            let owner = w[kCGWindowOwnerName as String] as? String ?? "?"
            let pid = w[kCGWindowOwnerPID as String] as? Int ?? -1
            let name = w[kCGWindowName as String] as? String ?? ""
            let layer = w[kCGWindowLayer as String] as? Int ?? -1
            print("covering: owner=\(owner) pid=\(pid) layer=\(layer) bounds=\(bx),\(by) \(bw)x\(bh) name='\(name)'")
        }
    }
default:
    FileHandle.standardError.write("unknown command \(args[1])\n".data(using: .utf8)!); exit(2)
}
