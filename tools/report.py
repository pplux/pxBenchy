#!/usr/bin/env python3
"""Aggregate pxBenchy result files (menu Save / Share JSON) from many devices into one static HTML page.

usage: tools/report.py <results_dir> [-o report.html]

Reads every *.json under <results_dir> (recursive), groups files by device (os, manufacturer, model, GPU name),
keeps the Vulkan info of the newest file per device and every run of every file. The page has no external
dependencies: devices overview, per-device Vulkan explorer (vk11 / vk13) and run fps per script.
"""

import argparse
import datetime
import json
import pathlib
import sys

FORMATS = """
UNDEFINED R4G4_UNORM_PACK8 R4G4B4A4_UNORM_PACK16 B4G4R4A4_UNORM_PACK16 R5G6B5_UNORM_PACK16 B5G6R5_UNORM_PACK16
R5G5B5A1_UNORM_PACK16 B5G5R5A1_UNORM_PACK16 A1R5G5B5_UNORM_PACK16 R8_UNORM R8_SNORM R8_USCALED R8_SSCALED
R8_UINT R8_SINT R8_SRGB R8G8_UNORM R8G8_SNORM R8G8_USCALED R8G8_SSCALED R8G8_UINT R8G8_SINT R8G8_SRGB
R8G8B8_UNORM R8G8B8_SNORM R8G8B8_USCALED R8G8B8_SSCALED R8G8B8_UINT R8G8B8_SINT R8G8B8_SRGB B8G8R8_UNORM
B8G8R8_SNORM B8G8R8_USCALED B8G8R8_SSCALED B8G8R8_UINT B8G8R8_SINT B8G8R8_SRGB R8G8B8A8_UNORM R8G8B8A8_SNORM
R8G8B8A8_USCALED R8G8B8A8_SSCALED R8G8B8A8_UINT R8G8B8A8_SINT R8G8B8A8_SRGB B8G8R8A8_UNORM B8G8R8A8_SNORM
B8G8R8A8_USCALED B8G8R8A8_SSCALED B8G8R8A8_UINT B8G8R8A8_SINT B8G8R8A8_SRGB A8B8G8R8_UNORM_PACK32
A8B8G8R8_SNORM_PACK32 A8B8G8R8_USCALED_PACK32 A8B8G8R8_SSCALED_PACK32 A8B8G8R8_UINT_PACK32
A8B8G8R8_SINT_PACK32 A8B8G8R8_SRGB_PACK32 A2R10G10B10_UNORM_PACK32 A2R10G10B10_SNORM_PACK32
A2R10G10B10_USCALED_PACK32 A2R10G10B10_SSCALED_PACK32 A2R10G10B10_UINT_PACK32 A2R10G10B10_SINT_PACK32
A2B10G10R10_UNORM_PACK32 A2B10G10R10_SNORM_PACK32 A2B10G10R10_USCALED_PACK32 A2B10G10R10_SSCALED_PACK32
A2B10G10R10_UINT_PACK32 A2B10G10R10_SINT_PACK32 R16_UNORM R16_SNORM R16_USCALED R16_SSCALED R16_UINT R16_SINT
R16_SFLOAT R16G16_UNORM R16G16_SNORM R16G16_USCALED R16G16_SSCALED R16G16_UINT R16G16_SINT R16G16_SFLOAT
R16G16B16_UNORM R16G16B16_SNORM R16G16B16_USCALED R16G16B16_SSCALED R16G16B16_UINT R16G16B16_SINT
R16G16B16_SFLOAT R16G16B16A16_UNORM R16G16B16A16_SNORM R16G16B16A16_USCALED R16G16B16A16_SSCALED
R16G16B16A16_UINT R16G16B16A16_SINT R16G16B16A16_SFLOAT R32_UINT R32_SINT R32_SFLOAT R32G32_UINT R32G32_SINT
R32G32_SFLOAT R32G32B32_UINT R32G32B32_SINT R32G32B32_SFLOAT R32G32B32A32_UINT R32G32B32A32_SINT
R32G32B32A32_SFLOAT R64_UINT R64_SINT R64_SFLOAT R64G64_UINT R64G64_SINT R64G64_SFLOAT R64G64B64_UINT
R64G64B64_SINT R64G64B64_SFLOAT R64G64B64A64_UINT R64G64B64A64_SINT R64G64B64A64_SFLOAT
B10G11R11_UFLOAT_PACK32 E5B9G9R9_UFLOAT_PACK32 D16_UNORM X8_D24_UNORM_PACK32 D32_SFLOAT S8_UINT
D16_UNORM_S8_UINT D24_UNORM_S8_UINT D32_SFLOAT_S8_UINT BC1_RGB_UNORM_BLOCK BC1_RGB_SRGB_BLOCK
BC1_RGBA_UNORM_BLOCK BC1_RGBA_SRGB_BLOCK BC2_UNORM_BLOCK BC2_SRGB_BLOCK BC3_UNORM_BLOCK BC3_SRGB_BLOCK
BC4_UNORM_BLOCK BC4_SNORM_BLOCK BC5_UNORM_BLOCK BC5_SNORM_BLOCK BC6H_UFLOAT_BLOCK BC6H_SFLOAT_BLOCK
BC7_UNORM_BLOCK BC7_SRGB_BLOCK ETC2_R8G8B8_UNORM_BLOCK ETC2_R8G8B8_SRGB_BLOCK ETC2_R8G8B8A1_UNORM_BLOCK
ETC2_R8G8B8A1_SRGB_BLOCK ETC2_R8G8B8A8_UNORM_BLOCK ETC2_R8G8B8A8_SRGB_BLOCK EAC_R11_UNORM_BLOCK
EAC_R11_SNORM_BLOCK EAC_R11G11_UNORM_BLOCK EAC_R11G11_SNORM_BLOCK ASTC_4x4_UNORM_BLOCK ASTC_4x4_SRGB_BLOCK
ASTC_5x4_UNORM_BLOCK ASTC_5x4_SRGB_BLOCK ASTC_5x5_UNORM_BLOCK ASTC_5x5_SRGB_BLOCK ASTC_6x5_UNORM_BLOCK
ASTC_6x5_SRGB_BLOCK ASTC_6x6_UNORM_BLOCK ASTC_6x6_SRGB_BLOCK ASTC_8x5_UNORM_BLOCK ASTC_8x5_SRGB_BLOCK
ASTC_8x6_UNORM_BLOCK ASTC_8x6_SRGB_BLOCK ASTC_8x8_UNORM_BLOCK ASTC_8x8_SRGB_BLOCK ASTC_10x5_UNORM_BLOCK
ASTC_10x5_SRGB_BLOCK ASTC_10x6_UNORM_BLOCK ASTC_10x6_SRGB_BLOCK ASTC_10x8_UNORM_BLOCK ASTC_10x8_SRGB_BLOCK
ASTC_10x10_UNORM_BLOCK ASTC_10x10_SRGB_BLOCK ASTC_12x10_UNORM_BLOCK ASTC_12x10_SRGB_BLOCK
ASTC_12x12_UNORM_BLOCK ASTC_12x12_SRGB_BLOCK
""".split()

