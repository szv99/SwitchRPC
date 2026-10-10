//
//  QRCode.swift
//  SwitchRPC
//

/// Small QR code encoder (ISO/IEC 18004) for showing the pairing URL on the
/// console. Single segment in alphanumeric or byte mode, versions 1-10, error
/// correction level L or M.
enum QRCode {
	enum ErrorCorrection: Int {
		case low = 0, medium = 1
	}

	/// Encodes text into a module matrix (`matrix[y][x]`, true = dark), without
	/// the quiet zone. Picks the smallest version, then level M if it still fits,
	/// so the code stays as small as possible on screen. Returns nil if the text
	/// doesn't fit in version 10.
	static func encode(_ text: String) -> [[Bool]]? {
		let bytes = Array(text.utf8)
		// uppercase-only text packs 5.5 bits per character instead of 8
		let alphanumeric = bytes.allSatisfy { alphanumericIndex($0) != nil }

		let dataBits = alphanumeric ? (bytes.count / 2) * 11 + (bytes.count % 2) * 6 : bytes.count * 8
		func fits(_ v: Int, _ ecc: ErrorCorrection) -> Bool {
			4 + countBits(v, alphanumeric) + dataBits <= dataCodewords(v, ecc) * 8
		}
		guard let version = (1...10).first(where: { fits($0, .low) }) else { return nil }
		let ecc: ErrorCorrection = fits(version, .medium) ? .medium : .low

		var bits = BitBuffer()
		bits.append(alphanumeric ? 0b0010 : 0b0100, 4)
		bits.append(bytes.count, countBits(version, alphanumeric))
		if alphanumeric {
			var i = 0
			while i + 1 < bytes.count {
				bits.append(alphanumericIndex(bytes[i])! * 45 + alphanumericIndex(bytes[i + 1])!, 11)
				i += 2
			}
			if i < bytes.count { bits.append(alphanumericIndex(bytes[i])!, 6) }
		} else {
			for byte in bytes { bits.append(Int(byte), 8) }
		}

		// terminator, byte alignment, then alternating pad bytes
		let capacity = dataCodewords(version, ecc) * 8
		bits.append(0, min(4, capacity - bits.count))
		bits.append(0, (8 - bits.count % 8) % 8)
		var pad = 0xEC
		while bits.count < capacity {
			bits.append(pad, 8)
			pad ^= 0xEC ^ 0x11
		}

		var symbol = Symbol(version: version, ecc: ecc)
		symbol.drawCodewords(addErrorCorrection(bits.bytes(), version, ecc))
		symbol.applyBestMask()
		return symbol.modules
	}

	/// Prints the matrix centered on the console. The spec asks for a 4 module
	/// quiet zone, but phones cope with less if the screen is tight.
	/// One module per console cell: libnx's default font is 16x16, so they're square.
	static func render(_ matrix: [[Bool]], quietZone quiet: Int = 4, consoleWidth: Int = 80) {
		let size = matrix.count
		let indent = String(repeating: " ", count: max(0, (consoleWidth - size - quiet * 2) / 2))

		for y in -quiet..<(size + quiet) {
			var line = indent
			var current = true  // rows start in the light quiet zone, so the first cell always sets the color
			for x in -quiet..<(size + quiet) {
				let dark = y >= 0 && y < size && x >= 0 && x < size && matrix[y][x]
				if dark != current {
					line += dark ? "\u{1B}[40m" : "\u{1B}[47m"
					current = dark
				}
				line += " "
			}
			print(line + "\u{1B}[0m")
		}
	}

	// MARK: - Capacity

	// [level][version], index 0 unused
	private static let eccCodewordsPerBlock = [
		[0, 7, 10, 15, 20, 26, 18, 20, 24, 30, 18],  // L
		[0, 10, 16, 26, 18, 24, 16, 18, 22, 22, 26],  // M
	]
	private static let errorCorrectionBlocks = [
		[0, 1, 1, 1, 1, 1, 2, 2, 2, 2, 4],  // L
		[0, 1, 1, 1, 2, 2, 4, 4, 4, 5, 5],  // M
	]

