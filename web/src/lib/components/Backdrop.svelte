<!-- The page's background: a living room in the evening, lamps on, out of
     focus - by day the same room in daylight. Drawn, not photographed: two
     dozen flat shapes and one CSS blur weigh a few hundred bytes, and a photo
     that survives a blur this strong would be ten times that.

     The view box is 120 x 100 and the room fills the screen by being cropped
     ("slice"): a phone held upright sees the middle 46 units, a wide screen
     the middle 65 in height. So what makes the room - the sofa, the pictures,
     the lamp behind it - is in the middle, and the window and the shelf are
     for wide screens.

     The last shape is a veil in the page's colour. It is what keeps the text
     that lies on the room readable: under it nothing by day is darker, and
     nothing at night lighter, than the grey text needs for 4.5:1, even before
     the blur. test/backdrop.test.ts holds that for every colour here; the
     lamp's light is never laid on thicker than it allows for (LAMP there). -->
<svg viewBox="0 0 120 100" preserveAspectRatio="xMidYMid slice" aria-hidden="true">
	<path style:fill="var(--wall)" d="M-20-20h160v140H-20z" />
	<!-- The window: sky, the glow over the horizon, a curtain. -->
	<path style:fill="var(--sky)" d="M4 12h30v50H4z" />
	<path style:fill="var(--dusk)" d="M4 44h30v18H4z" />
	<path style:fill="var(--linen)" d="M34 8h7v60h-7z" />
	<!-- Two pictures. -->
	<path style:fill="var(--frame)" d="M51 20h16v20H51zM71 26h9v12h-9z" />
	<path style:fill="var(--art)" d="M54 23h10v14H54z" />
	<!-- The shelf, with a strip of light under a board. -->
	<path style:fill="var(--frame)" d="M104 28h36v48h-36z" />
	<path style:fill="var(--shade)" d="M104 46h36v2h-36z" />
	<!-- The floor lamp's light on the wall, then the lamp. -->
	<g style:fill="var(--lamp)">
		<circle cx="80" cy="42" r="32" opacity=".14" />
		<circle cx="80" cy="42" r="21" opacity=".22" />
		<circle cx="80" cy="42" r="11" opacity=".4" />
	</g>
	<path style:fill="var(--shade)" d="M75 34h10l3 11H72z" />
	<path style:fill="var(--frame)" d="M79.5 45h1v31h-1z" />
	<!-- A plant. -->
	<g style:fill="var(--leaf)"><circle cx="40" cy="52" r="7" /><circle cx="37" cy="62" r="6" /></g>
	<!-- The sofa and two cushions. -->
	<rect style:fill="var(--sofa)" x="44" y="56" width="42" height="16" rx="4" />
	<rect style:fill="var(--seat)" x="42" y="66" width="46" height="10" rx="3" />
	<circle style:fill="var(--ochre)" cx="52" cy="62" r="5" />
	<circle style:fill="var(--linen)" cx="78" cy="63" r="4.5" />
	<!-- The floor, the lamp's pool of light on it, a rug, a table and a candle. -->
	<path style:fill="var(--floor)" d="M-20 76h160v44H-20z" />
	<ellipse style:fill="var(--lamp)" cx="82" cy="80" rx="20" ry="4" opacity=".4" />
	<ellipse style:fill="var(--rug)" cx="64" cy="91" rx="36" ry="8" />
	<rect style:fill="var(--frame)" x="54" y="80" width="22" height="5" rx="1" />
	<circle style:fill="var(--lamp)" cx="65" cy="78" r="2.5" opacity=".6" />
	<path style:fill="var(--veil)" d="M-20-20h160v140H-20z" />
</svg>

<style>
	/* Fixed, and larger than the screen by the blur's reach, so its soft edge
	 * is outside. As tall as the screen is with the browser's bars away
	 * (`lvh`): the bars coming and going must not resize it, because a resize
	 * blurs it again. */
	svg {
		position: fixed;
		top: -6vmax;
		left: -6vmax;
		z-index: -1;
		width: calc(100vw + 12vmax);
		height: calc(100lvh + 12vmax);
		pointer-events: none;
		filter: blur(2.4vmax);
		--wall: #e8dfcf;
		--sky: #d6e6f2;
		--dusk: #edf3ee;
		--linen: #f7f1e6;
		--frame: #a89a85;
		--art: #dcc6a2;
		--shade: #fff6e4;
		--lamp: #fff0d0;
		--leaf: #7f9c79;
		--sofa: #8ea19b;
		--seat: #a2b4ad;
		--ochre: #dca750;
		--floor: #c6a37b;
		--rug: #eee5d6;
		--veil: rgb(244 238 228 / .5);
	}
	@media (prefers-color-scheme: dark) {
		svg {
			--wall: #4d4138;
			--sky: #33425f;
			--dusk: #8b6a65;
			--linen: #a8977d;
			--frame: #2a2420;
			--art: #9a7b56;
			--shade: #c8975a;
			--lamp: #ffb25a;
			--leaf: #2c4633;
			--sofa: #2b3c3b;
			--seat: #354b4b;
			--ochre: #c98a3a;
			--floor: #2d2019;
			--rug: #6b5a4d;
			--veil: rgb(23 21 18 / .5);
		}
	}
</style>