# Bit flag tables, [bit, short, long]; from vulkan_core.h.
TABLES = {
    "formats": FORMATS,
    "formatFeatures": [
        [0x1, "S", "SAMPLED_IMAGE"], [0x2, "SI", "STORAGE_IMAGE"], [0x4, "SIA", "STORAGE_IMAGE_ATOMIC"],
        [0x8, "UTB", "UNIFORM_TEXEL_BUFFER"], [0x10, "STB", "STORAGE_TEXEL_BUFFER"],
        [0x20, "STBA", "STORAGE_TEXEL_BUFFER_ATOMIC"], [0x40, "VB", "VERTEX_BUFFER"],
        [0x80, "CA", "COLOR_ATTACHMENT"], [0x100, "CAB", "COLOR_ATTACHMENT_BLEND"],
        [0x200, "DS", "DEPTH_STENCIL_ATTACHMENT"], [0x400, "BS", "BLIT_SRC"], [0x800, "BD", "BLIT_DST"],
        [0x1000, "FL", "SAMPLED_IMAGE_FILTER_LINEAR"], [0x2000, "FC", "SAMPLED_IMAGE_FILTER_CUBIC"],
        [0x4000, "TS", "TRANSFER_SRC"], [0x8000, "TD", "TRANSFER_DST"],
        [0x10000, "MM", "SAMPLED_IMAGE_FILTER_MINMAX"], [0x20000, "MCS", "MIDPOINT_CHROMA_SAMPLES"],
        [0x40000, "YLF", "SAMPLED_IMAGE_YCBCR_CONVERSION_LINEAR_FILTER"],
        [0x80000, "YSR", "SAMPLED_IMAGE_YCBCR_CONVERSION_SEPARATE_RECONSTRUCTION_FILTER"],
        [0x100000, "YCE", "SAMPLED_IMAGE_YCBCR_CONVERSION_CHROMA_RECONSTRUCTION_EXPLICIT"],
        [0x200000, "YCEF", "SAMPLED_IMAGE_YCBCR_CONVERSION_CHROMA_RECONSTRUCTION_EXPLICIT_FORCEABLE"],
        [0x400000, "DJ", "DISJOINT"], [0x800000, "CCS", "COSITED_CHROMA_SAMPLES"],
        [0x1000000, "FDM", "FRAGMENT_DENSITY_MAP"], [0x2000000, "VDO", "VIDEO_DECODE_OUTPUT"],
        [0x4000000, "VDD", "VIDEO_DECODE_DPB"], [0x8000000, "VEI", "VIDEO_ENCODE_INPUT"],
        [0x10000000, "VED", "VIDEO_ENCODE_DPB"], [0x20000000, "ASV", "ACCELERATION_STRUCTURE_VERTEX_BUFFER"],
        [0x40000000, "FSR", "FRAGMENT_SHADING_RATE_ATTACHMENT"],
    ],
    "queueFlags": [
        [0x1, "G", "GRAPHICS"], [0x2, "C", "COMPUTE"], [0x4, "T", "TRANSFER"], [0x8, "SB", "SPARSE_BINDING"],
        [0x10, "P", "PROTECTED"], [0x20, "VD", "VIDEO_DECODE"], [0x40, "VE", "VIDEO_ENCODE"],
        [0x100, "OF", "OPTICAL_FLOW"],
    ],
    "memoryProperty": [
        [0x1, "DEVICE_LOCAL", ""], [0x2, "HOST_VISIBLE", ""], [0x4, "HOST_COHERENT", ""], [0x8, "HOST_CACHED", ""],
        [0x10, "LAZILY_ALLOCATED", ""], [0x20, "PROTECTED", ""], [0x40, "DEVICE_COHERENT_AMD", ""],
        [0x80, "DEVICE_UNCACHED_AMD", ""], [0x100, "RDMA_CAPABLE_NV", ""],
    ],
    "memoryHeap": [[0x1, "DEVICE_LOCAL", ""], [0x2, "MULTI_INSTANCE", ""]],
    "shaderStages": [
        [0x1, "VERTEX", ""], [0x2, "TESS_CONTROL", ""], [0x4, "TESS_EVAL", ""], [0x8, "GEOMETRY", ""],
        [0x10, "FRAGMENT", ""], [0x20, "COMPUTE", ""], [0x40, "TASK", ""], [0x80, "MESH", ""],
        [0x100, "RAYGEN", ""], [0x200, "ANY_HIT", ""], [0x400, "CLOSEST_HIT", ""], [0x800, "MISS", ""],
        [0x1000, "INTERSECTION", ""], [0x2000, "CALLABLE", ""],
    ],
    "subgroupOps": [
        [0x1, "BASIC", ""], [0x2, "VOTE", ""], [0x4, "ARITHMETIC", ""], [0x8, "BALLOT", ""], [0x10, "SHUFFLE", ""],
        [0x20, "SHUFFLE_RELATIVE", ""], [0x40, "CLUSTERED", ""], [0x80, "QUAD", ""], [0x100, "PARTITIONED_NV", ""],
        [0x200, "ROTATE", ""], [0x400, "ROTATE_CLUSTERED", ""],
    ],
    "resolveModes": [[0x1, "SAMPLE_ZERO", ""], [0x2, "AVERAGE", ""], [0x4, "MIN", ""], [0x8, "MAX", ""]],
    "deviceType": ["other", "integrated", "discrete", "virtual", "cpu"],
    "driverId": {
        1: "AMD_PROPRIETARY", 2: "AMD_OPEN_SOURCE", 3: "MESA_RADV", 4: "NVIDIA_PROPRIETARY",
        5: "INTEL_PROPRIETARY_WINDOWS", 6: "INTEL_OPEN_SOURCE_MESA", 7: "IMAGINATION_PROPRIETARY",
        8: "QUALCOMM_PROPRIETARY", 9: "ARM_PROPRIETARY", 10: "GOOGLE_SWIFTSHADER", 11: "GGP_PROPRIETARY",
        12: "BROADCOM_PROPRIETARY", 13: "MESA_LLVMPIPE", 14: "MOLTENVK", 15: "COREAVI_PROPRIETARY",
        16: "JUICE_PROPRIETARY", 17: "VERISILICON_PROPRIETARY", 18: "MESA_TURNIP", 19: "MESA_V3DV",
        20: "MESA_PANVK", 21: "SAMSUNG_PROPRIETARY", 22: "MESA_VENUS", 23: "MESA_DOZEN", 24: "MESA_NVK",
        25: "IMAGINATION_OPEN_SOURCE_MESA", 26: "MESA_HONEYKRISP", 27: "VULKAN_SC_EMULATION_ON_VULKAN",
        28: "MESA_KOSMICKRISP", 29: "MESA_GFXSTREAM", 30: "APE_SOFT",
    },
}


