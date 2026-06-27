#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Generate A35 main program data flow diagram using matplotlib."""

import os
import matplotlib.pyplot as plt
import matplotlib.patches as mpatches
from matplotlib.patches import FancyBboxPatch, FancyArrowPatch

# Setup Chinese font
plt.rcParams['font.sans-serif'] = ['Noto Sans CJK SC', 'SimHei', 'DejaVu Sans']
plt.rcParams['axes.unicode_minus'] = False

OUTPUT_DIR = '/home/alientek/dvr_project/mier/文档/figures'
os.makedirs(OUTPUT_DIR, exist_ok=True)
OUTPUT_FILE = os.path.join(OUTPUT_DIR, 'a35_data_flow.png')


def draw_box(ax, x, y, w, h, text, color, text_color='white', fontsize=10, bold=False):
    """Draw a rounded rectangle box with text."""
    box = FancyBboxPatch((x - w/2, y - h/2), w, h,
                         boxstyle="round,pad=0.02,rounding_size=0.02",
                         facecolor=color, edgecolor='#333333', linewidth=1.5)
    ax.add_patch(box)
    ax.text(x, y, text, ha='center', va='center', fontsize=fontsize,
            color=text_color, weight='bold' if bold else 'normal',
            wrap=True)
    return box


def draw_diamond(ax, x, y, size, text, color, fontsize=9):
    """Draw a diamond shape for decision."""
    diamond = mpatches.RegularPolygon((x, y), numVertices=4, radius=size,
                                      orientation=0.785398,
                                      facecolor=color, edgecolor='#333333', linewidth=1.5)
    ax.add_patch(diamond)
    ax.text(x, y, text, ha='center', va='center', fontsize=fontsize,
            color='white', weight='bold', wrap=True)
    return diamond


def draw_arrow(ax, start, end, color, label=None, label_pos=0.5, connectionstyle="arc3,rad=0"):
    """Draw arrow between two points."""
    arrow = FancyArrowPatch(start, end,
                            arrowstyle='->', mutation_scale=15,
                            color=color, linewidth=2,
                            connectionstyle=connectionstyle)
    ax.add_patch(arrow)
    if label:
        mx = start[0] + (end[0] - start[0]) * label_pos
        my = start[1] + (end[1] - start[1]) * label_pos
        ax.text(mx, my + 0.03, label, ha='center', va='bottom',
                fontsize=8, color=color, weight='bold',
                bbox=dict(boxstyle='round,pad=0.2', facecolor='white',
                         edgecolor=color, alpha=0.9))


def main():
    fig, ax = plt.subplots(figsize=(16, 9))
    ax.set_xlim(0, 16)
    ax.set_ylim(0, 9)
    ax.axis('off')
    ax.set_facecolor('#f8f9fa')
    fig.patch.set_facecolor('#f8f9fa')

    # Title
    ax.text(8, 8.5, 'A35 Linux 端主程序数据流图', ha='center', va='center',
            fontsize=20, weight='bold', color='#1a1a2e')

    # Column 1: Inputs
    draw_box(ax, 2.0, 6.8, 2.4, 0.9, 'M33 实时核\n(IMU / V2X)', '#6c5ce7')
    draw_box(ax, 2.0, 4.8, 2.4, 0.9, 'USB 摄像头\n1280x720 MJPEG', '#3498db')
    draw_box(ax, 2.0, 2.8, 2.4, 0.9, '毫米波雷达\nUSART1', '#e67e22')

    # Column 2: Main loop
    draw_box(ax, 5.8, 4.8, 2.4, 1.2, '主循环\n(摄像头帧采集)', '#2d3436', fontsize=12, bold=True)

    # Column 3: Processing
    draw_box(ax, 9.2, 6.4, 2.6, 0.9, 'MJPEG 解码\n640x480 RGB', '#1abc9c')
    draw_box(ax, 9.2, 5.2, 2.6, 0.9, 'NPU 推理\nSSD MobileNet V2', '#16a085')
    draw_box(ax, 9.2, 2.8, 2.6, 0.9, '雷达解析\nTTC 计算', '#d35400')

    # Column 4: Fusion decision
    draw_diamond(ax, 12.2, 4.8, 0.75, '融合\n决策', '#c0392b')

    # Column 5: Outputs
    draw_box(ax, 15.0, 6.5, 2.4, 0.8, 'DVR 保存\n前后 15s 录像', '#2980b9')
    draw_box(ax, 15.0, 4.8, 2.4, 0.8, 'LED 闪烁\nPD11', '#e74c3c')
    draw_box(ax, 15.0, 3.1, 2.4, 0.8, '骨传导音频\n提示音', '#8e44ad')

    # Arrows
    # Camera -> Main loop
    draw_arrow(ax, (3.2, 4.8), (4.6, 4.8), '#3498db', 'MJPEG 帧')

    # M33 -> Main loop
    draw_arrow(ax, (3.2, 6.35), (4.6, 5.25), '#6c5ce7', 'RPMsg\n事件',
               connectionstyle="arc3,rad=0.2")

    # Radar -> Main loop -> Radar parser
    draw_arrow(ax, (3.2, 2.8), (4.6, 3.8), '#e67e22', '串口数据',
               connectionstyle="arc3,rad=-0.2")
    draw_arrow(ax, (5.8, 3.6), (5.8, 2.8), '#e67e22', '读取')
    draw_arrow(ax, (7.0, 2.8), (8.0, 2.8), '#d35400', '目标数据')

    # Main loop -> NPU chain
    draw_arrow(ax, (5.8, 5.4), (5.8, 6.4), '#1abc9c', '每 5 帧')
    draw_arrow(ax, (6.7, 6.4), (7.9, 6.4), '#1abc9c', 'RGB 图')
    draw_arrow(ax, (9.2, 5.75), (9.2, 5.2), '#16a085', '推理')

    # NPU -> Fusion
    draw_arrow(ax, (10.5, 5.2), (11.45, 4.95), '#16a085', '道路用户')

    # Radar -> Fusion
    draw_arrow(ax, (10.5, 2.8), (11.45, 4.65), '#d35400', 'TTC/告警')

    # Fusion -> Outputs
    draw_arrow(ax, (12.95, 5.45), (13.8, 6.5), '#c0392b', '保存')
    draw_arrow(ax, (12.95, 4.8), (13.8, 4.8), '#c0392b', '闪烁')
    draw_arrow(ax, (12.95, 4.15), (13.8, 3.5), '#8e44ad', '播放')

    # Legend
    legend_items = [
        ('#3498db', '视频数据流'),
        ('#e67e22', '雷达数据流'),
        ('#1abc9c', 'NPU 处理流'),
        ('#c0392b', '触发信号'),
        ('#6c5ce7', 'RPMsg 事件'),
    ]
    for idx, (color, label) in enumerate(legend_items):
        x = 11.0 + idx * 1.9
        y = 0.5
        ax.plot([x, x + 0.3], [y, y], color=color, linewidth=3)
        ax.text(x + 0.4, y, label, va='center', fontsize=8, color='#333')

    plt.tight_layout()
    plt.savefig(OUTPUT_FILE, dpi=150, bbox_inches='tight',
                facecolor='#f8f9fa', edgecolor='none')
    print(f"Saved: {OUTPUT_FILE}")


if __name__ == '__main__':
    main()
