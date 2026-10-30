"""
经 JTAG 控制并读出开发板上的调试录音（main.c 中 rec_buf，7 路原始数据）

用法：
  python rec_dump.py --arm [门限dB]     待触发连续录音：第一个高于背景"门限"（默认 25dB）的声音出现后
                                         录约 15 秒自动停止（保留触发前约 1 秒），用于分步骤的标注采集
  python rec_dump.py --loud             只录响亮块的循环录音（默认模式），并清空
  python rec_dump.py --continuous       立即连续录满约 16 秒（不依赖声音触发门限）
  python rec_dump.py --status           查看录音状态
  python rec_dump.py [输出文件.npz]      读出录音：samples[块, 帧, 8 通道]（int32），blkno[块]
需要：开发板正在运行 sound_loc.elf、JTAG 已连接；SDK 2018.3 安装在 D:\\Xilinx
"""
import os
import struct
import subprocess
import sys
import tempfile

import numpy as np

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
ELF = os.path.join(ROOT, "sw", "workspace", "sound_loc", "Debug", "sound_loc.elf")
XSCT = os.environ.get("XSCT", r"D:\Xilinx\SDK\2018.3\bin\xsct.bat")
NM = os.environ.get("ARM_NM", r"D:\Xilinx\SDK\2018.3\gnu\aarch32\nt\gcc-arm-none-eabi\bin\arm-none-eabi-nm.exe")
FRAMES = 1024
WORDS_PER_BLOCK = FRAMES * 8
HEAD = 'connect\ntargets -set -nocase -filter {name =~ "*A9*#0"}\n'


def symbols():
    out = subprocess.run([NM, "-S", ELF], capture_output=True, text=True, check=True).stdout
    syms = {}
    for line in out.splitlines():
        p = line.split()
        if len(p) == 4:
            syms[p[3]] = (int(p[0], 16), int(p[1], 16))
    return syms


def run_tcl(text):
    tmp = tempfile.mkdtemp()
    tcl = os.path.join(tmp, "cmd.tcl")
    with open(tcl, "w") as fp:
        fp.write(HEAD + text + '\nputs "RECDUMP_TCL_OK"\n')
    result = subprocess.run([XSCT, tcl], capture_output=True, text=True, errors='replace', timeout=90)
    if result.returncode or 'RECDUMP_TCL_OK' not in result.stdout:
        raise RuntimeError('JTAG command failed; check board power, JTAG connection and running firmware.\n'
                           + (result.stdout + '\n' + result.stderr)[-3000:])
    return result.stdout


def addr(syms, name):
    return f"0x{syms[name][0]:08x}"


def main():
    syms = symbols()
    arg = sys.argv[1] if len(sys.argv) > 1 else ""
    frz, idx, mode, left = (addr(syms, n) for n in ("rec_freeze", "rec_idx", "rec_mode", "rec_left"))

    if arg in ("--arm", "--loud", "--continuous"):
        trig = float(sys.argv[2]) if arg == "--arm" and len(sys.argv) > 2 else 25.0
        trig_bits = struct.unpack("<I", struct.pack("<f", trig))[0]
        record_left = syms['rec_buf'][1] // (WORDS_PER_BLOCK * 4) if arg == '--continuous' else 0xffffffff
        run_tcl(f"mwr {frz} 1\nafter 100\nmwr {idx} 0\nmwr {left} {record_left}\n"
                f"mwr {addr(syms, 'rec_trig_db')} 0x{trig_bits:08x}\n"
                f"mwr {mode} {0 if arg == '--loud' else 1}\nmwr {frz} 0\n")
        print(f"armed (trigger {trig:.0f} dB above background)" if arg == "--arm"
              else "continuous recording started (about 16 s)" if arg == '--continuous'
              else "loud-block recording, cleared")
        return
    if arg == "--status":
        out = run_tcl(f'puts "STATUS [mrd -value {mode} 1] [mrd -value {idx} 1] '
                      f'[mrd -value {left} 1] [mrd -value {frz} 1]"\n')
        lines = [l for l in out.splitlines() if l.startswith("STATUS")]
        if not lines:
            raise RuntimeError('JTAG returned no recording status:\n' + out[-2000:])
        line = lines[0].split()
        lf = int(line[3]) - (1 << 32) if int(line[3]) >= 1 << 31 else int(line[3])
        print(f"mode={line[1]} recorded_blocks={line[2]} left={lf} frozen={line[4]}")
        return

    out_file = arg or os.path.join(ROOT, "rec", "rec.npz")
    os.makedirs(os.path.dirname(os.path.abspath(out_file)), exist_ok=True)
    buf_addr, buf_size = syms["rec_buf"]
    nblocks = buf_size // (WORDS_PER_BLOCK * 4)
    tmp = tempfile.mkdtemp()
    raw = os.path.join(tmp, "rec.bin").replace("\\", "/")
    bn_file = os.path.join(tmp, "blkno.bin").replace("\\", "/")
    idx_file = os.path.join(tmp, "idx.txt").replace("\\", "/")
    # 未绕回时只读已录的块，加快读出
    run_tcl(f"""set fz [mrd -value {frz} 1]
mwr {frz} 1
after 100
set i [mrd -value {idx} 1]
set f [open {idx_file} w]; puts $f $i; close $f
set n [expr {{$i < {nblocks} ? $i : {nblocks}}}]
if {{$n > 0}} {{
    mrd -bin -file {raw} 0x{buf_addr:08x} [expr {{$n * {WORDS_PER_BLOCK}}}]
    mrd -bin -file {bn_file} {addr(syms, "rec_blkno")} $n
}}
mwr {frz} $fz
""")
    rec_idx = int(open(idx_file).read().strip())
    n = min(rec_idx, nblocks)
    if n == 0:
        print("no recorded blocks")
        return
    words = np.fromfile(raw, dtype="<u4").reshape(n, FRAMES, 8)
    blkno = np.fromfile(bn_file, dtype="<u4")
    if rec_idx > nblocks:                       # 已绕回：最旧的块在 rec_idx % N
        order = [(rec_idx + i) % nblocks for i in range(nblocks)]
        words, blkno = words[order], blkno[order]
    samples = ((words & 0xFFFFFF).astype(np.int32) << 8) >> 8
    np.savez_compressed(out_file, samples=samples, blkno=blkno, rec_idx=rec_idx)
    print(f"saved {n} blocks ({n * FRAMES / 48828.125:.1f} s, rec_idx={rec_idx}) -> {out_file}")


if __name__ == "__main__":
    main()
