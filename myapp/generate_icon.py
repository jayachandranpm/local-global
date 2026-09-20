#!/usr/bin/env python3
"""Generate macOS .icns icon for Local Global — orbital logo design."""
import struct, zlib, os, subprocess, sys, math


def create_icon(size, filename):
    """Render the Local Global orbital logo at the given pixel size."""
    pixels = []
    cx, cy = size / 2.0, size / 2.0
    scale = size / 512.0  # reference design is 512x512

    # Background rounded rect params
    corner_r = 112 * scale

    # Orbital rings (rx, ry, angle_deg, stroke_width, r, g, b, alpha)
    rings = [
        (176 * scale, 68 * scale, -30, 3.0 * scale, 110, 231, 183, 0.35),
        (152 * scale, 62 * scale,  25, 4.5 * scale,  52, 211, 153, 0.55),
        (128 * scale, 56 * scale, -55, 6.0 * scale,  16, 185, 129, 1.0),
    ]

    # Satellite nodes (x, y, radius, r, g, b, alpha)
    satellites = [
        (358, 146, 9, 52, 211, 153, 1.0),
        (148, 358, 7, 16, 185, 129, 1.0),
        (392, 294, 6,  5, 150, 105, 1.0),
        (164, 170, 5, 52, 211, 153, 0.7),
    ]

    for y in range(size):
        row = []
        for x in range(size):
            fx, fy = float(x), float(y)

            # --- Background: dark rounded rect ---
            in_rect = True
            if fx < corner_r and fy < corner_r:
                if ((fx - corner_r) ** 2 + (fy - corner_r) ** 2) > corner_r ** 2:
                    in_rect = False
            elif fx > size - corner_r and fy < corner_r:
                if ((fx - (size - corner_r)) ** 2 + (fy - corner_r) ** 2) > corner_r ** 2:
                    in_rect = False
            elif fx < corner_r and fy > size - corner_r:
                if ((fx - corner_r) ** 2 + (fy - (size - corner_r)) ** 2) > corner_r ** 2:
                    in_rect = False
            elif fx > size - corner_r and fy > size - corner_r:
                if ((fx - (size - corner_r)) ** 2 + (fy - (size - corner_r)) ** 2) > corner_r ** 2:
                    in_rect = False

            if not in_rect:
                row.extend([0, 0, 0, 0])
                continue

            pr, pg, pb, pa = 13, 13, 13, 255

            # Subtle radial glow
            dist_c = math.sqrt((fx - cx) ** 2 + (fy - cy) ** 2)
            glow_r = 200 * scale
            if dist_c < glow_r:
                glow_t = 1.0 - dist_c / glow_r
                gi = glow_t * glow_t * 0.06
                pg = min(255, int(pg + 185 * gi * 10))
                pb = min(255, int(pb + 129 * gi * 10))

            # --- Orbital rings ---
            for rx, ry, angle, sw, rr, rg, rb, ra in rings:
                rad = math.radians(angle)
                cos_a, sin_a = math.cos(rad), math.sin(rad)
                dx, dy = fx - cx, fy - cy
                ex = cos_a * dx + sin_a * dy
                ey = -sin_a * dx + cos_a * dy
                if rx > 0 and ry > 0:
                    norm = math.sqrt((ex / rx) ** 2 + (ey / ry) ** 2)
                    if norm > 0:
                        dist_to_ellipse = abs(norm - 1.0) * min(rx, ry) * (1.0 + 0.5 * abs(norm - 1.0))
                    else:
                        dist_to_ellipse = min(rx, ry)
                    if dist_to_ellipse < sw:
                        t = 1.0 - dist_to_ellipse / sw
                        t = t * t
                        alpha = t * ra
                        pr = min(255, int(pr * (1 - alpha) + rr * alpha))
                        pg = min(255, int(pg * (1 - alpha) + rg * alpha))
                        pb = min(255, int(pb * (1 - alpha) + rb * alpha))

            # --- Core node ---
            core_glow = 42 * scale
            if dist_c < core_glow:
                gt = 1.0 - dist_c / core_glow
                gi2 = gt * gt * 0.12
                pg = min(255, int(pg + 185 * gi2))
                pb = min(255, int(pb + 129 * gi2))

            core_r2 = 34 * scale
            if dist_c < core_r2:
                gt = 1.0 - dist_c / core_r2
                gi2 = gt * gt * 0.2
                pr = min(255, int(pr * (1 - gi2) + 16 * gi2))
                pg = min(255, int(pg * (1 - gi2) + 185 * gi2))
                pb = min(255, int(pb * (1 - gi2) + 129 * gi2))

            core_solid = 26 * scale
            if dist_c < core_solid:
                t = 1.0 - dist_c / core_solid
                blend = min(1.0, t * 3)
                cr = int(52 * (1 - t) + 16 * t)
                cg = int(211 * (1 - t) + 185 * t)
                cb = int(153 * (1 - t) + 129 * t)
                pr = int(pr * (1 - blend) + cr * blend)
                pg = int(pg * (1 - blend) + cg * blend)
                pb = int(pb * (1 - blend) + cb * blend)

            # Inner highlight
            hl_cx, hl_cy = cx - 6 * scale, cy - 6 * scale
            hl_dist = math.sqrt((fx - hl_cx) ** 2 + (fy - hl_cy) ** 2)
            hl_r = 10 * scale
            if hl_dist < hl_r:
                ht = 1.0 - hl_dist / hl_r
                hi = ht * ht * 0.45
                pr = min(255, int(pr * (1 - hi) + 110 * hi))
                pg = min(255, int(pg * (1 - hi) + 231 * hi))
                pb = min(255, int(pb * (1 - hi) + 183 * hi))

            # --- Satellites ---
            for sx, sy, sr, srr, srg, srb, sa in satellites:
                ssx, ssy, ssr = sx * scale, sy * scale, sr * scale
                sd = math.sqrt((fx - ssx) ** 2 + (fy - ssy) ** 2)
                if sd < ssr * 1.5:
                    gt = 1.0 - sd / (ssr * 1.5)
                    gi2 = gt * gt * 0.3 * sa
                    pr = min(255, int(pr * (1 - gi2) + srr * gi2))
                    pg = min(255, int(pg * (1 - gi2) + srg * gi2))
                    pb = min(255, int(pb * (1 - gi2) + srb * gi2))
                if sd < ssr:
                    st = 1.0 - sd / ssr
                    si = min(1.0, st * 2.5) * sa
                    pr = min(255, int(pr * (1 - si) + srr * si))
                    pg = min(255, int(pg * (1 - si) + srg * si))
                    pb = min(255, int(pb * (1 - si) + srb * si))
                inner_r = ssr * 0.55
                if sd < inner_r:
                    it = 1.0 - sd / inner_r
                    ii = it * it * sa
                    pr = min(255, int(pr * (1 - ii) + min(255, srr + 60) * ii))
                    pg = min(255, int(pg * (1 - ii) + min(255, srg + 60) * ii))
                    pb = min(255, int(pb * (1 - ii) + min(255, srb + 60) * ii))

            row.extend([max(0, min(255, pr)), max(0, min(255, pg)),
                        max(0, min(255, pb)), pa])
        pixels.append(bytes(row))

    # Write minimal PNG
    def chunk(ctype, data):
        c = ctype + data
        crc = zlib.crc32(c) & 0xFFFFFFFF
        return struct.pack('>I', len(data)) + c + struct.pack('>I', crc)

    with open(filename, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n')
        f.write(chunk(b'IHDR', struct.pack('>IIBBBBB', size, size, 8, 6, 0, 0, 0)))
        raw = b''.join(b'\x00' + row for row in pixels)
        f.write(chunk(b'IDAT', zlib.compress(raw)))
        f.write(chunk(b'IEND', b''))


def main():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    iconset_dir = os.path.join(script_dir, 'AppIcon.iconset')
    os.makedirs(iconset_dir, exist_ok=True)

    sizes = [16, 32, 64, 128, 256, 512]
    for s in sizes:
        print(f"  Generating {s}x{s}...")
        create_icon(s, os.path.join(iconset_dir, f'icon_{s}x{s}.png'))
        if s <= 512:
            s2 = s * 2
            print(f"  Generating {s}x{s}@2x ({s2}x{s2})...")
            create_icon(s2, os.path.join(iconset_dir, f'icon_{s}x{s}@2x.png'))

    # Convert to .icns using iconutil (macOS only)
    icns_path = os.path.join(script_dir, 'AppIcon.icns')
    print(f"\nConverting to {icns_path}...")
    result = subprocess.run(
        ['iconutil', '-c', 'icns', iconset_dir, '-o', icns_path],
        capture_output=True, text=True
    )
    if result.returncode != 0:
        print(f"iconutil error: {result.stderr}", file=sys.stderr)
        sys.exit(1)

    print(f"✅ Created {icns_path}")
    # Clean up iconset
    import shutil
    shutil.rmtree(iconset_dir)
    print("Cleaned up iconset directory.")


if __name__ == '__main__':
    main()
