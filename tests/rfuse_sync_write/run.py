#!/usr/bin/env python3
"""실제 sync sbuf 함수들을 추출하여 오류/short write/읽기 차단 상태를 검사한다."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'driver/rfuse/rfuse_file.c').read_text()
start = source.index('struct rfuse_write_cache_page {')
end = source.index('static ssize_t rfuse_perform_write_sbuf', start)
with tempfile.TemporaryDirectory(prefix='rfuse-sync-test-') as directory:
    tmp = Path(directory)
    (tmp / 'sync_impl.h').write_text(source[start:end])
    binary = tmp / 'sync_test'
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-Wall', '-Wextra',
                    '-Werror', '-Wno-unused-parameter', '-Wno-sign-compare',
                    '-I', str(tmp), str(Path(__file__).with_name('test.c')),
                    '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