def device_key(info):
    return tuple(info.get(k, "") for k in ("os", "manufacturer", "model", "name"))


def pick_physical_device(mode, name):
    # The Vulkan info lists every physical device of that version; keep the one the runs used.
    if not mode or not mode.get("devices"):
        return None
    devs = mode["devices"]
    dev = next((d for d in devs if d.get("deviceName") == name), devs[0])
    return {
        "instance_version": mode.get("instance_version"),
        "instance_extensions": mode.get("instance_extensions", {}),
        "layers": mode.get("layers", []),
        "others": [d.get("deviceName") for d in devs if d is not dev],
        "device": dev,
    }


def load(results_dir):
    devices = {}
    for path in sorted(pathlib.Path(results_dir).rglob("*.json")):
        try:
            doc = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, ValueError) as e:
            print(f"skip {path}: {e}", file=sys.stderr)
            continue
        if not isinstance(doc, dict) or "pxbenchy" not in doc:
            print(f"skip {path}: not a pxBenchy result file", file=sys.stderr)
            continue
        runs = doc.get("runs") or []
        info = doc.get("device") or next((r.get("device_info") for r in runs if r.get("device_info")), None)
        if not info:
            print(f"skip {path}: no device_info", file=sys.stderr)
            continue
        rel = str(path.relative_to(results_dir))
        mtime = path.stat().st_mtime
        dev = devices.setdefault(device_key(info), {"files": [], "runs": [], "_vk_mtime": -1})
        dev["files"].append(rel)
        if mtime >= dev.get("_info_mtime", -1):
            dev["info"], dev["_info_mtime"] = info, mtime
        vk = doc.get("vulkan")
        if vk and mtime >= dev["_vk_mtime"]:
            dev["vulkan"] = {m: pick_physical_device(vk.get(m), info.get("name")) for m in ("vk11", "vk13")}
            dev["_vk_mtime"] = mtime
        for r in runs:
            r = dict(r)
            r["file"] = rel
            dev["runs"].append(r)
    out = []
    for dev in devices.values():
        info = dev["info"]
        label = " ".join(x for x in (info.get("manufacturer"), info.get("model")) if x) or info.get("name", "?")
        out.append({
            "label": label,
            "info": info,
            "files": dev["files"],
            "runs": dev["runs"],
            "vulkan": dev.get("vulkan") or {"vk11": None, "vk13": None},
        })
    out.sort(key=lambda d: (d["info"].get("os", ""), d["label"].lower()))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("results_dir")
    ap.add_argument("-o", "--output", help="output HTML (default: <results_dir>/report.html)")
    args = ap.parse_args()

    devices = load(args.results_dir)
    if not devices:
        sys.exit(f"no pxBenchy result files in {args.results_dir}")
    data = {
        "generated": datetime.datetime.now().strftime("%Y-%m-%d %H:%M"),
        "devices": devices,
        "tables": TABLES,
    }
    payload = json.dumps(data, separators=(",", ":")).replace("</", "<\\/")
    out = pathlib.Path(args.output or pathlib.Path(args.results_dir) / "report.html")
    out.write_text(PAGE.replace("/*DATA*/", payload), encoding="utf-8")
    nruns = sum(len(d["runs"]) for d in devices)
    print(f"{out}: {len(devices)} devices, {nruns} runs")


PAGE = r"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>pxBenchy Results</title>
<style>
:root {
  --bg: #fafaf9; --panel: #ffffff; --fg: #1c1917; --muted: #78716c; --line: #e7e5e4; --head: #f5f5f4;
  --accent: #2563eb; --yes: #15803d; --yes-bg: #dcfce7; --no: #b91c1c; --no-bg: #fee2e2; --diff: #fef9c3; --zebra: #f8f8f7; --cpu: #fff4e8; --gpu: #f5f1ff;
}
@media (prefers-color-scheme: dark) {
  :root {
    --bg: #0c0a09; --panel: #1c1917; --fg: #e7e5e4; --muted: #a8a29e; --line: #292524; --head: #231f1d;
    --accent: #60a5fa; --yes: #4ade80; --yes-bg: #14532d55; --no: #f87171; --no-bg: #7f1d1d55; --diff: #42381055; --zebra: #211e1c; --cpu: #2a1f15; --gpu: #221d30;
  }
}
* { box-sizing: border-box; }
body { margin: 0; background: var(--bg); color: var(--fg); font: 14px/1.45 system-ui, -apple-system, sans-serif; }
header { position: sticky; top: 0; z-index: 5; background: var(--panel); border-bottom: 1px solid var(--line);
  display: flex; gap: 20px; align-items: center; padding: 10px 16px; flex-wrap: wrap; }
header b { font-size: 15px; }
header nav a { margin-right: 14px; }
header .gen { color: var(--muted); margin-left: auto; font-size: 12px; }
main { padding: 16px; max-width: 1600px; margin: 0 auto; }
a { color: var(--accent); text-decoration: none; }
a:hover { text-decoration: underline; }
nav a.on { font-weight: 600; text-decoration: underline; }
h1 { font-size: 20px; margin: 4px 0 4px; }
h2 { font-size: 16px; margin: 0; display: inline; }
.muted { color: var(--muted); }
.cards { display: flex; gap: 12px; flex-wrap: wrap; margin: 8px 0 16px; }
.card { background: var(--panel); border: 1px solid var(--line); border-radius: 8px; padding: 10px 14px; min-width: 120px; }
.card .n { font-size: 22px; font-weight: 600; }
.wrap { overflow-x: auto; border: 1px solid var(--line); border-radius: 8px; background: var(--panel); }
table { border-collapse: collapse; width: 100%; }
th, td { padding: 4px 8px; border-bottom: 1px solid var(--line); text-align: left; vertical-align: top; }
th { background: var(--head); font-weight: 600; position: sticky; top: 0; white-space: nowrap; }
th.sort { cursor: pointer; }
th + th, td + td { border-left: 1px solid var(--line); }
tbody tr:nth-child(even) td { background: var(--zebra); }
tbody tr.cpu td, svg.chart.cpu { background: var(--cpu); }
tbody tr.gpu td, svg.chart.gpu { background: var(--gpu); }
td.num { text-align: right; font-variant-numeric: tabular-nums; }
td.k { font-family: ui-monospace, SFMono-Regular, Menlo, monospace; font-size: 12.5px; white-space: nowrap; width: 1%; }
td.v { font-family: ui-monospace, SFMono-Regular, Menlo, monospace; font-size: 12.5px; }
tr.click { cursor: pointer; }
tr.click:hover td { background: var(--head); }
tr.diff td.k { background: var(--diff); }
.yes { color: var(--yes); background: var(--yes-bg); border-radius: 4px; padding: 0 5px; }
.no { color: var(--no); background: var(--no-bg); border-radius: 4px; padding: 0 5px; }
.miss { color: var(--muted); }
.tag { display: inline-block; border: 1px solid var(--line); border-radius: 4px; padding: 0 5px; margin: 1px 2px 1px 0;
  font-size: 12px; white-space: nowrap; background: var(--head); }
