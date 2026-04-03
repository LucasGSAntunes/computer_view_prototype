#!/usr/bin/env python3
"""
Renders annotated video with object detections and accident analysis.

Accident detection strategy (hybrid):
  1. Uses C pipeline events if available
  2. Analyzes raw detection tracks for:
     - Track disappearance (vehicle stops being detected = impact)
     - Rapid convergence between vehicle bboxes
     - Sudden direction/speed changes
  3. Combines signals to classify accident vs normal traffic
"""

import argparse
import csv
import json
import cv2
import sys
import os
import math
import subprocess
from collections import defaultdict

# ── Colors (BGR) ──
CLASS_COLORS = {
    'car':       (56, 200, 56),
    'truck':     (255, 165, 0),
    'bus':       (200, 200, 0),
    'motorcycle':(200, 56, 200),
    'person':    (56, 56, 255),
}
DEFAULT_COLOR = (200, 200, 200)
ACCIDENT_RED = (0, 0, 255)
BANNER_BG = (0, 0, 180)


def get_color(cls):
    return CLASS_COLORS.get(cls, DEFAULT_COLOR)


def load_detections(csv_path, min_conf):
    dets = defaultdict(list)
    with open(csv_path) as f:
        for row in csv.DictReader(f):
            if float(row['confidence']) < min_conf:
                continue
            dets[int(row['frame'])].append(row)
    return dets


def build_tracks(csv_path, min_conf):
    """Build track histories from detections CSV."""
    tracks = defaultdict(list)
    with open(csv_path) as f:
        for row in csv.DictReader(f):
            if float(row['confidence']) < min_conf:
                continue
            tid = int(row['track_id'])
            if tid <= 0:
                continue
            tracks[tid].append({
                'frame': int(row['frame']),
                'cx': float(row['bbox_x']) + float(row['bbox_w']) / 2,
                'cy': float(row['bbox_y']) + float(row['bbox_h']) / 2,
                'w': float(row['bbox_w']),
                'h': float(row['bbox_h']),
                'class': row['class_name'],
                'conf': float(row['confidence']),
            })
    return tracks


