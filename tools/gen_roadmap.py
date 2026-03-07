#!/usr/bin/env python3
"""AlJefra OS — Roadmap HTML Generator

Reads ROADMAP.md (the single source of truth) and generates
website/roadmap.html automatically.  This ensures the website
always matches the actual roadmap state.

Usage:
    python3 tools/gen_roadmap.py                # Generate roadmap.html
    python3 tools/gen_roadmap.py --check        # Check if out of date
    python3 tools/gen_roadmap.py --dry-run      # Preview without writing

No external dependencies — pure Python 3.6+.
"""

import argparse
import os
import re
import sys
from pathlib import Path

# ── Locate repo root ──────────────────────────────────────────────────

def find_repo_root():
    script_dir = Path(__file__).resolve().parent
    candidate = script_dir.parent
    if (candidate / "ROADMAP.md").exists():
        return candidate
    cwd = Path.cwd()
    if (cwd / "ROADMAP.md").exists():
        return cwd
    print("ERROR: Cannot find ROADMAP.md", file=sys.stderr)
    sys.exit(2)


# ── Parse ROADMAP.md ──────────────────────────────────────────────────

def parse_roadmap(text):
    """Parse ROADMAP.md into structured sections.
    Returns a list of dicts: {letter, title, status, items: [{text, done, details}]}
    """
    sections = []
    current = None
    in_metrics = False
    metrics = []
    in_hw = False
    hw_tiers = []
    current_tier = None

    for line in text.split('\n'):
        # Section headers: ### A. Core Kernel (DONE)
        m = re.match(r'^###\s+([A-Z])\.\s+(.+?)(?:\s+\((.+?)\))?\s*$', line)
        if m:
            letter, title, status = m.group(1), m.group(2), m.group(3) or ''
            current = {
                'letter': letter,
                'title': title.strip(),
                'status': status.strip(),
                'items': [],
            }
            sections.append(current)
            in_metrics = False
            in_hw = False
            continue

        # Checkbox items: - [x] or - [ ]
        m = re.match(r'^-\s+\[([ xX])\]\s+(.+)$', line)
        if m and current is not None:
            done = m.group(1).lower() == 'x'
            text = m.group(2).strip()
            current['items'].append({'text': text, 'done': done})
            continue

        # Metrics table
        if line.startswith('| Metric'):
            in_metrics = True
            continue
        if in_metrics and line.startswith('|--'):
            continue
        if in_metrics and line.startswith('|'):
            cols = [c.strip() for c in line.split('|')[1:-1]]
            if len(cols) >= 3:
                metrics.append({'metric': cols[0], 'target': cols[1], 'status': cols[2]})
            elif len(cols) >= 2:
                metrics.append({'metric': cols[0], 'target': cols[1], 'status': ''})
            continue
        if in_metrics and not line.startswith('|'):
            in_metrics = False

        # Hardware tiers
        if '### Tier' in line:
            in_hw = True
            tier_m = re.match(r'^###\s+(.+)$', line)
            if tier_m:
                current_tier = {'name': tier_m.group(1).strip(), 'devices': ''}
                hw_tiers.append(current_tier)
            continue
        if in_hw and line.startswith('---'):
            in_hw = False
            current_tier = None
            continue
        if in_hw and current_tier and line.strip() and not line.startswith('#') and not line.startswith('*'):
            current_tier['devices'] = line.strip()
            continue

    return sections, metrics, hw_tiers


# ── Count stats ───────────────────────────────────────────────────────

def compute_stats(sections):
    total_items = sum(len(s['items']) for s in sections)
    done_items = sum(sum(1 for i in s['items'] if i['done']) for s in sections)
    all_done = all(
        s['status'].upper().startswith('DONE') or
        all(i['done'] for i in s['items'])
        for s in sections if s['items']
    )
    return total_items, done_items, all_done


# ── Extract line count from ROADMAP.md metrics ───────────────────────

def get_line_count(metrics):
    for m in metrics:
        if 'codebase' in m['metric'].lower() or 'lines' in m['metric'].lower():
            # Extract number from status like "90,242 lines"
            nums = re.findall(r'[\d,]+', m['status'])
            if nums:
                return nums[0]
            nums = re.findall(r'[\d,]+', m['target'])
            if nums:
                return nums[0]
    return '90,000'


# ── HTML escaping ─────────────────────────────────────────────────────

def esc(text):
    return (text
            .replace('&', '&amp;')
            .replace('<', '&lt;')
            .replace('>', '&gt;')
            .replace('"', '&quot;'))


def md_to_html_inline(text):
    """Convert basic inline markdown to HTML (arrows, dashes)."""
    text = esc(text)
    text = text.replace(' → ', ' &rarr; ')
    text = text.replace(' — ', ' &mdash; ')
    return text


# ── Generate HTML ─────────────────────────────────────────────────────

