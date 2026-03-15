# SPDX-License-Identifier: MIT
# AlJefra OS -- Marketplace SQLite Database

"""
SQLite persistence layer for the AlJefra Driver Marketplace.

Tables:
    drivers     — driver catalog (replaces catalog.json)
    evolutions  — AI evolution submissions
    reviews     — community audit / review entries
    metrics     — driver performance telemetry

The database file lives at server/marketplace.db by default.
On first run, tables are auto-created via init_db().
"""

import json
import sqlite3
import time
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

from models import DriverMeta

BASE_DIR = Path(__file__).resolve().parent
DEFAULT_DB_PATH = BASE_DIR / "marketplace.db"


def _dict_factory(cursor: sqlite3.Cursor, row: tuple) -> dict:
    """Row factory that returns dicts instead of tuples."""
    return {col[0]: row[i] for i, col in enumerate(cursor.description)}


class MarketplaceDB:
    """SQLite-backed storage for the AlJefra Driver Marketplace."""

    def __init__(self, db_path: Optional[Path] = None) -> None:
        self.db_path = db_path or DEFAULT_DB_PATH
        self.conn = sqlite3.connect(str(self.db_path), check_same_thread=False)
        self.conn.row_factory = _dict_factory
        self.conn.execute("PRAGMA journal_mode=WAL")
        self.conn.execute("PRAGMA foreign_keys=ON")
        self._init_tables()

    def _init_tables(self) -> None:
        """Create tables if they don't exist."""
        self.conn.executescript("""
            CREATE TABLE IF NOT EXISTS drivers (
                id          INTEGER PRIMARY KEY AUTOINCREMENT,
                name        TEXT    NOT NULL,
                version     TEXT    NOT NULL DEFAULT '1.0.0',
                vendor_id   TEXT    NOT NULL,
                device_id   TEXT    NOT NULL,
                arch        TEXT    NOT NULL,
                category    TEXT    NOT NULL DEFAULT 'other',
                size_bytes  INTEGER NOT NULL DEFAULT 0,
                sha256      TEXT    NOT NULL DEFAULT '',
                description TEXT    NOT NULL DEFAULT '',
                min_os_ver  TEXT    NOT NULL DEFAULT '1.0',
                flags       INTEGER NOT NULL DEFAULT 0,
                created_at  REAL    NOT NULL DEFAULT (strftime('%s','now')),
                updated_at  REAL    NOT NULL DEFAULT (strftime('%s','now')),
                UNIQUE(vendor_id, device_id, arch)
            );

            CREATE TABLE IF NOT EXISTS evolutions (
                id                INTEGER PRIMARY KEY AUTOINCREMENT,
                base_driver       TEXT    NOT NULL,
                base_version      TEXT    NOT NULL DEFAULT '0.0.0',
                arch              TEXT    NOT NULL DEFAULT 'x86_64',
                optimization_type TEXT    NOT NULL DEFAULT 'performance',
                metrics_json      TEXT    NOT NULL DEFAULT '{}',
                description       TEXT    NOT NULL DEFAULT '',
                author            TEXT    NOT NULL DEFAULT 'anonymous',
                status            TEXT    NOT NULL DEFAULT 'pending_review',
                has_binary        INTEGER NOT NULL DEFAULT 0,
                created_at        REAL    NOT NULL DEFAULT (strftime('%s','now'))
            );

            CREATE TABLE IF NOT EXISTS reviews (
                id                INTEGER PRIMARY KEY AUTOINCREMENT,
                driver_name       TEXT    NOT NULL,
                version           TEXT    NOT NULL DEFAULT '0.0.0',
                arch              TEXT    NOT NULL DEFAULT 'x86_64',
                reviewer          TEXT    NOT NULL DEFAULT 'anonymous',
                verdict           TEXT    NOT NULL DEFAULT 'pending',
                comments          TEXT    NOT NULL DEFAULT '',
                stability_score   INTEGER NOT NULL DEFAULT 0,
                security_score    INTEGER NOT NULL DEFAULT 0,
                performance_score INTEGER NOT NULL DEFAULT 0,
                status            TEXT    NOT NULL DEFAULT 'pending',
                created_at        REAL    NOT NULL DEFAULT (strftime('%s','now'))
            );

            CREATE TABLE IF NOT EXISTS metrics (
                id          INTEGER PRIMARY KEY AUTOINCREMENT,
                driver_name TEXT    NOT NULL,
                version     TEXT    NOT NULL DEFAULT '0.0.0',
                arch        TEXT    NOT NULL DEFAULT 'x86_64',
                uptime_secs INTEGER NOT NULL DEFAULT 0,
                error_count INTEGER NOT NULL DEFAULT 0,
                extra_json  TEXT    NOT NULL DEFAULT '{}',
                created_at  REAL    NOT NULL DEFAULT (strftime('%s','now'))
            );

            CREATE TABLE IF NOT EXISTS systems (
                id                 INTEGER PRIMARY KEY AUTOINCREMENT,
                system_key         TEXT    NOT NULL UNIQUE,
                arch               TEXT    NOT NULL DEFAULT 'x86_64',
                cpu_vendor         TEXT    NOT NULL DEFAULT '',
                cpu_model          TEXT    NOT NULL DEFAULT '',
                ram_mb             INTEGER NOT NULL DEFAULT 0,
                os_version         TEXT    NOT NULL DEFAULT '0.0.0',
                manifest_json      TEXT    NOT NULL DEFAULT '{}',
                desired_apps_json  TEXT    NOT NULL DEFAULT '[]',
                desired_drivers_json TEXT  NOT NULL DEFAULT '[]',
                last_seen          REAL    NOT NULL DEFAULT (strftime('%s','now')),
                created_at         REAL    NOT NULL DEFAULT (strftime('%s','now'))
            );

            CREATE TABLE IF NOT EXISTS sync_requests (
                id            INTEGER PRIMARY KEY AUTOINCREMENT,
                system_key    TEXT    NOT NULL,
                request_type  TEXT    NOT NULL,
                target_key    TEXT    NOT NULL,
                status        TEXT    NOT NULL DEFAULT 'pending',
                details_json  TEXT    NOT NULL DEFAULT '{}',
                created_at    REAL    NOT NULL DEFAULT (strftime('%s','now')),
                updated_at    REAL    NOT NULL DEFAULT (strftime('%s','now')),
                UNIQUE(system_key, request_type, target_key)
            );

            CREATE INDEX IF NOT EXISTS idx_drivers_vendor_device
                ON drivers(vendor_id, device_id);
            CREATE INDEX IF NOT EXISTS idx_evolutions_driver
                ON evolutions(base_driver);
            CREATE INDEX IF NOT EXISTS idx_reviews_driver
                ON reviews(driver_name);
            CREATE INDEX IF NOT EXISTS idx_metrics_driver
                ON metrics(driver_name);
            CREATE INDEX IF NOT EXISTS idx_systems_key
                ON systems(system_key);
            CREATE INDEX IF NOT EXISTS idx_sync_requests_system
                ON sync_requests(system_key);
        """)
        self.conn.commit()

    def close(self) -> None:
        self.conn.close()

    # ── Driver catalog ──

    def upsert_driver(self, meta: DriverMeta) -> int:
        """Insert or update a driver in the catalog. Returns row ID."""
        cur = self.conn.execute("""
            INSERT INTO drivers (name, version, vendor_id, device_id, arch,
                                 category, size_bytes, sha256, description,
                                 min_os_ver, flags, updated_at)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, strftime('%s','now'))
            ON CONFLICT(vendor_id, device_id, arch) DO UPDATE SET
                name        = excluded.name,
                version     = excluded.version,
                category    = excluded.category,
                size_bytes  = excluded.size_bytes,
                sha256      = excluded.sha256,
                description = excluded.description,
                min_os_ver  = excluded.min_os_ver,
                flags       = excluded.flags,
                updated_at  = strftime('%s','now')
        """, (
            meta.name, meta.version,
            meta.vendor_id.lower(), meta.device_id.lower(),
            meta.arch, meta.category,
            meta.size_bytes, meta.sha256, meta.description,
            meta.min_os_version, meta.flags,
        ))
        self.conn.commit()
        return cur.lastrowid

    def find_driver(self, vendor_id: str, device_id: str, arch: str
                    ) -> Optional[dict]:
        """Find a driver by (vendor, device, arch)."""
        row = self.conn.execute("""
            SELECT * FROM drivers
            WHERE vendor_id = ? AND device_id = ? AND arch = ?
        """, (vendor_id.lower(), device_id.lower(), arch)).fetchone()
        return row

    def find_driver_with_fallback(self, vendor_id: str, device_id: str,
                                   arch: str) -> Optional[dict]:
        """Find driver, falling back to arch='any'."""
        row = self.find_driver(vendor_id, device_id, arch)
        if row:
            return row
        return self.find_driver(vendor_id, device_id, "any")

    def list_drivers(self, arch: Optional[str] = None,
                     category: Optional[str] = None,
                     page: int = 1, per_page: int = 50
                     ) -> Tuple[List[dict], int]:
        """List drivers with optional filters and pagination."""
        where_clauses = []
        params: list = []
        if arch:
            where_clauses.append("arch = ?")
            params.append(arch)
        if category:
            where_clauses.append("category = ?")
            params.append(category)

        where = ("WHERE " + " AND ".join(where_clauses)) if where_clauses else ""

        total = self.conn.execute(
            f"SELECT COUNT(*) as cnt FROM drivers {where}", params
        ).fetchone()["cnt"]

        offset = (page - 1) * per_page
        rows = self.conn.execute(
            f"SELECT * FROM drivers {where} ORDER BY name, arch "
            f"LIMIT ? OFFSET ?",
            params + [per_page, offset]
        ).fetchall()

        return rows, total

    def delete_driver(self, vendor_id: str, device_id: str, arch: str
                      ) -> bool:
        """Remove a driver. Returns True if deleted."""
        cur = self.conn.execute("""
            DELETE FROM drivers
            WHERE vendor_id = ? AND device_id = ? AND arch = ?
        """, (vendor_id.lower(), device_id.lower(), arch))
        self.conn.commit()
        return cur.rowcount > 0

    def driver_row_to_meta(self, row: dict) -> DriverMeta:
        """Convert a database row to a DriverMeta object."""
        return DriverMeta(
            name=row["name"],
            version=row["version"],
            vendor_id=row["vendor_id"],
            device_id=row["device_id"],
            arch=row["arch"],
            category=row["category"],
            size_bytes=row["size_bytes"],
            sha256=row["sha256"],
            description=row["description"],
            min_os_version=row.get("min_os_ver", "1.0"),
            flags=row["flags"],
        )

    # ── Evolution log ──

    def add_evolution(self, entry: Dict[str, Any]) -> int:
        """Record an evolution submission. Returns the evolution ID."""
        cur = self.conn.execute("""
            INSERT INTO evolutions (base_driver, base_version, arch,
                                     optimization_type, metrics_json,
                                     description, author, status, has_binary)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
        """, (
            entry.get("base_driver", "unknown"),
            entry.get("base_version", "0.0.0"),
            entry.get("arch", "x86_64"),
            entry.get("optimization_type", "performance"),
            json.dumps(entry.get("metrics", {})),
            entry.get("description", ""),
            entry.get("author", "anonymous"),
            "pending_review",
            1 if entry.get("has_binary") else 0,
        ))
        self.conn.commit()
        return cur.lastrowid

    def list_evolutions(self, driver: Optional[str] = None,
                        status: Optional[str] = None) -> List[dict]:
        """List evolution entries with optional filters."""
        where_clauses = []
        params: list = []
        if driver:
            where_clauses.append("base_driver = ?")
            params.append(driver)
        if status:
            where_clauses.append("status = ?")
            params.append(status)

        where = ("WHERE " + " AND ".join(where_clauses)) if where_clauses else ""
        rows = self.conn.execute(
            f"SELECT * FROM evolutions {where} ORDER BY id DESC", params
        ).fetchall()

        # Parse metrics JSON back to dict
        for r in rows:
            try:
                r["metrics"] = json.loads(r.pop("metrics_json", "{}"))
            except (json.JSONDecodeError, KeyError):
                r["metrics"] = {}
        return rows

    # ── Reviews ──

    def add_review(self, review: Dict[str, Any]) -> int:
        """Record a driver review. Returns the review ID."""
        cur = self.conn.execute("""
            INSERT INTO reviews (driver_name, version, arch, reviewer,
                                  verdict, comments, stability_score,
                                  security_score, performance_score, status)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        """, (
            review.get("driver_name", "unknown"),
            review.get("version", "0.0.0"),
            review.get("arch", "x86_64"),
            review.get("reviewer", "anonymous"),
            review.get("verdict", "pending"),
            review.get("comments", ""),
            review.get("stability_score", 0),
            review.get("security_score", 0),
            review.get("performance_score", 0),
            review.get("verdict", "pending"),
        ))
        self.conn.commit()
        return cur.lastrowid

    def list_reviews(self, driver: Optional[str] = None,
                     status: Optional[str] = None) -> List[dict]:
        """List reviews with optional filters."""
        where_clauses = []
        params: list = []
        if driver:
            where_clauses.append("driver_name = ?")
            params.append(driver)
        if status:
            where_clauses.append("status = ?")
            params.append(status)

        where = ("WHERE " + " AND ".join(where_clauses)) if where_clauses else ""
        return self.conn.execute(
            f"SELECT * FROM reviews {where} ORDER BY id DESC", params
        ).fetchall()

    def get_review(self, review_id: int) -> Optional[dict]:
        """Get a single review by ID."""
        return self.conn.execute(
            "SELECT * FROM reviews WHERE id = ?", (review_id,)
        ).fetchone()

    def count_approvals(self, driver_name: str, version: str) -> int:
        """Count approved reviews for a driver version."""
        row = self.conn.execute("""
            SELECT COUNT(*) as cnt FROM reviews
            WHERE driver_name = ? AND version = ? AND status = 'approved'
        """, (driver_name, version)).fetchone()
        return row["cnt"] if row else 0

    # ── Metrics ──

    def add_metrics(self, metrics: Dict[str, Any]) -> int:
        """Record driver telemetry. Returns the metrics ID."""
        extra = {k: v for k, v in metrics.items()
                 if k not in ("driver_name", "version", "arch",
                              "uptime_secs", "error_count")}
        cur = self.conn.execute("""
            INSERT INTO metrics (driver_name, version, arch,
                                  uptime_secs, error_count, extra_json)
            VALUES (?, ?, ?, ?, ?, ?)
        """, (
            metrics.get("driver_name", "unknown"),
            metrics.get("version", "0.0.0"),
            metrics.get("arch", "x86_64"),
            metrics.get("uptime_secs", 0),
            metrics.get("error_count", 0),
            json.dumps(extra),
        ))
        self.conn.commit()
        return cur.lastrowid

    # ── Machine sync / demand queue ──

    def upsert_system(self, system_key: str, manifest: Dict[str, Any],
                      os_version: str, desired_apps: List[str],
                      desired_drivers: Optional[List[str]] = None) -> int:
        desired_drivers = desired_drivers or []
        cur = self.conn.execute("""
            INSERT INTO systems (system_key, arch, cpu_vendor, cpu_model, ram_mb,
                                 os_version, manifest_json, desired_apps_json,
                                 desired_drivers_json, last_seen)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, strftime('%s','now'))
            ON CONFLICT(system_key) DO UPDATE SET
                arch = excluded.arch,
                cpu_vendor = excluded.cpu_vendor,
                cpu_model = excluded.cpu_model,
                ram_mb = excluded.ram_mb,
                os_version = excluded.os_version,
                manifest_json = excluded.manifest_json,
                desired_apps_json = excluded.desired_apps_json,
                desired_drivers_json = excluded.desired_drivers_json,
                last_seen = strftime('%s','now')
        """, (
            system_key,
            manifest.get("arch", "x86_64"),
            manifest.get("cpu_vendor", ""),
            manifest.get("cpu_model", ""),
            int(manifest.get("ram_mb", 0)),
            os_version,
            json.dumps(manifest),
            json.dumps(desired_apps),
            json.dumps(desired_drivers),
        ))
        self.conn.commit()
        return cur.lastrowid

    def queue_sync_request(self, system_key: str, request_type: str,
                           target_key: str, details: Dict[str, Any]) -> int:
        cur = self.conn.execute("""
            INSERT INTO sync_requests (system_key, request_type, target_key, status, details_json, updated_at)
            VALUES (?, ?, ?, 'pending', ?, strftime('%s','now'))
            ON CONFLICT(system_key, request_type, target_key) DO UPDATE SET
                details_json = excluded.details_json,
                updated_at = strftime('%s','now')
        """, (system_key, request_type, target_key, json.dumps(details)))
        self.conn.commit()
        return cur.lastrowid

    def count_sync_requests(self, system_key: str,
                            request_type: Optional[str] = None) -> int:
        if request_type:
            row = self.conn.execute("""
                SELECT COUNT(*) as cnt FROM sync_requests
                WHERE system_key = ? AND request_type = ?
            """, (system_key, request_type)).fetchone()
        else:
            row = self.conn.execute("""
                SELECT COUNT(*) as cnt FROM sync_requests
                WHERE system_key = ?
            """, (system_key,)).fetchone()
        return int(row["cnt"]) if row else 0

    # ── Migration: import existing catalog.json ──

    def import_catalog_json(self, catalog_path: Path) -> int:
        """Import drivers from a catalog.json file. Returns count imported."""
        if not catalog_path.exists():
            return 0
        with open(catalog_path, "r") as f:
            data = json.load(f)
        count = 0
        for entry in data.get("drivers", []):
            meta = DriverMeta(
                name=entry["name"],
                version=entry.get("version", "1.0.0"),
                vendor_id=entry["vendor_id"].replace("0x", "").lower(),
                device_id=entry["device_id"].replace("0x", "").lower(),
                arch=entry["arch"],
                category=entry.get("category", "other"),
                size_bytes=entry.get("size_bytes", 0),
                sha256=entry.get("sha256", ""),
                description=entry.get("description", ""),
                min_os_version=entry.get("min_os_version", "1.0"),
                flags=entry.get("flags", 0),
            )
            self.upsert_driver(meta)
            count += 1
        return count