	private static let alphanumericCharset = Array("0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ $%*+-./:".utf8)

	private static func alphanumericIndex(_ c: UInt8) -> Int? {
		alphanumericCharset.firstIndex(of: c)
	}

	private static func countBits(_ version: Int, _ alphanumeric: Bool) -> Int {
		if alphanumeric { return version < 10 ? 9 : 11 }
		return version < 10 ? 8 : 16
	}

	/// Modules left for data and error correction once the function patterns are placed.
	private static func rawDataModules(_ version: Int) -> Int {
		var result = (16 * version + 128) * version + 64
		if version >= 2 {
			let alignmentCount = version / 7 + 2
			result -= (25 * alignmentCount - 10) * alignmentCount - 55
			if version >= 7 { result -= 36 }
		}
		return result
	}

	private static func dataCodewords(_ version: Int, _ ecc: ErrorCorrection) -> Int {
		rawDataModules(version) / 8
			- eccCodewordsPerBlock[ecc.rawValue][version] * errorCorrectionBlocks[ecc.rawValue][version]
	}

	// MARK: - Reed-Solomon

	/// Splits the data into blocks, appends each block's error correction
	/// codewords, and interleaves the blocks.
	private static func addErrorCorrection(_ data: [UInt8], _ version: Int, _ ecc: ErrorCorrection) -> [UInt8] {
		let blockCount = errorCorrectionBlocks[ecc.rawValue][version]
		let eccLength = eccCodewordsPerBlock[ecc.rawValue][version]
		let rawCodewords = rawDataModules(version) / 8
		let shortBlockCount = blockCount - rawCodewords % blockCount
		let shortBlockLength = rawCodewords / blockCount
		let divisor = reedSolomonDivisor(eccLength)

		var blocks: [[UInt8]] = []
		var offset = 0
		for i in 0..<blockCount {
			let dataLength = shortBlockLength - eccLength + (i < shortBlockCount ? 0 : 1)
			var block = Array(data[offset..<(offset + dataLength)])
			offset += dataLength
			let eccBytes = reedSolomonRemainder(block, divisor)
			if i < shortBlockCount { block.append(0) }  // placeholder so all blocks line up, skipped below
			blocks.append(block + eccBytes)
		}

		var result: [UInt8] = []
		for i in 0..<blocks[0].count {
			for (j, block) in blocks.enumerated() where i != shortBlockLength - eccLength || j >= shortBlockCount {
				result.append(block[i])
			}
		}
		return result
	}

	private static func reedSolomonDivisor(_ degree: Int) -> [UInt8] {
		var result = [UInt8](repeating: 0, count: degree)
		result[degree - 1] = 1
		var root: UInt8 = 1
		for _ in 0..<degree {
			for j in 0..<degree {
				result[j] = gfMultiply(result[j], root)
				if j + 1 < degree { result[j] ^= result[j + 1] }
			}
			root = gfMultiply(root, 0x02)
		}
		return result
	}

	private static func reedSolomonRemainder(_ data: [UInt8], _ divisor: [UInt8]) -> [UInt8] {
		var result = [UInt8](repeating: 0, count: divisor.count)
		for byte in data {
			let factor = byte ^ result.removeFirst()
			result.append(0)
			for i in 0..<divisor.count {
				result[i] ^= gfMultiply(divisor[i], factor)
			}
		}
		return result
	}

	/// Multiplication in GF(2^8) modulo x^8 + x^4 + x^3 + x^2 + 1.
	private static func gfMultiply(_ x: UInt8, _ y: UInt8) -> UInt8 {
		var z = 0
		for i in stride(from: 7, through: 0, by: -1) {
			z = (z << 1) ^ ((z >> 7) * 0x11D)
			z ^= ((Int(y) >> i) & 1) * Int(x)
		}
		return UInt8(z)
	}

