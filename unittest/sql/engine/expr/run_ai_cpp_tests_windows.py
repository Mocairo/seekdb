"""Compile/run AI unit cases using an existing Windows CMake production build.

No compiler flags or dependency paths are duplicated: reuse compile_commands
and the seekdb link libraries from Ninja. Run after building ob_sql_static.
Usage: python unittest/sql/engine/expr/run_ai_cpp_tests_windows.py build_release
"""
import json
import os
from pathlib import Path
import subprocess
import sys


def main():
    if os.name != 'nt':
        raise SystemExit('This adapter requires a Windows CMake/Ninja build.')
    build = Path(sys.argv[1]).resolve()
    tests = Path(__file__).resolve().parent
    out = build / 'ai_function_tests'
    out.mkdir(exist_ok=True)
    commands = json.loads((build / 'compile_commands.json').read_text())
    command = next(c['command'] for c in commands if 'engine_expr_15' in c['file'])
    # Standalone headers expose legacy memset warnings hidden by unity includes.
    prefix = command.split(' /Fo', 1)[0] + ' -Wno-nontrivial-memcall'
    objects = []
    for source in ['ob_expr_ai_prompt_test.cpp', 'test_ob_openai_utils.cpp']:
        obj = out / (source + '.obj')
        args = ['/Fo' + str(obj), '/Fd' + str(out) + os.sep, '-c', '--', str(tests / source)]
        print('Compiling ' + source, flush=True)
        subprocess.run(prefix + ' ' + subprocess.list2cmdline(args), cwd=build, check=True)
        objects.append(str(obj))
    ninja = (build / 'build.ninja').read_text()
    line = next(line for line in ninja.splitlines() if line.startswith('build ') and 'seekdb.exe:' in line)
    start = ninja.index(line)
    block = ninja[start:ninja.index('\n\n', start)]
    values = {}
    for line in block.splitlines()[1:]:
        if ' = ' in line:
            key, value = line.strip().split(' = ', 1)
            values[key] = value.replace('$:', ':').replace('$ ', ' ')
    executable = out / 'ai_function_tests.exe'
    response = out / 'link.rsp'
    response.write_text(subprocess.list2cmdline(objects + ['/out:' + str(executable),
                        '/pdb:' + str(out / 'ai_function_tests.pdb'), '/nologo', '/OPT:REF', '/OPT:ICF'])
                        + '\n' + values['LINK_PATH'] + '\n' + values['LINK_LIBRARIES'])
    compiler = command.split('  /nologo', 1)[0].strip('"')
    linker = Path(compiler).with_name('lld-link.exe')
    print('Linking AI unit tests', flush=True)
    subprocess.run([str(linker), '@' + str(response)], cwd=build, check=True)
    env = os.environ.copy()
    env['PATH'] = str(build / 'src/observer') + os.pathsep + env.get('PATH', '')
    subprocess.run([str(executable)], cwd=out, env=env, check=True)


if __name__ == '__main__':
    try:
        main()
    except subprocess.CalledProcessError as error:
        raise SystemExit(error.returncode)
