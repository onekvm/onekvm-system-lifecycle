#!/usr/bin/env python3
"""Run the actual script in a temp tree: the per-boot path and module seeding."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import unittest

SCRIPT = Path(__file__).with_name('prepare-persistence')
MAGIC = 'sha256:' + 'a' * 64
SHELLS = [shell for shell in ('bash', 'dash') if shutil.which(shell)]
# Commands logged by the fixture; all except mount run the real program.
LOGGED = ('mount', 'cmp', 'cp', 'mkdir', 'rm', 'ln', 'mv', 'chmod', 'find', 'grep', 'awk')


class PersistenceTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='onekvm-persistence-')
        self.addCleanup(self.temporary.cleanup)
        self.root = root = Path(self.temporary.name)
        self.share = root / 'share'
        self.baseline = self.share / 'opkg-baseline/info'
        self.baseline.mkdir(parents=True)
        (self.share / 'build-id').write_text('build-one\n')
        (self.share / 'kernel-magic').write_text(MAGIC + '\n')
        (self.share / 'defaults/opkg').mkdir(parents=True)
        (self.share / 'defaults/opkg/feeds.conf').write_text('src test http://example.invalid\n')
        gpg = root / 'etc/opkg/gpg'
        gpg.mkdir(parents=True)
        for name in ('pubring.kbx', 'trustdb.gpg'):
            (gpg / name).write_text('key\n')
        (root / 'etc/onekvm').mkdir()
        (root / 'etc/onekvm/config.toml').write_text('initial = true\n')
        (root / 'cmdline').write_text('console=ttyS0 rauc.slot=A ro\n')
        (root / 'osrelease').write_text('test-kernel\n')
        # The initramfs refreshed the upper for this image, so image files stay.
        (root / 'image-build-id').write_text('build-one\n')
        (root / 'image-kernel-magic').write_text(MAGIC + '\n')
        self.updates = root / 'modules/test-kernel/updates'
        self.immutable = root / 'immutable'
        (self.immutable / 'usr').mkdir(parents=True)
        self.persistent = root / f'storage/onekvm/kernel-modules/A/test-kernel/{"a" * 64}/updates'
        self.opkg = root / 'data/pkg-state/slots/A/opkg'
        self.log = root / 'commands.jsonl'

        source = SCRIPT.read_text()
        for old, new in [
            ('/proc/cmdline', root / 'cmdline'),
            ('/proc/sys/kernel/osrelease', root / 'osrelease'),
            ('/usr/share/onekvm', self.share),
            ('/usr/lib/modules', root / 'modules'),
            ('/mnt/storage', root / 'storage'),
            ('/data/', f'{root}/data/'),
            ('/etc/opkg', root / 'etc/opkg'),
            ('/etc/onekvm', root / 'etc/onekvm'),
            ('/var/lib/machines', root / 'machines'),
            ('/var/log', root / 'log'),
            ('/var/tmp', root / 'tmp'),
            ('/run/onekvm', root / 'run'),
            ('/mnt/.rofs', self.immutable),
            ('immutable_root=/rofs', f'immutable_root={self.immutable}'),
            ('/.onekvm-image-build-id', root / 'image-build-id'),
            ('/.onekvm-kernel-magic', root / 'image-kernel-magic'),
        ]:
            self.assertIn(old, source)
            source = source.replace(old, str(new))
        self.script = root / 'prepare-persistence'
        self.script.write_text(source)

        bin_dir = root / 'bin'
        bin_dir.mkdir()
        stub = bin_dir / 'stub'
        stub.write_text('''#!/usr/bin/env python3
import json, os, pathlib, shutil, sys
name = pathlib.Path(sys.argv[0]).name
with open(os.environ['COMMAND_LOG'], 'a') as log:
    log.write(json.dumps([name] + sys.argv[1:]) + '\\n')
if name == 'mount':
    sys.exit(0)
path = os.environ['PATH'].split(os.pathsep, 1)[1]
os.execv(shutil.which(name, path=path), [name] + sys.argv[1:])
''')
        stub.chmod(0o755)
        for command in LOGGED:
            (bin_dir / command).symlink_to(stub)
        self.env = {
            **os.environ,
            'PATH': f'{bin_dir}{os.pathsep}{os.environ["PATH"]}',
            'COMMAND_LOG': str(self.log),
            'ONEKVM_PERSISTENCE_STATUS_DIR': str(root / 'status'),
        }

    def package(self, info, name, *modules, magic=MAGIC):
        paths = [self.updates / module for module in modules]
        info.mkdir(parents=True, exist_ok=True)
        (info / f'onekvm-kernel-module-{name}.list').write_text(''.join(f'{p}\n' for p in paths))
        (info / f'onekvm-kernel-module-{name}.control').write_text(
            f'Package: onekvm-kernel-module-{name}\nX-OneKVM-Kernel-Magic: {magic}\n')
        return paths

    def opkg_transaction(self, name='changed', version='1.0'):
        """Install (or with version None remove) one package as opkg does:
        rewrite the slot status, keeping every other entry and install time."""
        installed = getattr(self, 'installed', {})
        self.installed = installed
        self.transactions = getattr(self, 'transactions', 0) + 1
        if version is None:
            installed.pop(name, None)
        else:
            installed[name] = (version, 1700000000 + self.transactions)
        self.opkg.mkdir(parents=True, exist_ok=True)
        (self.opkg / 'status').write_text(''.join(
            f'Package: {package}\nVersion: {v}\nStatus: install ok installed\n'
            f'Architecture: riscv64\nInstalled-Time: {t}\n\n'
            for package, (v, t) in sorted(installed.items())))

    def immutable_module(self, module, data):
        path = Path(f'{self.immutable}{self.updates}/{module}')
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        return path

    def boot(self, shell):
        """Run once as a fresh boot: nothing is bound and /run is empty."""
        self.log.unlink(missing_ok=True)
        shutil.rmtree(self.root / 'run', ignore_errors=True)
        result = subprocess.run([shell, str(self.script)], env=self.env, text=True,
                                capture_output=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        status = (self.root / 'status/persistence-status').read_text()
        self.assertIn('state=ready', status)
        self.assertFalse((self.root / 'run/.module-seed-inputs').exists())
        self.assertFalse((self.root / 'run/.module-seed-packages').exists())
        self.assertFalse(list(self.persistent.parent.glob('.onekvm-module-seed-packages.tmp')))
        self.assertFalse(list(self.persistent.parent.glob('.onekvm-module-seed.tmp')))
        self.assertEqual((self.root / 'run/slot').read_text(), 'A\n')
        self.assertEqual(os.readlink(self.root / 'run/opkg'), str(self.opkg))
        self.assertEqual((self.root / 'run/opkg-gpg/pubring.kbx').stat().st_mode & 0o777, 0o600)
        self.assertEqual(os.readlink(self.root / 'etc/onekvm'), str(self.root / 'storage/onekvm/config'))
        self.records = [json.loads(line) for line in self.log.read_text().splitlines()]
        self.assertIn(['mount', '--bind', str(self.persistent), str(self.updates)], self.records)
        self.assertIn(['mount', '--bind', str(self.root / 'storage/onekvm/machines'),
                       str(self.root / 'machines')], self.records)
        # Module comparisons only; one cmp checks the recorded inputs.
        return [r for r in self.records if r[0] == 'cmp' and r[-1].endswith('.ko')]

    def each_shell(self, scenario):
        for shell in SHELLS:
            with self.subTest(shell=shell):
                self.setUp()
                scenario(shell)

    def test_unchanged_boot_only_restores_runtime_state(self):
        def scenario(shell):
            self.package(self.baseline, 'group', 'group/a.ko', 'group/b.ko')
            self.immutable_module('group/a.ko', b'module a')
            self.immutable_module('group/b.ko', b'module b')
            self.boot(shell)
            self.assertEqual((self.root / 'storage/onekvm/config/config.toml').read_text(), 'initial = true\n')
            self.assertEqual((self.persistent / 'group/a.ko').read_bytes(), b'module a')
            self.assertEqual((self.persistent / 'group/b.ko').read_bytes(), b'module b')
            self.assertTrue((self.persistent.parent / '.onekvm-module-seed').is_file())
            for _ in range(2):
                self.assertEqual(self.boot(shell), [])
                commands = sorted({r[0] for r in self.records})
                self.assertEqual(commands, ['chmod', 'cmp', 'cp', 'find', 'ln', 'mkdir', 'mount', 'mv', 'rm'])
                self.assertEqual([r for r in self.records if r[0] == 'mkdir'],
                                 [['mkdir', '-p', '-m', '0700', str(self.root / 'run/opkg-gpg')]])
                self.assertEqual([r for r in self.records if r[0] == 'rm'],
                                 [['rm', '-f', str(self.root / 'run/.module-seed-inputs')]])
                self.assertEqual([r for r in self.records if r[0] == 'ln'],
                                 [['ln', '-sfn', str(self.opkg), str(self.root / 'run/opkg')]])
                self.assertEqual([r[0] for r in self.records if r[0] == 'cp'], ['cp'])
                self.assertFalse([r for r in self.records if r[0] == 'grep'])
        self.each_shell(scenario)

    def test_new_build_id_compares_and_refreshes(self):
        def scenario(shell):
            self.package(self.baseline, 'group', 'group/a.ko')
            self.immutable_module('group/a.ko', b'module one')
            self.boot(shell)
            # A new image with the same size and mtime for the module.
            source = self.immutable_module('group/a.ko', b'module two')
            os.utime(source, (0, 0))
            os.utime(self.persistent / 'group/a.ko', (0, 0))
            for name in ('share/build-id', 'image-build-id'):
                (self.root / name).write_text('build-two\n')
            self.assertTrue(self.boot(shell))
            self.assertEqual((self.persistent / 'group/a.ko').read_bytes(), b'module two')
            self.assertEqual((self.opkg / '.baseline-build-id').read_text(), 'build-two\n')
            self.assertEqual(self.boot(shell), [])
        self.each_shell(scenario)

    def test_replaced_or_removed_copy_is_restored(self):
        def scenario(shell):
            self.package(self.baseline, 'group', 'group/a.ko')
            self.immutable_module('group/a.ko', b'module a')
            self.boot(shell)
            copy = self.persistent / 'group/a.ko'
            stat = copy.stat()
            time.sleep(0.01)
            copy.write_bytes(b'damaged!')
            os.utime(copy, ns=(stat.st_atime_ns, stat.st_mtime_ns))
            self.assertTrue(self.boot(shell))
            self.assertEqual(copy.read_bytes(), b'module a')
            copy.unlink()
            self.boot(shell)
            self.assertEqual(copy.read_bytes(), b'module a')
            self.assertEqual(self.boot(shell), [])
        self.each_shell(scenario)

    def test_package_transaction_seeds_new_module(self):
        def scenario(shell):
            self.package(self.baseline, 'group', 'group/a.ko')
            self.immutable_module('group/a.ko', b'module a')
            self.boot(shell)
            self.assertEqual(self.boot(shell), [])
            # A package installed while updates/ was not bound left its payload
            # in the overlay; opkg rewrote the slot status.
            (installed,) = self.package(self.opkg / 'info', 'extra', 'extra/c.ko')
            installed.parent.mkdir(parents=True)
            installed.write_bytes(b'module c')
            self.opkg_transaction('onekvm-kernel-module-extra')
            self.boot(shell)
            self.assertEqual((self.persistent / 'extra/c.ko').read_bytes(), b'module c')
            self.assertEqual(self.boot(shell), [])
        self.each_shell(scenario)

    def test_package_without_modules_skips_the_comparison(self):
        def scenario(shell):
            self.package(self.baseline, 'group', 'group/a.ko')
            self.immutable_module('group/a.ko', b'module a')
            self.boot(shell)
            (installed,) = self.package(self.opkg / 'info', 'extra', 'extra/c.ko')
            installed.parent.mkdir(parents=True)
            installed.write_bytes(b'module c')
            self.opkg_transaction('onekvm-kernel-module-extra')
            self.boot(shell)
            self.assertEqual(self.boot(shell), [])
            # An application package: its own file list and a new status
            # that keeps the module package's entry.
            info = self.opkg / 'info'
            (info / 'onekvm-ui.list').write_text('/usr/share/onekvm/web/index.html\n')
            self.opkg_transaction('onekvm-ui')
            self.assertEqual(self.boot(shell), [])
            self.assertTrue([r for r in self.records if r[0] == 'awk'])
            self.assertEqual(self.boot(shell), [])
            self.assertFalse([r for r in self.records if r[0] == 'awk'])
        self.each_shell(scenario)

    def test_module_package_upgrade_and_removal_compare_again(self):
        def scenario(shell):
            self.package(self.baseline, 'group', 'group/a.ko')
            self.immutable_module('group/a.ko', b'module a')
            self.boot(shell)
            (installed,) = self.package(self.opkg / 'info', 'extra', 'extra/c.ko')
            installed.parent.mkdir(parents=True)
            installed.write_bytes(b'module c')
            self.opkg_transaction('onekvm-kernel-module-extra')
            self.boot(shell)
            self.assertEqual(self.boot(shell), [])
            # A reinstall of the same version records a new install time.
            self.opkg_transaction('onekvm-kernel-module-extra')
            self.assertTrue(self.boot(shell))
            self.assertEqual(self.boot(shell), [])
            self.opkg_transaction('onekvm-kernel-module-extra', '2.0')
            self.assertTrue(self.boot(shell))
            self.assertEqual(self.boot(shell), [])
            for suffix in ('list', 'control'):
                (self.opkg / f'info/onekvm-kernel-module-extra.{suffix}').unlink()
            self.opkg_transaction('onekvm-kernel-module-extra', None)
            self.assertTrue(self.boot(shell))
            self.assertEqual(self.boot(shell), [])
        self.each_shell(scenario)

    def test_mismatched_kernel_magic_is_still_ignored(self):
        def scenario(shell):
            self.package(self.baseline, 'old', 'old/a.ko', magic='sha256:' + 'b' * 64)
            self.immutable_module('old/a.ko', b'old module')
            self.boot(shell)
            self.assertFalse((self.persistent / 'old/a.ko').exists())
            self.assertEqual(self.boot(shell), [])
        self.each_shell(scenario)

    def test_broken_config_link_is_repaired(self):
        def scenario(shell):
            self.boot(shell)
            link = self.root / 'etc/onekvm'
            link.unlink()
            link.mkdir()
            self.boot(shell)
            self.assertTrue(link.is_symlink())
            # The migration marker keeps the earlier configuration.
            self.assertEqual((self.root / 'storage/onekvm/config/config.toml').read_text(), 'initial = true\n')
        self.each_shell(scenario)


if __name__ == '__main__':
    if not SHELLS:
        sys.exit('bash or dash is required')
    unittest.main(verbosity=2)
