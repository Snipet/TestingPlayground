// Tools/logo/logo.swift: the playgrounds' mark, "Caret" (a wave written up to the editor's caret: "~|"), and
// everything made from it. This file is the one source of the mark's geometry: every number is below, and every asset
// is written from them, the editor's top-bar mark (Source/editor/LogoMark.h) included.
//
//   swift Tools/logo/logo.swift [--font <JetBrainsMono-Regular.ttf>] [--out <dir>] [--preview <dir>]
//
// It reads products/*.cmake (each product's key, name and kind) and Source/editor/Theme.h (the instrument's accent),
// and writes, under the repository (or --out <dir>, to compare):
//   Assets/logo/<kind>/        kind = effect | instrument (a product's PRODUCT_KIND):
//       icon.iconset/          the macOS app icon, 16 to 512 pt at 1x and 2x (16, 32, 64, 128, 256, 512, 1024 px)
//       icon.icns              iconutil's .icns of it: each bundle's Icon.icns (CMakeLists.txt)
//       icon.svg               the app icon's master (1024)
//       installer-1024.png     the app icon at 1024, for an installer's window (and JUCE's ICON_BIG)
//       extension-512.png      the Claude Desktop extension's icon (also Source/mcp/icon-<key>.png, every product)
//       mark-graphite.svg, mark-paper.svg, mark-flat.svg   the mark alone: on dark, on light, in one colour
//       lockup-graphite.png, lockup-paper.png              the mark beside the product's name
//   Assets/logo/readme-graphite.png, readme-paper.png       the README's header (a <picture>)
//   Assets/logo/construction.png                            the construction, drawn and measured
//   Assets/logo/sheet.png                                   every size of both, on both themes, and the top bar
//                                                           (in FunkGui's pixels: its rasteriser mirrored below)
//   Source/editor/LogoMark.h                                the top bar's mark at 1x and 2x (generated: do not edit)
//   Source/mcp/icon-<key>.png                               each product's extension icon
// --font names JetBrains Mono (FunkGui's fonts/upstream/JetBrainsMono-Regular.ttf); without it the script takes the
// one a build fetched (build-*/_deps/funkgui-src/fonts/upstream/), so build once or pass it. --preview <dir> also
// writes enlargements of the small sizes there, to look at. The output is the same bytes run after run.
//
// THE CONSTRUCTION. Mark units u; the baseline is y = 0 and y grows downward.
//   Stroke     2u, round caps and joins.
//   Wave       one cycle of two circular arcs that meet on the baseline with one tangent: crossings (0,0), (8,0),
//              (16,0); crest (4,-3), trough (12,3). Each arc has chord 8 and sagitta 3, so its radius is
//              (4^2 + 3^2) / (2 * 3) = 25/6, and the crest, the crossing and the chord's foot make a 3-4-5 triangle;
//              tan(theta/2) = 3/4, theta = 73.74 deg, the slope at every crossing.
//   Caret      x = 20, from -6 to 6 (ink -7 to 7, the stem 2u). The synth's wave ends in a cap whose ink reaches
//              x 17, one stroke (2u) short of the caret's ink at 19; the effect's, risen, 17.22 (1.78u short).
//   Instrument the second half is the crest's arc turned over: a pure cycle (the synth).
//   Effect     the second half is processed: flanks leave (8,0) on the arc's own tangent (no kink at the join), turn
//              through fillets of 1.5u (centreline; tangent length 1.5 * 3/4 = 1.125u) onto a flat floor, and climb
//              back. Optical corrections: the floor's outer edge at 3.88u, not the round trough's 4.00u, so the two
//              read equally deep (4.00u at u < 6, 128 px and below, its edge on a whole pixel); the floor 92% of the
//              stroke thick (a horizontal reads heavier than a curve of the same weight), thinned on its inner side;
//              and the right flank runs on 0.75u past the baseline, so it reads as long as the left one, which the
//              crest's arc carries on (without it the flank reads as a hook).
//   Ink box    x -1 to 21, y -7 to 7 (22u x 14u). Placed 0.5u left of its ground's centre: the caret's solid stem
//              weighs the right side.
//   App icon   Apple's macOS grid: 1024 canvas, the body 824 at 100, continuous corners r 185.4; the body #33353B (top)
//              to #1C1D21; a light rim 3 px inside the edge (14% white at the top, 6% from the middle down), so the
//              body keeps its edge on a dark Dock; shadow down 10, blur 20, black 30%. u = size * 26 / 1024: 26 at
//              1024 (the mark 572 px wide, 69% of the body; stroke 52; crest and trough +-78), 13 at 512, 6.5 at 256,
//              3.25 at 128, 1.625 at 64. Hinting: the crest's top and the trough's bottom on whole pixels
//              (oy = floor(c - 4u) + 4u), the caret's stem on whole columns (2u, or floor(2u) where 2u is not whole: 6
//              at 128, 3 at 64), the floor's edge (round(3.88u)) and thickness (round(0.92 * 2u)) whole.
//   32 px      on its own grid: x unit 0.75, y unit 1, stroke 2: crossings at x 8, 14, 20; the crest's top on row 12,
//              the trough's bottom on row 20 (the effect's floor sharp-cornered); the caret columns 23-24, rows 9-22,
//              square ends; body 26 at 3.
//   16 px      set pixel by pixel (pix16): 1 px lines, the crest on row 6, the trough on row 9, the caret column 12,
//              rows 4-11; the effect's trough a box whose right flank climbs to row 7; body 14 at 1.
//   Top bar    the editor's 26 px tile (radius 8, the accent at 16%), the mark in the accent alone, one drawing a
//              backing scale (LogoMark.h; Panel.cpp picks the 1x one at a Canvas::dpi() of exactly 1, the 2x one at any
//              other): the wave an areaStrip (FunkGui's AREA columns, half a device px wide, between the stroke's top
//              and bottom edges: one primitive over each sample, so no antialiased fringe is blended twice, as a
//              polyline's capsules blend theirs where they overlap) and the caret one primitive. FunkGui antialiases
//              every edge over one device px each side (a smoothstep): an edge on a whole device px renders as about
//              16% outside and 84% inside it, the crispest it draws, which the hints below keep at a zoom of 100%
//              and 200%.
//              2x: u 0.75, stroke 1.5 (3 device px), baseline 13: the crest's top 10.0 and the trough's bottom 16.0
//                  (device px 20 and 32); the caret a capsule at x 20.25, its stem device px 39-42.
//              1x: one stroke for both, 2 px, so they weigh the same: x unit 0.75 (crossings 5, 11, 17) and y unit
//                  2/3, so the crest's centreline is on 11 and the trough's on 15 (rows 10-11 and 14-15 whole); the
//                  caret 2 px on columns 20-21, rows 8-17, square ends (as at 32 px), two columns clear of the wave;
//                  the effect's floor sharp-cornered, 4.7 px long on rows 14-15, the instrument's trough round.
//              At other scales (a zoom of 125% or 150%) the 2x drawing lands where it lands, antialiased. The sheet
//              shows the tiles in FunkGui's own pixels (its rasteriser, mirrored here: gui.dump's PNGs, byte for byte).
//   Extension  512, the full square (corners 0.225 * 512), u 16. README: both marks at u 8, 8u apart, beside
//              JetBrains Mono at 64 px (a product's own lockup: its mark alone).
//   Colours    the wave in ink (#ECEAE4 on the icon and GRAPHITE, #17181A on PAPER); the caret in the product's
//              accent: the effect's #FF5A1F (PAPER #E8451A, FunkGui's own), the instrument's Theme.h's (#4FC3F7,
//              PAPER #0B6E99). The top bar's mark is the theme's accent alone.
import CoreGraphics
import CoreText
import Foundation
import ImageIO
import UniformTypeIdentifiers

let PI = Double.pi

// MARK: - where things are