def generate_html(sections, metrics, hw_tiers):
    total, done, all_done = compute_stats(sections)
    loc = get_line_count(metrics)
    pct = int(done / total * 100) if total > 0 else 0
    remaining = total - done

    # Status label
    if all_done:
        status_word = "shipped"
        status_note = "Everything shipped in one release. No half measures"
    else:
        status_word = "ships"
        status_note = "Everything ships in one release. No half measures"

    lines = []
    lines.append("""<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <meta name="description" content="AlJefra OS v0.7.3 Roadmap - The first Qatari operating system. AI chat, desktop GUI, driver marketplace, secure boot, 3-architecture support.">
    <title>Roadmap - AlJefra OS</title>
    <link rel="stylesheet" href="css/style.css">
</head>
<body>
    <nav>
        <div class="nav-inner">
            <div class="logo">AlJefra OS</div>
            <button class="nav-toggle" type="button" aria-label="Toggle navigation" aria-expanded="false"><span></span><span></span><span></span></button>
            <ul class="nav-links">
                <li><a href="index.html">Home</a></li>
                <li><a href="docs.html">Docs</a></li>
                <li><a href="architecture.html">Architecture</a></li>
                <li><a href="marketplace.html">Marketplace</a></li>
                <li><a href="roadmap.html">Roadmap</a></li>
                <li><a href="download.html">Download</a></li>
                <li><a href="https://github.com/qatarit-com/AlJefra-OS" class="nav-github" target="_blank"><svg viewBox="0 0 16 16"><path d="M8 0C3.58 0 0 3.58 0 8c0 3.54 2.29 6.53 5.47 7.59.4.07.55-.17.55-.38 0-.19-.01-.82-.01-1.49-2.01.37-2.53-.49-2.69-.94-.09-.23-.48-.94-.82-1.13-.28-.15-.68-.52-.01-.53.63-.01 1.08.58 1.23.82.72 1.21 1.87.87 2.33.66.07-.52.28-.87.51-1.07-1.78-.2-3.64-.89-3.64-3.95 0-.87.31-1.59.82-2.15-.08-.2-.36-1.02.08-2.12 0 0 .67-.21 2.2.82.64-.18 1.32-.27 2-.27.68 0 1.36.09 2 .27 1.53-1.04 2.2-.82 2.2-.82.44 1.1.16 1.92.08 2.12.51.56.82 1.27.82 2.15 0 3.07-1.87 3.75-3.65 3.95.29.25.54.73.54 1.48 0 1.07-.01 1.93-.01 2.2 0 .21.15.46.55.38A8.013 8.013 0 0016 8c0-4.42-3.58-8-8-8z"/></svg>GitHub</a></li>
            </ul>
        </div>
    </nav>

    <section class="page-header">
        <h1>v0.7.3 Production Roadmap</h1>
        <p>""" + esc(status_note) + """</p>
    </section>""")

    # Status cards
    arch_count = '3'
    for m in metrics:
        if 'architect' in m['metric'].lower():
            nums = re.findall(r'\d+', m['target'])
            if nums:
                arch_count = nums[0]

    green = 'var(--accent-green)'
    lines.append(f"""
    <!-- Status Overview (auto-generated from ROADMAP.md) -->
    <section class="section">
        <h2>Launch Status</h2>
        <div style="display: grid; grid-template-columns: repeat(auto-fit, minmax(200px, 1fr)); gap: 1rem; margin: 1.5rem 0;">
            <div style="background: var(--bg-secondary); border: 1px solid {green}; border-radius: 8px; padding: 1.25rem; text-align: center;">
                <div style="font-family: var(--font-mono); font-size: 2rem; color: {green};">{esc(loc)}</div>
                <div style="color: var(--text-secondary); font-size: 0.85rem;">Lines of Code</div>
            </div>
            <div style="background: var(--bg-secondary); border: 1px solid {green}; border-radius: 8px; padding: 1.25rem; text-align: center;">
                <div style="font-family: var(--font-mono); font-size: 2rem; color: {green};">{remaining}</div>
                <div style="color: var(--text-secondary); font-size: 0.85rem;">Items Remaining</div>
            </div>
            <div style="background: var(--bg-secondary); border: 1px solid {green}; border-radius: 8px; padding: 1.25rem; text-align: center;">
                <div style="font-family: var(--font-mono); font-size: 2rem; color: {green};">{esc(arch_count)}</div>
                <div style="color: var(--text-secondary); font-size: 0.85rem;">Architectures</div>
            </div>
            <div style="background: var(--bg-secondary); border: 1px solid {green}; border-radius: 8px; padding: 1.25rem; text-align: center;">
                <div style="font-family: var(--font-mono); font-size: 2rem; color: {green};">{pct}%</div>
                <div style="color: var(--text-secondary); font-size: 0.85rem;">v0.7.3 Complete</div>
            </div>
        </div>
    </section>""")

    # Feature sections
    for sec in sections:
        sec_done = all(i['done'] for i in sec['items']) if sec['items'] else False
        is_done = sec['status'].upper().startswith('DONE') or sec_done

        if is_done:
            heading = f"{sec['letter']}. {esc(sec['title'])} &mdash; Complete &#x2705;"
        else:
            heading = f"{sec['letter']}. {esc(sec['title'])} &mdash; In Progress"

        lines.append(f"""
    <section class="section">
        <h2>{heading}</h2>
        <table>
            <thead><tr><th>Feature</th><th>Details</th><th>Status</th></tr></thead>
            <tbody>""")

        for item in sec['items']:
            # Split item text into name and details at first parenthesis or dash
            text = item['text']
            # Try to split at " — " or " - " for details
            parts = re.split(r'\s+[—\-]\s+', text, maxsplit=1)
            if len(parts) == 2:
                name, details = parts
            else:
                # Try parenthetical
                pm = re.match(r'^(.+?)\s*\((.+)\)\s*$', text)
                if pm:
                    name, details = pm.group(1), pm.group(2)
                else:
                    name = text
                    details = ''

            status_color = green if item['done'] else 'var(--accent-orange)'
            status_text = 'Done' if item['done'] else 'In Progress'

            lines.append(f'                <tr><td>{md_to_html_inline(name)}</td>'
                        f'<td>{md_to_html_inline(details)}</td>'
                        f'<td style="color: {status_color};">{status_text}</td></tr>')

        lines.append("""            </tbody>
        </table>
    </section>""")

    # Hardware support
    if hw_tiers:
        lines.append("""
    <section class="section">
        <h2>Hardware Support</h2>""")
        for tier in hw_tiers:
            lines.append(f"        <h3>{esc(tier['name'])}</h3>")
            lines.append(f"        <p>{esc(tier['devices'])}</p>")
        lines.append("    </section>")

    # Metrics
    if metrics:
        lines.append("""
    <section class="section">
        <h2>v0.7.3 Metrics</h2>
        <table>
            <thead><tr><th>Metric</th><th>Target</th><th>Status</th></tr></thead>
            <tbody>""")
        for m in metrics:
            status = m.get('status', '')
            if status.lower() in ('done', 'achieved') or 'done' in status.lower():
                color = green
            elif 'progress' in status.lower():
                color = 'var(--text-secondary)'
            else:
                color = green
            lines.append(f'                <tr><td>{esc(m["metric"])}</td>'
                        f'<td>{esc(m["target"])}</td>'
                        f'<td style="color: {color};">{esc(status)}</td></tr>')
        lines.append("""            </tbody>
        </table>
    </section>""")

    # Footer
    lines.append("""
    <footer>
        <p>&copy; 2026 AlJefra OS v0.7.3 | The First Qatari Operating System | <a href="https://os.aljefra.com">os.aljefra.com</a></p>
        <p style="margin-top: 0.75rem; font-size: 0.85rem;">Developed by <a href="https://www.QatarIT.com" style="color: var(--accent-green);">Qatar IT</a> &mdash; <a href="https://www.QatarIT.com">www.QatarIT.com</a></p>
    </footer>
</body>
</html>
""")

    return '\n'.join(lines)


