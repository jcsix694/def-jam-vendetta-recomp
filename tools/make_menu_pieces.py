"""Makes overrides/vendetta_menu_pieces.inc (the menu themes' catalogue as a C++ table) from config/menu_pieces.csv.

python tools/make_menu_pieces.py        run after config/menu_pieces.csv changes; the game is built with the table, so
                                        nothing is read at run time. Each live piece becomes one row:
  {name, kind, owner, source, a, b, u0, v0, u1, v1, first return address, how many, grows, joined, join, s0, t0, s1, t1}
  kind: 1 picture, 2 words (a = the text's address), 3 words built at run time, 4 a plain quad, 5 a screen copy,
        6 a letter of the banner font (changed by a theme's banner font, never as a picture)
  owner: 0 menu (a theme can change it), 1 wrestler, 2 arena, 3 the game's own
  source (pictures): 1 a PRM pack (a = pack, b = record), 2 a group (a = group, b = record)
  grows: 1 = a bar, drawn from its left edge as wide as a value (u1 = the widest seen)
  joined: the one piece a part belongs to (an index into kMenuJoined, -1 for a piece drawn whole), join: 1 halves (a
        fixed layout: a theme's Fit / Fill / Stretch on the whole), 2 stretch (a frame whose middles stretch with its size:
        always stretched); s0 t0 s1 t1: the part's share of the one picture (0-1)
  the return addresses the piece was drawn from follow in their own table (the rows point into it).
"""
import csv
import os

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
src = os.path.join(root, 'config', 'menu_pieces.csv')
out = os.path.join(root, 'overrides', 'vendetta_menu_pieces.inc')

with open(src, encoding='utf-8') as f:
    rows = list(csv.DictReader(l for l in f if not l.startswith('#')))

KIND = {'picture': 1, 'words': 2, 'words-built': 3, 'plain': 4, 'copy': 5, 'banner': 6}
OWNER = {'menu': 0, 'wrestler': 1, 'arena': 2, 'game': 3}
SOURCE = {'prm': 1, 'group': 2}
JOIN = {'halves': 1, 'stretch': 2}
entries, ras, joined = [], [], []
for r in rows:
    if r['seen'] != 'live':
        continue
    kind = KIND[r['kind']]
    uv = [int(v) for v in r['uv'].split()] if r['uv'] else [0, 0, 0, 0]
    a = int(r['a'], 16) if kind == 2 else (int(r['a']) if r['a'] else 0)
    b = int(r['b']) if r['b'] else 0
    first = len(ras)
    ras.extend(int(x, 16) for x in r['ra'].split())
    j = -1
    if r.get('joined'):
        if r['joined'] not in joined:
            joined.append(r['joined'])
        j = joined.index(r['joined'])
    share = [float(v) for v in r['share'].split()] if r.get('share') else [0.0, 0.0, 1.0, 1.0]
    entries.append((r['name'], kind, OWNER[r['owner']], SOURCE.get(r['source'], 0), a, b, uv, first, len(ras) - first,
                    1 if r.get('grows') == 'width' else 0, j, JOIN.get(r.get('join', ''), 0), share))

with open(out, 'w', encoding='utf-8', newline='\n') as f:
    f.write('// The menu themes\' catalogue as a table: made by tools/make_menu_pieces.py from config/menu_pieces.csv (do not\n'
            '// edit; run the tool after the csv changes). See the tool for the columns.\n')
    f.write(f'static const uint32_t kMenuPieceRas[{max(1, len(ras))}] = {{\n')
    for i in range(0, len(ras), 10):
        f.write('    ' + ', '.join(f'0x{x:06X}u' for x in ras[i:i + 10]) + ',\n')
    f.write('};\n\n')
    f.write(f'static const char *const kMenuJoined[{max(1, len(joined))}] = {{\n')
    for name in joined or ['']:
        f.write(f'    "{name}",\n')
    f.write('};\n\n')
    f.write(f'static const MenuPiece kMenuPieces[{len(entries)}] = {{\n')
    for name, kind, owner, source, a, b, uv, first, n, grows, j, join, sh in entries:
        f.write(f'    {{"{name}", {kind}, {owner}, {source}, 0x{a:X}u, {b}u, {uv[0]}, {uv[1]}, {uv[2]}, {uv[3]}, {first}u, {n}u, {grows}, '
                f'{j}, {join}, {sh[0]:.4f}f, {sh[1]:.4f}f, {sh[2]:.4f}f, {sh[3]:.4f}f}},\n')
    f.write('};\n')
print(f'{len(entries)} pieces ({len(joined)} drawn in parts), {len(ras)} return addresses -> {out}')
