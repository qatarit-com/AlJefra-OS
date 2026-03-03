# SPDX-License-Identifier: MIT
# AlJefra OS -- Driver Storage Backend

"""
SQLite-backed driver storage with on-disk .ajdrv binary files.

Layout:
    server/marketplace.db             — SQLite database (catalog, evolutions, reviews, metrics)
    server/drivers/{arch}/{vid}_{did}.ajdrv  — driver binary files

On first run, if marketplace.db is empty but catalog.json exists,
the catalog is automatically migrated to SQLite.
"""

import hashlib
import json
import os
from pathlib import Path
from typing import Dict, List, Optional, Tuple

from models import Arch, Category, DriverMeta
from database import MarketplaceDB


# ── Paths ──

BASE_DIR = Path(__file__).resolve().parent
DRIVERS_DIR = BASE_DIR / "drivers"
CATALOG_FILE = BASE_DIR / "catalog.json"


class DriverStore:
    """SQLite-backed catalog + on-disk .ajdrv binary files."""

    def __init__(self) -> None:
        self.db = MarketplaceDB()
        self._maybe_migrate()

    def _maybe_migrate(self) -> None:
        """If DB is empty but catalog.json exists, migrate."""
        rows, total = self.db.list_drivers(page=1, per_page=1)
        if total == 0 and CATALOG_FILE.exists():
            count = self.db.import_catalog_json(CATALOG_FILE)
            if count > 0:
                print(f"[db] Migrated {count} drivers from catalog.json → SQLite")

    # ── Query ──

    def list_all(
        self,
        arch: Optional[str] = None,
        category: Optional[str] = None,
        page: int = 1,
        per_page: int = 50,
    ) -> Tuple[List[dict], int]:
        """Return catalog entries, optionally filtered. Returns (items, total)."""
        rows, total = self.db.list_drivers(arch=arch, category=category,
                                            page=page, per_page=per_page)
        items = []
        for row in rows:
            meta = self.db.driver_row_to_meta(row)
            items.append(meta.to_catalog_json())
        return items, total

    def find(
        self, vendor_id: str, device_id: str, arch: str
    ) -> Optional[DriverMeta]:
        """Find a specific driver by (vendor, device, arch)."""
        row = self.db.find_driver(vendor_id, device_id, arch)
        if row is None:
            return None
        return self.db.driver_row_to_meta(row)

    def find_for_device(
        self, vendor_id: str, device_id: str, arch: str
    ) -> Optional[DriverMeta]:
        """Find a driver matching a device, checking arch-specific first,
        then fallback to 'any' arch."""
        row = self.db.find_driver_with_fallback(vendor_id, device_id, arch)
        if row is None:
            return None
        return self.db.driver_row_to_meta(row)

    # ── Driver binary access ──

    def driver_path(self, vendor_id: str, device_id: str, arch: str) -> Path:
        """Filesystem path for a driver binary."""
        v = vendor_id.lower()
        d = device_id.lower()
        return DRIVERS_DIR / arch / f"{v}_{d}.ajdrv"

    def get_driver_binary(
        self, vendor_id: str, device_id: str, arch: str
    ) -> Optional[bytes]:
        """Read a driver binary from disk. Returns None if not found."""
        path = self.driver_path(vendor_id, device_id, arch)
        if not path.exists():
            return None
        with open(path, "rb") as f:
            return f.read()

    # ── Add / Upload ──

    def add_driver(self, meta: DriverMeta, binary: bytes) -> None:
        """Store a new driver binary and update the catalog."""
        v = meta.vendor_id.lower()
        d = meta.device_id.lower()
        arch = meta.arch

        # Ensure directory exists
        arch_dir = DRIVERS_DIR / arch
        arch_dir.mkdir(parents=True, exist_ok=True)

        # Write binary
        path = arch_dir / f"{v}_{d}.ajdrv"
        with open(path, "wb") as f:
            f.write(binary)

        # Update metadata
        meta.size_bytes = len(binary)
        meta.sha256 = hashlib.sha256(binary).hexdigest()
        meta.fill_download_url()

        # Persist to SQLite
        self.db.upsert_driver(meta)

    def remove_driver(
        self, vendor_id: str, device_id: str, arch: str
    ) -> bool:
        """Remove a driver from the store. Returns True if it existed."""
        # Remove binary
        path = self.driver_path(vendor_id, device_id, arch)
        if path.exists():
            path.unlink()

        return self.db.delete_driver(vendor_id, device_id, arch)

    # ── Recommendation engine ──

    def recommend(
        self, vendor_id: str, device_id: str, arch: str, has_driver: bool
    ) -> Optional[dict]:
        """Generate a driver recommendation for a device.
        Returns a recommendation dict or None."""
        meta = self.find_for_device(vendor_id, device_id, arch)
        if meta is None:
            return None

        # Determine priority
        if has_driver:
            priority = "optional"  # already has a built-in driver
        else:
            # Classify by category
            cat = Category.from_string(meta.category)
            if cat in (Category.STORAGE, Category.NETWORK):
                priority = "critical"
            elif cat in (Category.DISPLAY, Category.GPU):
                priority = "recommended"
            else:
                priority = "optional"

        meta.fill_download_url()
        return {
            "vendor_id": meta.vendor_id,
            "device_id": meta.device_id,
            "driver_name": meta.name,
            "version": meta.version,
            "priority": priority,
            "download_url": meta.download_url,
        }
