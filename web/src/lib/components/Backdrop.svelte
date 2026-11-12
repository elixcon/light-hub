<!-- The page's background: a living room in the evening, out of focus, lit
     by a floor lamp, a table lamp on a sideboard and a candle - by day the
     same room in daylight. Drawn, not photographed: two dozen flat shapes and
     one CSS blur weigh a few hundred bytes, and a photo that survives a blur
     this strong would be ten times that.

     The view box is 120 x 100 and the room fills the screen by being cropped
     ("slice"): a phone held upright sees the middle 46 units, a wide screen
     the middle 65 in height. So the two lamps and the plant between them are
     in the middle, and the window, the sofa, the picture and the armchair are
     for wide screens.

     The last shape is a veil. It is what keeps the text that lies on the room
     readable: under it nothing by day is darker, and nothing at night
     lighter, than the grey text needs for 4.5:1, even before the blur.
     test/backdrop.test.ts holds that for every colour here, the lamps' light
     at full strength included, so how thick it is laid on does not matter. -->
<svg viewBox="0 0 120 100" preserveAspectRatio="xMidYMid slice" aria-hidden="true">
	<path style:fill="var(--wall)" d="M-20-20h160v140H-20z" />
	<!-- The window and its curtain, a picture. -->
	<path style:fill="var(--sky)" d="M-20-20h32v72h-32z" />
	<path style:fill="var(--linen)" d="M12-20h8v100h-8z" />
	<path style:fill="var(--frame)" d="M100 14h24v30h-24z" />
	<path style:fill="var(--wood)" d="M104 18h16v22h-16z" />
	<!-- The floor and the sideboard. -->
	<path style:fill="var(--floor)" d="M-20 76h160v44H-20z" />
	<path style:fill="var(--wood)" d="M68 58h72v22H68z" />
	<!-- The lamps' light on the wall, the floor lamp, the table lamp. -->
	<g style:fill="var(--lamp)">
		<circle cx="47" cy="36" r="34" opacity=".2" />
		<circle cx="47" cy="36" r="20" opacity=".35" />
		<circle cx="79" cy="48" r="26" opacity=".25" />
		<circle cx="79" cy="50" r="13" opacity=".5" />
		<ellipse cx="84" cy="58" rx="22" ry="3" />
	</g>
	<path style:fill="var(--frame)" d="M46.5 40h1v40h-1z" />
	<g style:fill="var(--shade)">
		<rect x="38" y="24" width="18" height="17" rx="3" />
		<path d="M73 39h12l4 12H69z" />
	</g>
	<circle style:fill="var(--linen)" cx="93" cy="54" r="3.5" />
	<!-- A plant. -->
	<g style:fill="var(--leaf)">
		<circle cx="63" cy="30" r="7" /><circle cx="60" cy="43" r="7" /><circle cx="64" cy="56" r="5" />
	</g>
	<!-- The sofa and two cushions, a table with a candle, an armchair. -->
	<rect style:fill="var(--sofa)" x="-20" y="64" width="64" height="50" rx="7" />
	<circle style:fill="var(--ochre)" cx="27" cy="60" r="8" />
	<circle style:fill="var(--seat)" cx="40" cy="64" r="7" />
	<ellipse style:fill="var(--wood)" cx="64" cy="84" rx="26" ry="6" />
	<circle style:fill="var(--lamp)" cx="62" cy="79" r="5" opacity=".6" />
	<circle style:fill="var(--shade)" cx="62" cy="79" r="2" />
	<circle style:fill="var(--seat)" cx="116" cy="106" r="24" />
	<ellipse style:fill="var(--linen)" cx="84" cy="104" rx="14" ry="7" />
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
		filter: blur(2.1vmax);
		--wall: #e9d3ab;
		--sky: #bcd6ee;
		--linen: #f8eedc;
		--frame: #a8906c;
		--wood: #c9a674;
		--shade: #fff0c8;
		--lamp: #ffdf9a;
		--leaf: #7f9c79;
		--sofa: #f3e8d4;
		--seat: #8ea19b;
		--ochre: #d99a56;
		--floor: #d4b48a;
		--veil: rgb(246 238 224 / .45);
	}
	@media (prefers-color-scheme: dark) {
		svg {
			--wall: #3f2a18;
			--sky: #2c4c88;
			--linen: #9a7c58;
			--frame: #1c120a;
			--wood: #6e4420;
			--shade: #c27a16;
			--lamp: #c2741c;
			--leaf: #1f3320;
			--sofa: #937452;
			--seat: #263a2c;
			--ochre: #8a3a1c;
			--floor: #20150c;
			--veil: rgb(26 18 10 / .4);
		}
	}
</style>
