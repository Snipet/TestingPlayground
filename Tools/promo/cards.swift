// Tools/promo/cards.swift: the promotional videos' text, drawn for scripts/promo.py (this ffmpeg has no text filter).
//
//   swift Tools/promo/cards.swift <spec.json> <out-dir>
//
// It writes, at the spec's size (1080 x 1920):
//   overlay.png  transparent but for the header (the product's mark and name, the title, the subtitle), the two
//                videos' labels, their rounded corners (ground over each rect's corners) and hairlines, and the footer
//   end.png      the end card: opaque ground, the title, "made in" and the product's lockup
// in JetBrains Mono (the plugin's font: FunkGui's fonts/upstream/, the spec's "font") and the theme's colours.
//
//   layout.json  where the compositor puts the videos: panel and code [x, y, w, h] (the panel scaled to fit when it is
//                too tall; code's height the part of the code video shown, from its top; 0 when there is no room)
//
// The spec: w, h, font, mark (a PNG), lockup (a PNG), colours ground/ink/ink70/ink52/ink16/accent as "#rrggbb",
// title, subtitle, product, tagline, footer, the videos' sizes panelSize and codeSize [w, h] and their labels
// panelLabel and codeLabel. Every position is in the output's pixels, from the top-left.
import AppKit
import CoreGraphics
import CoreText
import Foundation

func fail(_ s: String) -> Never {
    FileHandle.standardError.write(("cards: " + s + "\n").data(using: .utf8)!)
    exit(1)
}

let args = CommandLine.arguments
guard args.count == 3 else { fail("usage: swift Tools/promo/cards.swift <spec.json> <out-dir>") }
guard let data = FileManager.default.contents(atPath: args[1]),
      let spec = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else { fail("cannot read \(args[1])") }
let outDir = URL(fileURLWithPath: args[2])

func str(_ k: String) -> String { spec[k] as? String ?? "" }
func num(_ k: String) -> Double { (spec[k] as? NSNumber)?.doubleValue ?? 0 }
func size(_ k: String) -> CGSize {
    guard let a = spec[k] as? [NSNumber], a.count == 2 else { return .zero }
    return CGSize(width: a[0].doubleValue, height: a[1].doubleValue)
}
func colour(_ k: String) -> CGColor {
    let s = str(k).trimmingCharacters(in: CharacterSet(charactersIn: "#"))
    guard s.count == 6, let v = UInt32(s, radix: 16) else { fail("colour \(k) is not #rrggbb") }
    return CGColor(srgbRed: CGFloat((v >> 16) & 255) / 255, green: CGFloat((v >> 8) & 255) / 255,
                   blue: CGFloat(v & 255) / 255, alpha: 1)
}

let W = Int(num("w")), H = Int(num("h"))
guard W > 0, H > 0 else { fail("no size") }

let fontDescriptor: CTFontDescriptor = {
    guard let ds = CTFontManagerCreateFontDescriptorsFromURL(URL(fileURLWithPath: str("font")) as CFURL) as? [CTFontDescriptor],
          let d = ds.first else { fail("cannot read the font \(str("font"))") }
    return d
}()
func font(_ size: Double) -> CTFont { CTFontCreateWithFontDescriptor(fontDescriptor, size, nil) }

func image(_ path: String) -> CGImage {
    guard let src = CGImageSourceCreateWithURL(URL(fileURLWithPath: path) as CFURL, nil),
          let img = CGImageSourceCreateImageAtIndex(src, 0, nil) else { fail("cannot read \(path)") }
    return img
}

// A context whose y runs down from the top, as the spec's positions do.
func canvas() -> CGContext {
    guard let c = CGContext(data: nil, width: W, height: H, bitsPerComponent: 8, bytesPerRow: 0,
                            space: CGColorSpace(name: CGColorSpace.sRGB)!,
                            bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue) else { fail("no context") }
    c.translateBy(x: 0, y: CGFloat(H))
    c.scaleBy(x: 1, y: -1)
    c.setShouldAntialias(true)
    c.setShouldSmoothFonts(false)
    return c
}

func line(_ text: String, _ size: Double, _ col: CGColor, tracking: Double = 0) -> CTLine {
    let attrs: [NSAttributedString.Key: Any] = [
        NSAttributedString.Key(kCTFontAttributeName as String): font(size),
        NSAttributedString.Key(kCTForegroundColorAttributeName as String): col,
        NSAttributedString.Key(kCTKernAttributeName as String): tracking,
    ]
    return CTLineCreateWithAttributedString(NSAttributedString(string: text, attributes: attrs))
}

