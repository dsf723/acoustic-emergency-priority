"""Chinese operator console for the existing labelled capture pipeline."""
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import capture_labelled as capture


def main():
    print('救护车声音项目：标注录音')
    print('先给开发板上电，接好 JTAG；使用手机和音箱播放，电脑不会自动播放。')
    labels = {'1': 'ambulance', '2': 'background', '3': 'police', '4': 'fire',
              '5': 'horn', '6': 'speech', '7': 'music', '8': 'clap'}
    while True:
        print('\n1 救护车  2 安静背景  3 警车  4 消防车  5 喇叭  6 说话  7 音乐  8 拍手  0 退出')
        choice = input('选择本次录音类别：').strip()
        if choice == '0': return 0
        if choice not in labels:
            print('请输入 0 到 8。'); continue
        label = labels[choice]
        if label == 'ambulance':
            print('若仍用“制片帮素材188809”这一个文件，来源编号填 zhipianbang_188809。')
        print('同一原文件的不同重录必须使用同一个来源编号，避免训练/测试混淆。')
        source_id = input('来源编号（如 zhipianbang_188809 / police_clip_01 / room_background）：').strip()
        if not source_id:
            print('来源编号不能为空。'); continue
        volume = input('音量设置（可留空，例如手机60%、音箱固定）：').strip()
        notes = input('方向、距离、房间等备注（可留空）：').strip()
        args = capture.make_parser().parse_args(['--label', label, '--source-id', source_id,
                                                 '--volume', volume, '--notes', notes])
        if label == 'background':
            print('暂停警笛，保持安静。按回车后录约16秒，导出可能还需约1分钟。')
        else:
            print('打开单曲循环，在阵列位置确认声音清楚；保持位置和音量不变。')
            print('按回车后录约16秒，录满即冻结；导出期间可以停止播放。')
        session, ok = capture.capture(args)
        print(('录音完整，仍需听音复核：' if ok else '本次未通过，请查看错误并重录：') + str(session))


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (EOFError, KeyboardInterrupt):
        print('\n已结束。')
