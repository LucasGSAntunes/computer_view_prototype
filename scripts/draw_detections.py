#!/usr/bin/env python3
"""
Draws bounding boxes with labels on video frames from detections CSV.

Usage:
  python3 scripts/draw_detections.py \
    -i test_midia/video.mp4 \
    -d output/detections.csv \
    -o output/annotated.mp4 \
    -c 0.50
"""

import argparse
import csv
import cv2
import sys
from collections import defaultdict

# Color palette (BGR) per class
COLORS = [
    (56, 56, 255),    # red
    (151, 157, 255),  # salmon
    (31, 112, 255),   # orange
    (29, 178, 255),   # yellow
    (49, 210, 207),   # lime
    (10, 249, 72),    # green
    (23, 204, 146),   # teal
    (134, 219, 61),   # cyan
    (211, 188, 0),    # blue
    (209, 85, 0),     # dark blue
    (255, 56, 56),    # indigo
    (180, 105, 255),  # pink
    (238, 104, 123),  # purple
    (153, 50, 204),   # violet
    (106, 90, 205),   # slate
    (0, 191, 255),    # gold
]


def get_color(class_id):
    return COLORS[class_id % len(COLORS)]


def load_detections(csv_path, min_confidence):
    """Load detections from CSV, grouped by frame number."""
    detections = defaultdict(list)
    total = 0
    filtered = 0

    with open(csv_path) as f:
        reader = csv.DictReader(f)
        for row in reader:
            total += 1
            conf = float(row["confidence"])
            if conf < min_confidence:
                continue
            filtered += 1
            frame_num = int(row["frame"])
            detections[frame_num].append({
                "class_name": row["class_name"],
                "class_id": int(row["class_id"]),
                "confidence": conf,
                "x": float(row["bbox_x"]),
                "y": float(row["bbox_y"]),
                "w": float(row["bbox_w"]),
                "h": float(row["bbox_h"]),
                "track_id": int(row["track_id"]),
            })

    print(f"Loaded {filtered}/{total} detections (conf >= {min_confidence:.2f})")
    return detections


def draw_frame(frame, dets):
    """Draw bounding boxes and labels on a single frame."""
    for det in dets:
        x1 = int(det["x"])
        y1 = int(det["y"])
        x2 = int(det["x"] + det["w"])
        y2 = int(det["y"] + det["h"])

        color = get_color(det["class_id"])
        thickness = 2

        # Draw box
        cv2.rectangle(frame, (x1, y1), (x2, y2), color, thickness)

        # Label text
        label = f'{det["class_name"]} {det["confidence"]:.0%}'
        if det["track_id"] > 0:
            label += f' #{det["track_id"]}'

        # Label background
        font = cv2.FONT_HERSHEY_SIMPLEX
        font_scale = 0.45
        font_thickness = 1
        (tw, th), baseline = cv2.getTextSize(label, font, font_scale, font_thickness)

        label_y = max(y1, th + 4)
        cv2.rectangle(frame, (x1, label_y - th - 4), (x1 + tw + 4, label_y + 2), color, -1)
        cv2.putText(frame, label, (x1 + 2, label_y - 2), font, font_scale,
                    (255, 255, 255), font_thickness, cv2.LINE_AA)

    return frame


def main():
    parser = argparse.ArgumentParser(description="Draw detections on video")
    parser.add_argument("-i", "--input", required=True, help="Input video")
    parser.add_argument("-d", "--detections", required=True, help="Detections CSV")
    parser.add_argument("-o", "--output", required=True, help="Output video")
    parser.add_argument("-c", "--confidence", type=float, default=0.50,
                        help="Minimum confidence threshold (default: 0.50)")
    parser.add_argument("--fps", type=float, default=0, help="Override output FPS")
    args = parser.parse_args()

    # Load detections
    detections = load_detections(args.detections, args.confidence)

    # Open input video
    cap = cv2.VideoCapture(args.input)
    if not cap.isOpened():
        print(f"Error: cannot open {args.input}")
        sys.exit(1)

    width = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))
    height = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
    fps = args.fps if args.fps > 0 else cap.get(cv2.CAP_PROP_FPS)
    total_frames = int(cap.get(cv2.CAP_PROP_FRAME_COUNT))

    print(f"Video: {width}x{height}, {fps:.2f} fps, {total_frames} frames")

    # Output writer
    fourcc = cv2.VideoWriter_fourcc(*"mp4v")
    writer = cv2.VideoWriter(args.output, fourcc, fps, (width, height))

    frame_num = 0
    det_frames = 0

    while True:
        ret, frame = cap.read()
        if not ret:
            break

        dets = detections.get(frame_num, [])
        if dets:
            frame = draw_frame(frame, dets)
            det_frames += 1

        writer.write(frame)
        frame_num += 1

        if frame_num % 100 == 0:
            print(f"  Frame {frame_num}/{total_frames}")

    cap.release()
    writer.release()

    print(f"\nDone: {frame_num} frames, {det_frames} with detections")
    print(f"Output: {args.output}")


if __name__ == "__main__":
    main()
