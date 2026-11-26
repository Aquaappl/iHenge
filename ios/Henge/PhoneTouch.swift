import SwiftUI
import UIKit

/// A virtual iPhone's screen as a touchscreen: each finger on the view goes to the guest's
/// USB touchscreen where it is on the picture (x and y over 0..0x7fff of the screen). Lay it
/// exactly over the displayed screen. Lifting a finger that went down on the bottom edge and
/// moved up sends Home (the engine turns it into Cmd+H: iOS starts no home swipe from it).
struct PhoneTouch: UIViewRepresentable {
    func makeUIView(context: Context) -> Surface { Surface() }

    /// The Home button: a finger up from the very bottom edge, as on the Mac (where it was tried): the lock screen takes
    /// the swipe itself and unlocks (Cmd+H alone does not), and one starting in the bottom 1% that rises 12% also makes
    /// the engine send Cmd+H, which is Home elsewhere. Finger 15, which PhoneTouch hands out last.
    static func home() {
        HengeLog.log("input", "home button")
        let ys: [Int32] = [32700, 31000, 29000, 27000, 25000, 23000, 21000, 19000]
        let x: Int32 = 16384
        func now() -> UInt32 { UInt32(truncatingIfNeeded: Int(ProcessInfo.processInfo.systemUptime * 1000)) }
        Task.detached {
            for (i, y) in ys.enumerated() {
                let ts = now()
                henge_vm_touch(15, i == 0 ? 0 : 1, x, y, ts)
                if i > 0 { henge_vm_touch_frame(ts) }
                try? await Task.sleep(nanoseconds: 120_000_000)
            }
            henge_vm_touch(15, 2, x, ys.last!, now())
        }
    }
    func updateUIView(_ view: Surface, context: Context) {}

    final class Surface: UIView {
        private var ids: [ObjectIdentifier: Int32] = [:]
        private var freeIds = Set<Int32>(1...15)
        private var logged = 0

        override init(frame: CGRect) {
            super.init(frame: frame)
            isMultipleTouchEnabled = true
            backgroundColor = .clear
        }

        required init?(coder: NSCoder) { fatalError("not from a storyboard") }

        private func point(_ t: UITouch) -> (Int32, Int32) {
            let p = t.location(in: self)
            let w = max(Double(bounds.width), 1), h = max(Double(bounds.height), 1)
            let x = min(max(Double(p.x) / w, 0), 1) * 32767, y = min(max(Double(p.y) / h, 0), 1) * 32767
            return (Int32(x), Int32(y))
        }

        private static func ms(_ t: TimeInterval) -> UInt32 { UInt32(truncatingIfNeeded: Int(t * 1000)) }

        override func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent?) {
            for t in touches where t.type != .indirectPointer {
                guard let id = freeIds.min() else { break }
                freeIds.remove(id)
                ids[ObjectIdentifier(t)] = id
                let (x, y) = point(t)
                henge_vm_touch(id, 0, x, y, Self.ms(t.timestamp))
                if logged < 12 {
                    logged += 1
                    HengeLog.log("input", "touchscreen: finger \(id) down at \(x),\(y)")
                }
            }
        }

        override func touchesMoved(_ touches: Set<UITouch>, with event: UIEvent?) {
            var moved = false, ts: UInt32 = 0
            for t in touches {
                guard let id = ids[ObjectIdentifier(t)] else { continue }
                let (x, y) = point(t)
                ts = Self.ms(t.timestamp)
                henge_vm_touch(id, 1, x, y, ts)
                moved = true
            }
            if moved { henge_vm_touch_frame(ts) }
        }

        override func touchesEnded(_ touches: Set<UITouch>, with event: UIEvent?) { lift(touches) }
        override func touchesCancelled(_ touches: Set<UITouch>, with event: UIEvent?) { lift(touches) }

        private func lift(_ touches: Set<UITouch>) {
            for t in touches {
                guard let id = ids.removeValue(forKey: ObjectIdentifier(t)) else { continue }
                let (x, y) = point(t)
                henge_vm_touch(id, 2, x, y, Self.ms(t.timestamp))
                freeIds.insert(id)
            }
        }
    }
}