.tag.warn { border-color: var(--no); color: var(--no); }
.tag.ver { border-color: var(--accent); color: var(--accent); font-size: 11px; }
dialog#run { width: min(1200px, 96vw); max-height: 92vh; padding: 0; border: 1px solid var(--line); border-radius: 10px;
  background: var(--bg); color: var(--fg); }
dialog#run::backdrop { background: #0008; }
dialog#run .hd { position: sticky; top: 0; z-index: 2; display: flex; gap: 10px; align-items: center; padding: 10px 14px;
  background: var(--panel); border-bottom: 1px solid var(--line); }
dialog#run .hd button { margin-left: auto; background: none; border: 0; color: var(--fg); font-size: 20px; cursor: pointer; }
dialog#run .wrap { margin: 0 12px; }
.runrow { padding: 6px 12px; border-top: 1px solid var(--line); cursor: pointer; }
.runrow:hover { background: var(--head); }
details { background: var(--panel); border: 1px solid var(--line); border-radius: 8px; margin: 10px 0; }
details > summary { padding: 8px 12px; cursor: pointer; }
details > .wrap { border: 0; border-top: 1px solid var(--line); border-radius: 0 0 8px 8px; max-height: 70vh; overflow: auto; }
.bar { display: flex; gap: 12px; align-items: center; flex-wrap: wrap; margin: 10px 0; }
.bar input[type=search] { padding: 5px 8px; border: 1px solid var(--line); border-radius: 6px; background: var(--panel);
  color: var(--fg); min-width: 240px; }