# ── Main ──────────────────────────────────────────────────────────────

def main():
    ap = argparse.ArgumentParser(
        description="Generate website/roadmap.html from ROADMAP.md")
    ap.add_argument("--check", action="store_true",
                    help="Check if roadmap.html is up to date (exit 1 if stale)")
    ap.add_argument("--dry-run", action="store_true",
                    help="Print generated HTML to stdout without writing")
    ap.add_argument("--repo-root", metavar="DIR",
                    help="Path to the repository root")
    args = ap.parse_args()

    repo = Path(args.repo_root).resolve() if args.repo_root else find_repo_root()

    roadmap_md = repo / "ROADMAP.md"
    roadmap_html = repo / "website" / "roadmap.html"

    md_text = roadmap_md.read_text()
    sections, metrics, hw_tiers = parse_roadmap(md_text)
    html = generate_html(sections, metrics, hw_tiers)

    if args.dry_run:
        print(html)
        return 0

    if args.check:
        if roadmap_html.exists():
            current = roadmap_html.read_text()
            if current.strip() == html.strip():
                print("roadmap.html is up to date.")
                return 0
            else:
                print("roadmap.html is OUT OF DATE. Run: python3 tools/gen_roadmap.py")
                return 1
        else:
            print("roadmap.html does not exist. Run: python3 tools/gen_roadmap.py")
            return 1

    # Write
    roadmap_html.write_text(html)
    print(f"Generated {roadmap_html}")
    print(f"  Sections: {len(sections)}")
    total, done, all_done = compute_stats(sections)
    print(f"  Items: {done}/{total} done ({int(done/total*100)}%)")
    if all_done:
        print("  Status: ALL COMPLETE")
    return 0


if __name__ == "__main__":
    sys.exit(main())
