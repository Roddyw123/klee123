"""Convert a fuzzer corpus directory into a KLEE template file."""

import argparse
import os
import sys

def read_corpus(corpus_dir, max_size):
    """Read all files in the corpus directory, return list of byte arrays."""
    files = []
    for name in sorted(os.listdir(corpus_dir)):
        path = os.path.join(corpus_dir, name)
        if not os.path.isfile(path):
            continue
        data = open(path, 'rb').read()
        if len(data) > max_size:
            continue
        if len(data) == 0:
            continue
        files.append(data)
    return files

def analyze_positions(corpus):
    """For each byte position, compute the set of distinct values."""
    min_len = min(len(f) for f in corpus)
    positions = []
    for i in range(min_len):
        values = set(f[i] for f in corpus)
        positions.append(values)
    return positions, min_len

def infer_constraint(values):
    """Given a set of byte values, infer the tightest constraint."""
    if all(0x30 <= v <= 0x39 for v in values):
        return 'digit'
    if all((0x41 <= v <= 0x5A) or (0x61 <= v <= 0x7A) for v in values):
        return 'alpha'
    if all((0x41 <= v <= 0x5A) or (0x61 <= v <= 0x7A) or
           (0x30 <= v <= 0x39) for v in values):
        return 'alnum'
    if all(0x20 <= v <= 0x7E for v in values):
        return 'print'
    lo, hi = min(values), max(values)
    if hi - lo < 64:
        return f'range:{lo}-{hi}'
    return None

def needs_hex_mode(positions):
    """Check if any concrete byte is non-printable."""
    for values in positions:
        if len(values) == 1:
            v = next(iter(values))
            if v < 32 or v > 126:
                return True
    return False

def generate_text_template(positions, marker, use_constraints):
    """Generate a text-mode .tpl template."""
    out = []
    for values in positions:
        if len(values) == 1:
            v = next(iter(values))
            if v == ord(marker):
                return None
            out.append(chr(v))
        else:
            if use_constraints:
                c = infer_constraint(values)
                if c:
                    out.append(f'{marker}{{{c}}}')
                else:
                    out.append(marker)
            else:
                out.append(marker)
    return ''.join(out)

def generate_hex_template(positions, use_constraints):
    """Generate a hex-mode .btpl template."""
    out = []
    for values in positions:
        if len(values) == 1:
            out.append(f'{next(iter(values)):02X}')
        else:
            if use_constraints:
                c = infer_constraint(values)
                if c:
                    out.append(f'?{{{c}}}')
                else:
                    out.append('??')
            else:
                out.append('??')
    lines = []
    for i in range(0, len(out), 16):
        lines.append(' '.join(out[i:i+16]))
    return '\n'.join(lines) + '\n'

def main():
    parser = argparse.ArgumentParser(
        description='Convert fuzzer corpus to KLEE template')
    parser.add_argument('corpus_dir', help='Directory with corpus files')
    parser.add_argument('-o', '--output', default=None,
                        help='Output file (default: stdout)')
    parser.add_argument('--mode', choices=['text', 'hex', 'auto'],
                        default='auto', help='Template format')
    parser.add_argument('--marker', default='?',
                        help='Marker char for text mode (default: ?)')
    parser.add_argument('--infer-constraints', action='store_true',
                        help='Infer type constraints from corpus values')
    parser.add_argument('--min-files', type=int, default=2,
                        help='Minimum corpus files (default: 2)')
    parser.add_argument('--max-size', type=int, default=10*1024*1024,
                        help='Max file size in bytes')
    args = parser.parse_args()

    corpus = read_corpus(args.corpus_dir, args.max_size)
    if len(corpus) < args.min_files:
        print(f'Error: need >= {args.min_files} corpus files, '
              f'found {len(corpus)}', file=sys.stderr)
        sys.exit(1)

    positions, length = analyze_positions(corpus)
    n_sym = sum(1 for v in positions if len(v) > 1)
    n_con = length - n_sym
    print(f'Corpus: {len(corpus)} files, {length} bytes common length',
          file=sys.stderr)
    print(f'  {n_con} concrete bytes, {n_sym} symbolic bytes',
          file=sys.stderr)

    use_hex = args.mode == 'hex'
    if args.mode == 'auto':
        use_hex = needs_hex_mode(positions)

    if use_hex:
        template = generate_hex_template(positions, args.infer_constraints)
    else:
        template = generate_text_template(positions, args.marker,
                                           args.infer_constraints)
        if template is None:
            print('Warning: marker char appears in concrete data, '
                  'falling back to hex mode', file=sys.stderr)
            template = generate_hex_template(positions,
                                              args.infer_constraints)
            use_hex = True

    if args.output:
        with open(args.output, 'w') as f:
            f.write(template)
        ext = '.btpl' if use_hex else '.tpl'
        print(f'Wrote {args.output} ({ext} format)', file=sys.stderr)
    else:
        sys.stdout.write(template)

if __name__ == '__main__':
    main()