let CWD = URL(fileURLWithPath: FileManager.default.currentDirectoryPath)
let ROOT = URL(fileURLWithPath: #filePath, relativeTo: CWD).standardizedFileURL
    .deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()

func fail(_ s: String) -> Never {
    FileHandle.standardError.write(("logo.swift: " + s + "\n").data(using: .utf8)!)
    exit(1)
}

var ARGS: [String: String] = [:]
do {
    var a = Array(CommandLine.arguments.dropFirst())
    while !a.isEmpty {
        let k = a.removeFirst()
        guard ["--font", "--out", "--preview"].contains(k), !a.isEmpty else {
            fail("usage: swift Tools/logo/logo.swift [--font <ttf>] [--out <dir>] [--preview <dir>]")
        }
        ARGS[k] = a.removeFirst()
    }
}
let OUT = ARGS["--out"].map { URL(fileURLWithPath: $0, relativeTo: CWD).standardizedFileURL } ?? ROOT
let PREVIEW = ARGS["--preview"].map { URL(fileURLWithPath: $0, relativeTo: CWD).standardizedFileURL }

func out(_ rel: String) -> String {
    let url = OUT.appendingPathComponent(rel)
    try? FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
    return url.path
}

// MARK: - the products (products/<key>.cmake) and the instrument's accent (Source/editor/Theme.h)

enum Kind: String, CaseIterable { case effect, instrument }

struct Product { let key, name: String; let kind: Kind }

func readProducts() -> [Product] {
    let dir = ROOT.appendingPathComponent("products")
    guard let files = try? FileManager.default.contentsOfDirectory(atPath: dir.path) else { fail("no products/") }
    var list: [Product] = []
    for f in files.sorted() where f.hasSuffix(".cmake") {
        guard let text = try? String(contentsOf: dir.appendingPathComponent(f), encoding: .utf8) else { continue }
        var v: [String: String] = [:]
        for line in text.split(separator: "\n") {
            let s = String(line)
            guard s.hasPrefix("set(PRODUCT_"), let q0 = s.firstIndex(of: "\""), let q1 = s.lastIndex(of: "\""),
                  q0 < q1 else { continue }
            v[String(s.dropFirst(4).prefix { $0 != " " })] = String(s[s.index(after: q0)..<q1])
        }
        guard let name = v["PRODUCT_NAME"], let kind = v["PRODUCT_KIND"].flatMap(Kind.init) else {
            fail("products/\(f): no PRODUCT_NAME or PRODUCT_KIND")
        }
        list.append(Product(key: String(f.dropLast(6)), name: name, kind: kind))
    }
    return list
}

// MARK: - colour

struct C {
    var r, g, b, a: Double
    let hex: UInt32
    init(_ hex: UInt32, _ a: Double = 1) {
        self.hex = hex
        r = Double((hex >> 16) & 255) / 255; g = Double((hex >> 8) & 255) / 255; b = Double(hex & 255) / 255
        self.a = a
    }
    var cg: CGColor { CGColor(srgbRed: r, green: g, blue: b, alpha: a) }
    func alpha(_ x: Double) -> C { var c = self; c.a = x; return c }
    var css: String { String(format: "#%06X", hex) }
}
let sRGB = CGColorSpace(name: CGColorSpace.sRGB)!

// The instrument's accent, dark and light, as Theme.h's productTheme() sets it (its `t.accent = dark ? ...` line).
func instrumentAccent() -> (C, C) {
    let path = ROOT.appendingPathComponent("Source/editor/Theme.h")
    guard let text = try? String(contentsOf: path, encoding: .utf8) else { fail("no Source/editor/Theme.h") }
    let col = "funkgui::Col\\{\\s*0x([0-9A-Fa-f]{2}),\\s*0x([0-9A-Fa-f]{2}),\\s*0x([0-9A-Fa-f]{2})\\s*\\}"
    let re = try! NSRegularExpression(pattern: "t\\.accent\\s*=\\s*dark\\s*\\?\\s*\(col)\\s*:\\s*\(col)")
    guard let m = re.firstMatch(in: text, range: NSRange(text.startIndex..., in: text)) else {
        fail("Source/editor/Theme.h: no `t.accent = dark ? funkgui::Col{ ... } : funkgui::Col{ ... }`")
    }
    func byte(_ i: Int) -> UInt32 { UInt32(text[Range(m.range(at: i), in: text)!], radix: 16)! }
    return (C(byte(1) << 16 | byte(2) << 8 | byte(3)), C(byte(4) << 16 | byte(5) << 8 | byte(6)))
}

// funkgui::Theme::graphite() and paper(): the ground, the ink ladder's ends and the effect's accent; the top bar's
// card is ChatStyle.h's (the ground mixed 3.5% toward ink100 on GRAPHITE, 62% toward white on PAPER, truncated).
struct Theme {
    let name: String
    let ground, ink, ink52, card: C
    let effectAccent, instrumentAccent: C
    func accent(_ k: Kind) -> C { k == .effect ? effectAccent : instrumentAccent }
}
let (INSTRUMENT_DARK, INSTRUMENT_LIGHT) = instrumentAccent()
let GRAPHITE = Theme(name: "graphite", ground: C(0x16171A), ink: C(0xECEAE4), ink52: C(0x8A8E95), card: C(0x1D1E21),
                     effectAccent: C(0xFF5A1F), instrumentAccent: INSTRUMENT_DARK)
let PAPER = Theme(name: "paper", ground: C(0xEDEBE6), ink: C(0x17181A), ink52: C(0x777776), card: C(0xF8F7F5),
                  effectAccent: C(0xE8451A), instrumentAccent: INSTRUMENT_LIGHT)

// MARK: - geometry

struct P2 {
    var x, y: Double
    init(_ x: Double, _ y: Double) { self.x = x; self.y = y }
    static func + (a: P2, b: P2) -> P2 { P2(a.x + b.x, a.y + b.y) }
    static func - (a: P2, b: P2) -> P2 { P2(a.x - b.x, a.y - b.y) }
    static func * (a: P2, k: Double) -> P2 { P2(a.x * k, a.y * k) }
    var angle: Double { atan2(y, x) }
    var cg: CGPoint { CGPoint(x: x, y: y) }
}
func polar(_ c: P2, _ r: Double, _ a: Double) -> P2 { P2(c.x + r * cos(a), c.y + r * sin(a)) }

// A path of lines, circular arcs (centre, radius, from angle a0 to a1, in y-down coordinates) and cubics.
enum Seg { case line(P2); case arc(P2, Double, Double, Double); case cubic(P2, P2, P2) }

struct Path2 {
    var start: P2
    var segs: [Seg] = []
    var closed = false
    init(_ start: P2, closed: Bool = false) { self.start = start; self.closed = closed }
    var end: P2 {
        guard let s = segs.last else { return start }
        switch s {
        case .line(let p): return p
        case .arc(let c, let r, _, let a1): return polar(c, r, a1)
        case .cubic(_, _, let p): return p
        }
    }
    mutating func line(_ p: P2) { segs.append(.line(p)) }
    mutating func arc(_ c: P2, _ r: Double, _ a0: Double, _ a1: Double) { segs.append(.arc(c, r, a0, a1)) }
    mutating func cubic(_ c1: P2, _ c2: P2, _ p: P2) { segs.append(.cubic(c1, c2, p)) }
    // An arc from the current point around c to the angle a1, the short way.
    mutating func arcTo(_ c: P2, _ r: Double, _ a1: Double) {
        let a0 = (end - c).angle
        var d = a1 - a0
        while d > PI { d -= 2 * PI }
        while d < -PI { d += 2 * PI }
        segs.append(.arc(c, r, a0, a0 + d))
    }
    // The path at px = (ox + x * s, oy + y * s).
    func mapped(ox: Double, oy: Double, s: Double) -> Path2 {
        let f = { (p: P2) in P2(ox + p.x * s, oy + p.y * s) }
        var q = Path2(f(start), closed: closed)
        for g in segs {
            switch g {
            case .line(let p): q.segs.append(.line(f(p)))
            case .arc(let c, let r, let a0, let a1): q.segs.append(.arc(f(c), r * s, a0, a1))
            case .cubic(let c1, let c2, let p): q.segs.append(.cubic(f(c1), f(c2), f(p)))
            }
        }
        return q
    }
    // An arc as cubics of at most 90 degrees each: their control points and ends.
    static func arcPieces(_ c: P2, _ r: Double, _ a0: Double, _ a1: Double) -> [(P2, P2, P2)] {
        let n = max(1, Int(ceil(abs(a1 - a0) / (PI / 2) - 1e-9)))
        let da = (a1 - a0) / Double(n), k = 4.0 / 3.0 * tan(da / 4)
        return (0..<n).map { i in
            let t0 = a0 + Double(i) * da, t1 = t0 + da
            let p0 = polar(c, r, t0), p3 = polar(c, r, t1)
            return (P2(p0.x - k * r * sin(t0), p0.y + k * r * cos(t0)),
                    P2(p3.x + k * r * sin(t1), p3.y - k * r * cos(t1)), p3)
        }
    }
    var cg: CGPath {
        let p = CGMutablePath()
        p.move(to: start.cg)
        for g in segs {
            switch g {
            case .line(let q): p.addLine(to: q.cg)
            case .arc(let c, let r, let a0, let a1):
                for (c1, c2, q) in Path2.arcPieces(c, r, a0, a1) {
                    p.addCurve(to: q.cg, control1: c1.cg, control2: c2.cg)
                }
            case .cubic(let c1, let c2, let q): p.addCurve(to: q.cg, control1: c1.cg, control2: c2.cg)
            }
        }
        if closed { p.closeSubpath() }
        return p
    }
    // SVG path data: arcs as SVG arcs of at most 90 degrees (exact circles, no flag ambiguity).
    var svg: String {
        var s = "M\(num(start.x)) \(num(start.y))"
        for g in segs {
            switch g {
            case .line(let q): s += "L\(num(q.x)) \(num(q.y))"
            case .arc(let c, let r, let a0, let a1):
                let n = max(1, Int(ceil(abs(a1 - a0) / (PI / 2) - 1e-9)))
                for i in 1...n {
                    let q = polar(c, r, a0 + (a1 - a0) * Double(i) / Double(n))
                    s += "A\(num(r)) \(num(r)) 0 0 \(a1 > a0 ? 1 : 0) \(num(q.x)) \(num(q.y))"
                }
            case .cubic(let c1, let c2, let q):
                s += "C\(num(c1.x)) \(num(c1.y)) \(num(c2.x)) \(num(c2.y)) \(num(q.x)) \(num(q.y))"
            }
        }
        return closed ? s + "Z" : s
    }
    // Points along the path (a polyline's): arcs at most `step` radians apart in an even number of steps (so the
    // crest's and the trough's extremes, their arcs' middles, are points), lines in pieces of at most `piece`.
    func sampled(step: Double, piece: Double = .infinity) -> [P2] {
        var pts = [start]
        for g in segs {
            switch g {
            case .line(let q):
                let p0 = pts.last!, n = max(1, Int(ceil(hypot(q.x - p0.x, q.y - p0.y) / piece - 1e-9)))
                for i in 1...n { pts.append(p0 + (q - p0) * (Double(i) / Double(n))) }
            case .arc(let c, let r, let a0, let a1):
                var n = max(2, Int(ceil(abs(a1 - a0) / step - 1e-9)))
                n += n % 2
                for i in 1...n { pts.append(polar(c, r, a0 + (a1 - a0) * Double(i) / Double(n))) }
            case .cubic(_, _, let q): pts.append(q)
            }
        }
        var kept: [P2] = []
        for p in pts where kept.isEmpty || hypot(p.x - kept.last!.x, p.y - kept.last!.y) > 1e-9 { kept.append(p) }
        return kept
    }
}

func num(_ x: Double) -> String {
    var s = String(format: "%.4f", x)
    while s.hasSuffix("0") { s.removeLast() }
    if s.hasSuffix(".") { s.removeLast() }
    return s == "-0" ? "0" : s
}

struct Line { var p: P2; var d: P2 }
func intersect(_ a: Line, _ b: Line) -> P2 {
    let den = a.d.x * b.d.y - a.d.y * b.d.x
    let t = ((b.p.x - a.p.x) * b.d.y - (b.p.y - a.p.y) * b.d.x) / den
    return a.p + a.d * t
}
func shifted(_ l: Line, _ v: P2) -> Line { Line(p: l.p + v, d: l.d) }
// The circle of radius r tangent to lines a and b, its centre on the side of a's unit normal na and of b's nb: its
// centre and its tangent points on a and on b.
func fillet(_ a: Line, _ na: P2, _ b: Line, _ nb: P2, _ r: Double) -> (c: P2, ta: P2, tb: P2) {
    let c = intersect(shifted(a, na * r), shifted(b, nb * r))
    return (c, c - na * r, c - nb * r)
}

// MARK: - THE CONSTRUCTION (units)

enum M {
    static let amp = 3.0                                     // crest -3, trough 3
    static let radius = (4.0 * 4.0 + amp * amp) / (2 * amp)  // 25/6: chord 8, sagitta 3
    static let theta = asin(4.0 / radius)                    // 73.74 deg: the slope at every crossing
    static let crestCentre = P2(4, -amp + radius), troughCentre = P2(12, amp - radius)
    static let crestFrom = -PI / 2 - theta, crestTo = -PI / 2 + theta
    static let stroke = 2.0
    static let caretX = 20.0, caretHalf = 6.0                // the caret's centreline: x 20, y -6 to 6
    static let floorEdge = 3.88                              // the effect's floor's outer edge (the trough's: 4)
    static let floorWeight = 0.92                            // its thickness, of the stroke
    static let fillet = 1.5                                  // its fillets' radius (centreline)
    static let rise = 0.75                                   // its last flank, past the baseline
    static let optical = 0.5                                 // the ink box, left of its ground's centre
    static let box = (x0: -1.0, x1: 21.0, y0: -7.0, y1: 7.0)
    static let uPer1024 = 26.0                               // the app icon's unit, per 1024 px
}

// The wave's centreline: D the effect's floor (centreline), f its fillets (0: sharp), rise its last flank's run past
// the baseline.
func centreline(_ k: Kind, D: Double, f: Double, rise: Double) -> Path2 {
    let th = M.theta
    var p = Path2(P2(0, 0))
    p.arc(M.crestCentre, M.radius, M.crestFrom, M.crestTo)
    switch k {
    case .instrument:
        p.arc(M.troughCentre, M.radius, PI / 2 + th, PI / 2 - th)
    case .effect:
        let cl = P2(8 + D / tan(th), D), cr = P2(16 - D / tan(th), D)
        if f > 0 {
            let t = f * tan(th / 2)
            let fl = P2(cl.x + t, D - f), fr = P2(cr.x - t, D - f)
            p.line(polar(fl, f, PI / 2 + th)); p.arc(fl, f, PI / 2 + th, PI / 2)
            p.line(P2(fr.x, D)); p.arc(fr, f, PI / 2, PI / 2 - th)
        } else {
            p.line(cl); p.line(cr)
        }
        p.line(P2(16 + rise / tan(th), -rise))
    }
    return p
}

// The wave's outline, filled: w the half stroke; the effect's floor between Fi (its inner edge) and Fo (its outer),
// its fillets f + w outside and f - w inside (each a circle tangent to its flank's edge and its floor's).
func waveOutline(_ k: Kind, w: Double, Fo: Double, Fi: Double, f: Double, rise: Double) -> Path2 {
    let th = M.theta
    let nU = P2(sin(th), -cos(th))           // the crest's outward normal at (8,0): the left flank's inner side
    let nR = P2(-sin(th), -cos(th))          // the right flank's inner side
    var p = Path2(polar(M.crestCentre, M.radius + w, M.crestFrom), closed: true)
    p.arc(M.crestCentre, M.radius + w, M.crestFrom, M.crestTo)
    switch k {
    case .instrument:
        p.arc(M.troughCentre, M.radius - w, PI / 2 + th, PI / 2 - th)
        p.arc(P2(16, 0), w, nR.angle, nR.angle + PI)
        p.arc(M.troughCentre, M.radius + w, PI / 2 - th, PI / 2 + th)
    case .effect:
        let flank = Line(p: P2(8, 0), d: P2(cos(th), sin(th)))
        let inner = shifted(flank, nU * w), outer = shifted(flank, nU * -w)
        let floorIn = Line(p: P2(0, Fi), d: P2(1, 0)), floorOut = Line(p: P2(0, Fo), d: P2(1, 0))
        let up = P2(0, -1)
        let mirror = { (q: P2) in P2(24 - q.x, q.y) }
        if f - w > 1e-9 {
            let fi = fillet(inner, nU, floorIn, up, f - w)
            p.line(fi.ta); p.arcTo(fi.c, f - w, (fi.tb - fi.c).angle)
            p.line(mirror(fi.tb)); p.arcTo(mirror(fi.c), f - w, (mirror(fi.ta) - mirror(fi.c)).angle)
        } else {
            let corner = intersect(inner, floorIn)
            p.line(corner); p.line(mirror(corner))
        }
        let end = P2(16 + rise / tan(th), -rise)
        p.line(end + nR * w)
        p.arc(end, w, nR.angle, nR.angle + PI)
        let fo = fillet(outer, nU, floorOut, up, f + w)
        p.line(mirror(fo.ta)); p.arcTo(mirror(fo.c), f + w, (mirror(fo.tb) - mirror(fo.c)).angle)
        p.line(fo.tb); p.arcTo(fo.c, f + w, (fo.ta - fo.c).angle)
        p.line(P2(8, 0) - nU * w)
    }
    p.arc(M.crestCentre, M.radius - w, M.crestTo, M.crestFrom)
    let back = P2(sin(th), cos(th))
    p.arc(P2(0, 0), w, back.angle, back.angle + PI)
    return p
}

// The caret's stem with round ends: its centre x, its centreline's top and bottom, its width.
func caretOutline(x: Double, top: Double, bottom: Double, width: Double) -> Path2 {
    let r = width / 2
    var p = Path2(P2(x - r, top), closed: true)
    p.line(P2(x - r, bottom)); p.arc(P2(x, bottom), r, PI, 0)
    p.line(P2(x + r, top)); p.arc(P2(x, top), r, 0, -PI)
    return p
}

// MARK: - placements

// The mark as outlines at one scale (64 px and up, the extension, the README, the SVG masters): px = o + u * unit.
struct FillPlace {
    var ox, oy, u: Double
    var caretW: Double                       // px
    var floorEdge, floorThick: Double        // units
    func wave(_ k: Kind) -> Path2 {
        waveOutline(k, w: M.stroke / 2, Fo: floorEdge, Fi: floorEdge - floorThick, f: M.fillet, rise: M.rise)
            .mapped(ox: ox, oy: oy, s: u)
    }
    var caret: Path2 {
        caretOutline(x: ox + M.caretX * u, top: oy - M.caretHalf * u, bottom: oy + M.caretHalf * u, width: caretW)
    }
}

// Centred on (cx, cy) at u px a unit, hinted: the crest's top and the trough's bottom on whole pixels, the caret's
// stem on whole columns, the floor's edge and thickness whole (its edge the trough's below u 6, where 3.88u cannot
// round to a pixel of its own).
func fillPlace(cx: Double, cy: Double, u: Double) -> FillPlace {
    let caretW = (M.stroke * u).rounded(.down)
    var ox = cx - (10 + M.optical) * u
    let stemLeft = ox + M.caretX * u - caretW / 2
    ox += stemLeft.rounded() - stemLeft
    let oy = (cy - 4 * u).rounded(.down) + 4 * u
    let edge = u >= 6 ? (M.floorEdge * u).rounded() / u : 4.0
    let thick = (M.floorWeight * M.stroke * u).rounded() / u
    return FillPlace(ox: ox, oy: oy, u: u, caretW: caretW, floorEdge: edge, floorThick: thick)
}

// The mark as a stroked centreline on a hinted grid (32 px, the top bar): px = (ox + x * ux, oy + y * uy); the caret
// a capsule from caretTop to caretBottom on its centreline (caretRound), or a rectangle whose edges they are.
struct StrokePlace {
    var ox, oy, ux, uy, stroke: Double
    var floor, fillet, rise: Double          // units (the effect's floor's centreline)
    var caretX, caretTop, caretBottom, caretW: Double   // px
    var caretRound: Bool
    var dpi = 1.0                            // device px a px: the top bar's columns are half of one
    // CoreGraphics' polyline (32 px): arcs in even steps whose chords stray at most 0.01 device px, lines in pieces as
    // long.
    var step: Double { 2 * acos(1 - 0.01 / (M.radius * ux * dpi)) }       // radians
    var piece: Double { M.radius * step }                                  // units
    func points(_ k: Kind) -> [P2] { points(k, step: step, piece: piece) }
    // The centreline in px, its arcs and lines in steps of at most `step` radians and `piece` units.
    func points(_ k: Kind, step: Double, piece: Double) -> [P2] {
        centreline(k, D: floor, f: fillet, rise: rise).sampled(step: step, piece: piece)
            .map { P2(ox + $0.x * ux, oy + $0.y * uy) }
    }
    // The caret's ink: x0, y0, x1, y1.
    var caretBox: (Double, Double, Double, Double) {
        let r = caretW / 2, e = caretRound ? r : 0
        return (caretX - r, caretTop - e, caretX + r, caretBottom + e)
    }
    // The stroke's band as AREA columns: at xs, its top and bottom edges. The band is the centreline grown by half
    // the stroke (round caps and joins), which every vertical cuts in one interval; its edges are found over the
    // centreline at 0.005 px (an error below 1e-5 px), at the ends of the ink and every half device px between.
    func strip(_ k: Kind) -> (xs: [Double], tops: [Double], bottoms: [Double]) {
        let r = stroke / 2
        let pts = points(k, step: 0.001, piece: 0.005)
        let left = pts.map { $0.x }.min()! - r, right = pts.map { $0.x }.max()! + r
        let col = 0.5 / dpi
        var xs = [left]
        var x = ((left / col).rounded(.down) + 1) * col
        while x < right - 1e-9 { if x > left + 1e-9 { xs.append(x) }; x += col }
        xs.append(right)
        var tops: [Double] = [], bottoms: [Double] = []
        for x in xs {
            var t = Double.infinity, b = -Double.infinity
            for p in pts where abs(x - p.x) <= r + 1e-9 {
                let h = max(0, r * r - (x - p.x) * (x - p.x)).squareRoot()
                t = min(t, p.y - h); b = max(b, p.y + h)
            }
            tops.append(t); bottoms.append(b)
        }
        return (xs, tops, bottoms)
    }
    // FunkGui's primitives for the mark at (x, y), as LogoMark.h's numbers make them in Panel.cpp.
    func prims(_ k: Kind, _ c: C, x: Double, y: Double) -> [FGPrim] {
        let s = strip(k)
        var v: [FGPrim] = []
        for i in 0..<(s.xs.count - 1) {
            v.append(.area(x0: x + s.xs[i], x1: x + s.xs[i + 1], t0: y + s.tops[i], t1: y + s.tops[i + 1],
                           b0: y + s.bottoms[i], b1: y + s.bottoms[i + 1], col: c))
        }
        let (x0, y0, x1, y1) = caretBox
        if caretRound {
            v.append(.segment(x0: x + caretX, y0: y + caretTop, x1: x + caretX, y1: y + caretBottom, w: caretW, col: c))
        } else {
            v.append(.rrect(x: x + x0, y: y + y0, w: x1 - x0, h: y1 - y0, r: 0, col: c))
        }
        return v
    }
}

let PLACE32 = StrokePlace(ox: 8, oy: 16, ux: 0.75, uy: 1, stroke: 2, floor: 3, fillet: 0, rise: M.rise,
                          caretX: 24, caretTop: 9, caretBottom: 23, caretW: 2, caretRound: false)
// The top bar's tile (26 px): at 2x the grid whose stem covers device px 39-42; at 1x one 2 px stroke, hinted.
let TILE = 26.0, TILE_RADIUS = 8.0, TILE_ALPHA = 0.16
let TOPBAR2X = StrokePlace(ox: 5.25, oy: 13, ux: 0.75, uy: 0.75, stroke: 1.5, floor: 3, fillet: M.fillet,
                           rise: M.rise, caretX: 20.25, caretTop: 8.5, caretBottom: 17.5, caretW: 1.5,
                           caretRound: true, dpi: 2)
let TOPBAR1X = StrokePlace(ox: 5, oy: 13, ux: 0.75, uy: 2.0 / 3.0, stroke: 2, floor: 3, fillet: 0,
                           rise: M.rise, caretX: 21, caretTop: 8, caretBottom: 18, caretW: 2,
                           caretRound: false, dpi: 1)

// The 16 px mark, set pixel by pixel: rows 4 to 11, '#' the wave, 'C' the caret. One cycle in columns 3-10 (the
// crest on row 6, the trough on row 9), column 11 empty, the caret in column 12. The effect's trough is a box whose
// right flank climbs to row 7, as its left; the instrument's is the crest turned over.
func pix16(_ k: Kind) -> [String] {
    switch k {
    case .instrument: return [
        "............C...",
        "............C...",
        "....##......C...",
        "...#..#.....C...",
        ".......#..#.C...",
        "........##..C...",
        "............C...",
        "............C..."]
    case .effect: return [
        "............C...",
        "............C...",
        "....##......C...",
        "...#..#...#.C...",
        ".......#..#.C...",
        ".......####.C...",
        "............C...",
        "............C..."]
    }
}

// MARK: - drawing

func context(_ w: Int, _ h: Int) -> CGContext {
    let ctx = CGContext(data: nil, width: w, height: h, bitsPerComponent: 8, bytesPerRow: 0, space: sRGB,
                        bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
    ctx.translateBy(x: 0, y: CGFloat(h)); ctx.scaleBy(x: 1, y: -1)
    ctx.setShouldAntialias(true); ctx.interpolationQuality = .high
    return ctx
}
func savePNG(_ img: CGImage, _ path: String) {
    let url = URL(fileURLWithPath: path) as CFURL
    guard let d = CGImageDestinationCreateWithURL(url, UTType.png.identifier as CFString, 1, nil) else {
        fail("cannot write \(path)")
    }
    CGImageDestinationAddImage(d, img, nil)
    if !CGImageDestinationFinalize(d) { fail("cannot write \(path)") }
}
func saveText(_ s: String, _ path: String) {
    do { try s.write(toFile: path, atomically: true, encoding: .utf8) } catch { fail("cannot write \(path)") }
}
func enlarge(_ img: CGImage, _ k: Int) -> CGImage {
    let ctx = CGContext(data: nil, width: img.width * k, height: img.height * k, bitsPerComponent: 8, bytesPerRow: 0,
                        space: sRGB, bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
    ctx.interpolationQuality = .none
    ctx.draw(img, in: CGRect(x: 0, y: 0, width: img.width * k, height: img.height * k))
    return ctx.makeImage()!
}
func put(_ ctx: CGContext, _ img: CGImage, _ r: CGRect, nearest: Bool = false) {
    ctx.saveGState()
    ctx.interpolationQuality = nearest ? .none : .high
    ctx.translateBy(x: 0, y: r.minY + r.maxY); ctx.scaleBy(x: 1, y: -1)
    ctx.draw(img, in: r)
    ctx.restoreGState()
}
func fill(_ ctx: CGContext, _ p: Path2, _ c: C) {
    ctx.setFillColor(c.cg); ctx.addPath(p.cg); ctx.fillPath()
}
func strokePoints(_ ctx: CGContext, _ pts: [P2], _ w: Double, _ c: C, cap: CGLineCap = .round) {
    let p = CGMutablePath()
    p.move(to: pts[0].cg)
    for q in pts.dropFirst() { p.addLine(to: q.cg) }
    ctx.saveGState()
    ctx.setLineCap(cap); ctx.setLineJoin(.round); ctx.setLineWidth(w); ctx.setStrokeColor(c.cg)
    ctx.addPath(p); ctx.strokePath()
    ctx.restoreGState()
}
func drawStroked(_ ctx: CGContext, _ sp: StrokePlace, _ k: Kind, wave: C, caret: C) {
    strokePoints(ctx, sp.points(k), sp.stroke, wave)
    if sp.caretRound {
        strokePoints(ctx, [P2(sp.caretX, sp.caretTop), P2(sp.caretX, sp.caretBottom)], sp.caretW, caret)
    } else {
        ctx.setFillColor(caret.cg)
        ctx.fill(CGRect(x: sp.caretX - sp.caretW / 2, y: sp.caretTop, width: sp.caretW,
                        height: sp.caretBottom - sp.caretTop))
    }
}

// Apple's continuous-corner rounded square (the app icon's body).
func squircle(_ x: Double, _ y: Double, _ w: Double, _ r: Double) -> Path2 {
    let a = 1.52866483, b = 1.08849299, c = 0.86840701, d = 0.63149399
    let e = 0.07491100, f = 0.37282401, g = 0.16905001
    let x0 = x, y0 = y, x1 = x + w, y1 = y + w
    var p = Path2(P2(x0 + a * r, y0), closed: true)
    p.line(P2(x1 - a * r, y0))
    p.cubic(P2(x1 - b * r, y0), P2(x1 - c * r, y0), P2(x1 - d * r, y0 + e * r))
    p.cubic(P2(x1 - f * r, y0 + g * r), P2(x1 - g * r, y0 + f * r), P2(x1 - e * r, y0 + d * r))
    p.cubic(P2(x1, y0 + c * r), P2(x1, y0 + b * r), P2(x1, y0 + a * r))
    p.line(P2(x1, y1 - a * r))
    p.cubic(P2(x1, y1 - b * r), P2(x1, y1 - c * r), P2(x1 - e * r, y1 - d * r))
    p.cubic(P2(x1 - g * r, y1 - f * r), P2(x1 - f * r, y1 - g * r), P2(x1 - d * r, y1 - e * r))
    p.cubic(P2(x1 - c * r, y1), P2(x1 - b * r, y1), P2(x1 - a * r, y1))
    p.line(P2(x0 + a * r, y1))
    p.cubic(P2(x0 + b * r, y1), P2(x0 + c * r, y1), P2(x0 + d * r, y1 - e * r))
    p.cubic(P2(x0 + f * r, y1 - g * r), P2(x0 + g * r, y1 - f * r), P2(x0 + e * r, y1 - d * r))
    p.cubic(P2(x0, y1 - c * r), P2(x0, y1 - b * r), P2(x0, y1 - a * r))
    p.line(P2(x0, y0 + a * r))
    p.cubic(P2(x0, y0 + b * r), P2(x0, y0 + c * r), P2(x0 + e * r, y0 + d * r))
    p.cubic(P2(x0 + g * r, y0 + f * r), P2(x0 + f * r, y0 + g * r), P2(x0 + d * r, y0 + e * r))
    p.cubic(P2(x0 + c * r, y0), P2(x0 + b * r, y0), P2(x0 + a * r, y0))
    return p
}

let BODY_TOP = C(0x33353B), BODY_BOTTOM = C(0x1C1D21)
let RIM_TOP = 0.14, RIM = 0.06
let WAVE_INK = GRAPHITE.ink

struct Body {
    var x, w, r: Double
    var shadow, rim: Bool
    var rimW = 0.0, shadowDy = 0.0, shadowBlur = 0.0
}
// Apple's grid (1024: the body 824 at 100, corners 185.4); 32 and 16 px on whole pixels (26 at 3, 14 at 1); the
// extension's icon the full square.
func body(size: Int, full: Bool) -> Body {
    let s = Double(size), k = s / 1024
    if full { return Body(x: 0, w: s, r: 0.225 * s, shadow: false, rim: true, rimW: max(1, 3 * k)) }
    switch size {
    case 32: return Body(x: 3, w: 26, r: 26 * 0.225, shadow: true, rim: false, shadowDy: 10 * k, shadowBlur: 1)
    case 16: return Body(x: 1, w: 14, r: 14 * 0.225, shadow: false, rim: false)
    default: return Body(x: 100 * k, w: 824 * k, r: 185.4 * k, shadow: true, rim: size >= 64, rimW: max(1, 3 * k),
                         shadowDy: 10 * k, shadowBlur: max(1, 20 * k))
    }
}

func drawBody(_ ctx: CGContext, _ b: Body) {
    let path = squircle(b.x, b.x, b.w, b.r).cg
    if b.shadow {
        ctx.saveGState()
        // The context is flipped; CG's shadow offset is not.
        ctx.setShadow(offset: CGSize(width: 0, height: -b.shadowDy), blur: b.shadowBlur, color: C(0x000000, 0.30).cg)
        ctx.setFillColor(BODY_BOTTOM.cg); ctx.addPath(path); ctx.fillPath()
        ctx.restoreGState()
    }
    ctx.saveGState()
    ctx.addPath(path); ctx.clip()
    let g = CGGradient(colorsSpace: sRGB, colors: [BODY_TOP.cg, BODY_BOTTOM.cg] as CFArray, locations: [0, 1])!
    ctx.drawLinearGradient(g, start: CGPoint(x: 0, y: b.x), end: CGPoint(x: 0, y: b.x + b.w), options: [])
    if b.rim {
        ctx.addPath(path); ctx.setLineWidth(2 * b.rimW)
        ctx.replacePathWithStrokedPath(); ctx.clip()
        let cols = [C(0xFFFFFF, RIM_TOP).cg, C(0xFFFFFF, RIM).cg, C(0xFFFFFF, RIM).cg] as CFArray
        let rim = CGGradient(colorsSpace: sRGB, colors: cols, locations: [0, 0.5, 1])!
        ctx.drawLinearGradient(rim, start: CGPoint(x: 0, y: b.x), end: CGPoint(x: 0, y: b.x + b.w), options: [])
    }
    ctx.restoreGState()
}

func iconPlace(size: Int, full: Bool) -> FillPlace {
    let s = Double(size)
    return fillPlace(cx: s / 2, cy: s / 2, u: full ? 16 * s / 512 : s * M.uPer1024 / 1024)
}

func appIcon(_ k: Kind, size: Int, full: Bool = false) -> CGImage {
    let ctx = context(size, size)
    drawBody(ctx, body(size: size, full: full))
    let caret = GRAPHITE.accent(k)
    if !full && size == 16 {
        for (i, row) in pix16(k).enumerated() {
            for (j, ch) in row.enumerated() where ch != "." {
                ctx.setFillColor((ch == "C" ? caret : WAVE_INK).cg)
                ctx.fill(CGRect(x: j, y: 4 + i, width: 1, height: 1))
            }
        }
    } else if !full && size == 32 {
        drawStroked(ctx, PLACE32, k, wave: WAVE_INK, caret: caret)
    } else {
        let fp = iconPlace(size: size, full: full)
        fill(ctx, fp.wave(k), WAVE_INK)
        fill(ctx, fp.caret, caret)
    }
    return ctx.makeImage()!
}

// MARK: - FunkGui's rasteriser
// canvas/SoftRaster.cpp's rrect, segment and area (fs_ui.sc's, which the GPU runs), mirrored: each primitive's
// coverage (1 - smoothstep(-aa, aa, d), aa one device px), its quad (the AA apron, 1.5 px; an AREA's x range
// half-open, by the sample's centre), source-over blending on gamma-encoded values, ss x ss samples a device px,
// box-filtered and rounded once. For the top bar's tile it gives gui.dump's PNG pixels, byte for byte (ss 2).
enum FGPrim {
    case rrect(x: Double, y: Double, w: Double, h: Double, r: Double, col: C)
    case segment(x0: Double, y0: Double, x1: Double, y1: Double, w: Double, col: C)
    case area(x0: Double, x1: Double, t0: Double, t1: Double, b0: Double, b1: Double, col: C)
}
func fgSmooth(_ e0: Double, _ e1: Double, _ x: Double) -> Double {
    let t = min(max((x - e0) / max(e1 - e0, 1e-9), 0), 1)
    return t * t * (3 - 2 * t)
}
// Coverage of a primitive at the logical point (x, y), aa = one device px in logical px.
func fgCoverage(_ p: FGPrim, _ x: Double, _ y: Double, _ aa: Double) -> Double {
    switch p {
    case .rrect(let rx, let ry, let w, let h, let r0, _):
        let bx = w / 2, by = h / 2, lx = x - (rx + bx), ly = y - (ry + by)
        let r = min(r0, min(bx, by))
        let qx = abs(lx) - bx + r, qy = abs(ly) - by + r
        let d = hypot(max(qx, 0), max(qy, 0)) + min(max(qx, qy), 0) - r
        if abs(lx) > bx + 1.5 || abs(ly) > by + 1.5 { return 0 }
        return 1 - fgSmooth(-aa, aa, d)
    case .segment(let ax, let ay, let bx, let by, let w, _):
        let r = w / 2
        if x < min(ax, bx) - r - 1.5 || x >= max(ax, bx) + r + 1.5 || y < min(ay, by) - r - 1.5
            || y >= max(ay, by) + r + 1.5 { return 0 }
        let dx = bx - ax, dy = by - ay
        let t = min(max(((x - ax) * dx + (y - ay) * dy) / max(dx * dx + dy * dy, 1e-6), 0), 1)
        let d = hypot(x - ax - dx * t, y - ay - dy * t) - r
        return 1 - fgSmooth(-aa, aa, d)
    case .area(let x0, let x1, let t0, let t1, let b0, let b1, _):
        if x < x0 || x >= x1 { return 0 }
        let top = min(t0, t1, b0, b1) - 1.5, bot = max(t0, t1, b0, b1) + 1.5
        if y < top || y >= bot { return 0 }
        let t = (x - x0) / (x1 - x0)
        let yT = t0 + (t1 - t0) * t, yB = b0 + (b1 - b0) * t
        let sT = (t1 - t0) / (x1 - x0), sB = (b1 - b0) / (x1 - x0)
        let dT = (yT - y) / (1 + sT * sT).squareRoot(), dB = (y - yB) / (1 + sB * sB).squareRoot()
        return 1 - fgSmooth(-aa, aa, max(dT, dB))
    }
}
func fgColour(_ p: FGPrim) -> C {
    switch p {
    case .rrect(_, _, _, _, _, let c), .segment(_, _, _, _, _, let c), .area(_, _, _, _, _, _, let c): return c
    }
}
// w x h logical px at dpi, on `ground`, ss x ss samples a device px.
func fgRender(_ prims: [FGPrim], w: Double, h: Double, dpi: Double, ground: C, ss: Int = 2) -> CGImage {
    let W = Int((w * dpi).rounded()), H = Int((h * dpi).rounded())
    var buf = [UInt8](repeating: 0, count: W * H * 4)
    let aa = 1 / dpi
    for j in 0..<H {
        for i in 0..<W {
            var acc = (0.0, 0.0, 0.0)
            for sj in 0..<ss {
                for si in 0..<ss {
                    let x = (Double(i) + (Double(si) + 0.5) / Double(ss)) / dpi
                    let y = (Double(j) + (Double(sj) + 0.5) / Double(ss)) / dpi
                    var c = (ground.r, ground.g, ground.b)
                    for p in prims {
                        let col = fgColour(p)
                        let a = fgCoverage(p, x, y, aa) * col.a
                        if a <= 0 { continue }
                        c = (col.r * a + c.0 * (1 - a), col.g * a + c.1 * (1 - a), col.b * a + c.2 * (1 - a))
                    }
                    acc = (acc.0 + c.0, acc.1 + c.1, acc.2 + c.2)
                }
            }
            let n = Double(ss * ss), o = (j * W + i) * 4
            buf[o] = UInt8((acc.0 / n * 255).rounded()); buf[o + 1] = UInt8((acc.1 / n * 255).rounded())
            buf[o + 2] = UInt8((acc.2 / n * 255).rounded()); buf[o + 3] = 255
        }
    }
    let ctx = CGContext(data: &buf, width: W, height: H, bitsPerComponent: 8, bytesPerRow: W * 4, space: sRGB,
                        bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
    return ctx.makeImage()!
}

// The top bar's tile as the editor draws it (the primitives Panel.cpp makes from LogoMark.h, in FunkGui's pixels), at
// a scale (1 or 2), on the theme's card.
func topBarTile(_ k: Kind, _ th: Theme, scale: Int) -> CGImage {
    let acc = th.accent(k)
    var prims: [FGPrim] = [.rrect(x: 0, y: 0, w: TILE, h: TILE, r: TILE_RADIUS,
                                  col: acc.alpha(Double(Int(TILE_ALPHA * 255)) / 255))]
    prims += (scale >= 2 ? TOPBAR2X : TOPBAR1X).prims(k, acc, x: 0, y: 0)
    return fgRender(prims, w: TILE, h: TILE, dpi: Double(scale), ground: th.card)
}

// MARK: - text

var MONO: CTFont? = nil
func fontURL() -> URL {
    if let f = ARGS["--font"] { return URL(fileURLWithPath: f, relativeTo: CWD) }
    let fm = FileManager.default
    for d in ((try? fm.contentsOfDirectory(atPath: ROOT.path)) ?? []).sorted() where d.hasPrefix("build-") {
        let u = ROOT.appendingPathComponent(d)
            .appendingPathComponent("_deps/funkgui-src/fonts/upstream/JetBrainsMono-Regular.ttf")
        if fm.fileExists(atPath: u.path) { return u }
    }
    fail("no JetBrains Mono: build once (FunkGui's font lands in build-*/_deps) or pass --font <ttf>")
}
func font(_ size: Double) -> CTFont {
    if MONO == nil {
        guard let ds = CTFontManagerCreateFontDescriptorsFromURL(fontURL() as CFURL) as? [CTFontDescriptor],
              let d = ds.first else { fail("cannot read the font") }
        MONO = CTFontCreateWithFontDescriptor(d, 12, nil)
    }
    return CTFontCreateCopyWithAttributes(MONO!, size, nil, nil)
}
func textLine(_ s: String, _ size: Double, _ col: C, tracking: Double = 0) -> CTLine {
    let attrs: [NSAttributedString.Key: Any] = [.init(kCTFontAttributeName as String): font(size),
                                                .init(kCTForegroundColorAttributeName as String): col.cg,
                                                .init(kCTKernAttributeName as String): tracking]
    return CTLineCreateWithAttributedString(NSAttributedString(string: s, attributes: attrs))
}
// A line's glyphs' ink, from its origin on the baseline (y up).
func inkBounds(_ line: CTLine) -> CGRect { CTLineGetBoundsWithOptions(line, .useGlyphPathBounds) }
// Draws a line of text with its baseline at y; `inkLeft` puts its first glyph's ink (not its origin) at x.
func text(_ ctx: CGContext, _ s: String, _ x: Double, _ baseline: Double, _ size: Double, _ col: C,
          tracking: Double = 0, inkLeft: Bool = false) {
    let line = textLine(s, size, col, tracking: tracking)
    let dx = inkLeft ? inkBounds(line).minX : 0
    ctx.saveGState()
    ctx.textMatrix = CGAffineTransform(scaleX: 1, y: -1)
    ctx.textPosition = CGPoint(x: x - dx, y: baseline)
    CTLineDraw(line, ctx)
    ctx.restoreGState()
}

// MARK: - SVG

func svgDoc(_ w: Double, _ h: Double, _ body: String, viewBox: String? = nil) -> String {
    let vb = viewBox ?? "0 0 \(num(w)) \(num(h))"
    return "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"\(num(w))\" height=\"\(num(h))\" viewBox=\"\(vb)\">\n"
        + body + "</svg>\n"
}

// The mark alone, in units (the viewBox its ink box), at its ideal numbers (the floor's edge 3.88u, 1.84u thick).
func markSVG(_ k: Kind, wave: String, caret: String) -> String {
    let fp = FillPlace(ox: 0, oy: 0, u: 1, caretW: M.stroke, floorEdge: M.floorEdge,
                       floorThick: M.floorWeight * M.stroke)
    let b = M.box
    let paths = "  <path d=\"\(fp.wave(k).svg)\" fill=\"\(wave)\"/>\n"
        + "  <path d=\"\(fp.caret.svg)\" fill=\"\(caret)\"/>\n"
    return svgDoc(22 * 24, 14 * 24, paths, viewBox: "\(num(b.x0)) \(num(b.y0)) \(num(b.x1 - b.x0)) \(num(b.y1 - b.y0))")
}

// The app icon at 1024, as installer-1024.png draws it (the same numbers; the shadow a Gaussian of half CG's blur).
func iconSVG(_ k: Kind) -> String {
    let b = body(size: 1024, full: false), fp = iconPlace(size: 1024, full: false)
    let d = squircle(b.x, b.x, b.w, b.r).svg
    let y0 = num(b.x), y1 = num(b.x + b.w)
    let s = """
      <defs>
        <linearGradient id="body" x1="0" y1="\(y0)" x2="0" y2="\(y1)" gradientUnits="userSpaceOnUse">
          <stop offset="0" stop-color="\(BODY_TOP.css)"/>
          <stop offset="1" stop-color="\(BODY_BOTTOM.css)"/>
        </linearGradient>
        <linearGradient id="rim" x1="0" y1="\(y0)" x2="0" y2="\(y1)" gradientUnits="userSpaceOnUse">
          <stop offset="0" stop-color="#FFFFFF" stop-opacity="\(num(RIM_TOP))"/>
          <stop offset="0.5" stop-color="#FFFFFF" stop-opacity="\(num(RIM))"/>
          <stop offset="1" stop-color="#FFFFFF" stop-opacity="\(num(RIM))"/>
        </linearGradient>
        <filter id="shadow" x="-20%" y="-20%" width="140%" height="140%">
          <feGaussianBlur in="SourceAlpha" stdDeviation="\(num(b.shadowBlur / 2))"/>
          <feOffset dy="\(num(b.shadowDy))"/>
          <feComponentTransfer><feFuncA type="linear" slope="0.3"/></feComponentTransfer>
        </filter>
        <clipPath id="clip"><path d="\(d)"/></clipPath>
      </defs>
      <path d="\(d)" fill="#000000" filter="url(#shadow)"/>
      <path d="\(d)" fill="url(#body)"/>
      <path d="\(d)" fill="none" stroke="url(#rim)" stroke-width="\(num(2 * b.rimW))" clip-path="url(#clip)"/>
      <path d="\(fp.wave(k).svg)" fill="\(WAVE_INK.css)"/>
      <path d="\(fp.caret.svg)" fill="\(GRAPHITE.accent(k).css)"/>

    """
    return svgDoc(1024, 1024, s)
}

// MARK: - lockups (the README)

// A card of the theme's ground, 1360 x 400 with 24 px corners: the mark at u 8 (stroke 16, ink 176 x 112) with its
// baseline on the card's middle, beside two lines of JetBrains Mono, the name at 64 px and a line under it at 28 px
// in ink52, 5u of air between the caret's ink and the text's; the group centred across the card.
func lockup(_ k: Kind, _ th: Theme, name: String, line: String) -> CGImage {
    let W = 1360.0, H = 400.0, u = 8.0, air = 5 * u
    let ctx = context(Int(W), Int(H))
    let card = CGPath(roundedRect: CGRect(x: 0, y: 0, width: W, height: H), cornerWidth: 24, cornerHeight: 24,
                      transform: nil)
    ctx.setFillColor(th.ground.cg); ctx.addPath(card); ctx.fillPath()
    let textW = max(inkBounds(textLine(name, 64, th.ink)).width, inkBounds(textLine(line, 28, th.ink52)).width)
    let left = ((W - (M.box.x1 - M.box.x0) * u - air - textW) / 2).rounded()     // the ink box's left
    let fp = fillPlace(cx: left + (10 + 1 + M.optical) * u, cy: H / 2, u: u)
    fill(ctx, fp.wave(k), th.ink)
    fill(ctx, fp.caret, th.accent(k))
    let x = fp.ox + (M.caretX + 1) * u + air
    text(ctx, name, x, 196, 64, th.ink, inkLeft: true)
    text(ctx, line, x, 252, 28, th.ink52, inkLeft: true)
    return ctx.makeImage()!
}

// The README's header: the same card with both marks, the effect's then the instrument's (one construction, the
// trough and the caret's colour apart), 8u between the first's caret and the second's wave (four times the caret's
// own gap, so they read as two), 6u before the text.
func familyLockup(_ th: Theme, name: String, line: String) -> CGImage {
    let W = 1360.0, H = 400.0, u = 8.0, gap = 8 * u, air = 6 * u, ink = (M.box.x1 - M.box.x0) * u
    let ctx = context(Int(W), Int(H))
    let card = CGPath(roundedRect: CGRect(x: 0, y: 0, width: W, height: H), cornerWidth: 24, cornerHeight: 24,
                      transform: nil)
    ctx.setFillColor(th.ground.cg); ctx.addPath(card); ctx.fillPath()
    let textW = max(inkBounds(textLine(name, 64, th.ink)).width, inkBounds(textLine(line, 28, th.ink52)).width)
    let left = ((W - 2 * ink - gap - air - textW) / 2).rounded()                 // the first ink box's left
    var right = 0.0
    for (i, k) in [Kind.effect, .instrument].enumerated() {
        let fp = fillPlace(cx: left + Double(i) * (ink + gap) + (10 + 1 + M.optical) * u, cy: H / 2, u: u)
        fill(ctx, fp.wave(k), th.ink)
        fill(ctx, fp.caret, th.accent(k))
        right = fp.ox + (M.caretX + 1) * u
    }
    text(ctx, name, right + air, 196, 64, th.ink, inkLeft: true)
    text(ctx, line, right + air, 252, 28, th.ink52, inkLeft: true)
    return ctx.makeImage()!
}

// MARK: - the construction sheet

func constructionSheet() -> CGImage {
    let W = 1600, H = 1480, g = 52.0
    let th = GRAPHITE
    let ctx = context(W, H)
    ctx.setFillColor(th.ground.cg); ctx.fill(CGRect(x: 0, y: 0, width: W, height: H))
    let X0 = 200.0, Y0 = 500.0                     // unit (0, 0)
    func px(_ p: P2) -> P2 { P2(X0 + p.x * g, Y0 + p.y * g) }
    let grid = th.ink.alpha(0.07), gridBold = th.ink.alpha(0.16), note = th.ink52, accent = th.effectAccent
    let blue = th.instrumentAccent
    // the grid: every unit; the baseline and the crossings' verticals bolder
    ctx.setLineWidth(1)
    for i in -3...24 {
        let x = X0 + Double(i) * g + 0.5
        ctx.setStrokeColor((i % 8 == 0 ? gridBold : grid).cg)
        ctx.move(to: CGPoint(x: x, y: Y0 - 8 * g)); ctx.addLine(to: CGPoint(x: x, y: Y0 + 8 * g))
        ctx.strokePath()
    }
    for j in -8...8 {
        let y = Y0 + Double(j) * g + 0.5
        ctx.setStrokeColor((j == 0 ? gridBold : grid).cg)
        ctx.move(to: CGPoint(x: X0 - 3 * g, y: y)); ctx.addLine(to: CGPoint(x: X0 + 24 * g, y: y))
        ctx.strokePath()
    }
    func dashed(_ p: Path2, _ c: C, _ w: Double = 1.5, _ dash: [CGFloat] = [6, 5]) {
        ctx.saveGState(); ctx.setLineDash(phase: 0, lengths: dash); ctx.setLineWidth(w); ctx.setStrokeColor(c.cg)
        ctx.addPath(p.cg); ctx.strokePath(); ctx.restoreGState()
    }
    func line(_ pts: [P2], _ c: C, _ w: Double = 1.5) { strokePoints(ctx, pts.map(px), w, c, cap: .butt) }
    func dot(_ p: P2, _ c: C, _ r: Double = 5) {
        let q = px(p)
        ctx.setFillColor(c.cg); ctx.fillEllipse(in: CGRect(x: q.x - r, y: q.y - r, width: 2 * r, height: 2 * r))
    }
    func circle(_ c: P2, _ r: Double) -> Path2 {
        var p = Path2(P2(c.x + r, c.y)); p.arc(c, r, 0, 2 * PI); return p.mapped(ox: X0, oy: Y0, s: g)
    }
    func label(_ s: String, _ p: P2, _ c: C, dx: Double = 0, dy: Double = 0, size: Double = 17) {
        let q = px(p); text(ctx, s, q.x + dx, q.y + dy, size, c)
    }
    let th0 = M.theta
    // the effect, filled at its ideal numbers; the caret
    let fp = FillPlace(ox: X0, oy: Y0, u: g, caretW: M.stroke * g, floorEdge: M.floorEdge,
                       floorThick: M.floorWeight * M.stroke)
    fill(ctx, fp.wave(.effect), th.ink.alpha(0.18))
    fill(ctx, fp.caret, accent.alpha(0.28))
    // the instrument's trough, dashed: the crest turned over
    var trough = Path2(P2(8, 0)); trough.arc(M.troughCentre, M.radius, PI / 2 + th0, PI / 2 - th0)
    dashed(trough.mapped(ox: X0, oy: Y0, s: g), blue, 2)
    // the centrelines
    let effect = centreline(.effect, D: M.floorEdge - 1, f: M.fillet, rise: M.rise)
    strokePoints(ctx, effect.sampled(step: PI / 90).map(px), 2, accent)
    line([P2(M.caretX, -M.caretHalf), P2(M.caretX, M.caretHalf)], accent, 2)
    // the crest's circle, its centre and radii; the 3-4-5 triangle
    dashed(circle(M.crestCentre, M.radius), note.alpha(0.7), 1)
    for p in [P2(0, 0), P2(8, 0), P2(4, -3)] { line([M.crestCentre, p], note, 1) }
    line([P2(4, -3), P2(8, -3), P2(8, 0)], th.ink.alpha(0.6))
    line([P2(4, -3), P2(8, 0)], th.ink.alpha(0.6))
    dot(M.crestCentre, th.ink); dot(M.troughCentre, blue)
    for p in [P2(4, -3), P2(0, 0), P2(8, 0), P2(16, 0)] { dot(p, th.ink, 4) }
    // theta at (8,0): from the baseline to the tangent
    var ang = Path2(P2(8 + 1.6, 0)); ang.arc(P2(8, 0), 1.6, 0, th0)
    strokePoints(ctx, ang.mapped(ox: X0, oy: Y0, s: g).sampled(step: PI / 90), 1.5, th.ink)
    // the fillets (centreline circles), the floor's outer edge (solid) and the round trough's (dashed)
    let D = M.floorEdge - 1, t = M.fillet * tan(th0 / 2)
    for c in [P2(8 + D / tan(th0) + t, D - M.fillet), P2(16 - D / tan(th0) - t, D - M.fillet)] {
        dashed(circle(c, M.fillet), note.alpha(0.8), 1, [3, 3]); dot(c, note, 3.5)
    }
    line([P2(8.6, M.floorEdge), P2(15.4, M.floorEdge)], th.ink, 1)
    var t4 = Path2(px(P2(10.5, 4))); t4.line(px(P2(13.5, 4))); dashed(t4, blue, 1.5)
    // the rise past the baseline, and the gap to the caret
    let end = P2(16 + M.rise / tan(th0), -M.rise)
    dot(end, accent, 4)
    let capX = end.x + 1                           // the effect's cap's ink, at its right (17.22; the synth's 17)
    line([P2(capX, 1.4), P2(19, 1.4)], th.ink, 1)
    line([P2(capX, 1.1), P2(capX, 1.7)], th.ink, 1); line([P2(19, 1.1), P2(19, 1.7)], th.ink, 1)
    // the ink box
    var bx = Path2(px(P2(M.box.x0, M.box.y0)), closed: true)
    bx.line(px(P2(M.box.x1, M.box.y0))); bx.line(px(P2(M.box.x1, M.box.y1))); bx.line(px(P2(M.box.x0, M.box.y1)))
    dashed(bx, note.alpha(0.8), 1, [2, 4])

    text(ctx, "CARET  ·  CONSTRUCTION", 60, 60, 16, note, tracking: 1.5)
    text(ctx, "mark units u: one square = 1u; baseline y = 0, y down; stroke 2u, round caps and joins", 60, 92, 17,
         th.ink)
    label("crest (4, -3)", P2(4, -3), th.ink, dx: -58, dy: -16)
    label("4", P2(6, -3), th.ink, dx: -5, dy: -10); label("3", P2(8, -1.5), th.ink, dx: 10, dy: 6)
    label("5", P2(6, -1.5), th.ink, dx: -26, dy: 2)
    label("C1 (4, 7/6)", M.crestCentre, th.ink, dx: 12, dy: 26)
    label("r = 25/6", P2(1.6, 0.5), note, dx: -40, dy: 26)
    label("θ = 73.74°", P2(8, 0), th.ink, dx: 96, dy: 34)
    label("(0,0)", P2(0, 0), note, dx: -62, dy: -10); label("(8,0)", P2(8, 0), note, dx: -62, dy: -12)
    label("(16,0)", P2(16, 0), note, dx: 58, dy: 44)
    label("C2 (12, -7/6)", M.troughCentre, blue, dx: 12, dy: -10)
    label("+0.75u", end, accent, dx: 14, dy: -6)
    label(num(19 - capX) + "u", P2(18, 1.4), th.ink, dx: -26, dy: 28)
    label("caret x 20", P2(20, -7), accent, dx: 34, dy: 16)
    label("y -6 to 6", P2(20, -7), accent, dx: 34, dy: 40)
    label("ink box 22u x 14u", P2(-1, 7), note, dx: 0, dy: 30)
    let notes = [
        ("WAVE", "two arcs of chord 8 and sagitta 3: r = (4² + 3²) / (2·3) = 25/6, a 3-4-5 triangle under each"),
        ("", "half; tangent where they meet, at the slope θ = 2·atan(3/4) = 73.74° at every crossing"),
        ("SYNTH", "the second half is the crest turned over (C2, dashed): a pure cycle"),
        ("EFFECT", "the second half processed: flanks on the arc's tangent (no kink at (8,0)), fillets 1.5u"),
        ("", "(centreline; tangent 1.5 · 3/4 = 1.125u), a flat floor; its last flank runs on 0.75u past the baseline"),
        ("", "(+0.75u), so it reads as long as the first, which the crest's arc carries on"),
        ("FLOOR", "outer edge 3.88u (solid), not the round trough's 4.00u (dashed), so the two read as deep; 92% of"),
        ("", "the stroke thick (1.84u, thinned inside), as a horizontal reads heavier; 4.00u when u < 6 (128 px and"),
        ("", "below), its edge on a pixel"),
        ("CARET", "x 20, y -6 to 6 (ink -7 to 7): one stroke (2u) from the synth's cap (ink x 17), 1.78u from the"),
        ("", "effect's, which the rise carries right; the ink box 22u x 14u (dotted) sits 0.5u left of its ground's"),
        ("", "centre, as the caret's solid stem weighs the right"),
    ]
    var y = 960.0
    for (head, body) in notes {
        if !head.isEmpty { text(ctx, head, 60, y, 15, note, tracking: 1) }
        text(ctx, body, 160, y, 16, th.ink)
        y += 24
    }
    y += 16
    text(ctx, "APP ICON", 60, y, 15, note, tracking: 1)
    text(ctx, "u = size · 26/1024; the crest's top, the trough's bottom, the floor and the caret's stem on whole "
         + "pixels:", 160, y, 16, th.ink)
    y += 24
    var cells: [String] = []
    for s in [1024, 512, 256, 128, 64] {
        let p = iconPlace(size: s, full: false)
        let floor = "\(num(p.oy + p.floorEdge * p.u))/\(num(p.floorThick * p.u))"
        cells.append("\(s): u \(num(p.u)), stem \(num(p.caretW)), crest \(num(p.oy - 4 * p.u)), "
                     + "trough \(num(p.oy + 4 * p.u)), floor \(floor)")
    }
    text(ctx, cells[0] + "   " + cells[1], 160, y, 14, note)
    text(ctx, cells[2] + "   " + cells[3], 160, y + 20, 14, note)
    text(ctx, cells[4] + "   (px: the crest's top, the trough's bottom, the floor's edge/thickness)", 160, y + 40, 14,
         note)
    y += 80
    text(ctx, "TOP BAR", 60, y, 15, note, tracking: 1)
    text(ctx, "the 26 px tile, in the accent alone: the wave AREA columns, the caret one primitive (LogoMark.h):",
         160, y, 16, th.ink)
    text(ctx, "2x: u 0.75, stroke 1.5 (3 device px), the crest's top 10.0, the trough's bottom 16.0, the stem "
         + "device px 39-42", 160, y + 24, 14, note)
    text(ctx, "1x: one 2 px stroke (x unit 0.75, y unit 2/3): the crest rows 10-11, the trough rows 14-15, the "
         + "caret columns 20-21, rows 8-17", 160, y + 44, 14, note)
    return ctx.makeImage()!
}

// MARK: - the review sheet

func reviewSheet(_ names: [Kind: String]) -> CGImage {
    let W = 1600, H = 1280
    let ctx = context(W, H)
    for (i, th) in [PAPER, GRAPHITE].enumerated() {
        let y0 = Double(i) * Double(H) / 2
        ctx.setFillColor(th.ground.cg); ctx.fill(CGRect(x: 0, y: y0, width: Double(W), height: Double(H) / 2))
        text(ctx, "CARET  ·  \(th.name.uppercased())", 40, y0 + 44, 15, th.ink52, tracking: 1.5)
        text(ctx, "the top bar's tiles in FunkGui's own pixels (its rasteriser, mirrored: gui.dump's)", 830, y0 + 44,
             14, th.ink52)
        for (j, k) in Kind.allCases.enumerated() {
            let x0 = 40.0 + Double(j) * 790
            text(ctx, names[k] ?? k.rawValue, x0, y0 + 84, 17, th.ink)
            var x = x0
            for size in [256, 128, 64, 32, 16] {
                let s = Double(size)
                put(ctx, appIcon(k, size: size), CGRect(x: x, y: y0 + 104 + (256 - s) / 2, width: s, height: s))
                x += s + 18
            }
            let big = { (img: CGImage, x: Double, y: Double, w: Double) in
                put(ctx, img, CGRect(x: x, y: y, width: w, height: w), nearest: true)
            }
            big(enlarge(appIcon(k, size: 32), 4), x0, y0 + 384, 128)
            big(enlarge(appIcon(k, size: 16), 8), x0 + 146, y0 + 384, 128)
            text(ctx, "32, x4", x0, y0 + 532, 14, th.ink52)
            text(ctx, "16, x8", x0 + 146, y0 + 532, 14, th.ink52)
            // the top bar's card as on a Retina screen (drawn at 2x), the name and kind beside the tile
            let bx = x0 + 300, by = y0 + 384
            let bar = CGPath(roundedRect: CGRect(x: bx, y: by, width: 440, height: 80), cornerWidth: 24,
                             cornerHeight: 24, transform: nil)
            ctx.setFillColor(th.card.cg); ctx.addPath(bar); ctx.fillPath()
            put(ctx, topBarTile(k, th, scale: 2), CGRect(x: bx + 20, y: by + 14, width: 52, height: 52))
            text(ctx, names[k] ?? k.rawValue, bx + 92, by + 37, 24, th.ink)
            text(ctx, k == .effect ? "Effect" : "Instrument", bx + 92, by + 63, 20, th.ink52)
            // the tile at 2x (x2) and at 1x (x4 and actual size)
            big(enlarge(topBarTile(k, th, scale: 2), 2), bx, by + 96, 104)
            big(enlarge(topBarTile(k, th, scale: 1), 4), bx + 124, by + 96, 104)
            put(ctx, topBarTile(k, th, scale: 1), CGRect(x: bx + 248, y: by + 135, width: 26, height: 26))
            text(ctx, "2x, x2", bx, by + 220, 14, th.ink52)
            text(ctx, "1x, x4", bx + 124, by + 220, 14, th.ink52)
            text(ctx, "1x", bx + 248, by + 220, 14, th.ink52)
            put(ctx, appIcon(k, size: 512, full: true), CGRect(x: bx + 330, y: by + 96, width: 104, height: 104))
            text(ctx, "extension", bx + 330, by + 220, 14, th.ink52)
        }
    }
    return ctx.makeImage()!
}

// MARK: - the editor's header

func header() -> String {
    func f(_ x: Double) -> String { (num(x).contains(".") ? num(x) : num(x) + ".0") + "f" }
    func floats(_ v: [Double]) -> String {
        var lines: [String] = [], cur = ""
        for (i, x) in v.enumerated() {
            let s = f(x) + (i + 1 < v.count ? "," : "")
            if !cur.isEmpty && 12 + cur.count + 1 + s.count > 120 { lines.append(cur); cur = "" }
            cur += (cur.isEmpty ? "" : " ") + s
        }
        lines.append(cur)
        return lines.map { "            " + $0 }.joined(separator: "\n")
    }
    var arrays = "", marks = "", maxN = 0
    for (k, kName) in [(Kind.effect, "Effect"), (.instrument, "Instrument")] {
        for (sp, sName) in [(TOPBAR1X, "1x"), (TOPBAR2X, "2x")] {
            let id = "k\(kName)\(sName)"
            let st = sp.strip(k)
            maxN = max(maxN, st.xs.count)
            for (part, v) in [("Xs", st.xs), ("Tops", st.tops), ("Bottoms", st.bottoms)] {
                arrays += "        inline constexpr float \(id)\(part)[] = {\n\(floats(v))\n        };\n"
            }
            let (x0, y0, x1, y1) = sp.caretBox
            marks += "    inline constexpr Mark \(id){\n"
                + "        { detail::\(id)Xs, detail::\(id)Tops, detail::\(id)Bottoms, \(st.xs.count) },\n"
                + "        { \(f(x0)), \(f(y0)), \(f(x1)), \(f(y1)), \(sp.caretRound) } };\n"
        }
    }
    let p1 = TOPBAR1X, p2 = TOPBAR2X
    let c1 = p1.caretBox, c2 = p2.caretBox
    let s2 = num(p2.stroke), w2 = num(p2.caretW), x2 = num(p2.caretX), d0 = num(c2.0 * 2), d1 = num(c2.2 * 2)
    let s1 = num(p1.stroke), cA = num(c1.0), cB = num(c1.2 - 1), rA = num(c1.1), rB = num(c1.3 - 1)
    return """
    // Source/editor/LogoMark.h: GENERATED by Tools/logo/logo.swift (swift Tools/logo/logo.swift): do not edit;
    // change the script and run it again. The top bar's mark, "~|" (a wave written up to the editor's caret), in its
    // 26 px tile, in the theme's accent alone, px from the tile's top-left. The wave is Canvas::areaStrip's columns
    // (half a device px wide) between its stroke's top and bottom edges, one primitive over each sample, and the caret
    // one primitive: a capsule (Canvas::segment) or a rectangle (Canvas::rrect). FunkGui antialiases each edge over a
    // device px each side, so an edge on a whole device px shows about 16% outside it and 84% inside, its crispest.
    // One drawing a backing scale, hinted for it; mark() picks by Canvas::dpi(), which is the backing scale times the
    // editor's zoom, so the hints hold at a zoom of 100% and 200%; at 125% and 150% the 2x drawing is antialiased
    // where it lands:
    //   2x  u 0.75, stroke \(s2) (3 device px): the crest's top on 10.0 and the trough's bottom on 16.0 (device px 20
    //       and 32); the caret a capsule \(w2) wide at x \(x2) (its stem device px \(d0)-\(d1))
    //   1x  one stroke, \(s1) px, for the wave and the caret: the crest's centreline on 11 and the trough's on 15
    //       (rows 10-11 and 14-15); the caret on columns \(cA)-\(cB), rows \(rA)-\(rB), square ends; the
    //       effect's floor 4.7 px long, sharp-cornered, the instrument's trough round
    // The effect's trough is processed (flat-floored, its last flank on past the baseline), the instrument's the crest
    // turned over (logo.swift has the construction). gui.dump's MARK checks the numbers and the pixels.
    #pragma once

    namespace playground::ui::logo
    {
        // The tile under the mark: its side, its corner radius and the accent's alpha in it.
        inline constexpr float kTile = \(f(TILE)), kTileRadius = \(f(TILE_RADIUS)), kTileAlpha = \(f(TILE_ALPHA));

        // The wave: Canvas::areaStrip's points, its top and bottom edges at each x (n - 1 columns).
        struct Strip
        {
            const float* xs;                     // px in the tile
            const float* tops;
            const float* bottoms;
            int n;
        };

        // The caret's ink, px in the tile: a capsule (round) or a rectangle.
        struct Stem
        {
            float x0, y0, x1, y1;
            bool round;
        };

        struct Mark
        {
            Strip wave;
            Stem caret;
        };

        inline constexpr int kMaxPoints = \(maxN);         // the most points a Strip has

        namespace detail
        {
    \(arrays)    }

    \(marks)
        // The mark for the product's kind at the canvas's scale (Canvas::dpi()): the 1x drawing at exactly 1 device px
        // a logical px, else the 2x one.
        constexpr const Mark& mark(bool instrument, float dpi)
        {
            if (dpi > 0.99f && dpi < 1.01f)
                return instrument ? kInstrument1x : kEffect1x;
            return instrument ? kInstrument2x : kEffect2x;
        }
    }

    """
}

// MARK: - main

func run(_ cmd: String, _ args: [String]) {
    let p = Process()
    p.executableURL = URL(fileURLWithPath: cmd)
    p.arguments = args
    do { try p.run() } catch { fail("cannot run \(cmd)") }
    p.waitUntilExit()
    if p.terminationStatus != 0 { fail("\(cmd) \(args.joined(separator: " ")) failed") }
}

let products = readProducts()
var names: [Kind: String] = [:]
for p in products where names[p.kind] == nil { names[p.kind] = p.name }
var written: [String] = []
func write(_ img: CGImage, _ rel: String) { savePNG(img, out(rel)); written.append(rel) }
func write(_ s: String, _ rel: String) { saveText(s, out(rel)); written.append(rel) }

for k in Kind.allCases {
    let dir = "Assets/logo/\(k.rawValue)"
    for pt in [16, 32, 128, 256, 512] {
        for scale in [1, 2] {
            write(appIcon(k, size: pt * scale), "\(dir)/icon.iconset/icon_\(pt)x\(pt)\(scale == 2 ? "@2x" : "").png")
        }
    }
    run("/usr/bin/iconutil", ["-c", "icns", out("\(dir)/icon.iconset"), "-o", out("\(dir)/icon.icns")])
    written.append("\(dir)/icon.icns")
    write(appIcon(k, size: 1024), "\(dir)/installer-1024.png")
    write(appIcon(k, size: 512, full: true), "\(dir)/extension-512.png")
    write(iconSVG(k), "\(dir)/icon.svg")
    write(markSVG(k, wave: GRAPHITE.ink.css, caret: GRAPHITE.accent(k).css), "\(dir)/mark-graphite.svg")
    write(markSVG(k, wave: PAPER.ink.css, caret: PAPER.accent(k).css), "\(dir)/mark-paper.svg")
    write(markSVG(k, wave: "currentColor", caret: "currentColor"), "\(dir)/mark-flat.svg")
    if let name = names[k] {
        let line = k == .effect ? "an effect you write while it plays" : "a synth you write while it plays"
        for th in [GRAPHITE, PAPER] { write(lockup(k, th, name: name, line: line), "\(dir)/lockup-\(th.name).png") }
    }
}
for p in products { write(appIcon(p.kind, size: 512, full: true), "Source/mcp/icon-\(p.key).png") }
// The README's header: the repository has the effect's name and holds both, so it shows both marks.
if let name = names[.effect] {
    let line = "an effect and a synth you write while they play"
    for th in [GRAPHITE, PAPER] {
        write(familyLockup(th, name: name, line: line), "Assets/logo/readme-\(th.name).png")
    }
}
write(constructionSheet(), "Assets/logo/construction.png")
write(reviewSheet(names), "Assets/logo/sheet.png")
write(header(), "Source/editor/LogoMark.h")

if let pv = PREVIEW {
    try? FileManager.default.createDirectory(at: pv, withIntermediateDirectories: true)
    func preview(_ img: CGImage, _ name: String) { savePNG(img, pv.appendingPathComponent(name).path) }
    for k in Kind.allCases {
        let n = k.rawValue
        preview(enlarge(appIcon(k, size: 16), 16), "\(n)-16-x16.png")
        preview(enlarge(appIcon(k, size: 32), 8), "\(n)-32-x8.png")
        preview(enlarge(appIcon(k, size: 64), 8), "\(n)-64-x8.png")
        preview(enlarge(appIcon(k, size: 128), 4), "\(n)-128-x4.png")
        for th in [GRAPHITE, PAPER] {
            preview(enlarge(topBarTile(k, th, scale: 1), 12), "\(n)-tile1x-\(th.name)-x12.png")
            preview(enlarge(topBarTile(k, th, scale: 2), 6), "\(n)-tile2x-\(th.name)-x6.png")
        }
    }
}
for w in written { print(w) }
