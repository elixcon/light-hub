#!/usr/bin/env python3
"""Package this project for a stock Shelly Gen4's own firmware update page.

A Gen4 takes a zip: a manifest naming four parts - boot, pt, app and fs - and
it refuses a package that is missing any of them. `pt` carries a partition table
and an address to write it at; `app` goes into whichever app slot the unit is
not running from, which on this layout is `app_0` at 0x20000.

Two things the stock installer (firmware 2.0.0) will not do:

  - it will not take a table that moves `nvs` ("pt check failed : Cannot
    relocate nvs"), so it never takes ours;
  - it writes the pt wherever `addr` says, and our bootloader reads 0x8000.

So the package hands it **the unit's own table, written at 0x8000**, and the
updater as the app. Afterwards our bootloader finds a table it can boot from at
0x8000, the boot state the installer wrote points at `app_0`, and that is the
updater - which sees a layout that is not ours and offers the ordinary
conversion (updater/main/convert.c). The updater is already at the address our
factory partition starts at, so that conversion copies nothing: it writes the
real table and the bootloader and erases what is left of the old firmware.

The stock table has to be the one that unit is running. Get it from the package
Shelly would install itself:

    curl -sk https://updates.shelly.cloud/update/<app code>   # e.g. Mini1G4
    # take .stable.url from the answer, download that zip, pass it as --stock-zip

    idf.py build
    python3 tools/make-shelly-zip.py --hardware shelly-1-mini-gen4

Read the unit's code and slot from its stock firmware first:

    curl http://<ip>/rpc/Shelly.GetDeviceInfo

`app` is the hardware code (pass it as --app-code if it is not in the table
below) and `slot` must be 1: the stock installer writes the slot it is not
running from and will not target slot 1, so a factory unit on slot 0 needs any
stock update applied before this one. See docs/converting.md.

The manifest format is the one the two working third-party Gen4 projects use,
automatous-io/shelly-gen4-esphome and automatous-io/shelly-1-gen4-matter-thread
(both Apache-2.0); this is our own implementation of it. Both keep the stock
table at 0x10000; this package gives the `pt` part an address of its own.
"""
import argparse, datetime, hashlib, json, math, os, struct, sys, zipfile

# The stock Gen4 layout (8 MB, table at 0x10000), shared across the line. Only
# the names matter to the installer, but the addresses are what decides where
# the parts placed by name land in our own table - see the map this prints.
STOCK = {
    "otadata": (0x11000, 0x2000),
    "nvs":     (0x14000, 0xC000),
    "app_0":   (0x20000, 0x300000),
    "fs_0":    (0x320000, 0xE0000),
    "app_1":   (0x400000, 0x300000),
}

# Shelly's hardware codes. Only codes confirmed against real hardware belong
# here; read an unknown unit's from its stock web UI or Shelly.GetDeviceInfo.
HARDWARE = {
    "shelly-1-gen4":        "S1G4",
    "shelly-1pm-gen4":      "S1PMG4",
    "shelly-2pm-gen4":      "S2PMG4",
    "shelly-1-mini-gen4":   "Mini1G4",
    "shelly-1pm-mini-gen4": "Mini1PMG4",
}

PLATFORM = "esp32c6"
# Kept above any stock version, so the unit never reads the package as a
# downgrade. The real version is in the app descriptor and the filename.
MANIFEST_VER = "99.0.0"
# The whole package is plaintext. A unit with flash encryption on could not use
# it anyway - our bootloader carries no keys - and Gen4 units have none.
ENCRYPT = False

# The boot part's min_version. It does not stop the installer writing the
# bootloader (even at 0.0.0 it logs "update? 1"), so the one shipped has to boot
# the unit - see set_flash_size.
BOOT_MIN = "0.0.0"

IMAGE_MAGIC = 0xE9
APP_DESC_MAGIC = 0xABCD5432
UPDATER_PROJECT = "elixcon_light_hub_updater"
FLASH_LIMIT = 4 * 1024 * 1024   # what partitions.csv is built for


def die(msg):
    sys.exit("make-shelly-zip: " + msg)


def read(path, what):
    if not os.path.isfile(path):
        die(f"no {what} at {path} - run `idf.py build` first, or pass its path")
    with open(path, "rb") as f:
        return f.read()


def app_desc(image, path):
    """(project_name, version) from the esp_app_desc_t at offset 32."""
    if len(image) < 32 + 256 or image[0] != IMAGE_MAGIC:
        die(f"{path} does not start with an ESP32 application image")
    magic, = struct.unpack_from("<I", image, 32)
    if magic != APP_DESC_MAGIC:
        die(f"{path} has no app descriptor - this looks like a bootloader or a merged image")
    version = image[48:80].split(b"\0")[0].decode("ascii", "replace")
    project = image[80:112].split(b"\0")[0].decode("ascii", "replace")
    return project, version


