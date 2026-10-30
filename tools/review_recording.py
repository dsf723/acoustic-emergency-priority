"""Inspect dataset metadata; --approve records an operator's completed listening review.

Do not approve solely because the structure passed. Listen to the recording and
confirm both the sound label and original source before using --approve.
"""
import argparse
from datetime import datetime, timezone
import json
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('session', type=Path)
    parser.add_argument('--approve', action='store_true', help='Operator confirms completed listening/label review')
    parser.add_argument('--note', default='')
    args = parser.parse_args()
    path = args.session / 'metadata.json' if args.session.is_dir() else args.session
    m = json.loads(path.read_text(encoding='utf-8'))
    quality = m.get('quality', {})
    print(json.dumps({k: m.get(k) for k in ('session_id', 'status', 'label', 'source_id')}, ensure_ascii=False, indent=2))
    print('content_review_status:', quality.get('content_review_status', 'not_reviewed'))
    print('content_review_note:', quality.get('content_review_note', ''))
    if args.approve:
        if m.get('status') != 'complete' or quality.get('validation_status') != 'passed':
            parser.error('Only structurally complete recordings with passed validation may be approved; re-record failed sessions.')
        quality.update(content_review_status='approved', content_review_note=args.note,
                       content_reviewed_at_utc=datetime.now(timezone.utc).isoformat(),
                       content_review_provenance='operator_listening_and_label_review')
        m['quality'] = quality
        temporary = path.with_suffix('.json.tmp')
        temporary.write_text(json.dumps(m, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
        temporary.replace(path)
        print('Review saved. This records an operator judgement, not machine verification of ambulance identity.')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
