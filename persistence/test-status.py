#!/usr/bin/env python3
"""Run the actual shell script up to controlled failures; all writes stay in a temp dir."""
from pathlib import Path
import os, subprocess, tempfile
SCRIPT=Path(__file__).with_name('prepare-persistence')
for mode,stage in [('slot','boot-slot'),('keys','package-keys'),('config','config')]:
 with tempfile.TemporaryDirectory(prefix='onekvm-persistence-test-') as scratch:
  env={**os.environ,'ONEKVM_PERSISTENCE_STATUS_DIR':scratch,'PERSIST_TEST_MODE':mode}
  stubs=r'''
cat() { if [ "$1" = /proc/cmdline ]; then [ "$PERSIST_TEST_MODE" = slot ] || printf 'rauc.slot=A\n'; else return 97; fi; }
uname() { printf 'test-kernel\n'; }
mkdir() {
 case "$*" in *"$ONEKVM_PERSISTENCE_STATUS_DIR"*) command mkdir "$@";; *'/mnt/storage/onekvm/config'*) return 73;; *) :;; esac
}
cp() { [ "$PERSIST_TEST_MODE" != keys ]; }
chmod() { :; }
rm() { :; }
mv() { case "$*" in *"$ONEKVM_PERSISTENCE_STATUS_DIR"*) command mv "$@";; *) return 98;; esac; }
. "$1"
'''
  p=subprocess.run(['/bin/bash','-c',stubs,'status-test',str(SCRIPT.resolve())],env=env,capture_output=True,text=True)
  assert p.returncode != 0,(mode,p.stdout,p.stderr)
  status=dict(line.split('=',1) for line in (Path(scratch)/'persistence-status').read_text().splitlines())
  assert status['state']=='failed' and status['stage']==stage,(mode,status,p.stderr)
  assert int(status['exit_code'])==p.returncode
  assert f'stage={stage}' in p.stderr
  assert not (Path(scratch)/'.persistence-status.tmp').exists()
  print(f'{mode}: exit {p.returncode}, atomic failure status preserved for {stage}')
