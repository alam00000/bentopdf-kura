import re
import sys

PATTERNS = [
    (rb'D:\d{14}', b'D:00000000000000'),
    (rb'\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d', b'0000-00-00T00:00:00'),
    (rb'uuid:[0-9a-fA-F-]{36}', b'uuid:' + b'0' * 36),
    (rb'<[0-9a-fA-F]{32}>', b'<' + b'0' * 32 + b'>'),
]


def norm(path):
    data = open(path, 'rb').read()
    for pattern, repl in PATTERNS:
        data = re.sub(pattern, repl, data)
    return data


sys.exit(0 if norm(sys.argv[1]) == norm(sys.argv[2]) else 1)
