// Pixel sprite sheets, authored as character grids and rendered server-side to
// the cube's native RGB565 at whatever scale it asks for. The cube does no
// decoding and no scaling — nearest-neighbour happens here, so the chunky
// pixels arrive exactly as drawn.
//
// RGB565 has no alpha, so "transparent" cells are baked to the theme's scene
// background: a sheet is therefore per-theme, which the stem the firmware
// caches under already encodes.
package main

// Every frame is petSpriteGrid x petSpriteGrid cells; requested pixel sizes
// must be a multiple so cells stay square and crisp.
const petSpriteGrid = 12

// Palette roles, one rune per cell:
//
//	'.' background   'o' body   'd' body-dark / outline
//	'e' eye          'p' accent (blush, collar)   'w' highlight
type petSpritePalette map[rune]uint32

// Indexed by theme id (modern, retro-lcd, b/w, amoled-neon) — must stay in
// step with petThemeMax and the firmware's theme table.
var petSpritePalettes = [petThemeMax + 1]petSpritePalette{
	{'.': 0x0A0F22, 'o': 0xF4F1DE, 'd': 0x264653, 'e': 0x10162A, 'p': 0xE76F51, 'w': 0xFFFFFF},
	{'.': 0x9BBC0F, 'o': 0x0F380F, 'd': 0x306230, 'e': 0x9BBC0F, 'p': 0x306230, 'w': 0x8BAC0F},
	{'.': 0xF2F2F2, 'o': 0x141414, 'd': 0x4D4D4D, 'e': 0xF2F2F2, 'p': 0x8A8A8A, 'w': 0xFFFFFF},
	{'.': 0x000000, 'o': 0x00E5FF, 'd': 0x0077AA, 'e': 0x001018, 'p': 0xFF2D95, 'w': 0xFFFFFF},
}

// BIT, a round little cat. Frame order is part of the wire contract with the
// firmware: 0-1 idle breath, 2 blink, 3-4 walk, 5-6 sleep, 7 happy.
var petSpriteBit = [][petSpriteGrid]string{
	{ // 0: idle
		"............",
		"..d......d..",
		"..dd....dd..",
		"..oooooooo..",
		".oooooooooo.",
		".ooeooooeoo.",
		".oooooooooo.",
		".opoodoopoo.",
		".oooooooooo.",
		"..oooooooo..",
		"..oo....oo..",
		"............",
	},
	{ // 1: idle, breath in (one row taller ears, squashed body)
		"............",
		"..d......d..",
		"..dd....dd..",
		"............",
		".oooooooooo.",
		".ooeooooeoo.",
		".oooooooooo.",
		".opoodoopoo.",
		".oooooooooo.",
		".oooooooooo.",
		"..oo....oo..",
		"............",
	},
	{ // 2: blink
		"............",
		"..d......d..",
		"..dd....dd..",
		"..oooooooo..",
		".oooooooooo.",
		".oodooooddo.",
		".oooooooooo.",
		".opoodoopoo.",
		".oooooooooo.",
		"..oooooooo..",
		"..oo....oo..",
		"............",
	},
	{ // 3: walk, left feet forward
		"............",
		"..d......d..",
		"..dd....dd..",
		"..oooooooo..",
		".oooooooooo.",
		".ooeooooeoo.",
		".oooooooooo.",
		".opoodoopoo.",
		".oooooooooo.",
		"..oooooooo..",
		".oo......oo.",
		"............",
	},
	{ // 4: walk, feet together mid-stride
		"............",
		"..d......d..",
		"..dd....dd..",
		"..oooooooo..",
		".oooooooooo.",
		".ooeooooeoo.",
		".oooooooooo.",
		".opoodoopoo.",
		".oooooooooo.",
		"..oooooooo..",
		"...oo..oo...",
		"............",
	},
	{ // 5: sleep, curled low
		"............",
		"............",
		"............",
		"............",
		"..d......d..",
		"..oooooooo..",
		".oodoooodoo.",
		".oooooooooo.",
		".oooooooooo.",
		"..oooooooo..",
		"............",
		"............",
	},
	{ // 6: sleep, breath
		"............",
		"............",
		"............",
		"..d......d..",
		"..oooooooo..",
		".oodoooodoo.",
		".oooooooooo.",
		".oooooooooo.",
		".oooooooooo.",
		"..oooooooo..",
		"............",
		"............",
	},
	{ // 7: happy — ^ eyes, open mouth, mid-hop
		"............",
		"..d......d..",
		"..dd....dd..",
		"..oooooooo..",
		".oooooooooo.",
		".owdoooowdo.",
		".oooooooooo.",
		".opoedeopoo.",
		".oooooooooo.",
		"..oooooooo..",
		"............",
		"............",
	},
}

var petSprites = map[string][][petSpriteGrid]string{
	"bit": petSpriteBit,
}

// petSheetNameFor maps a species id to its sheet stem, or reports that the
// species is vector-drawn and has no sheet. Stems stay short: the firmware
// prefixes them into 8.3 SD filenames.
func petSheetNameFor(species int) (string, bool) {
	switch species {
	case 1:
		return "bit", true
	default:
		return "", false
	}
}

// encodePetSheet renders one species at one theme and frame size into a single
// LVGL .bin: header w = size, h = frames x size, frames stacked vertically.
func encodePetSheet(name string, theme, size int) ([]byte, bool) {
	frames, ok := petSprites[name]
	if !ok || theme < 0 || theme >= len(petSpritePalettes) {
		return nil, false
	}
	palette := petSpritePalettes[theme]
	scale := size / petSpriteGrid
	stride := size * 2
	height := len(frames) * size
	out := make([]byte, lvglHeaderLen+stride*height)
	out[0] = lvImageHeaderMagic
	out[1] = lvColorFormatRGB565
	putU16(out[4:], uint16(size))
	putU16(out[6:], uint16(height))
	putU16(out[8:], uint16(stride))

	o := lvglHeaderLen
	for _, frame := range frames {
		for row := 0; row < petSpriteGrid; row++ {
			// Render one cell row of pixels, then repeat it scale times.
			line := make([]byte, stride)
			for col := 0; col < petSpriteGrid; col++ {
				rgb, known := palette[rune(frame[row][col])]
				if !known {
					rgb = palette['.']
				}
				v := uint16(rgb>>19)<<11 | uint16(rgb>>10&0x3F)<<5 | uint16(rgb>>3&0x1F)
				for i := 0; i < scale; i++ {
					x := (col*scale + i) * 2
					line[x] = byte(v)
					line[x+1] = byte(v >> 8)
				}
			}
			for i := 0; i < scale; i++ {
				copy(out[o:], line)
				o += stride
			}
		}
	}
	return out, true
}
