import { test } from "node:test";
import assert from "node:assert/strict";
import { cleanRules, eventText, rowsUsed, blankRule, lightChoices, delayLabel } from "../src/lib/buttons.ts";
import { sensorCards, sensorState } from "../src/lib/sensors.ts";
import type { ButtonDef, Device, Group, Rule } from "../src/lib/types.ts";

const groups = [{ id: 1, name: "Kitchen" }, { id: 2, name: "Hall" }] as unknown as Group[];
const devices = [
  { ieee: "0x0011223344556677", name: "Reading lamp",
    endpoints: [{ ep: 11, features: ["onoff", "brightness"], curves: { level: false, chroma: false } }] },
  { ieee: "0x8899AABBCCDDEEFF", name: "Hue dimmer",
    endpoints: [{ ep: 1, features: ["onoff", "switch"], curves: { level: false, chroma: false } }] },
] as unknown as Device[];

const room = (group: number, rest: Partial<Rule> = {}): Rule => ({ ...blankRule(), group, do: "toggle", ...rest });
const lamp = (rest: Partial<Rule> = {}): Rule =>
  ({ ...blankRule(), ieee: "0x0011223344556677", ep: 11, do: "toggle", ...rest });

test("a remote's endpoint is not a light to pick", () => {
  const l = lightChoices(devices);
  assert.equal(l.length, 1);
  assert.equal(l[0].name, "Reading lamp");
  assert.equal(l[0].dims, true);
});

test("cleanRules sends a room or a light, never both", () => {
  const out = cleanRules([room(1), lamp({ and: true, group: 7 })]);
  assert.deepEqual(out[0], { group: 1, ieee: null, ep: null, and: false, when: "always", arg: 30, do: "toggle", value: 60, delay: 0 });
  assert.deepEqual(out[1], { group: 0, ieee: "0x0011223344556677", ep: 11, and: true, when: "always", arg: 0, do: "toggle", value: 60, delay: 0 });
});

test("cleanRules drops rows with nothing to act on", () => {
  assert.deepEqual(cleanRules([blankRule()]), []);
  assert.equal(cleanRules([room(1), { ...blankRule(), and: true }]).length, 1);
  assert.equal(cleanRules([room(1), room(2, { do: "none", and: true })]).length, 1, "a joined do-nothing does nothing");
});

test("a head that does nothing is kept when rules are joined to it", () => {
  /* "When the kitchen is off, leave it, and switch the reading lamp on." */
  const out = cleanRules([room(1, { when: "off", do: "none" }), lamp({ and: true, do: "on" })]);
  assert.equal(out.length, 2);
  assert.equal(out[0].do, "none");
  assert.equal(out[0].when, "off");
});

test("a dropped head hands its case to the first joined rule", () => {
  const out = cleanRules([
    room(1),
    { ...blankRule(), when: "below", arg: 40 },           /* a head with no target */
    lamp({ and: true, do: "on" }),
    room(2, { and: true, do: "on" }),
  ]);
  assert.equal(out.length, 3);
  assert.equal(out[1].and, false, "the lamp now opens the second case");
  assert.equal(out[1].when, "below");
  assert.equal(out[1].arg, 40);
  assert.equal(out[2].and, true, "and the hall stays joined to it, not to the first case");
});

test("the first rule never joins anything", () => {
  const out = cleanRules([{ ...blankRule() }, lamp({ and: true })]);
  assert.equal(out.length, 1);
  assert.equal(out[0].and, false);
});

test("eventText joins a case with and, and the cases with otherwise", () => {
  const rules = cleanRules([
    room(1, { when: "off", do: "on" }),
    lamp({ and: true, do: "set", value: 30 }),
    room(1, { do: "off" }),
    lamp({ and: true, do: "off" }),
  ]);
  assert.equal(eventText(rules, groups, devices),
    "Turn on Kitchen and Reading lamp to 30 % while Kitchen is off, otherwise turn off Kitchen and turn off Reading lamp");
});

