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
    guard let down = CGEvent(mouseEventSource: nil, mouseType: .leftMouseDown, mouseButtonClick: 1, mouseCursorPosition: pt, mouseButton: .left),
          let up = CGEvent(mouseEventSource: nil, mouseType: .leftMouseUp, mouseButtonClick: 1, mouseCursorPosition: pt, mouseButton: .left) else {
        FileHandle.standardError.write("event creation failed\n".data(using: .utf8)!); exit(1)
    }
    down.post(tap: .cghidEventTap)
    usleep(60_000)
    up.post(tap: .cghidEventTap)
default:
    FileHandle.standardError.write("unknown command \(args[1])\n".data(using: .utf8)!); exit(2)
}