# esp_image_header_t: byte 3 is the flash size (high nibble) and speed (low),
# byte 23 says whether a SHA-256 of the image is appended.
FLASH_NIBBLE = {1: 0, 2: 1, 4: 2, 8: 3, 16: 4, 32: 5, 64: 6, 128: 7}


def set_flash_size(image, mb, path):
    """The same header patch esptool makes when it writes at offset 0.

    The bootloader takes the chip size from its own header and refuses a
    partition table with any row past it (esp_partition_table_verify), so the
    one in the package has to be built for - or told about - the whole chip the
    unit's own table describes. Our own table never reaches past 4 MB, and the
    conversion writes the updater's own bootloader over this one.
    """
    if mb not in FLASH_NIBBLE:
        die(f"no image header flash size for {mb} MB")
    out = bytearray(image)
    out[3] = (FLASH_NIBBLE[mb] << 4) | (out[3] & 0x0F)
    if out[23] == 1:
        if hashlib.sha256(bytes(image[:-32])).digest() != image[-32:]:
            die(f"{path} says it carries a SHA-256 and does not; refusing to patch its header")
        out[-32:] = hashlib.sha256(bytes(out[:-32])).digest()
    return bytes(out)


def parse_table(blob, path):
    """[(label, type, subtype, addr, size)] from a binary partition table."""
    parts = []
    for off in range(0, len(blob) - 31, 32):
        e = blob[off:off + 32]
        magic, = struct.unpack_from("<H", e, 0)
        if magic == 0xFFFF:
            break
        if magic == 0xEBEB:     # the MD5 entry
            continue
        if magic != 0x50AA:
            die(f"{path} is not a partition table")
        ptype, subtype = e[2], e[3]
        addr, size = struct.unpack_from("<II", e, 4)
        parts.append((e[12:28].split(b"\0")[0].decode("ascii", "replace"), ptype, subtype, addr, size))
    if not parts:
        die(f"{path} holds no partitions")
    return parts


