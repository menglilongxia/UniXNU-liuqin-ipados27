#!/usr/bin/env python3
"""Fetch SHA-256-pinned SM8475 Adreno 730 firmware for Xiaomi Pad 6 Pro (liuqin).

Downloads gmu_gen70000.bin, a730_sqe.fw, and Xiaomi liuqin-signed a730_zap.{mdt,b01,b02}
into .cache/firmware/liuqin-gpu/ so proprietary blobs are never committed to source control.
"""
import concurrent.futures
import hashlib
import http.client
import os
from pathlib import Path
import time
import urllib.error
import urllib.parse
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / '.cache/firmware/liuqin-gpu'
COMMIT = 'bf81e7c2afa2e289368d204a426c5dca3b94bc9c'
BASE_URL = f'https://raw.githubusercontent.com/Xiaomi-SM8475/android_vendor_xiaomi_liuqin/{COMMIT}/proprietary/vendor/firmware'

PINS = {
    'gmu_gen70000.bin': {
        'bytes': 57272,
        'sha256': 'b2d3f0e2a98fd6259ab9ffef16df63265542e6cc9a970d53d476e823d432042a',
    },
    'a730_sqe.fw': {
        'bytes': 74676,
        'sha256': 'e67d1829f57fc8326d806234b932dc3b78e28a9940df34c70752f17d5f38b7aa',
    },
    'a730_zap.mdt': {
        'bytes': 3564,
        'sha256': '1acb1e3ab783f75941eb427953c647d12ab4552c74a3c89c090f63636aaf2d9c',
    },
    'a730_zap.b01': {
        'bytes': 2236,
        'sha256': '1fc98f97cf354b9704b20d0848f7b2739638539a57d88ecf5cc71ca1c5c4e6b9',
    },
    'a730_zap.b02': {
        'bytes': 3416,
        'sha256': '3d284460e2848dcfe6b03f73292a97eb579344e4763d1a8e36d0107c05865dac',
    },
}

TOKEN = os.environ.get('GITHUB_TOKEN') or os.environ.get('GH_TOKEN')


def download(url):
    headers = {'User-Agent': 'unixnu-liuqin-adreno-firmware'}
    if TOKEN and urllib.parse.urlsplit(url).hostname in ('api.github.com', 'raw.githubusercontent.com'):
        headers['Authorization'] = f'Bearer {TOKEN}'
    req = urllib.request.Request(url, headers=headers)
    for attempt in range(4):
        try:
            with urllib.request.urlopen(req, timeout=45) as resp:
                return resp.read()
        except (OSError, EOFError, http.client.IncompleteRead) as err:
            if isinstance(err, urllib.error.HTTPError) and err.code not in (429, 500, 502, 503, 504):
                raise
            if attempt == 3:
                raise
            time.sleep(attempt + 1)


def fetch_one(item):
    name, expected = item
    path = OUT / name
    if path.is_file():
        data = path.read_bytes()
    else:
        data = download(f'{BASE_URL}/{name}')
    if len(data) != expected['bytes'] or hashlib.sha256(data).hexdigest() != expected['sha256']:
        raise SystemExit(f'Pinned firmware hash/size mismatch for {name}: got {len(data)}B {hashlib.sha256(data).hexdigest()}')
    if not path.is_file():
        path.parent.mkdir(parents=True, exist_ok=True)
        part = path.with_suffix(path.suffix + '.part')
        part.write_bytes(data)
        part.replace(path)
    return len(data)


if __name__ == '__main__':
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        sizes = list(pool.map(fetch_one, PINS.items()))
    print(f'PASS: {len(sizes)} pinned liuqin Adreno 730 firmware files verified ({sum(sizes)} bytes)')