def analyze_for_accident(tracks, total_frames, fps):
    """
    Detect accidents by analyzing track behavior.

    Key signals:
    1. A vehicle track that was moving and suddenly disappears (impact)
    2. Two vehicle tracks that converge rapidly then one/both stop
    3. A vehicle whose motion direction changes drastically (spin/rollover)

    Returns: (is_accident, accident_frame, accident_region)
    """
    vehicle_classes = {'car', 'truck', 'bus', 'motorcycle'}

    # Filter vehicle tracks with enough history
    v_tracks = {}
    for tid, pts in tracks.items():
        if pts[0]['class'] in vehicle_classes and len(pts) >= 5:
            v_tracks[tid] = pts

    if len(v_tracks) < 2:
        return False, None, None

    # Get frame dimensions from first track's bbox
    all_pts = [p for pts in v_tracks.values() for p in pts]
    frame_w = max(p['cx'] + p['w'] / 2 for p in all_pts) if all_pts else 640
    frame_h = max(p['cy'] + p['h'] / 2 for p in all_pts) if all_pts else 360
    edge_margin = 50  # pixels from edge = "exited frame"

    # Signal 1: Track disappearance while moving (NOT near frame edge)
    disappearances = []
    for tid, pts in v_tracks.items():
        last_frame = pts[-1]['frame']
        first_frame = pts[0]['frame']
        duration = last_frame - first_frame

        # Skip tracks that last until end of video
        if last_frame > total_frames - 10:
            continue
        # Skip very short tracks
        if duration < 5:
            continue

        # Skip tracks whose last position is near the frame edge
        # (they just drove out of view, not an accident)
        lx, ly = pts[-1]['cx'], pts[-1]['cy']
        near_edge = (lx < edge_margin or lx > frame_w - edge_margin or
                     ly < edge_margin or ly > frame_h - edge_margin)
        if near_edge:
            continue

        # Calculate total distance moved
        total_dist = sum(
            math.hypot(pts[i+1]['cx'] - pts[i]['cx'], pts[i+1]['cy'] - pts[i]['cy'])
            for i in range(len(pts) - 1)
        )

        # If vehicle moved significantly and disappeared mid-frame area
        if total_dist > 30:
            disappearances.append({
                'tid': tid,
                'frame': last_frame,
                'cx': lx,
                'cy': ly,
                'dist': total_dist,
                'duration': duration,
            })

    # Signal 1b: Single vehicle with sudden stop (decel + bbox size change = impact)
    for tid, pts in v_tracks.items():
        if len(pts) < 10:
            continue

        # Calculate per-point speed
        speeds = [0.0]
        for k in range(1, len(pts)):
            df = pts[k]['frame'] - pts[k-1]['frame']
            if df <= 0:
                speeds.append(speeds[-1])
                continue
            dt = df / fps
            dx = pts[k]['cx'] - pts[k-1]['cx']
            dy = pts[k]['cy'] - pts[k-1]['cy']
            speeds.append(math.hypot(dx, dy) / dt)

        # Look for sudden deceleration: speed drops by > 60% within a short window
        for k in range(5, len(pts) - 2):
            # Average speed over previous 5 frames
            prev_speeds = speeds[max(0, k-5):k]
            avg_before = sum(prev_speeds) / max(1, len(prev_speeds))
            # Average speed over next 3 frames
            next_speeds = speeds[k:min(len(speeds), k+3)]
            avg_after = sum(next_speeds) / max(1, len(next_speeds))

            if avg_before < 15:  # was barely moving, skip
                continue

            speed_drop = 1.0 - (avg_after / avg_before) if avg_before > 0 else 0

            # Also check bbox size change (impact = sudden size change)
            size_before = pts[max(0, k-3)]['w'] * pts[max(0, k-3)]['h']
            size_after = pts[min(len(pts)-1, k+3)]['w'] * pts[min(len(pts)-1, k+3)]['h']
            size_ratio = size_after / size_before if size_before > 0 else 1.0

            # Vehicle that was moving > 15px/s, drops speed by > 60%,
            # AND bbox changes size significantly (impact distortion)
            if speed_drop > 0.6 and (size_ratio > 1.5 or size_ratio < 0.6):
                # Check that vehicle doesn't end near edge (it stopped, not exited)
                last_pt = pts[-1]
                lx, ly = last_pt['cx'], last_pt['cy']
                near_edge = (lx < edge_margin or lx > frame_w - edge_margin or
                             ly < edge_margin or ly > frame_h - edge_margin)
                if not near_edge:
                    acc_frame = pts[k]['frame']
                    region = (lx - last_pt['w'], ly - last_pt['h'],
                              last_pt['w'] * 2, last_pt['h'] * 2)
                    return True, acc_frame, region

    # Signal 2: Pairs of vehicles that converge
    convergences = []
    tids = list(v_tracks.keys())
    for i in range(len(tids)):
        for j in range(i + 1, len(tids)):
            pts_a = v_tracks[tids[i]]
            pts_b = v_tracks[tids[j]]

            # Find overlapping frame range
            frames_a = {p['frame']: p for p in pts_a}
            frames_b = {p['frame']: p for p in pts_b}
            common = sorted(set(frames_a) & set(frames_b))

            if len(common) < 3:
                continue

            # Check if distance is decreasing over time
            dists = []
            for f in common:
                d = math.hypot(frames_a[f]['cx'] - frames_b[f]['cx'],
                               frames_a[f]['cy'] - frames_b[f]['cy'])
                dists.append((f, d))

            if len(dists) < 3:
                continue

            # Check for convergence: distance drops significantly
            max_d = max(d for _, d in dists)
            min_d = min(d for _, d in dists)
            min_frame = min(f for f, d in dists if d == min_d)

            if max_d > 30 and min_d < max_d * 0.3:
                convergences.append({
                    'tid_a': tids[i],
                    'tid_b': tids[j],
                    'frame': min_frame,
                    'max_dist': max_d,
                    'min_dist': min_d,
                })

    # Combine signals
    accident_frame = None
    accident_region = None

    # Check if any disappearance coincides with a convergence (within 0.5s)
    for dis in disappearances:
        for conv in convergences:
            if (conv['tid_a'] == dis['tid'] or conv['tid_b'] == dis['tid']):
                if abs(conv['frame'] - dis['frame']) < fps * 0.5:
                    accident_frame = min(conv['frame'], dis['frame'])
                    other_tid = conv['tid_b'] if conv['tid_a'] == dis['tid'] else conv['tid_a']
                    # Region around both tracks
                    pts = v_tracks[dis['tid']][-1]
                    accident_region = (pts['cx'] - 60, pts['cy'] - 60, 120, 120)
                    return True, accident_frame, accident_region

    # If we have convergence + post-event disappearance NOT near edge
    for conv in convergences:
        tid_a_pts = v_tracks[conv['tid_a']]
        tid_b_pts = v_tracks[conv['tid_b']]
        last_a = tid_a_pts[-1]['frame']
        last_b = tid_b_pts[-1]['frame']
        la_x, la_y = tid_a_pts[-1]['cx'], tid_a_pts[-1]['cy']
        lb_x, lb_y = tid_b_pts[-1]['cx'], tid_b_pts[-1]['cy']
        # Check if end position is near edge (= drove out of view)
        a_near_edge = (la_x < edge_margin or la_x > frame_w - edge_margin or
                       la_y < edge_margin or la_y > frame_h - edge_margin)
        b_near_edge = (lb_x < edge_margin or lb_x > frame_w - edge_margin or
                       lb_y < edge_margin or lb_y > frame_h - edge_margin)
        # At least one must disappear mid-frame (not at edge) shortly after convergence
        a_disappeared = (last_a - conv['frame'] < fps * 1 and
                         last_a < total_frames - 10 and not a_near_edge)
        b_disappeared = (last_b - conv['frame'] < fps * 1 and
                         last_b < total_frames - 10 and not b_near_edge)
        if a_disappeared or b_disappeared:
            cx = (la_x + lb_x) / 2
            cy = (la_y + lb_y) / 2
            return True, conv['frame'], (cx - 60, cy - 60, 120, 120)

    # Pair disappearance: disabled for normal traffic (too many false positives)
    # Only convergence + immediate disappearance signals are used above

    return False, None, None