func width(_ l: CTLine) -> Double { CTLineGetTypographicBounds(l, nil, nil, nil) }

// Words into lines no wider than `maxW` at `size`; the size shrinks (to `minSize`) until it takes at most `maxLines`.
func wrap(_ text: String, size: Double, minSize: Double, maxW: Double, maxLines: Int, _ col: CGColor) -> ([CTLine], Double) {
    var s = size
    while true {
        var lines: [String] = []
        var cur = ""
        for w in text.split(separator: " ").map(String.init) {
            let next = cur.isEmpty ? w : cur + " " + w
            if !cur.isEmpty && width(line(next, s, col)) > maxW {
                lines.append(cur)
                cur = w
            } else {
                cur = next
            }
        }
        if !cur.isEmpty { lines.append(cur) }
        let widest = lines.map { width(line($0, s, col)) }.max() ?? 0
        if (lines.count <= maxLines && widest <= maxW) || s <= minSize {
            return (lines.map { line($0, s, col) }, s)
        }
        s -= 2
    }
}

// Draws `l` with its baseline at y (top-down), from x.
func draw(_ c: CGContext, _ l: CTLine, x: Double, baseline y: Double) {
    c.saveGState()
    c.textMatrix = CGAffineTransform(scaleX: 1, y: -1)
    c.textPosition = CGPoint(x: x, y: y)
    CTLineDraw(l, c)
    c.restoreGState()
}

// The image drawn upright in a top-down context.
func drawImage(_ c: CGContext, _ img: CGImage, in r: CGRect) {
    c.saveGState()
    c.translateBy(x: r.minX, y: r.maxY)
    c.scaleBy(x: 1, y: -1)
    c.interpolationQuality = .high
    c.draw(img, in: CGRect(x: 0, y: 0, width: r.width, height: r.height))
    c.restoreGState()
}

func write(_ c: CGContext, _ name: String) {
    guard let img = c.makeImage() else { fail("no image") }
    let url = outDir.appendingPathComponent(name)
    guard let dst = CGImageDestinationCreateWithURL(url as CFURL, "public.png" as CFString, 1, nil) else { fail("cannot write \(url.path)") }
    CGImageDestinationAddImage(dst, img, nil)
    if !CGImageDestinationFinalize(dst) { fail("cannot write \(url.path)") }
}

let ground = colour("ground"), ink = colour("ink"), ink70 = colour("ink70"), ink52 = colour("ink52")
let ink16 = colour("ink16"), accent = colour("accent")
let margin = 40.0
let radius = 22.0
try? FileManager.default.createDirectory(at: outDir, withIntermediateDirectories: true)

// ---- the layout ---------------------------------------------------------------------------------------------------
// The header: the mark's row, the title (two lines at most), the subtitle (two at most); then the panel under its label,
// the code under its label, and the footer at the bottom. The panel keeps its size unless that leaves the code less
// than kMinCode px; then it shrinks (to 60% at most), and the code takes what is left, from the code video's top.
let titleWrap = wrap(str("title"), size: 76, minSize: 44, maxW: Double(W) - 2 * margin, maxLines: 2, ink)
let subWrap = str("subtitle").isEmpty ? ([CTLine](), 32.0)
    : wrap(str("subtitle"), size: 32, minSize: 24, maxW: Double(W) - 2 * margin, maxLines: 2, ink70)
var headerBottom = 190.0 + titleWrap.1 * 0.78 + Double(titleWrap.0.count - 1) * titleWrap.1 * 1.12
if !subWrap.0.isEmpty { headerBottom += titleWrap.1 * 0.34 + 4 + Double(subWrap.0.count) * subWrap.1 * 1.3 }
let labelRoom = 58.0                       // a label over a video
let footerTop = Double(H) - 200
let kMinCode = 420.0
let panelSrc = size("panelSize"), codeSrc = size("codeSize")
let panelTop = headerBottom + labelRoom + 28    // the header's last line, then air, then the label
var panelW = Double(W) - 2 * margin
var panelH = panelSrc.width > 0 ? panelSrc.height * panelW / panelSrc.width : 0
let codeRoom = { (ph: Double) -> Double in footerTop - (panelTop + ph + labelRoom + 12) }
if codeSrc.height > 0 && codeRoom(panelH) < kMinCode {
    let ph = max(panelH * 0.6, panelH - (kMinCode - codeRoom(panelH)))
    panelW *= ph / panelH
    panelH = ph
}
let panelRect = CGRect(x: (Double(W) - panelW) / 2, y: panelTop, width: panelW, height: panelH)
let codeTop = panelRect.maxY + labelRoom + 12
let codeH = max(0, min(codeSrc.height * (Double(W) - 2 * margin) / max(1, codeSrc.width), footerTop - codeTop))
let codeRect = CGRect(x: margin, y: codeTop, width: Double(W) - 2 * margin, height: codeH >= 160 ? codeH.rounded(.down) : 0)

