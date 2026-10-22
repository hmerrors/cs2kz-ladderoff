"""Build the Windows x64 Metamod plugin with an existing CS2 SDK checkout."""

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys


PROJECT = Path(__file__).resolve().parent


def default_deps_root():
    override = os.environ.get('KZLADDEROFF_DEPS_ROOT')
    if override:
        return Path(override)
    for parent in PROJECT.parents:
        candidate = parent / 'showpos-deps'
        if (candidate / 'hl2sdk-cs2').is_dir() and (candidate / 'metamod-source').is_dir():
            return candidate
    return None


def msvc_environment():
    env = {key.upper(): value for key, value in os.environ.items()}
    if shutil.which('cl.exe', path=env.get('PATH')):
        return env

    vswhere = Path(os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)')) / 'Microsoft Visual Studio' / 'Installer' / 'vswhere.exe'
    if not vswhere.is_file():
        raise RuntimeError('Visual Studio Build Tools were not found; run from an x64 Native Tools prompt or install VS 2022 Build Tools.')
    installation = subprocess.check_output(
        [str(vswhere), '-latest', '-products', '*', '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath'],
        text=True,
    ).strip()
    if not installation:
        raise RuntimeError('Visual Studio x64 C++ Build Tools were not found.')
    vcvars = Path(installation) / 'VC' / 'Auxiliary' / 'Build' / 'vcvars64.bat'
    if not vcvars.is_file():
        raise RuntimeError(f'Missing MSVC environment script: {vcvars}')

    output = subprocess.check_output(
        f'call "{vcvars}" >nul && set',
        shell=True,
        text=True,
        encoding='mbcs',
    )
    for line in output.splitlines():
        if '=' in line and not line.startswith('='):
            key, value = line.split('=', 1)
            env[key.upper()] = value
    if not shutil.which('cl.exe', path=env.get('PATH')):
        raise RuntimeError('vcvars64.bat completed but cl.exe is unavailable.')
    return env


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--deps-root', type=Path, default=default_deps_root(), help='Directory containing metamod-source and hl2sdk-cs2')
    parser.add_argument('--mms-path', type=Path, help='Override Metamod:Source path')
    parser.add_argument('--sdk-path', type=Path, help='Override Source 2 SDK path')
    parser.add_argument('--ambuild-path', type=Path, default=os.environ.get('AMBUILD_SOURCE'), help='AMBuild source directory, if ambuild2 is not installed')
    parser.add_argument('--build-dir', type=Path, default=PROJECT.parent.parent / 'work' / 'kzladderoff-build-win64', help='Intermediate build directory')
    parser.add_argument('--jobs', type=int, default=4)
    args = parser.parse_args()

    mms = args.mms_path or (args.deps_root / 'metamod-source' if args.deps_root else None)
    sdk = args.sdk_path or (args.deps_root / 'hl2sdk-cs2' if args.deps_root else None)
    if not mms or not (mms / 'hl2sdk-manifests' / 'SdkHelpers.ambuild').is_file():
        parser.error('Metamod:Source is missing; pass --deps-root or --mms-path')
    if not sdk or not (sdk / 'public' / 'gametrace.h').is_file():
        parser.error('Source 2 CS2 SDK is missing; pass --deps-root or --sdk-path')
    if args.jobs < 1:
        parser.error('--jobs must be positive')

    env = msvc_environment()
    env['PYTHONIOENCODING'] = 'utf-8'
    env['PYTHONUTF8'] = '1'
    if args.ambuild_path:
        ambuild = Path(args.ambuild_path).resolve()
        if not (ambuild / 'ambuild2' / 'run.py').is_file():
            parser.error('--ambuild-path must point to an AMBuild source checkout')
        env['PYTHONPATH'] = str(ambuild) + os.pathsep + env.get('PYTHONPATH', '')
    subprocess.run([sys.executable, '-c', 'import ambuild2'], env=env, check=True)

    build_dir = args.build_dir.resolve()
    build_dir.mkdir(parents=True, exist_ok=True)
    configured = subprocess.run(
        [sys.executable, str(PROJECT / 'configure.py'), '--mms-path', str(mms.resolve()), '--sdk-path', str(sdk.resolve())],
        cwd=build_dir,
        env=env,
        capture_output=True,
        text=True,
        encoding='utf-8',
        errors='replace',
    )
    print(configured.stdout, end='')
    print(configured.stderr, end='', file=sys.stderr)
    if configured.returncode or 'Traceback (most recent call last)' in configured.stderr or not (build_dir / '.ambuild2' / 'graph').is_file():
        raise RuntimeError('AMBuild configuration failed; see the output above.')
    subprocess.run(
        [sys.executable, '-c', 'from ambuild2.run import cli_run; cli_run()', '-j', str(args.jobs)],
        cwd=build_dir,
        env=env,
        check=True,
    )
    print(build_dir / 'kzladderoff' / 'windows-x86_64' / 'kzladderoff.dll')


if __name__ == '__main__':
    main()
