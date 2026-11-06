#!/usr/bin/env python3
"""Build apollo's Windows DSDT from Silicium-ACPI's Xiaomi pipa (SM8250-AC / Kona) table.

    git clone --depth 1 https://github.com/Project-Silicium/Silicium-ACPI
    python3 make-apollo-dsdt.py Silicium-ACPI/Platforms/Xiaomi/pipa/Decompiled/DSDT.dsl Dsdt.asl
        [--drop NAME,NAME,...] [--no-thermal]

--drop and --no-thermal remove further devices / all thermal zones on top of the fixed list;
they exist for bisecting boot crashes (see README.md, "Bisecting a boot crash").

Needs `iasl` on PATH. Steps (each one checked on the device, see README.md):
  1. drop pipa devices that are board-specific, unsafe on apollo or need pipa's memory carve-outs
  2. QCOM05xx -> QCOM25xx (hardware IDs used by WOA-Project/windows_silicon_qcom_kona)
  3. UFS0 PEP rails -> apollo's measured rails (VCC LDO17_A 2.496 V, ref clock LDO6_A 1.2 V)
  4. repair everything that referenced a dropped device, until iasl compiles with 0 errors
"""
import os, re, subprocess, sys

DROP = {
    'SDC2', 'AUDS', 'PMBT', 'PMBM', 'BCL1', 'UARD', 'UR14', 'UR18', 'UR20', 'I2C5', 'I2C8', 'IC10', 'SPI4',
    'PILC', 'CDI', 'SCSS', 'ADSP', 'AMSS', 'QSM', 'SSDD', 'MPTM', 'PDSR', 'CDSP', 'SPSS', 'TFTP', 'LLC',
    'MMU0', 'MMU1', 'IMM0', 'IMM1', 'GPU0', 'TREE', 'QPPX', 'IPC0', 'GLNK', 'ARPC', 'ARPD', 'RFS0', 'IPA',
    'GSI', 'QDIG', 'SSM', 'GPS', 'CSEC', 'QWPP', 'SOCP', 'QDSS', 'QCSP', 'QCDB', 'SERB', 'RMNT', 'MBRG',
    'RMAT', 'DPLB', 'SSVC', 'HWN0', 'CAMP', 'CAMS', 'CAMF', 'CAMI', 'FLSH', 'MPCS', 'JPGE', 'VFE0', 'SEN2',
    'USBA', 'CFSA', 'MPA', 'MPA1', 'MBJ0', 'MBJ1', 'MBJ2', 'MBJ3', 'MBS0', 'MBS1', 'MBS2', 'MBS3', 'MSKN',
    'MJCT', 'HWN1', 'TSC5', 'RVRM', 'DBUS', 'CCID', 'QDCI', 'BTH0', 'ADC1', 'ADC2', 'ADC3',
    'PCI0', 'PCI1',  # depend on QPPX (PCIe), which stays disabled
}


def block_end(lines, start):
    depth, opened = 0, False
    for k in range(start, len(lines)):
        for ch in lines[k]:
            if ch == '{':
                depth += 1
                opened = True
            elif ch == '}':
                depth -= 1
        if opened and depth == 0:
            return k
    raise ValueError(f'unbalanced block at line {start + 1}')


def drop_devices(text, drop, no_thermal=False):
    out, lines, i = [], text.splitlines(keepends=True), 0
    depth, removed = 0, set()
    while i < len(lines):
        if no_thermal and re.match(r'\s*ThermalZone \(', lines[i]) and depth <= 2:
            i = block_end(lines, i) + 1
            continue
        m = re.match(r'\s*Device \((\w+)\)', lines[i])
        if m and m.group(1) in drop and depth <= 2:
            removed.add(m.group(1))
            i = block_end(lines, i) + 1
            continue
        depth += lines[i].count('{') - lines[i].count('}')
        out.append(lines[i])
        i += 1
    missing = drop - removed
    if missing:
        sys.exit(f'devices not found in input (wrong source table?): {sorted(missing)}')
    return ''.join(out)


def remap_ufs_rails(text):
    lines, cur, edits = text.splitlines(keepends=True), None, 0
    for k, l in enumerate(lines):
        m = re.search(r'"\\\\_SB\.([A-Z0-9_.]+)"', l)
        if m:
            cur = m.group(1)
        if cur != 'UFS0':
            continue
        new = (l.replace('PPP_RESOURCE_ID_LDO10_A', 'PPP_RESOURCE_ID_LDO17_A')
                .replace('PPP_RESOURCE_ID_LDO3_C', 'PPP_RESOURCE_ID_LDO6_A'))
        new = re.sub(r'0x002C4FC0|0x00263540', '0x00261600', new)   # 2.904 V / 2.504 V -> 2.496 V
        if new != l:
            lines[k] = new
            edits += 1
    if edits == 0:
        sys.exit('UFS0 rail entries not found')
    return ''.join(lines), edits


def compile_errors(path):
    r = subprocess.run(['iasl', '-ve', path], cwd=os.path.dirname(os.path.abspath(path)),
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True).stdout
    lines, errs = r.splitlines(), []
    for i, l in enumerate(lines):
        if l.startswith('Error'):
            loc = next((lines[j] for j in range(i - 1, max(i - 4, -1), -1)
                        if re.match(r'\S+\s+\d+:', lines[j])), None)
            if loc:
                errs.append((int(re.match(r'\S+\s+(\d+):', loc).group(1)), l))
    summary = [l for l in lines if 'Compilation' in l]
    return errs, (summary[-1] if summary else r[-400:])