def rows_under(table, start, end, pt_addr):
    """Which rows of our table a written range lands in."""
    if end <= pt_addr:
        return "the bootloader"
    if start == pt_addr:
        return "the partition table itself"
    hit = []
    for label, _t, _s, addr, size in table:
        if start < addr + size and addr < end:
            whole = start <= addr and addr + size <= end
            hit.append(label if whole else label + " (part)")
    return ", ".join(hit) if hit else "nothing"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--hardware", choices=sorted(HARDWARE), help="Shelly model to stamp the package for")
    ap.add_argument("--app-code", help="Shelly hardware code, for a unit not in the table")
    ap.add_argument("--compatible", help="manifest 'compatible' (default: <app-code>*)")
    ap.add_argument("--version", help="version for the filename (default: the updater's own)")
    ap.add_argument("--build-dir", default="build", help="build directory (default: build)")
    ap.add_argument("--bootloader", help="override the bootloader carried but not written")
    ap.add_argument("--boot-min", default=BOOT_MIN,
                    help=f"boot part min_version (default: {BOOT_MIN}, below any stock version, so the "
                         f"installer carries our bootloader without writing it)")
    ap.add_argument("--table", help="override our own table (checked, not shipped)")
    ap.add_argument("--stock-zip", help="the Shelly firmware package this unit would install")
    ap.add_argument("--stock-pt", help="that package's partition-table.bin, if it is already unpacked")
    ap.add_argument("--app", help="override the updater image")
    ap.add_argument("--pt-addr", default=None, help="where the table is written (default: the build's own offset)")
    ap.add_argument("-o", "--out", help="output path (default: <build-dir>/<stem>.zip)")
    args = ap.parse_args()

    app_code = args.app_code or (args.hardware and HARDWARE[args.hardware])
    if not app_code:
        die("pass --hardware <model> or --app-code <code from the unit's stock firmware>")
    compatible = args.compatible or app_code + "*"
    hardware = args.hardware or app_code.lower()

    bd = args.build_dir
    boot = read(args.bootloader or os.path.join(bd, "bootloader", "bootloader.bin"), "bootloader")
    # Not shipped: what the conversion will install once the updater runs. It is
    # read to check that the address the package depends on is still ours.
    table_path = args.table or os.path.join(bd, "partition_table", "partition-table-full.bin")
    table_bin = read(table_path, "partition table")
    app_path = args.app or os.path.join(bd, "updater", "elixcon_light_hub_updater.bin")
    app = read(app_path, "updater image")

    # The unit's own table, which is what the pt part carries.
    if args.stock_pt:
        stock_bin = read(args.stock_pt, "stock partition table")
    elif args.stock_zip:
        with zipfile.ZipFile(args.stock_zip) as z:
            names = [n for n in z.namelist() if n.endswith("partition-table.bin")]
            if not names:
                die(f"{args.stock_zip} has no partition-table.bin - is it a Shelly firmware package?")
            stock_bin = z.read(names[0])
    else:
        die("pass --stock-zip <the firmware package Shelly would install on this unit> "
            "(curl -sk https://updates.shelly.cloud/update/<app code>, then download .stable.url), "
            "or --stock-pt <its partition-table.bin>")

    # Where our table belongs. Taken from the build, because the bootloader in
    # this package reads it from wherever that build put it.
    cfg_path = os.path.join(bd, "config", "sdkconfig.json")
    cfg = json.load(open(cfg_path)) if os.path.isfile(cfg_path) else {}
    built_at = cfg.get("PARTITION_TABLE_OFFSET")
    pt_addr = int(args.pt_addr, 0) if args.pt_addr else built_at
    if pt_addr is None:
        die(f"no PARTITION_TABLE_OFFSET in {cfg_path} - pass --pt-addr")
    if built_at is not None and pt_addr != built_at:
        die(f"--pt-addr 0x{pt_addr:x} is not where this build's bootloader looks for the table "
            f"(0x{built_at:x}); the device would not boot")

    # The checks. There is no second chance on the device: an installed package
    # that does not boot needs the UART pads.
    project, version = app_desc(app, app_path)
    if project != UPDATER_PROJECT:
        die(f"{app_path} is '{project}', not the updater - use "
            f"{os.path.join(bd, 'updater', UPDATER_PROJECT + '.bin')}")
    version = args.version or version

    stock = parse_table(stock_bin, args.stock_pt or args.stock_zip)
    stock_app = [p for p in stock if p[0] == "app_0"]
    if len(stock_app) != 1:
        die("the stock table has no app_0 row; the installer places the app by that name")
    stock_app_addr, stock_app_size = stock_app[0][3], stock_app[0][4]
    if not [p for p in stock if p[0] == "app_1"]:
        die("the stock table has no app_1 row, so the unit has no slot to be running from")
    if not [p for p in stock if p[0] == "fs_0"]:
        die("the stock table has no fs_0 row, and the package must carry an fs part")

    table = parse_table(table_bin, table_path)
    factory = [p for p in table if p[1] == 0x00 and p[2] == 0x00]
    if len(factory) != 1:
        die(f"{table_path} has {len(factory)} factory app partitions; the updater needs exactly one "
            f"(the full table, not the derived one the coordinator is built against)")
    label, _t, _s, faddr, fsize = factory[0]
    if faddr != stock_app_addr:
        die(f"'{label}' is at 0x{faddr:x}, but the package can only place the app where the stock "
            f"table's app_0 is, at 0x{stock_app_addr:x}")
    if len(app) > min(fsize, stock_app_size):
        die(f"the updater is {len(app)} bytes and does not fit ('{label}' is {fsize}, app_0 is {stock_app_size})")
    end = max(a + s for _l, _t, _s, a, s in table)
    if end > FLASH_LIMIT:
        die(f"the table reaches 0x{end:x}, past the {FLASH_LIMIT >> 20} MB this project is built for")
    if boot[0] != IMAGE_MAGIC:
        die("the bootloader does not start with an ESP32 image header")
    if len(boot) > pt_addr:
        die(f"the bootloader is {len(boot)} bytes and the table at 0x{pt_addr:x} would cut it")

    # The stock table describes the whole 8 MB chip, and the bootloader that
    # reads it must know that much flash, or it rejects every row past its own
    # idea of the chip and the unit does not boot.
    stock_end = max(a + sz for _l, _t, _s, a, sz in stock)
    need_mb = 1 << max(0, math.ceil(math.log2(max(stock_end, end) / (1024 * 1024))))
    boot = set_flash_size(boot, need_mb, args.bootloader or "the bootloader")

    # Its manifest parser reads {boot:{src,min_version},pt:{src,addr},app:{src},
    # fs:{src}} and nothing else, and a file in the zip that no part claims is a
    # file it can abort over. So the package carries those and no more; the
    # boot state is the installer's own business, which is why there is no
    # otadata part here.
    fs = b"\xff" * STOCK["fs_0"][1]

    # part name -> (member, bytes, address it is written at, extra manifest keys)
    #
    # The installer writes `boot` and `pt` at the raw addresses given and nothing
    # after them, so the parts placed by name go first, while the table on the
    # flash is still the stock one they are resolved against. The table switch
    # and the bootloader come last, in that order: until the bootloader is
    # replaced, the stock one still reads the stock table at 0x10000, which this
    # package never writes over.
    #
    # `app` and `fs` carry an address as well as a name because both readings
    # give the same answer - the stock `app_0` and our `updater` are both at
    # 0x20000 - so whichever the installer prefers, they land in the same place.
    parts = [
        # The backend refuses a package that is missing any of boot/pt/app/fs
        # ("Missing a part"). It compares `min_version` with the bootloader it
        # is running and logs "Boot: cur .., new .., min .., update? N", and it
        # writes this bootloader whatever min_version says - hence the patched
        # flash size above.
        ("boot", "bootloader.bin", boot, 0x0,
         {"type": "boot", "addr": 0x0, "min_version": args.boot_min, "encrypt": ENCRYPT}),
        # The unit's own table, at the offset our bootloader reads. Its content
        # is unchanged, so the installer's relocation checks have nothing to
        # object to; the copy is what our updater boots from afterwards, and the
        # conversion replaces it with the real one.
        ("pt", "partition-table.bin", stock_bin, pt_addr,
         {"type": "pt", "addr": pt_addr, "encrypt": ENCRYPT}),
        # The updater, into the slot the unit is not running from. The backend
        # chooses that itself - its manifest schema has no address for an app -
        # so the unit must be on slot 1 for this to land at app_0.
        ("app", "app.bin", app, stock_app_addr,
         {"type": "app", "encrypt": ENCRYPT}),
        # The installer wants an fs part. 0xFF over a partition we do not keep.
        ("fs", "fs.img", fs, STOCK["fs_0"][0],
         {"type": "fs", "fs_size": STOCK["fs_0"][1], "encrypt": ENCRYPT}),
    ]

    manifest_parts = {}
    for name, member, data, _addr, extra in parts:
        manifest_parts[name] = {"src": member, "size": len(data),
                                "cs_sha256": hashlib.sha256(data).hexdigest(), **extra}

    stem = f"elixcon-light-hub-{hardware}-v{version}-shelly-gen4-ota"
    now = datetime.datetime.now(datetime.timezone.utc)
    manifest = {
        "name": app_code,
        "platform": PLATFORM,
        "version": MANIFEST_VER,
        "build_id": now.strftime("%Y%m%d-%H%M%S") + f"/{stem}",
        "build_timestamp": now.strftime("%Y-%m-%dT%H:%M:%SZ"),
        "parts": manifest_parts,
        "compatible": compatible,
    }

    out = args.out or os.path.join(bd, stem + ".zip")
    blobs = {"manifest.json": json.dumps(manifest, indent=2).encode()}
    for _name, member, data, _addr, _extra in parts:
        blobs[member] = data
    order = ["manifest.json"] + [member for _n, member, _d, _a, _e in parts]
    with zipfile.ZipFile(out, "w", zipfile.ZIP_STORED) as z:
        for member in order:
            z.writestr(member, blobs[member])

    # Read the finished file back rather than trust what went in.
    with zipfile.ZipFile(out) as z:
        if z.namelist() != order:
            die(f"{out} came out as {z.namelist()}")
        got = json.loads(z.read("manifest.json"))
        for name, spec in got["parts"].items():
            if "src" not in spec:
                continue
            data = z.read(spec["src"])
            if len(data) != spec["size"] or hashlib.sha256(data).hexdigest() != spec["cs_sha256"]:
                die(f"the {name} part does not match its manifest entry in {out}")

    print(f"{out}")
    print(f"  {UPDATER_PROJECT} {version}, name={app_code}, compatible={compatible}, "
          f"{os.path.getsize(out) / 1024 / 1024:.1f} MB")
    print(f"  the unit must be running slot 1, and there is no way back without the UART pads")
    print(f"  the bootloader's header is set to {need_mb} MB, for the table it has to read")
    print(f"  then Convert on its page finishes the layout\n")
    print(f"  {'part':8} {'writes':>21}  lands in")
    for name, addr, length in [(n, a, len(d)) for n, _m, d, a, _e in parts]:
        print(f"  {name:8} 0x{addr:06x}-0x{addr + length:06x}  "
              f"{rows_under(table, addr, addr + length, pt_addr)}")


if __name__ == "__main__":
    main()