.bar select { padding: 4px; background: var(--panel); color: var(--fg); border: 1px solid var(--line); border-radius: 6px; }
.seg a { padding: 3px 9px; border: 1px solid var(--line); margin-right: -1px; background: var(--panel); }
.seg a.on { background: var(--accent); color: var(--bg); text-decoration: none; }
.devpick label { margin-right: 10px; white-space: nowrap; font-size: 13px; }
.pct { display: inline-block; height: 8px; background: var(--accent); border-radius: 2px; vertical-align: middle; margin-right: 6px; }
.compare td.c { text-align: center; white-space: nowrap; font-family: ui-monospace, Menlo, monospace; font-size: 12px; }
.compare th.c { writing-mode: vertical-rl; transform: rotate(180deg); text-align: left; max-height: 220px; font-weight: 500; }
.legend { font-size: 12px; color: var(--muted); }
.tag.st1 { border-color: #d97706; color: #d97706; }
.tag.st2 { border-color: var(--no); color: var(--no); background: var(--no-bg); }
.charts { display: grid; grid-template-columns: repeat(auto-fill, minmax(380px, 1fr)); gap: 8px; padding: 8px 12px; }
svg.chart { width: 100%; height: auto; background: var(--panel); border: 1px solid var(--line); border-radius: 6px; }
svg.chart .ln { fill: none; stroke-width: 1.5; vector-effect: non-scaling-stroke; }
svg.chart .band { stroke: none; opacity: 0.18; }
svg.chart .c1 { stroke: #dc2626; fill: #dc2626; }
svg.chart .c2 { stroke: var(--accent); fill: var(--accent); }
svg.chart .c3 { stroke: #059669; fill: #059669; }
svg.chart .ln.c1, svg.chart .ln.c2, svg.chart .ln.c3 { fill: none; }
svg.chart rect.st1 { fill: #f59e0b; opacity: 0.18; }
svg.chart rect.st2 { fill: #dc2626; opacity: 0.22; }
svg.chart rect.hit { fill: transparent; }
svg.chart rect.hit.on { fill: var(--fg); opacity: 0.08; }
svg.chart .ax { stroke: var(--line); vector-effect: non-scaling-stroke; }
svg.chart text { fill: var(--muted); font-size: 10px; }
svg.chart .ttl { fill: var(--fg); font-size: 11px; }
</style>
</head>
<body>
<header>
  <b>pxBenchy results</b>
  <nav id="nav"></nav>
  <span class="gen" id="gen"></span>
</header>
<main id="app"></main>
<dialog id="run" onclick="if (event.target === this) this.close()"></dialog>
<script id="data" type="application/json">/*DATA*/</script>
<script>
"use strict";
const D = JSON.parse(document.getElementById("data").textContent);
const T = D.tables;
const DEVS = D.devices;
const MODES = ["vk11", "vk13"];
const app = document.getElementById("app");
document.getElementById("gen").textContent = "generated " + D.generated;

// ---------- helpers

const esc = s => String(s).replace(/[&<>"]/g, c => ({"&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;"}[c]));
const bits = (v, table, useLong) => {
  const out = [];
  let rest = v;
  for (const [b, s, l] of table) { if (v & b) { out.push(useLong && l ? l : s); rest &= ~b; } }
  if (rest) out.push("0x" + (rest >>> 0).toString(16));
  return out;
};
const tags = list => list.map(x => `<span class="tag">${esc(x)}</span>`).join("") || '<span class="miss">none</span>';
const fmtBytes = n => {
  const u = ["B", "KiB", "MiB", "GiB", "TiB"];
  let i = 0;
  while (n >= 1024 && i < u.length - 1) { n /= 1024; i++; }
  return (i ? n.toFixed(n < 10 ? 2 : 1) : n) + " " + u[i];
};
const sampleCounts = v => { const o = []; for (let b = 1; b <= 64; b <<= 1) { if (v & b) o.push(b); } return o.join(",") || "0"; };

// Value formatting by field name: decodes the raw flags / enums the app saves.
function fmtVal(key, v) {
  if (v === undefined) return '<span class="miss">-</span>';
  if (v === null) return '<span class="miss">null</span>';
  if (v === true) return '<span class="yes">yes</span>';
  if (v === false) return '<span class="no">no</span>';
  if (typeof v === "number") {
    if (/SampleCounts$/.test(key)) return esc(sampleCounts(v));
    if (/(supportedStages|SupportedStages|requiredSubgroupSizeStages)$/.test(key)) return tags(bits(v, T.shaderStages));
    if (/(supportedOperations|SupportedOperations)$/.test(key)) return tags(bits(v, T.subgroupOps));
    if (/ResolveModes$/.test(key)) return tags(bits(v, T.resolveModes));
    if (key === "driverID") return esc(T.driverId[v] || v) + ` <span class="muted">(${v})</span>`;
    if (key === "deviceType") return esc(T.deviceType[v] || v);
    if (/(Size|Alignment|Range)$/.test(key) && v >= 4096 && v % 1024 === 0) return esc(v) + ` <span class="muted">${fmtBytes(v)}</span>`;
    return esc(v);
  }
  if (Array.isArray(v)) return esc(v.join(" x "));
  if (typeof v === "object") return esc(JSON.stringify(v));
  return esc(v);
}

function kvTable(obj, cls) {
  const rows = Object.entries(obj || {}).map(([k, v]) =>
    `<tr class="f"><td class="k">${esc(k)}</td><td class="v">${fmtVal(k, v)}</td></tr>`).join("");
  return `<div class="wrap ${cls || ""}"><table><tbody>${rows}</tbody></table></div>`;
}
function section(title, body, open, id, badge) {
  return `<details ${open ? "open" : ""} ${id ? `id="${id}"` : ""}><summary><h2>${esc(title)}</h2> ${badge || ""}</summary>${body}</details>`;
}
// vk11 and vk13 in one table. perMode: [[mode, rows]] (rows null = mode absent), each row [key, [[tdClass, html], ...]].
// A row only one mode has gets a version badge on its first cell; a cell whose html differs shows "a [1.1] / b [1.3]".
const VER = {vk11: "1.1", vk13: "1.3"};
const verTag = ms => ms.map(m => `<span class="tag ver">${VER[m]}</span>`).join("");
function mergeTable(perMode, head, sortKeys) {
  const have = perMode.filter(([, rows]) => rows);
  const maps = have.map(([m, rows]) => [m, new Map(rows)]);
  let keys = [...new Set(have.flatMap(([, rows]) => rows.map(r => r[0])))];
  if (sortKeys) keys = keys.sort(sortKeys);
  const body = keys.map(k => {
    const in_ = maps.filter(([, mp]) => mp.has(k));
    let diff = false;
    const cells = in_[0][1].get(k).map(([cls], c) => {
      const vals = in_.map(([m, mp]) => [m, mp.get(k)[c][1]]);
      if (vals.every(([, v]) => v === vals[0][1])) return [cls, vals[0][1]];
      diff = true;
      return [cls, vals.map(([m, v]) => `${v} <span class="muted">[${VER[m]}]</span>`).join(" / ")];
    });
    if (in_.length < maps.length) cells[0] = [cells[0][0], `${cells[0][1]} ${verTag(in_.map(([m]) => m))}`];
    return `<tr class="f ${diff ? "diff" : ""}">${cells.map(([cls, h]) => `<td class="${cls}">${h}</td>`).join("")}</tr>`;
  }).join("");
  const th = head ? `<thead><tr>${head.map(h => `<th>${h}</th>`).join("")}</tr></thead>` : "";
  return `<div class="wrap"><table>${th}<tbody>${body}</tbody></table></div>`;
}
const kvRows = obj => obj && Object.entries(obj).map(([k, v]) => [k, [["k", esc(k)], ["v", fmtVal(k, v)]]]);
function vkDevice(dev, mode) { const m = dev.vulkan[mode]; return m && m.device; }
function maxApi(dev) {
  for (const m of ["vk13", "vk11"]) { const d = vkDevice(dev, m); if (d) return d.apiVersion; }
  return dev.info.api_version || "";
}
function driverStr(info) {
  return [info.driver_name, info.driver_info || info.driver_version_decoded].filter(Boolean).join(" ");
}
function osStr(info) {
  if (info.os === "android") return `Android ${info.os_version || "?"}` + (info.sdk ? ` (sdk ${info.sdk})` : "");
  if (info.os === "macos") return "macOS " + (info.os_version || "");
  return info.os || "";
}
// Runs are captured in Release without validation or Tracy: flag only the exceptions.
function buildWarn(r) {
  const di = r.device_info || {};
  const w = [di.build_type && di.build_type !== "Release" && di.build_type, r.validation && "validation", di.tracy && "tracy"];
  return w.filter(Boolean).map(x => `<span class="tag warn" title="not a Release capture">${esc(x)}</span>`).join("");
}
function apiTag(i, mode) {
  return DEVS[i].vulkan[mode] ? `<a class="tag" href="#/device/${i}/${mode}">${mode}</a>` : "";
}

// Sortable tables: click a th.sort; numeric when both cells parse as numbers.
document.addEventListener("click", e => {
  const th = e.target.closest("th.sort");
  if (!th) return;
  const table = th.closest("table"), idx = [...th.parentNode.children].indexOf(th);
  const dir = th.dataset.dir === "1" ? -1 : 1;
  th.parentNode.querySelectorAll("th").forEach(x => delete x.dataset.dir);
  th.dataset.dir = dir === 1 ? "1" : "-1";
  const tb = table.tBodies[0];
  const val = tr => tr.children[idx].dataset.s ?? tr.children[idx].textContent.trim();
  [...tb.rows].sort((a, b) => {
    const x = val(a), y = val(b), nx = parseFloat(x), ny = parseFloat(y);
    return dir * (!isNaN(nx) && !isNaN(ny) ? nx - ny : x.localeCompare(y, undefined, {numeric: true}));
  }).forEach(tr => tb.appendChild(tr));
});

// Row filter for every tr.f under app.
function applyFilter(q) {
  q = q.toLowerCase();
  app.querySelectorAll("tr.f").forEach(tr => { tr.style.display = !q || tr.textContent.toLowerCase().includes(q) ? "" : "none"; });
}

// ---------- overview

function viewOverview() {
  const nruns = DEVS.reduce((a, d) => a + d.runs.length, 0);
  const nfiles = DEVS.reduce((a, d) => a + d.files.length, 0);
  const scripts = new Set(DEVS.flatMap(d => d.runs.map(r => r.script)));
  const n13 = DEVS.filter(d => d.vulkan.vk13).length;
  const rows = DEVS.map((d, i) => {
    const info = d.info, vd = vkDevice(d, "vk13") || vkDevice(d, "vk11");
    return `<tr class="click f" onclick="location.hash='#/device/${i}'">
      <td><a href="#/device/${i}">${esc(d.label)}</a></td>
      <td>${esc([info.soc_manufacturer, info.soc_model].filter(Boolean).join(" "))}</td>
      <td>${esc(info.name || "")}</td>
      <td>${esc(driverStr(info))}</td>
      <td data-s="${esc(maxApi(d))}">${esc(maxApi(d))}</td>
      <td>${apiTag(i, "vk11")}${apiTag(i, "vk13")}</td>
      <td>${esc(osStr(info))}</td>
      <td class="num">${vd ? Object.keys(vd.extensions || {}).length : ""}</td>
      <td class="num">${d.runs.length}</td><td class="num">${d.files.length}</td></tr>`;
  }).join("");
  const heads = ["Device", "SoC", "GPU", "Driver", "Max API", "Vulkan info", "OS", "Dev ext", "Runs", "Files"];
  app.innerHTML = `
    <div class="cards">
      <div class="card"><div class="n">${DEVS.length}</div>devices</div>
      <div class="card"><div class="n">${n13}</div>with a 1.3 instance</div>
      <div class="card"><div class="n">${scripts.size}</div>scripts</div>
      <div class="card"><div class="n">${nruns}</div>runs</div>
      <div class="card"><div class="n">${nfiles}</div>files</div>
    </div>
    <div class="bar"><input type="search" placeholder="filter devices" oninput="applyFilter(this.value)"></div>
    <div class="wrap"><table><thead><tr>${heads.map(h => `<th class="sort">${h}</th>`).join("")}</tr></thead>
    <tbody>${rows}</tbody></table></div>`;
}

// ---------- device

function memoryRows(d) {
  return [...(d.memoryHeaps || []).map((h, i) => [`heap ${i}`, [["k", `heap ${i}`], ["v", `${fmtBytes(h.size)} ${tags(bits(h.flags, T.memoryHeap))}`]]]),
    ...(d.memoryTypes || []).map((t, i) => [`type ${i}`, [["k", `type ${i}`], ["v", `heap ${t.heapIndex} ${tags(bits(t.propertyFlags, T.memoryProperty))}`]]])];
}
const QUEUE_HEAD = ["#", "flags", "count", "timestamp bits", "min transfer granularity"];
const queueRows = d => (d.queueFamilies || []).map((q, i) => [i, [["num", i], ["", tags(bits(q.queueFlags, T.queueFlags, true))],
  ["num", q.queueCount], ["num", q.timestampValidBits], ["", esc((q.minImageTransferGranularity || []).join(" x "))]]]);
function featTags(v) {
  return bits(v, T.formatFeatures).map(s => `<span class="tag" title="${esc(T.formatFeatures.find(f => f[1] === s)?.[2] || s)}">${s}</span>`).join("");
}
const FMT_LEGEND = `<div class="legend" style="padding:6px 12px">${T.formatFeatures.map(f => `${f[1]} = ${f[2]}`).join(" · ")}</div>`;
const FORMAT_HEAD = ["VkFormat", "name", "optimal tiling", "linear tiling", "buffer"];
const formatRows = d => Object.entries(d.formats || {}).map(([k, [lin, opt, buf]]) =>
  [k, [["num", k], ["k", esc(T.formats[k] || "?")], ["", featTags(opt)], ["", featTags(lin)], ["", featTags(buf)]]]);
const extRows = exts => Object.entries(exts || {}).map(([k, v]) => [k, [["k", esc(k)], ["num", v]]]);
const TIMER_COLS = ["n", "median_ms", "p95_ms", "p99_ms", "mean_ms", "sd_ms", "min_ms", "max_ms"];

// Thermal status numbers per platform (docs/running.md, Report).
const STATUS = {android: ["none", "light", "moderate", "severe", "critical", "emergency", "shutdown"],
  macos: ["nominal", "fair", "serious", "critical"]};
const statusName = (os, v) => (STATUS[os] || [])[v] ?? String(v);
const statusCls = v => v >= 2 ? "st2" : v >= 1 ? "st1" : "st0";
function thermalMax(r) {
  const s = r.series;
  if (!s || !s.t_s.length) return null;
  return {status: Math.max(...s.thermal_status), headroom: Math.max(...s.thermal_headroom),
    start: s.thermal_headroom[0], end: s.thermal_headroom[s.thermal_headroom.length - 1]};
}
function thermalTag(r) {
  const m = thermalMax(r), os = r.device_info?.os;
  if (!m || !m.status) return "";
  return `<span class="tag ${statusCls(m.status)}" title="peak thermal status">${esc(statusName(os, m.status))}</span>`;
}

// Inline SVG line chart over the run's windows. lines: [{v, lo, hi, cls}] (lo / hi: optional band); null = gap.
// rects: optional per-window background classes. pad: y range margin, as a fraction of max - min.
// name / tips: short name and per-window values, shown in the title while any chart of the run is hovered (syncHover).
function chart(t, lines, o) {
  const W = 600, H = 130, L = 46, R = 8, T = 18, B = 18;
  const vals = lines.flatMap(l => [...l.v, ...(l.hi || []), ...(l.lo || [])]).filter(v => v !== null);
  let lo = o.ymin ?? Math.min(...vals), hi = Math.max(o.ymaxMin ?? -Infinity, ...vals);
  if (!isFinite(lo) || !isFinite(hi)) { lo = 0; hi = 1; }
  if (o.pad) { const m = (hi - lo) * o.pad; lo -= m; hi += m; }
  if (hi - lo < 1e-9) { hi = lo + (lo ? Math.abs(lo) * 0.1 : 1); }
  const tmax = t[t.length - 1] || 1;
  const x = i => L + (t[i] / tmax) * (W - L - R), x0 = i => i ? x(i - 1) : L;
  const y = v => T + (1 - (v - lo) / (hi - lo)) * (H - T - B);
  const segs = v => { const out = []; let cur = []; v.forEach((a, i) => { if (a === null) { if (cur.length) out.push(cur); cur = []; } else cur.push(i); }); if (cur.length) out.push(cur); return out; };
  let svg = "";
  if (o.rects) o.rects.forEach((c, i) => { if (c) svg += `<rect class="${c}" x="${x0(i)}" y="${T}" width="${x(i) - x0(i)}" height="${H - T - B}"/>`; });
  for (const l of lines) {
    if (l.lo) for (const sg of segs(l.lo)) {
      const pts = sg.map(i => `${x(i)},${y(l.hi[i])}`).concat(sg.slice().reverse().map(i => `${x(i)},${y(l.lo[i])}`));
      svg += `<polygon class="band ${l.cls}" points="${pts.join(" ")}"/>`;
    }
    for (const sg of segs(l.v)) {
      if (sg.length === 1) svg += `<circle class="pt ${l.cls}" cx="${x(sg[0])}" cy="${y(l.v[sg[0]])}" r="2"/>`;
      else svg += `<polyline class="ln ${l.cls}" points="${sg.map(i => `${x(i)},${y(l.v[i])}`).join(" ")}"/>`;
    }
  }
  const fmt = v => Math.abs(v) >= 100 ? v.toFixed(0) : v.toFixed(Math.abs(v) >= 10 ? 1 : 2);
  svg += `<line class="ax" x1="${L}" y1="${H - B}" x2="${W - R}" y2="${H - B}"/>`;
  svg += `<text class="lab" x="${L - 4}" y="${T + 4}" text-anchor="end">${fmt(hi)}</text><text class="lab" x="${L - 4}" y="${H - B}" text-anchor="end">${fmt(lo)}</text>`;
  svg += `<text class="lab" x="${L}" y="${H - 4}">0 s</text><text class="lab" x="${W - R}" y="${H - 4}" text-anchor="end">${tmax.toFixed(1)} s</text>`;
  svg += `<text class="ttl" x="${L}" y="12" data-t="${esc(o.title)}" data-n="${esc(o.name)}">${esc(o.title)}</text>`;
  if (o.tips) o.tips.forEach((tip, i) => { svg += `<rect class="hit" data-i="${i}" data-v="${esc(tip)}" x="${x0(i)}" y="${T}" width="${x(i) - x0(i)}" height="${H - T - B}"/>`; });
  return `<svg class="chart ${o.cls || ""}" viewBox="0 0 ${W} ${H}">${svg}</svg>`;
}
function runCharts(r) {
  const s = r.series;
  if (!s || !s.t_s.length) return "";
  const os = r.device_info?.os, t = s.t_s;
  const tip = i => `${t[i].toFixed(1)} s · ${s.frames[i]} fr`;
  const ms = v => (+v).toFixed(3);
  let html = chart(t, [{v: s.thermal_headroom, cls: "c1"}], {title: "thermal headroom (bands: status)", name: "headroom", pad: 0.2,
    rects: s.thermal_status.map(v => v ? statusCls(v) : ""),
    tips: t.map((_, i) => `${tip(i)} · ${s.thermal_headroom[i]} · ${statusName(os, s.thermal_status[i])}`)});
  for (const tm of r.timers || []) {
    const ts = tm.series;
    if (!ts || !tm.n) continue;
    html += chart(t, [{v: ts.mean_ms, lo: ts.min_ms, hi: ts.max_ms, cls: tm.kind === "gpu" ? "c2" : "c3"}],
      {title: `${tm.kind} ${tm.name} (ms: mean, band min-max)`, name: `${tm.kind} ${tm.name}`, cls: tm.kind,
       tips: t.map((_, i) => ts.mean_ms[i] === null ? tip(i) : `${tip(i)} · mean ${ms(ts.mean_ms[i])} min ${ms(ts.min_ms[i])} max ${ms(ts.max_ms[i])}`)});
  }
  return `<div class="charts">${html}</div>`;
}
// Hovering a window in one chart highlights that window in every chart of the run and shows its values in each title.
function syncHover(box, i) {
  for (const svg of box.querySelectorAll("svg.chart")) {
    const hit = i === null ? null : svg.querySelector(`rect.hit[data-i="${i}"]`), ttl = svg.querySelector("text.ttl");
    svg.querySelectorAll("rect.hit.on").forEach(x => x.classList.remove("on"));
    if (hit) hit.classList.add("on");
    ttl.textContent = hit ? `${ttl.dataset.n}: ${hit.dataset.v}` : ttl.dataset.t;
  }
}
document.addEventListener("mouseover", e => {
  const hit = e.target.closest?.("svg.chart rect.hit");
  if (hit) syncHover(hit.closest(".charts"), hit.dataset.i);
});
document.addEventListener("mouseout", e => {
  const box = e.target.closest?.(".charts");
  if (box && !box.contains(e.relatedTarget)) syncHover(box, null);
});

const fpsOf = r => r.fps ?? (r.measured_time_s ? r.measured_frames / r.measured_time_s : null);
const fpsStr = r => { const f = fpsOf(r); return f == null ? "" : f >= 100 ? f.toFixed(0) : f.toFixed(1); };

function runSummary(r) {
  const flags = [r.vulkan && "vk" + r.vulkan, r.offscreen ? "offscreen" : r.present_mode].filter(Boolean);
  return `<b>${esc(r.script)}</b> <b>${fpsStr(r)} fps</b> ${tags(flags)} ${thermalTag(r)}${buildWarn(r)}`;
}
function runBody(r) {
  const timers = (r.timers || []).map(t => `<tr class="f ${esc(t.kind)}"><td>${esc(t.kind)}</td><td class="k">${esc(t.name)}</td>
    ${TIMER_COLS.map(c => `<td class="num">${t[c] == null ? "" : (c === "n" ? t[c] : (+t[c]).toFixed(3))}</td>`).join("")}</tr>`).join("");
  const m = thermalMax(r);
  const thermal = m ? `thermal headroom ${m.start} → ${m.end} (max ${m.headroom}), status max ${esc(statusName(r.device_info?.os, m.status))} · ` : "";
  const battery = r.battery_c ? `battery ${r.battery_c} C · ` : "";
  return `${runCharts(r)}<div class="wrap"><table><thead><tr><th>kind</th><th>timer</th>${TIMER_COLS.map(c => `<th>${c}</th>`).join("")}</tr></thead>
    <tbody>${timers}</tbody></table></div>
    <div class="muted" style="padding:6px 12px">frames ${r.measured_frames} (+${r.frames_warmup} warmup) in ${(+r.measured_time_s).toFixed(2)} s ·
    ${thermal}${battery}file ${esc(r.file)}</div>`;
}
// Every link to one run opens it in this modal: device i, run j of DEVS[i].runs.
function showRun(i, j) {
  const r = DEVS[i].runs[j], dlg = document.getElementById("run");
  dlg.innerHTML = `<div class="hd"><a href="#/device/${i}" onclick="document.getElementById('run').close()">${esc(DEVS[i].label)}</a>
    ${runSummary(r)}<button autofocus onclick="this.closest('dialog').close()" title="close">×</button></div>${runBody(r)}`;
  dlg.showModal();
}

function viewDevice(i, anchor) {
  const dev = DEVS[i];
  if (!dev) { app.innerHTML = "unknown device"; return; }
  let html = `<h1>${esc(dev.label)}</h1>
    <div class="muted">${esc(dev.info.name || "")} · ${esc(driverStr(dev.info))} · ${esc(osStr(dev.info))}</div>
    <div class="bar"><input type="search" id="flt" placeholder="filter rows (limits, features, extensions, formats...)" oninput="applyFilter(this.value)">
    <span class="legend">Vulkan 1.1 and 1.3 instances together: a badge marks what only one has, [1.1] / [1.3] values that differ.</span></div>`;
  html += section("Device info", kvTable(dev.info), true);
  const inst = MODES.map(x => [x, dev.vulkan[x]]);
  const devs = MODES.map(x => [x, vkDevice(dev, x)]);
  const present = devs.filter(([, d]) => d).map(([x]) => x);
  if (present.length) {
    // f(d) -> rows; title badge when only some modes have the section.
    const both = (title, f, open, head, sortKeys) => {
      const per = devs.map(([x, d]) => [x, d ? f(d) : null]);
      const has = per.filter(([, r]) => r).map(([x]) => x);
      return has.length ? section(title, mergeTable(per, head, sortKeys), open, null, has.length < present.length ? verTag(has) : "") : "";
    };
    const byName = (a, b) => a < b ? -1 : a > b ? 1 : 0;
    html += section("Instance", mergeTable(inst.map(([x, m]) => [x, m && kvRows({instance_version: m.instance_version,
      layers: m.layers.join(", ") || "none", other_physical_devices: m.others.join(", ") || "none"})])) +
      mergeTable(inst.map(([x, m]) => [x, m && extRows(m.instance_extensions)]), ["name", "spec"], byName), false);
    html += both("Properties", d => kvRows(Object.fromEntries(Object.entries(d).filter(([k, v]) => typeof v !== "object" || v === null))), true);
    html += both("Limits", d => kvRows(d.limits), false);
    html += both("Sparse properties", d => kvRows(d.sparseProperties), false);
    html += both("Features (1.0)", d => kvRows(d.features), false);
    const structs = [...new Set(devs.flatMap(([, d]) => d ? Object.keys(d).filter(k => k.startsWith("VkPhysicalDevice")) : []))];
    for (const k of structs) html += both(k, d => kvRows(d[k]), false);
    const nExt = new Set(devs.flatMap(([, d]) => d ? Object.keys(d.extensions || {}) : [])).size;
    html += both(`Device extensions (${nExt})`, d => extRows(d.extensions), false, ["name", "spec"], byName);
    html += both("Memory", memoryRows, false);
    html += both("Queue families", queueRows, false, QUEUE_HEAD);
    const nFmt = new Set(devs.flatMap(([, d]) => d ? Object.keys(d.formats || {}) : [])).size;
    html += section(`Formats (${nFmt})`, FMT_LEGEND + mergeTable(devs.map(([x, d]) => [x, d && formatRows(d)]), FORMAT_HEAD, (a, b) => a - b), false);
  } else {
    html += '<p class="muted">No Vulkan info in the result files of this device.</p>';
  }
  html += section(`Runs (${dev.runs.length})`, `<div style="padding:0 10px">${dev.runs.map((r, j) => `<div class="runrow" onclick="showRun(${i}, ${j})">${runSummary(r)}</div>`).join("") || '<p class="muted">none</p>'}</div>`, true, "runs");
  html += section("Files", `<div style="padding:6px 12px">${dev.files.map(esc).join("<br>")}</div>`, false);
  app.innerHTML = html;
  // Filtering opens every section so matches are visible.
  document.getElementById("flt").addEventListener("input", e => {
    if (e.target.value) app.querySelectorAll("details").forEach(x => x.open = true);
  });
  if (anchor) document.getElementById(anchor)?.scrollIntoView();
}

// ---------- runs

function viewRuns() {
  const scripts = [...new Set(DEVS.flatMap(d => d.runs.map(r => r.script)))].sort();
  const head = DEVS.map((d, i) => `<th class="c"><a href="#/device/${i}/runs">${esc(d.label)}</a></th>`).join("");
  const rows = scripts.map(s => {
    let ndev = 0;
    const cells = DEVS.map((d, i) => {
      const rs = d.runs.filter(r => r.script === s);
      if (!rs.length) return '<td class="c"><span class="miss">-</span></td>';
      ndev++;
      return `<td class="c">${rs.slice().sort((a, b) => String(a.vulkan).localeCompare(String(b.vulkan))).map(r =>
        `<a class="tag" href="javascript:showRun(${i}, ${d.runs.indexOf(r)})">vk${esc(r.vulkan)} <b>${fpsStr(r)}</b></a>`).join("")}</td>`;
    }).join("");
    return `<tr class="f"><td class="k">${esc(s)}</td><td class="num">${ndev}/${DEVS.length}</td>${cells}</tr>`;
  }).join("");
  const all = DEVS.flatMap((d, i) => d.runs.map((r, j) => ({d, i, j, r})));
  const list = all.map(({d, i, j, r}) => `<tr class="f click" onclick="showRun(${i}, ${j})"><td><a href="#/device/${i}" onclick="event.stopPropagation()">${esc(d.label)}</a></td><td class="k">${esc(r.script)}</td>
    <td>${esc(r.vulkan)}</td><td class="num"><b>${fpsStr(r)}</b></td><td>${r.offscreen ? "offscreen" : esc(r.present_mode || "")}</td>
    <td class="num">${r.measured_frames ?? ""}</td><td class="num">${r.measured_time_s === undefined ? "" : (+r.measured_time_s).toFixed(2)}</td>
    <td data-s="${thermalMax(r)?.status ?? -1}">${thermalTag(r)}</td>
    <td>${esc(r.device_info?.version || "")} ${esc(r.device_info?.git || "")} ${buildWarn(r)}</td>
    <td class="k">${esc(r.file)}</td></tr>`).join("");
  const lh = ["Device", "Script", "Vulkan", "FPS", "Present", "Frames", "Time s", "Thermal max", "Version", "File"];
  app.innerHTML = `
    <div class="bar"><input type="search" placeholder="filter" oninput="applyFilter(this.value)">
    <span class="legend">FPS per script, device and Vulkan path (one tag per run).</span></div>
    <div class="wrap compare"><table><thead><tr><th class="sort">script</th><th class="sort">devices</th>${head}</tr></thead>
    <tbody>${rows}</tbody></table></div>
    <h1 style="margin-top:20px">All runs (${all.length})</h1>
    <div class="wrap" style="max-height:70vh"><table><thead><tr>${lh.map(h => `<th class="sort">${h}</th>`).join("")}</tr></thead>
    <tbody>${list}</tbody></table></div>`;
}

// ---------- router: #/  #/device/<i>[/<mode>|/runs]  #/runs

function route() {
  const p = location.hash.replace(/^#\/?/, "").split("/");
  const top = p[0] || "";
  document.getElementById("nav").innerHTML = [["", "Devices"], ["runs", "Runs"]].map(([k, l]) =>
    `<a class="${top === k || (k === "" && top === "device") ? "on" : ""}" href="#/${k}">${l}</a>`).join("");
  if (top === "device") viewDevice(+p[1], p[2] === "runs" ? "runs" : null);
  else if (top === "runs") viewRuns();
  else viewOverview();
}
window.addEventListener("hashchange", () => { route(); if (!location.hash.endsWith("/runs")) window.scrollTo(0, 0); });
route();
</script>
</body>
</html>
"""

if __name__ == "__main__":
    main()
