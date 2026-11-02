#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""生成上位机图标 host/ups.ico（构建资源，运行一次即可）。"""

import os

from PIL import Image, ImageDraw


def make(size: int = 256) -> Image.Image:
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    draw = ImageDraw.Draw(img)
    s = size / 256.0

    def box(values):
        return [value * s for value in values]

    draw.rounded_rectangle(box([0, 0, 255, 255]), radius=int(54 * s), fill=(29, 78, 216, 255))

    # 电池外壳与正极
    draw.rounded_rectangle(
        box([42, 82, 194, 184]), radius=int(18 * s),
        outline=(255, 255, 255, 255), width=max(2, int(14 * s)),
    )
    draw.rounded_rectangle(
        box([194, 112, 214, 154]), radius=int(6 * s), fill=(255, 255, 255, 255)
    )

    # 电量填充
    draw.rounded_rectangle(
        box([64, 104, 148, 162]), radius=int(7 * s), fill=(74, 222, 128, 255)
    )

    # 闪电
    bolt = [(146, 92), (108, 142), (132, 142), (114, 176), (160, 126), (134, 126)]
    draw.polygon([(x * s, y * s) for x, y in bolt], fill=(251, 191, 36, 255))
    return img


def main() -> None:
    target = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                          "host", "ups.ico")
    image = make(256)
    image.save(target, sizes=[(n, n) for n in (16, 24, 32, 48, 64, 128, 256)])
    print(target)


if __name__ == "__main__":
    main()
