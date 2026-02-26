#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# AlJefra OS -- Driver Marketplace REST API Server
#
# Endpoints:
#   GET  /v1/catalog                          -- list all drivers
#   POST /v1/manifest                         -- send hw manifest, get recommendations
#   GET  /v1/drivers/<vendor>/<device>/<arch>  -- download .ajdrv binary
#   POST /v1/drivers                          -- upload new driver package
#   GET  /v1/updates/<os_version>             -- check for OS/driver updates
#
# Quick start:
#   pip install -r requirements.txt
#   python app.py                # serves on 0.0.0.0:8080
#
# The server auto-seeds stub drivers on first run if catalog.json is absent.

import hashlib
import json
import os
import struct
import sys
from pathlib import Path

from flask import Flask, Response, jsonify, request, send_file
from flask_cors import CORS

# Add server/ to path so models and driver_store can be imported
sys.path.insert(0, str(Path(__file__).resolve().parent))

from models import Arch, Category, DriverMeta, HardwareManifest
from driver_store import DriverStore, CATALOG_FILE, DRIVERS_DIR

# ── Application setup ──

app = Flask(__name__)
CORS(app)  # Allow web dashboard access

# Current OS version advertised by the server
OS_VERSION = "1.0.0"

# Initialize the driver store (loads catalog.json)
store = DriverStore()


# ── Auto-seed on first run ──

def _auto_seed() -> None:
    """If no catalog.json exists, run the seed builder."""
    if CATALOG_FILE.exists():
        return
    print("[seed] No catalog.json found -- generating stub drivers...")
    from ajdrv_builder import seed_catalog
    seed_catalog()
    # Reload the store after seeding
    global store
    store = DriverStore()
    print("[seed] Done.\n")


# ── Endpoints ──

@app.route("/", methods=["GET"])
def index():
    """Health check / API info."""
    return jsonify({
        "service": "AlJefra Driver Marketplace",
        "version": "1.0.0",
        "endpoints": {
            "catalog":  "GET  /v1/catalog",
            "manifest": "POST /v1/manifest",
            "driver":   "GET  /v1/drivers/<vendor>/<device>/<arch>",
            "upload":   "POST /v1/drivers",
            "updates":  "GET  /v1/updates/<os_version>",
        },
    })


@app.route("/v1/catalog", methods=["GET"])
def get_catalog():
    """List all available drivers.

    Query params:
        arch     -- filter by architecture (x86_64, aarch64, riscv64)
        category -- filter by category (storage, network, input, display, gpu, bus)
        page     -- page number (default 1)
        per_page -- items per page (default 50)

    Response:
        { "drivers": [...], "total": N, "page": P }
    """
    arch = request.args.get("arch")
    category = request.args.get("category")
    page = int(request.args.get("page", 1))
    per_page = int(request.args.get("per_page", 50))

    drivers, total = store.list_all(
        arch=arch, category=category, page=page, per_page=per_page
    )

    return jsonify({
        "drivers": drivers,
        "total": total,
        "page": page,
    })


@app.route("/v1/manifest", methods=["POST"])
def post_manifest():
    """Receive a hardware manifest, return driver recommendations.

    The client (marketplace.c) sends JSON like:
        {
            "arch": "x86_64",
            "cpu_vendor": "GenuineIntel",
            "cpu_model": "...",
            "ram_mb": 256,
            "devices": [
                {"v": "8086", "d": "10d3", "c": 2, "s": 0, "has_drv": false},
                ...
            ]
        }

    Response:
        {
            "recommendations": [
                {
                    "vendor_id": "8086",
                    "device_id": "10d3",
                    "driver_name": "e1000",
                    "version": "1.0.0",
                    "priority": "critical",
                    "download_url": "/v1/drivers/8086/10d3/x86_64"
                }
            ],
            "os_update_available": false
        }
    """
    body = request.get_json(silent=True)
    if body is None:
        return jsonify({"error": "Invalid JSON body"}), 400

    manifest = HardwareManifest.from_json(body)
    recommendations = []

    for dev in manifest.devices:
        rec = store.recommend(
            vendor_id=dev.vendor,
            device_id=dev.device,
            arch=manifest.arch,
            has_driver=dev.has_driver,
        )
        if rec is not None:
            recommendations.append(rec)

    return jsonify({
        "recommendations": recommendations,
        "os_update_available": False,
    })


@app.route("/v1/drivers/<vendor>/<device>/<arch>", methods=["GET"])
def get_driver(vendor: str, device: str, arch: str):
    """Download a .ajdrv driver package.

    Path params:
        vendor -- 4-hex-char vendor ID (e.g. "8086")
        device -- 4-hex-char device ID (e.g. "10d3")
        arch   -- target architecture (x86_64, aarch64, riscv64)

    Response:
        Binary .ajdrv file with Content-Type: application/octet-stream
    """
    vendor = vendor.lower()
    device = device.lower()
    arch = arch.lower()

    # Validate arch
    if arch not in ("x86_64", "aarch64", "riscv64"):
        return jsonify({"error": f"Unknown architecture: {arch}"}), 400

    # Check if driver exists in catalog
    meta = store.find(vendor, device, arch)
    if meta is None:
        return jsonify({
            "error": "Driver not found",
            "vendor_id": vendor,
            "device_id": device,
            "arch": arch,
        }), 404

    # Read binary
    binary = store.get_driver_binary(vendor, device, arch)
    if binary is None:
        return jsonify({
            "error": "Driver binary missing from storage",
            "vendor_id": vendor,
            "device_id": device,
            "arch": arch,
        }), 500

    filename = f"{vendor}_{device}_{arch}.ajdrv"
    return Response(
        binary,
        mimetype="application/octet-stream",
        headers={
            "Content-Disposition": f'attachment; filename="{filename}"',
            "Content-Length": str(len(binary)),
            "X-AJDRV-Name": meta.name,
            "X-AJDRV-Version": meta.version,
            "X-AJDRV-SHA256": meta.sha256,
        },
    )


