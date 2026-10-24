"""Files on the Switch over WLAN through the sys-ftpd sysmodule (docs/switch.md "Remote testing").

sys-ftpd (github.com/cathery/sys-ftpd, formerly sys-ftpd-light) runs in the background from boot
(sdmc:/atmosphere/contents/420000000000000E, config sdmc:/config/sys-ftpd/config.ini) and serves the SD card on
TCP 5000, whatever runs in the foreground (home menu, hbmenu or the game). Same commands as tools/switch_mtp.py,
so the scripts can use either. Paths are relative to the SD card root, with / separators.

usage: python -I tools/switch_ftp.py check                      login + list the root (exit 1 when unreachable)
       python -I tools/switch_ftp.py ls [dir]
       python -I tools/switch_ftp.py get <file> <local file>
       python -I tools/switch_ftp.py put <local file> <dir>     (replaces a file of the same name, creates <dir>)
       python -I tools/switch_ftp.py rm <file>                  (exit 1 when the file does not exist)
Login: port/switch/ftp_cred.txt (git-ignored; lines host=, port=, user=, password=; SWITCH_FTP_CRED=<file> replaces
it). Host: --ip <a.b.c.d>, SWITCH_IP,
the file's host=, else port/switch/bot_ip.txt (the bot's address is the console's).
"""
import ftplib
import os
import socket
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
CRED_FILE = os.environ.get('SWITCH_FTP_CRED') or os.path.join(HERE, '..', 'port', 'switch', 'ftp_cred.txt')
IP_FILE = os.path.join(HERE, '..', 'port', 'switch', 'bot_ip.txt')
TRIES = 3


class Missing(Exception):
    pass


def settings(argv):
    if not os.path.exists(CRED_FILE):
        sys.exit(f'no {CRED_FILE} (host=, port=, user=, password=; see docs/switch.md)')
    cred = dict(line.strip().split('=', 1) for line in open(CRED_FILE) if '=' in line)
    host = cred.get('host')
    if '--ip' in argv:
        i = argv.index('--ip')
        host = argv[i + 1]
        del argv[i:i + 2]
    elif os.environ.get('SWITCH_IP'):
        host = os.environ['SWITCH_IP']
    if not host and os.path.exists(IP_FILE):
        host = open(IP_FILE).read().strip()
    return host, int(cred.get('port', 5000)), cred['user'], cred['password']


def sd(path):
    return '/' + '/'.join(p for p in path.replace('\\', '/').split('/') if p)


class Switch:
    def __init__(self, host, port, user, password):
        self.addr = (host, port, user, password)
        self.ftp = None

    def connect(self):
        host, port, user, password = self.addr
        self.close()
        ftp = ftplib.FTP(timeout=30)
        ftp.connect(host, port)
        ftp.login(user, password)
        ftp.set_pasv(True)
        ftp.voidcmd('TYPE I')
        self.ftp = ftp

    def close(self):
        if self.ftp is not None:
            try:
                self.ftp.quit()
            except (OSError, EOFError, ftplib.Error):
                self.ftp.close()
            self.ftp = None

    def retry(self, what, *args):
        """runs what(*args) with a fresh connection after a network or server error, TRIES times"""
        for attempt in range(1, TRIES + 1):
            try:
                if self.ftp is None:
                    self.connect()
                return what(*args)
            except Missing:
                raise
            except (OSError, EOFError, ftplib.Error) as e:
                self.close()
                if attempt == TRIES:
                    sys.exit(f'FTP {self.addr[0]}:{self.addr[1]} failed: {e}')
                time.sleep(3 * attempt)

    def _ls(self, directory):
        lines = []
        try:
            self.ftp.retrlines('LIST ' + sd(directory), lines.append)
        except ftplib.error_perm as e:
            raise Missing(f'no such folder: {directory} ({e})')
        return lines

    def _size(self, path):
        try:
            return self.ftp.size(sd(path))
        except ftplib.error_perm:
            return None

    def _get(self, path, local):
        tmp = local + '.part'
        missing = None
        with open(tmp, 'wb') as out:
            try:
                self.ftp.retrbinary('RETR ' + sd(path), out.write, blocksize=1 << 16)
            except ftplib.error_perm as e:
                missing = e
        if missing is not None:
            os.remove(tmp)
            raise Missing(f'not a file on the SD card: {path} ({missing})')
        os.replace(tmp, local)
        return os.path.getsize(local)

    def _mkdirs(self, directory):
        cur = ''
        for part in [p for p in directory.replace('\\', '/').split('/') if p]:
            cur += '/' + part
            try:
                self.ftp.mkd(cur)
            except ftplib.error_perm:
                pass  # exists

    def _put(self, local, directory):
        self._mkdirs(directory)
        name = os.path.basename(local)
        target = sd(directory + '/' + name)
        size = os.path.getsize(local)
        with open(local, 'rb') as src:
            self.ftp.storbinary('STOR ' + target, src, blocksize=1 << 16)
        written = self._size(target)
        if written is not None and written != size:
            raise ftplib.error_temp(f'upload of {name} incomplete ({written} of {size} bytes)')
        return size

    def _rm(self, path):
        try:
            self.ftp.delete(sd(path))
        except ftplib.error_perm as e:
            raise Missing(f'no such file: {path} ({e})')

    def ls(self, directory=''):
        return self.retry(self._ls, directory)

    def get(self, path, local):
        return self.retry(self._get, path, local)

    def put(self, local, directory):
        return self.retry(self._put, local, directory)

    def rm(self, path):
        return self.retry(self._rm, path)


def reachable(host, port, timeout=3):
    try:
        socket.create_connection((host, port), timeout=timeout).close()
        return True
    except OSError:
        return False


def main(argv):
    if len(argv) < 2:
        sys.exit(__doc__)
    host, port, user, password = settings(argv)
    command = argv[1]
    if command == 'check' and not reachable(host, port):
        sys.exit(f'FTP {host}:{port} not reachable (console off/asleep, or sys-ftpd not running)')
    switch = Switch(host, port, user, password)
    try:
        if command == 'check':
            switch.ls('')
            print(f'FTP ok {host}:{port}')
        elif command == 'ls':
            for line in switch.ls(argv[2] if len(argv) > 2 else ''):
                print(line)
        elif command == 'get':
            print(f'{switch.get(argv[2], argv[3])} bytes -> {argv[3]}')
        elif command == 'put':
            print(f'{switch.put(argv[2], argv[3])} bytes -> {argv[3]}/{os.path.basename(argv[2])}')
        elif command == 'rm':
            switch.rm(argv[2])
            print('deleted', argv[2])
        else:
            sys.exit(__doc__)
    except Missing as e:
        sys.exit(str(e))
    finally:
        switch.close()


if __name__ == '__main__':
    main(sys.argv)