def draw_detections(frame, dets):
    for d in dets:
        x1 = int(float(d['bbox_x']))
        y1 = int(float(d['bbox_y']))
        x2 = x1 + int(float(d['bbox_w']))
        y2 = y1 + int(float(d['bbox_h']))
        color = get_color(d['class_name'])

        cv2.rectangle(frame, (x1, y1), (x2, y2), color, 2)

        label = f"{d['class_name']} {float(d['confidence']):.0%}"
        tid = int(d['track_id'])
        if tid > 0:
            label += f" #{tid}"

        font = cv2.FONT_HERSHEY_SIMPLEX
        sc = 0.4
        (tw, th), _ = cv2.getTextSize(label, font, sc, 1)
        ly = max(y1, th + 4)
        cv2.rectangle(frame, (x1, ly - th - 4), (x1 + tw + 4, ly + 2), color, -1)
        cv2.putText(frame, label, (x1 + 2, ly - 2), font, sc, (255, 255, 255), 1, cv2.LINE_AA)


def draw_accident(frame, frame_num, acc_frame, region, fps):
    h, w = frame.shape[:2]
    is_during = acc_frame <= frame_num <= acc_frame + int(fps * 3)
    is_after = frame_num > acc_frame + int(fps * 3)

    if is_during and region:
        rx, ry, rw, rh = [int(v) for v in region]
        rx = max(0, rx); ry = max(0, ry)
        thick = 3 + int(2 * abs(math.sin(frame_num * 0.3)))
        cv2.rectangle(frame, (rx, ry), (rx + rw, ry + rh), ACCIDENT_RED, thick)
        cv2.putText(frame, "COLISAO DETECTADA", (rx, max(ry - 10, 20)),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.7, ACCIDENT_RED, 2, cv2.LINE_AA)

        # Red border
        cv2.rectangle(frame, (0, 0), (w - 1, h - 1), ACCIDENT_RED, 4)

        # Top banner
        bh = 40
        ov = frame.copy()
        cv2.rectangle(ov, (0, 0), (w, bh), BANNER_BG, -1)
        cv2.addWeighted(ov, 0.7, frame, 0.3, 0, frame)
        cv2.putText(frame, "ACIDENTE DETECTADO",
                    (w // 2 - 150, bh - 12),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.8, (255, 255, 255), 2, cv2.LINE_AA)

    if is_after:
        bh = 50
        ys = h - bh
        ov = frame.copy()
        cv2.rectangle(ov, (0, ys), (w, h), BANNER_BG, -1)
        cv2.addWeighted(ov, 0.7, frame, 0.3, 0, frame)
        cv2.putText(frame, "ACONTECEU ACIDENTE",
                    (w // 2 - 160, h - 15),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.9, (255, 255, 255), 2, cv2.LINE_AA)


def draw_safe_badge(frame):
    h, w = frame.shape[:2]
    bh = 30
    ov = frame.copy()
    cv2.rectangle(ov, (w - 250, 5), (w - 5, 5 + bh), (0, 150, 0), -1)
    cv2.addWeighted(ov, 0.7, frame, 0.3, 0, frame)
    cv2.putText(frame, "SEM ACIDENTE", (w - 240, 5 + bh - 8),
                cv2.FONT_HERSHEY_SIMPLEX, 0.6, (255, 255, 255), 2, cv2.LINE_AA)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-i", "--input", required=True)
    ap.add_argument("-d", "--data-dir", required=True)
    ap.add_argument("-o", "--output", required=True)
    ap.add_argument("-c", "--confidence", type=float, default=0.40)
    args = ap.parse_args()

    det_csv = os.path.join(args.data_dir, 'detections.csv')

    dets = load_detections(det_csv, args.confidence)
    tracks = build_tracks(det_csv, args.confidence)

    cap = cv2.VideoCapture(args.input)
    w = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))
    h = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
    fps = cap.get(cv2.CAP_PROP_FPS)
    total = int(cap.get(cv2.CAP_PROP_FRAME_COUNT))

    print(f"Video: {w}x{h}, {fps:.1f} fps, {total} frames")
    print(f"Detections: {sum(len(v) for v in dets.values())}, Tracks: {len(tracks)}")

    # Accident analysis
    is_accident, acc_frame, acc_region = analyze_for_accident(tracks, total, fps)

    if is_accident:
        print(f"*** ACIDENTE DETECTADO no frame {acc_frame} ({acc_frame/fps:.1f}s) ***")
    else:
        print("Sem acidente detectado.")

    # ffmpeg pipe
    try:
        import imageio_ffmpeg
        ffmpeg = imageio_ffmpeg.get_ffmpeg_exe()
    except ImportError:
        ffmpeg = 'ffmpeg'

    proc = subprocess.Popen([
        ffmpeg, '-y', '-f', 'rawvideo', '-pix_fmt', 'bgr24',
        '-s', f'{w}x{h}', '-r', str(fps), '-i', '-',
        '-c:v', 'libx264', '-preset', 'fast', '-crf', '20',
        '-pix_fmt', 'yuv420p', '-movflags', '+faststart', args.output
    ], stdin=subprocess.PIPE, stderr=subprocess.PIPE)

    fn = 0
    while True:
        ret, frame = cap.read()
        if not ret:
            break

        draw_detections(frame, dets.get(fn, []))

        if is_accident:
            draw_accident(frame, fn, acc_frame, acc_region, fps)
        else:
            draw_safe_badge(frame)

        proc.stdin.write(frame.tobytes())
        fn += 1
        if fn % 100 == 0:
            print(f"  Frame {fn}/{total}")

    cap.release()
    proc.stdin.close()
    proc.wait()
    print(f"Output: {args.output}")


if __name__ == "__main__":
    main()