test("eventText reads a single rule as before", () => {
  assert.equal(eventText([room(1, { when: "below", arg: 30, do: "set", value: 80 }), room(1, { do: "set", value: 30 })], groups, devices),
    "Kitchen to 80 % when under 30 %, otherwise Kitchen to 30 %");
  assert.equal(eventText([room(1, { when: "on", do: "off" }), room(1, { do: "on" })], groups, devices),
    "Turn off Kitchen while it is on, otherwise turn on Kitchen");
  assert.equal(eventText([lamp()], groups, devices), "Toggle Reading lamp");
  assert.equal(eventText([], groups, devices), null);
});

test("eventText names what is gone", () => {
  assert.equal(eventText([room(9)], groups, devices), "Toggle a room that is gone");
  assert.equal(eventText([lamp({ ieee: "0x0000000000000001" })], groups, devices), "Toggle a light that is gone");
});

test("rowsUsed counts every row but a gesture's one empty row", () => {
  assert.equal(rowsUsed({ short: [blankRule()], long: [room(1), { ...blankRule(), and: true }] }), 2);
});

test("a rule that waits says so, and a dim never waits", () => {
  assert.equal(eventText([room(1, { do: "on" }), room(1, { and: true, do: "off", delay: 600 })], groups, devices),
    "Turn on Kitchen and turn off Kitchen after 10 min");
  const out = cleanRules([room(1, { do: "off", delay: 90 }), room(2, { do: "dim_up", value: 6, delay: 30 })]);
  assert.equal(out[0].delay, 90);
  assert.equal(out[1].delay, 0, "a dim is sent without its delay");
  assert.equal(cleanRules([room(1, { delay: 99999 })])[0].delay, 65535, "and none past what the coordinator stores");
});

test("delays read as minutes and hours", () => {
  assert.equal(delayLabel(30), "30 s");
  assert.equal(delayLabel(90), "1 min 30 s");
  assert.equal(delayLabel(600), "10 min");
  assert.equal(delayLabel(3600), "1 h");
  assert.equal(delayLabel(5400), "1 h 30 min");
});

test("the sun is a condition of the case", () => {
  assert.equal(eventText([room(1, { do: "on", when: "sun_down" }), room(1, { do: "set", value: 30 })], groups, devices),
    "Turn on Kitchen while the sun is down, otherwise Kitchen to 30 %");
});

test("one card per sensor endpoint, with its record or none", () => {
  const sensors = [
    { ieee: "0x00158d00014dc9e0", name: "Hall motion", endpoints: [{ ep: 1, features: ["motion"] }] },
    { ieee: "0x0017880106a1b2c3", name: "Hue motion", endpoints: [{ ep: 1, features: ["switch"] }, { ep: 2, features: ["motion"] }] },
    ...devices,
  ] as unknown as Device[];
  const rec = (id: number, ieee: string, ep: number) =>
    ({ id, source: "sensor", ieee, ep, events: { motion: [], clear: [] } }) as unknown as ButtonDef;
  const cards = sensorCards(sensors, [rec(1, "0x00158d00014dc9e0", 1), rec(2, "0x0000000000000001", 1),
    { id: 3, source: "zigbee", ieee: "0x8899AABBCCDDEEFF", ep: 1 } as unknown as ButtonDef]);
  assert.equal(cards.length, 3, "two sensors and a record whose sensor is gone; the remote's button is not one");
  assert.equal(cards[0].button?.id, 1);
  assert.equal(cards[1].ep, 2, "the Hue's motion endpoint, not its remote one");
  assert.equal(cards[1].button, null);
  assert.equal(cards[2].device, null);
});

test("a sensor's state counts down between polls", () => {
  const b = { occupied: true, clear_in_ms: 80000 } as unknown as ButtonDef;
  assert.equal(sensorState(b), "Motion · no-motion rules in 1:20");
  assert.equal(sensorState(b, 20000), "Motion · no-motion rules in 1:00");
  assert.equal(sensorState({ occupied: false, clear_in_ms: null } as unknown as ButtonDef), "No motion");
  assert.equal(sensorState({ occupied: null, clear_in_ms: 5000 } as unknown as ButtonDef), "Not heard since the coordinator started · no-motion rules in 0:05");
});
