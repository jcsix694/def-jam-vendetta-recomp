"""Makes overrides/vendetta_menu_pieces.inc (the menu themes' catalogue as a C++ table) from config/menu_pieces.csv.

python tools/make_menu_pieces.py        run after config/menu_pieces.csv changes; the game is built with the table, so
                                        nothing is read at run time. Each live piece becomes one row:
  {name, kind, owner, source, a, b, u0, v0, u1, v1, first return address, how many, grows}
  kind: 1 picture, 2 words (a = the text's address), 3 words built at run time, 4 a plain quad, 5 a screen copy
  owner: 0 menu (a theme can change it), 1 wrestler, 2 arena, 3 the game's own
  source (pictures): 1 a PRM pack (a = pack, b = record), 2 a group (a = group, b = record)
  grows: 1 = a bar, drawn from its left edge as wide as a value (u1 = the widest seen)
  the return addresses the piece was drawn from follow in their own table (the rows point into it).
"""
import csv
import os

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
src = os.path.join(root, 'config', 'menu_pieces.csv')
out = os.path.join(root, 'overrides', 'vendetta_menu_pieces.inc')

with open(src, encoding='utf-8') as f:
    rows = list(csv.DictReader(l for l in f if not l.startswith('#')))

KIND = {'picture': 1, 'words': 2, 'words-built': 3, 'plain': 4, 'copy': 5}
OWNER = {'menu': 0, 'wrestler': 1, 'arena': 2, 'game': 3}
SOURCE = {'prm': 1, 'group': 2}
entries, ras = [], []
for r in rows:
    if r['seen'] != 'live':
        continue
    kind = KIND[r['kind']]
    uv = [int(v) for v in r['uv'].split()] if r['uv'] else [0, 0, 0, 0]
    a = int(r['a'], 16) if kind == 2 else (int(r['a']) if r['a'] else 0)
    b = int(r['b']) if r['b'] else 0
    first = len(ras)
    ras.extend(int(x, 16) for x in r['ra'].split())
    entries.append((r['name'], kind, OWNER[r['owner']], SOURCE.get(r['source'], 0), a, b, uv, first, len(ras) - first,
                    1 if r.get('grows') == 'width' else 0))

with open(out, 'w', encoding='utf-8', newline='\n') as f:
    f.write('// The menu themes\' catalogue as a table: made by tools/make_menu_pieces.py from config/menu_pieces.csv (do not\n'
            '// edit; run the tool after the csv changes). See the tool for the columns.\n')
    f.write(f'static const uint32_t kMenuPieceRas[{max(1, len(ras))}] = {{\n')
    for i in range(0, len(ras), 10):
        f.write('    ' + ', '.join(f'0x{x:06X}u' for x in ras[i:i + 10]) + ',\n')
    f.write('};\n\n')
    f.write(f'static const MenuPiece kMenuPieces[{len(entries)}] = {{\n')
    for name, kind, owner, source, a, b, uv, first, n, grows in entries:
        f.write(f'    {{"{name}", {kind}, {owner}, {source}, 0x{a:X}u, {b}u, {uv[0]}, {uv[1]}, {uv[2]}, {uv[3]}, {first}u, {n}u, {grows}}},\n')
    f.write('};\n')
print(f'{len(entries)} pieces, {len(ras)} return addresses -> {out}')
