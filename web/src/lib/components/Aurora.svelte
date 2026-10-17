<script lang="ts">
	/* The first screen's background: Svelte Bits' Aurora
	 * (sveltebits.xyz/backgrounds/aurora, a port of React Bits'), stops teal,
	 * sky and magenta, amplitude 0.7, blend 0.3, speed 0.5. The shader is
	 * theirs with those settings folded in as constants and the three-stop ramp
	 * written out; ogl is left out, since one triangle needs no scene graph.
	 * Drawn at CSS pixels, as ogl does by default: the light is soft, and a
	 * phone's screen at 3x would be nine times the work.
	 *
	 * It draws only while it can be seen (an IntersectionObserver: scrolled
	 * past the first screen, it stops). Reduced motion gets one still frame.
	 *
	 * Their shader lays the aurora over the page, and where it thins out its
	 * colour is near black at part alpha, so on anything but black its lower
	 * edge is a band darker than the page. Here it is worked into the page's
	 * own colour instead: on the dark theme's ink its light is added, and on
	 * the light theme's sand its colour is mixed in at the sand's own
	 * lightness, as a tint - light cannot be added to a page that is already
	 * light, and a darker tint would take contrast from the text. Either way
	 * the canvas is opaque and ends in exactly the page's colour. It covers
	 * the CSS wash, both being the first screen; the wash is what shows
	 * without WebGL 2. */
	import { onMount } from "svelte";

	const VERT = `#version 300 es
layout(location = 0) in vec2 p;
void main() { gl_Position = vec4(p, 0., 1.); }`;

	/* The shader's comments are out here, where the build drops them: the
	 * string is shipped as written.
	 *
	 * C0-C2 are #007f79 teal-600, #007eb1 sky-600 and #98005f magenta-600.
	 * TINT is, on sand, the most of the ramp mixed in (at the top of the
	 * screen), which is then brought back to the sand's own luminance: mixed
	 * plainly, teal in sand is grey, and a tint strong enough to show darkens
	 * the page under its grey text. Clamping costs magenta a little: grey text
	 * is 5.1:1 on sand and 4.8:1 in the magenta corner. */
	const FRAG = `#version 300 es
precision highp float;
uniform float uTime;
uniform vec2 uResolution;
uniform vec3 uPage;
uniform float uLight;
out vec4 fragColor;

const vec3 C0 = vec3(0., .498, .475);
const vec3 C1 = vec3(0., .494, .694);
const vec3 C2 = vec3(.596, 0., .373);
const float AMPLITUDE = .7;
const float BLEND = .3;
const float TINT = .4;
const vec3 LUMA = vec3(.2126, .7152, .0722);

vec3 permute(vec3 x) { return mod(((x * 34.) + 1.) * x, 289.); }

float snoise(vec2 v) {
  const vec4 C = vec4(.211324865405187, .366025403784439, -.577350269189626, .024390243902439);
  vec2 i = floor(v + dot(v, C.yy));
  vec2 x0 = v - i + dot(i, C.xx);
  vec2 i1 = (x0.x > x0.y) ? vec2(1., 0.) : vec2(0., 1.);
  vec4 x12 = x0.xyxy + C.xxzz;
  x12.xy -= i1;
  i = mod(i, 289.);
  vec3 p = permute(permute(i.y + vec3(0., i1.y, 1.)) + i.x + vec3(0., i1.x, 1.));
  vec3 m = max(.5 - vec3(dot(x0, x0), dot(x12.xy, x12.xy), dot(x12.zw, x12.zw)), 0.);
  m = m * m;
  m = m * m;
  vec3 x = 2. * fract(p * C.www) - 1.;
  vec3 h = abs(x) - .5;
  vec3 ox = floor(x + .5);
  vec3 a0 = x - ox;
  m *= 1.79284291400159 - .85373472095314 * (a0 * a0 + h * h);
  vec3 g;
  g.x = a0.x * x0.x + h.x * x0.y;
  g.yz = a0.yz * x12.xz + h.yz * x12.yw;
  return 130. * dot(m, g);
}

void main() {
  vec2 uv = gl_FragCoord.xy / uResolution;
  vec3 ramp = uv.x < .5 ? mix(C0, C1, uv.x * 2.) : mix(C1, C2, uv.x * 2. - 1.);
  float height = exp(snoise(vec2(uv.x * 2. + uTime * .1, uTime * .25)) * .5 * AMPLITUDE);
  float intensity = .6 * (uv.y * 2. - height + .2);
  float alpha = smoothstep(.2 - BLEND * .5, .2 + BLEND * .5, intensity);
  float a = intensity * alpha;
  if (uLight < .5) { fragColor = vec4(uPage + a * ramp, 1.); return; }
  vec3 page = pow(uPage, vec3(2.2));
  vec3 tint = pow(mix(uPage, ramp, TINT * a), vec3(2.2));
  tint = min(tint * dot(page, LUMA) / dot(tint, LUMA), 1.);
  fragColor = vec4(pow(tint, vec3(1. / 2.2)), 1.);
}`;

	let canvas: HTMLCanvasElement;

	onMount(() => {
		const still = matchMedia("(prefers-reduced-motion: reduce)");
		let gl: WebGL2RenderingContext | null = null;
		const dark = matchMedia("(prefers-color-scheme: dark)");
		let uTime: WebGLUniformLocation | null = null, uRes: WebGLUniformLocation | null = null;
		let uPage: WebGLUniformLocation | null = null, uLight: WebGLUniformLocation | null = null;
		let failed = false, visible = false, raf = 0, last = 0;

		/* Also what a restored context runs: everything but the context is gone. */
		function setup(): boolean {
			if (!gl) return false;
			const prog = gl.createProgram()!;
			for (const [type, src] of [[gl.VERTEX_SHADER, VERT], [gl.FRAGMENT_SHADER, FRAG]] as const) {
				const s = gl.createShader(type)!;
				gl.shaderSource(s, src);
				gl.compileShader(s);
				gl.attachShader(prog, s);
			}
			gl.linkProgram(prog);
			if (!gl.getProgramParameter(prog, gl.LINK_STATUS)) return false;
			gl.useProgram(prog);
			gl.bindBuffer(gl.ARRAY_BUFFER, gl.createBuffer());
			gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-1, -1, 3, -1, -1, 3]), gl.STATIC_DRAW);
			gl.enableVertexAttribArray(0);
			gl.vertexAttribPointer(0, 2, gl.FLOAT, false, 0, 0);
			uPage = gl.getUniformLocation(prog, "uPage");
			uLight = gl.getUniformLocation(prog, "uLight");
			uTime = gl.getUniformLocation(prog, "uTime");
			uRes = gl.getUniformLocation(prog, "uResolution");
			canvas.width = canvas.height = 0;   // so size() sets both
			return true;
		}

		function size() {
			const w = canvas.clientWidth, h = canvas.clientHeight;
			if (!gl || (w === canvas.width && h === canvas.height)) return;
			canvas.width = w;
			canvas.height = h;
			gl.viewport(0, 0, w, h);
			gl.uniform2f(uRes, w, h);
		}

		/* The page's own colour, and which theme that is. */
		function page() {
			const c = getComputedStyle(document.body).backgroundColor.match(/[\d.]+/g)!.map((v) => Number(v) / 255);
			gl!.uniform3f(uPage, c[0], c[1], c[2]);
			gl!.uniform1f(uLight, c[0] + c[1] + c[2] > 1.5 ? 1 : 0);
		}

		/* Speed 0.5 in ogl's terms is `t * 0.01 * speed * 0.1`, t in ms. */
		function draw(t: number) {
			gl!.uniform1f(uTime, t * 5e-4);
			gl!.drawArrays(gl!.TRIANGLES, 0, 3);
			last = t;
		}

		/* About 30 fps: the light drifts a fortieth of the width a second, and
		 * a 120 Hz screen would otherwise draw it four times as often. */
		function frame(t: number) {
			raf = requestAnimationFrame(frame);
			if (t - last >= 30) draw(t);
		}

		function update() {
			cancelAnimationFrame(raf);
			if (!visible || failed || gl?.isContextLost()) return;
			if (!gl) {
				gl = canvas.getContext("webgl2", { antialias: false, depth: false, stencil: false, powerPreference: "low-power" });
				if (!setup()) { failed = true; return; }
			}
			/* At once, not at the next frame: a resize has just cleared the canvas. */
			size();
			page();
			draw(still.matches ? 0 : performance.now());
			if (!still.matches) raf = requestAnimationFrame(frame);
		}

		const io = new IntersectionObserver(([e]) => { visible = e.isIntersecting; update(); });
		const ro = new ResizeObserver(update);
		io.observe(canvas);
		ro.observe(canvas);
		still.addEventListener("change", update);
		dark.addEventListener("change", update);
		canvas.addEventListener("webglcontextlost", (e) => { e.preventDefault(); cancelAnimationFrame(raf); });
		canvas.addEventListener("webglcontextrestored", () => { failed = !setup(); update(); });
		return () => {
			cancelAnimationFrame(raf);
			io.disconnect();
			ro.disconnect();
			still.removeEventListener("change", update);
			dark.removeEventListener("change", update);
		};
	});
</script>

<canvas bind:this={canvas} aria-hidden="true"></canvas>

<style>
	/* One screen tall at the top of the page, scrolling with it, like the
	 * wash it covers. */
	canvas {
		position: absolute;
		top: 0;
		left: 0;
		z-index: -1;
		width: 100%;
		height: 100svh;
		pointer-events: none;
	}
</style>