// ---- overlay.png ----------------------------------------------------------------------------------------------------
do {
    let c = canvas()
    // The header: the mark and the product's name, the title, the subtitle.
    let mark = image(str("mark"))
    let markSize = 56.0
    drawImage(c, mark, in: CGRect(x: margin, y: 84, width: markSize, height: markSize))
    draw(c, line(str("product"), 30, ink70), x: margin + markSize + 18, baseline: 84 + markSize / 2 + 10)
    var y = 190.0 + titleWrap.1 * 0.78
    for l in titleWrap.0 {
        draw(c, l, x: margin, baseline: y)
        y += titleWrap.1 * 1.12
    }
    y += titleWrap.1 * 0.34 - titleWrap.1 * 1.12 + 4 + subWrap.1 * 1.0
    for l in subWrap.0 {
        draw(c, l, x: margin, baseline: y)
        y += subWrap.1 * 1.3
    }

    // Each video's label above it, its corners rounded into the ground and a hairline round it.
    for (r, label) in [(panelRect, str("panelLabel")), (codeRect, str("codeLabel"))] where r.height > 0 {
        if !label.isEmpty {
            draw(c, line(label, 24, ink52, tracking: 1.5), x: r.minX + 4, baseline: r.minY - 18)
        }
        let rounded = CGPath(roundedRect: r, cornerWidth: radius, cornerHeight: radius, transform: nil)
        c.saveGState()
        c.addRect(r.insetBy(dx: -2, dy: -2))
        c.addPath(rounded)
        c.setFillColor(ground)
        c.fillPath(using: .evenOdd)
        c.restoreGState()
        c.addPath(rounded)
        c.setStrokeColor(ink16)
        c.setLineWidth(2)
        c.strokePath()
    }

    // The footer: the tagline in the accent, then the footer's words.
    let footerY = footerTop + 70
    if !str("tagline").isEmpty {
        let t = line(str("tagline"), 30, accent)
        draw(c, t, x: (Double(W) - width(t)) / 2, baseline: footerY)
    }
    if !str("footer").isEmpty {
        let f = line(str("footer"), 26, ink52)
        draw(c, f, x: (Double(W) - width(f)) / 2, baseline: footerY + 50)
    }
    write(c, "overlay.png")
}

// ---- layout.json ----------------------------------------------------------------------------------------------------
do {
    func ints(_ r: CGRect) -> [Int] { [Int(r.minX.rounded()), Int(r.minY.rounded()), Int(r.width.rounded()) & ~1, Int(r.height.rounded()) & ~1] }
    let layout: [String: Any] = ["panel": ints(panelRect), "code": ints(codeRect)]
    guard let json = try? JSONSerialization.data(withJSONObject: layout, options: [.sortedKeys]),
          (try? json.write(to: outDir.appendingPathComponent("layout.json"))) != nil else { fail("cannot write layout.json") }
}

// ---- end.png --------------------------------------------------------------------------------------------------------
do {
    let c = canvas()
    c.setFillColor(ground)
    c.fill(CGRect(x: 0, y: 0, width: W, height: H))
    let (titleLines, titleSize) = titleWrap
    var y = Double(H) * 0.36
    for l in titleLines {
        draw(c, l, x: (Double(W) - width(l)) / 2, baseline: y)
        y += titleSize * 1.12
    }
    let made = line("made in", 30, ink52, tracking: 2)
    draw(c, made, x: (Double(W) - width(made)) / 2, baseline: y + 40)
    let lockup = image(str("lockup"))
    let lw = Double(W) - 2 * margin
    let lh = lw * Double(lockup.height) / Double(lockup.width)
    drawImage(c, lockup, in: CGRect(x: margin, y: y + 70, width: lw, height: lh))
    if !str("footer").isEmpty {
        let f = line(str("footer"), 26, ink52)
        draw(c, f, x: (Double(W) - width(f)) / 2, baseline: y + 70 + lh + 70)
    }
    write(c, "end.png")
}