@app.route("/v1/drivers", methods=["POST"])
def upload_driver():
    """Upload a new .ajdrv driver package.

    Accepts multipart/form-data with:
        file     -- the .ajdrv binary
        metadata -- JSON string with optional extra fields

    Or application/octet-stream with the raw .ajdrv bytes
    (metadata extracted from the package header).
    """
    # Check for multipart upload
    if "file" in request.files:
        f = request.files["file"]
        binary = f.read()
        # Optional metadata JSON
        meta_json = request.form.get("metadata", "{}")
        try:
            extra = json.loads(meta_json)
        except json.JSONDecodeError:
            extra = {}
    elif request.content_type and "octet-stream" in request.content_type:
        binary = request.data
        extra = {}
    else:
        return jsonify({"error": "Upload .ajdrv via multipart 'file' field or application/octet-stream"}), 400

    if len(binary) < 64 + 64:
        return jsonify({"error": "File too small to be a valid .ajdrv package"}), 400

    # Parse the .ajdrv header
    AJDRV_MAGIC = 0x56444A41
    magic = struct.unpack_from("<I", binary, 0)[0]
    if magic != AJDRV_MAGIC:
        return jsonify({"error": f"Bad magic: 0x{magic:08X}, expected 0x{AJDRV_MAGIC:08X}"}), 400

    (
        _magic, _version, arch_code, cat_code,
        code_offset, code_size,
        name_offset, name_size,
        desc_offset, desc_size,
        _entry_offset, _sig_offset,
        vendor_id, device_id,
        _min_os, flags,
    ) = struct.unpack_from("<IIIIIIIIIIIIHHHH", binary, 0)

    # Extract name and description
    name = binary[name_offset:name_offset + name_size].rstrip(b"\x00").decode("utf-8", errors="replace")
    desc = binary[desc_offset:desc_offset + desc_size].rstrip(b"\x00").decode("utf-8", errors="replace")

    # Map codes to strings
    arch_str = {0: "x86_64", 1: "aarch64", 2: "riscv64", 0xFF: "any"}.get(arch_code, "x86_64")
    cat_str = {0: "storage", 1: "network", 2: "input", 3: "display", 4: "gpu", 5: "bus", 6: "other"}.get(cat_code, "other")

    vendor_hex = f"{vendor_id:04x}"
    device_hex = f"{device_id:04x}"

    meta = DriverMeta(
        name=name,
        version=extra.get("version", "1.0.0"),
        vendor_id=vendor_hex,
        device_id=device_hex,
        arch=arch_str,
        category=cat_str,
        description=desc,
        flags=flags,
    )

    store.add_driver(meta, binary)

    return jsonify({
        "status": "ok",
        "driver": meta.to_catalog_json(),
    }), 201


@app.route("/v1/updates/<os_version>", methods=["GET"])
def check_updates(os_version: str):
    """Check for OS/driver updates.

    Response:
        {
            "update_available": true/false,
            "version": "2.0.0",
            "changelog": "...",
            "download_url": "/v1/updates/2.0.0/x86_64"
        }
    """
    # Simple version comparison: if the client version < server OS_VERSION
    client_parts = os_version.split(".")
    server_parts = OS_VERSION.split(".")

    update_available = False
    try:
        for c, s in zip(client_parts, server_parts):
            if int(s) > int(c):
                update_available = True
                break
            elif int(s) < int(c):
                break
    except ValueError:
        pass

    return jsonify({
        "update_available": update_available,
        "version": OS_VERSION,
        "changelog": "Multi-architecture support, HAL layer, portable drivers" if update_available else "",
        "download_url": f"/v1/updates/{OS_VERSION}/x86_64" if update_available else "",
    })


# ── Error handlers ──

@app.errorhandler(404)
def not_found(e):
    return jsonify({"error": "Not found"}), 404


@app.errorhandler(405)
def method_not_allowed(e):
    return jsonify({"error": "Method not allowed"}), 405


@app.errorhandler(500)
def internal_error(e):
    return jsonify({"error": "Internal server error"}), 500


# ── Main ──

def main():
    host = os.environ.get("ALJEFRA_HOST", "0.0.0.0")
    port = int(os.environ.get("ALJEFRA_PORT", "8080"))
    debug = os.environ.get("ALJEFRA_DEBUG", "0") == "1"

    _auto_seed()

    print(f"AlJefra Driver Marketplace API")
    print(f"  Listening on {host}:{port}")
    print(f"  Catalog: {CATALOG_FILE}")
    print(f"  Drivers: {DRIVERS_DIR}/")
    print()

    app.run(host=host, port=port, debug=debug)


if __name__ == "__main__":
    main()