	// MARK: - Bit buffer

	private struct BitBuffer {
		private(set) var bits: [Bool] = []
		var count: Int { bits.count }

		mutating func append(_ value: Int, _ length: Int) {
			for i in stride(from: length - 1, through: 0, by: -1) {
				bits.append((value >> i) & 1 == 1)
			}
		}

		func bytes() -> [UInt8] {
			var result = [UInt8](repeating: 0, count: bits.count / 8)
			for (i, bit) in bits.enumerated() where bit {
				result[i / 8] |= 0x80 >> UInt8(i % 8)
			}
			return result
		}
	}

	// MARK: - Symbol

	private struct Symbol {
		let version: Int
		let ecc: ErrorCorrection
		let size: Int
		var modules: [[Bool]]
		var isFunction: [[Bool]]  // finder, timing, alignment, format and version modules

		init(version: Int, ecc: ErrorCorrection) {
			self.version = version
			self.ecc = ecc
			self.size = version * 4 + 17
			self.modules = [[Bool]](repeating: [Bool](repeating: false, count: size), count: size)
			self.isFunction = modules
			drawFunctionPatterns()
		}

		mutating func set(_ x: Int, _ y: Int, _ dark: Bool) {
			modules[y][x] = dark
			isFunction[y][x] = true
		}

		mutating func drawFunctionPatterns() {
			for i in 0..<size {
				set(6, i, i % 2 == 0)
				set(i, 6, i % 2 == 0)
			}

			// finder patterns with their separators
			for (cx, cy) in [(3, 3), (size - 4, 3), (3, size - 4)] {
				for dy in -4...4 {
					for dx in -4...4 {
						let x = cx + dx
						let y = cy + dy
						guard x >= 0, x < size, y >= 0, y < size else { continue }
						let distance = max(abs(dx), abs(dy))
						set(x, y, distance != 2 && distance != 4)
					}
				}
			}

			let positions = alignmentPositions()
			let last = positions.count - 1
			for (i, cy) in positions.enumerated() {
				for (j, cx) in positions.enumerated() {
					// skip the three corners taken by finder patterns
					if (i == 0 && j == 0) || (i == 0 && j == last) || (i == last && j == 0) { continue }
					for dy in -2...2 {
						for dx in -2...2 {
							set(cx + dx, cy + dy, max(abs(dx), abs(dy)) != 1)
						}
					}
				}
			}

			drawFormatBits(mask: 0)  // reserves the area, redrawn once the mask is picked

			if version >= 7 {
				var remainder = version
				for _ in 0..<12 { remainder = (remainder << 1) ^ ((remainder >> 11) * 0x1F25) }
				let bits = version << 12 | remainder
				for i in 0..<18 {
					let dark = (bits >> i) & 1 == 1
					let a = size - 11 + i % 3
					let b = i / 3
					set(a, b, dark)
					set(b, a, dark)
				}
			}
		}

		func alignmentPositions() -> [Int] {
			if version == 1 { return [] }
			let count = version / 7 + 2
			let step = (version * 8 + count * 3 + 5) / (count * 4 - 4) * 2
			var result = [6]
			var position = size - 7
			for _ in 0..<(count - 1) {
				result.insert(position, at: 1)
				position -= step
			}
			return result
		}

		mutating func drawFormatBits(mask: Int) {
			let data = (ecc == .low ? 0b01 : 0b00) << 3 | mask
			var remainder = data
			for _ in 0..<10 { remainder = (remainder << 1) ^ ((remainder >> 9) * 0x537) }
			let bits = (data << 10 | remainder) ^ 0x5412
			func bit(_ i: Int) -> Bool { (bits >> i) & 1 == 1 }

			// around the top left finder
			for i in 0...5 { set(8, i, bit(i)) }
			set(8, 7, bit(6))
			set(8, 8, bit(7))
			set(7, 8, bit(8))
			for i in 9..<15 { set(14 - i, 8, bit(i)) }

			// split between the top right and bottom left finders
			for i in 0..<8 { set(size - 1 - i, 8, bit(i)) }
			for i in 8..<15 { set(8, size - 15 + i, bit(i)) }
			set(8, size - 8, true)  // always dark
		}

