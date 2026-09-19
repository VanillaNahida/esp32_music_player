#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""编译前扫描 src/music/*.h，生成曲目表 src/score_playlist.h。

乐谱头文件（AudioLab 导出）格式固定：4 个宏 + static const ScoreNote g_score[]。
每首曲子的宏名和数组名都相同，直接连续 #include 必然冲突，因此生成代码对每首曲子：
  1) 先用 #define g_score g_score_<i> 把音符数组改名；
  2) include 之后立刻 #undef 掉 4 个宏。
这样 src/music/ 下的全部乐谱都能编进同一个固件，增删曲目无需改动任何源码。

由 platformio.ini 的 extra_scripts = pre:scripts/gen_score_playlist.py 在每次编译前调用。
"""

import glob
import os
import re
import sys

try:  # 中文日志在 cp936 控制台下不要因编码异常中断编译
    sys.stdout.reconfigure(errors="replace")
except Exception:
    pass

# ---- 工程根目录 ----
# 作为 PlatformIO 的 extra_scripts 由 SCons 执行时没有 __file__，
# 只能从 env("PROJECT_DIR") 取；直接 python 运行脚本时用 __file__ 兜底。
_ENV = None
try:
    Import("env")          # noqa: F821  —— SCons 注入的 env
    _ENV = env             # noqa: F821
except Exception:
    _ENV = None


def _find_project_dir():
    if _ENV is not None:
        d = _ENV.subst("$PROJECT_DIR")
        if d and os.path.isdir(d):
            return d
    try:
        here = os.path.dirname(os.path.abspath(__file__))
    except NameError:
        argv0 = sys.argv[0] if sys.argv else ""
        here = os.path.dirname(os.path.abspath(argv0)) if argv0 else os.getcwd()
    d = here
    for _ in range(4):          # 向上找带 platformio.ini 的目录
        if os.path.isfile(os.path.join(d, "platformio.ini")):
            return d
        nd = os.path.dirname(d)
        if nd == d:
            break
        d = nd
    return os.path.dirname(here)


PROJECT_DIR = _find_project_dir()
MUSIC_DIR = os.path.join(PROJECT_DIR, "src", "music")
OUT_PATH = os.path.join(PROJECT_DIR, "src", "score_playlist.h")

MACROS = ("SCORE_NAME", "SCORE_NOTE_COUNT", "SCORE_TOTAL_TIME_MS", "SCORE_TICK_US")


def read_score(path):
    """读取一个乐谱头文件；不是乐谱则返回 None。"""
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as fp:
            text = fp.read()
    except OSError as exc:
        print("[playlist] 读取失败 %s: %s" % (path, exc))
        return None

    def macro(name):
        mo = re.search(r"^#define\s+%s\s+(.+?)\s*$" % name, text, re.M)
        return mo.group(1) if mo else None

    title = macro("SCORE_NAME")
    count = macro("SCORE_NOTE_COUNT")
    if title is None or count is None:
        print("[playlist] 跳过 %s（不是乐谱头文件）" % os.path.basename(path))
        return None

    mo = re.search(r"^#ifndef\s+(\w+)\s*$", text, re.M)
    return {
        "file": os.path.basename(path),
        "title": title,
        "count": count,
        "total": macro("SCORE_TOTAL_TIME_MS") or "0u",
        "tick": macro("SCORE_TICK_US") or "1u",
        "guard": mo.group(1) if mo else "",
    }


def build_text(scores):
    out = []
    add = out.append
    add("/* ============================================================")
    add(" * score_playlist.h -- 曲目表（自动生成，请勿手工修改）")
    add(" *")
    add(" * 由 scripts/gen_score_playlist.py 编译前扫描 src/music/*.h 生成，")
    add(" * 收录 src/music/ 下的全部乐谱。新增曲目只需把 AudioLab 导出的 .h")
    add(" * 放进 src/music/ 再编译即可，不必修改任何源码。")
    add(" *")
    add(" * 编入的曲目数：%d" % len(scores))
    for i, s in enumerate(scores):
        add(" *   [%d] %-28s %s" % (i, s["file"], s["title"]))
    add(" * ============================================================ */")
    add("#ifndef __SCORE_PLAYLIST_H")
    add("#define __SCORE_PLAYLIST_H")
    add("")
    add('#include "Music.h"')
    add("")
    add("#define SCORE_COUNT %du" % len(scores))
    add("")
    for i, s in enumerate(scores):
        add("/* ---- [%d] %s : %s ---- */" % (i, s["file"], s["title"]))
        add("#define g_score g_score_%d" % i)
        add('#include "music/%s"' % s["file"])
        add("#undef g_score")
        add('static const char       SCORE_FILE_%d[]  = "%s";' % (i, s["file"]))
        add("static const char       SCORE_TITLE_%d[] = %s;" % (i, s["title"]))
        add("static const MusicScore SCORE_DEF_%d     = { %s, %s, %s, g_score_%d };"
            % (i, s["count"], s["total"], s["tick"], i))
        for name in MACROS:
            add("#undef %s" % name)
        add("")
    add("/* 曲目表：索引 -> 乐谱 / 曲名 / 乐谱文件名 */")
    add("static const MusicScore *const g_playlist[SCORE_COUNT] = {")
    for i, s in enumerate(scores):
        add("    &SCORE_DEF_%d,%s/* %d: %s */" % (i, " " * (2 if i < 10 else 1), i, s["title"]))
    add("};")
    add("static const char *const g_playlist_names[SCORE_COUNT] = {")
    for i, s in enumerate(scores):
        add("    SCORE_TITLE_%d,%s/* %s */" % (i, " " * (2 if i < 10 else 1), s["file"]))
    add("};")
    add("static const char *const g_playlist_files[SCORE_COUNT] = {")
    for i, s in enumerate(scores):
        add("    SCORE_FILE_%d," % i)
    add("};")
    add("")
    add("#endif /* __SCORE_PLAYLIST_H */")
    add("")
    return "\n".join(out)


def main():
    paths = sorted(glob.glob(os.path.join(MUSIC_DIR, "*.h")))
    scores = []
    guards = {}
    for path in paths:
        s = read_score(path)
        if not s:
            continue
        # 同一首曲子导出两次会共用包含保护宏，第二个 include 会被宏保护挡掉，
        # 导致生成的代码里宏未定义而编译报错 —— 这里直接跳过并提示。
        g = s["guard"]
        if g and g in guards:
            print("[playlist] 跳过 %s（包含保护宏 %s 与 %s 相同，可能是同一首曲子的重复导出）"
                  % (s["file"], g, guards[g]))
            continue
        if g:
            guards[g] = s["file"]
        scores.append(s)
    if not scores:
        print("[playlist] 警告：src/music/ 下没有找到任何乐谱头文件")

    text = build_text(scores)

    old = None
    if os.path.exists(OUT_PATH):
        with open(OUT_PATH, "r", encoding="utf-8", errors="replace") as fp:
            old = fp.read()
    if old != text:
        with open(OUT_PATH, "w", encoding="utf-8", newline="\n") as fp:
            fp.write(text)
        print("[playlist] 已生成 %s（%d 首）" % (os.path.relpath(OUT_PATH, PROJECT_DIR), len(scores)))
    else:
        print("[playlist] %d 首曲目无变化" % len(scores))

    for i, s in enumerate(scores):
        print("  #%-2d %-30s %s" % (i, s["file"], s["title"]))


# 注意：本脚本由 SCons（PlatformIO extra_scripts）exec 执行，此时 __name__ 并不是
# "__main__"，所以不能用 if __name__ == "__main__" 守卫，必须直接调用。
main()