def repair(path):
    for _ in range(15):
        errs, summary = compile_errors(path)
        missing = sorted({ln for ln, e in errs if 'not exist' in e}, reverse=True)
        if not missing:
            return errs, summary
        lines = open(path).read().splitlines(keepends=True)
        for ln in missing:
            i = ln - 1
            s = lines[i].strip()
            indent = lines[i][:len(lines[i]) - len(lines[i].lstrip())]
            if s.startswith('Scope ('):
                del lines[i:block_end(lines, i) + 1]
            elif (s.startswith('Notify (') or re.match(r'^\\?_SB\.[A-Z0-9_.]+\s*=', s)
                  or re.match(r'^\\?_SB\.[A-Z0-9_.]+,?(\s*//.*)?$', s)):
                del lines[i]
            elif s.startswith('Return (\\_SB.'):
                lines[i] = f'{indent}Return (Zero) // was: {s}\n'
            else:
                sys.exit(f'cannot repair line {ln}: {s}')
        text = re.sub(r',(\s*\n\s*\})', r'\1', ''.join(lines))   # trailing comma left before }
        lines, k = text.splitlines(keepends=True), 0
        while k < len(lines):                                   # thermal zones left with no devices
            if re.match(r'\s*ThermalZone \(', lines[k]):
                e = block_end(lines, k)
                if re.search(r'Name \(_TZD, Package \([^)]*\)[^\n]*\n\s*\{\s*\n\s*\}', ''.join(lines[k:e + 1])):
                    del lines[k:e + 1]
                    continue
            k += 1
        open(path, 'w').write(''.join(lines))
    return compile_errors(path)


def main(src, dst, extra=frozenset(), no_thermal=False):
    drop = DROP | set(extra)
    text = open(src).read()
    text = drop_devices(text, drop, no_thermal)
    n_ids = len(re.findall(r'QCOM05[0-9A-F]{2}', text))
    text = re.sub(r'QCOM05([0-9A-F]{2})', r'QCOM25\1', text)
    text, n_rails = remap_ufs_rails(text)
    text = re.sub(r'DefinitionBlock \("[^"]*", "DSDT"', 'DefinitionBlock ("DSDT.AML", "DSDT"', text, count=1)
    # decompiler quirks: _DEP written as strings; URS0's _HID given through an Alias
    text = re.sub(r'Name \(_DEP, Package \([^)]*\)[^\n]*\n\s*\{.*?\}\)',
                  lambda m: re.sub(r'"\\\\(_SB\.[A-Z0-9_.]+)"', r'\\\1', m.group(0)), text, flags=re.S)
    text = re.sub(r'\n(\s*)Alias \(URSI, _HID\)\n',
                  lambda m: (f'\n{m.group(1)}Method (_HID, 0, NotSerialized)  // was Alias (URSI, _HID)\n'
                             f'{m.group(1)}{{\n{m.group(1)}    Return (URSI ())\n{m.group(1)}}}\n'), text)
    # AGR0 is referenced long before it is defined; move it next to the other early objects
    lines = text.splitlines(keepends=True)
    a = next(i for i, l in enumerate(lines) if re.match(r'\s*Device \(AGR0\)', l))
    block = lines[a:block_end(lines, a) + 1]
    del lines[a:a + len(block)]
    at = next(i for i, l in enumerate(lines) if 'Name (PRP1,' in l) + 1
    lines[at:at] = ['\n'] + block
    # ...which makes AGR0._OST's store to PEP0.ROST a forward reference: declare it instead of losing it
    db = next(i for i, l in enumerate(lines) if l.startswith('DefinitionBlock'))
    brace = next(i for i in range(db, len(lines)) if lines[i].strip() == '{')
    lines.insert(brace + 1, '    External (\\_SB.PEP0.ROST, IntObj)  // defined in PEP0; AGR0 is placed before it\n')
    header =('/* apollo (Xiaomi Mi 10T / 10T Pro) DSDT, generated by make-apollo-dsdt.py from\n'
              ' * Silicium-ACPI Platforms/Xiaomi/pipa (SM8250-AC, Kona). Do not edit by hand. */\n')
    open(dst, 'w').write(header + ''.join(lines))
    errs, summary = repair(dst)
    print(f'dropped {len(drop)} devices{" and all thermal zones" if no_thermal else ""}, '
          f'renamed {n_ids} ids, {n_rails} UFS0 rail edits')
    print(summary)
    if errs:
        for ln, e in errs[:20]:
            print(f'  line {ln}: {e}')
        sys.exit(1)


if __name__ == '__main__':
    args, extra, no_thermal = sys.argv[1:], set(), False
    if '--no-thermal' in args:
        args.remove('--no-thermal')
        no_thermal = True
    if '--drop' in args:
        k = args.index('--drop')
        extra = {n for n in args[k + 1].split(',') if n}
        del args[k:k + 2]
    if len(args) != 2:
        sys.exit(__doc__)
    main(args[0], args[1], extra, no_thermal)