		/// Places the codewords in the zigzag order, two columns at a time from the right.
		mutating func drawCodewords(_ data: [UInt8]) {
			var i = 0
			var right = size - 1
			while right >= 1 {
				if right == 6 { right = 5 }  // skip the vertical timing pattern
				let upward = (right + 1) & 2 == 0
				for vertical in 0..<size {
					let y = upward ? size - 1 - vertical : vertical
					for x in [right, right - 1] where !isFunction[y][x] && i < data.count * 8 {
						modules[y][x] = (data[i / 8] >> UInt8(7 - i % 8)) & 1 == 1
						i += 1
					}
				}
				right -= 2
			}
		}

		/// XORs the mask pattern onto the data modules; applying it twice undoes it.
		mutating func applyMask(_ mask: Int) {
			for y in 0..<size {
				for x in 0..<size where !isFunction[y][x] {
					let invert: Bool
					switch mask {
					case 0: invert = (x + y) % 2 == 0
					case 1: invert = y % 2 == 0
					case 2: invert = x % 3 == 0
					case 3: invert = (x + y) % 3 == 0
					case 4: invert = (x / 3 + y / 2) % 2 == 0
					case 5: invert = x * y % 2 + x * y % 3 == 0
					case 6: invert = (x * y % 2 + x * y % 3) % 2 == 0
					default: invert = ((x + y) % 2 + x * y % 3) % 2 == 0
					}
					if invert { modules[y][x].toggle() }
				}
			}
		}

		mutating func applyBestMask() {
			var bestMask = 0
			var bestPenalty = Int.max
			for mask in 0..<8 {
				applyMask(mask)
				drawFormatBits(mask: mask)
				let score = penalty()
				if score < bestPenalty {
					bestPenalty = score
					bestMask = mask
				}
				applyMask(mask)
			}
			applyMask(bestMask)
			drawFormatBits(mask: bestMask)
		}

		/// Penalty score from the spec, lower is easier to scan.
		func penalty() -> Int {
			var score = 0

			for vertical in [false, true] {
				for i in 0..<size {
					func dark(_ j: Int) -> Bool {
						vertical ? modules[j][i] : modules[i][j]
					}
					func light(_ from: Int, _ to: Int) -> Bool {
						for j in max(from, 0)..<min(to, size) where dark(j) { return false }
						return true
					}

					// runs of 5 or more same colored modules
					var run = 1
					for j in 1..<size {
						if dark(j) == dark(j - 1) {
							run += 1
							score += run == 5 ? 3 : (run > 5 ? 1 : 0)
						} else {
							run = 1
						}
					}

					// 1:1:3:1:1 patterns that look like a finder, with 4 light modules on a side
					for j in 0...(size - 7)
					where dark(j) && !dark(j + 1) && dark(j + 2) && dark(j + 3) && dark(j + 4) && !dark(j + 5) && dark(j + 6) {
						if light(j - 4, j) || light(j + 7, j + 11) { score += 40 }
					}
				}
			}

			// 2x2 blocks of the same color
			for y in 0..<(size - 1) {
				for x in 0..<(size - 1) {
					let c = modules[y][x]
					if c == modules[y][x + 1] && c == modules[y + 1][x] && c == modules[y + 1][x + 1] {
						score += 3
					}
				}
			}

			// balance of dark and light modules, 10 points per 5% away from half
			var darkCount = 0
			for row in modules {
				for module in row where module { darkCount += 1 }
			}
			let total = size * size
			score += abs(darkCount * 2 - total) * 10 / total * 10

			return score
		}
	}
}
