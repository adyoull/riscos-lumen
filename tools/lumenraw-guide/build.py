#!/usr/bin/env python3
# Copyright (c) 2026 Andrew Youll
# Released under the MIT licence: see the LICENSE file.
# SPDX-License-Identifier: MIT
# Builds the LumenRaw guide: CSS (Lumen guide family, amber cover) + body + camera appendix from LibRaw's list.
import html, re, collections, os
HERE = os.path.dirname(os.path.abspath(__file__))
# cameras.txt is LibRaw's own list: cameralist.cpp, built against the host libraw.a, prints it.
cams = [l.rstrip('\n') for l in open(HERE + '/cameras.txt') if l.strip()]
special = [('OM Digital Solutions', 'OM Digital Solutions'), ('Digital Bolex', 'Digital Bolex'), ('Adobe Digital Negative', None)]
groups = collections.OrderedDict()
for c in cams:
    maker = None
    for pre, name in special:
        if c.startswith(pre):
            maker = name or 'DNG'; model = c[len(pre):].strip() if name else c; break
    if maker is None:
        maker, _, model = c.partition(' ')
    groups.setdefault(maker, []).append(model)
order = sorted(groups, key=lambda m: (m != 'DNG', m.lower()))
out = ['<p class="small">Jump to: ' + ' &middot; '.join('<a href="#cam-%s">%s</a>' % (re.sub(r'\W', '', m), html.escape(m)) for m in order) + '</p>']
for m in order:
    ms = groups[m]
    out.append('<h4 id="cam-%s">%s <span class="count">%d</span></h4>' % (re.sub(r'\W', '', m), html.escape(m if m != 'DNG' else 'Adobe DNG'), len(ms)))
    out.append('<p class="cams">' + ' &middot; '.join(html.escape(x) for x in ms) + '</p>')
appendix = '\n'.join(out)
css = re.search(r'<style>(.*?)</style>', open(HERE + '/../../docs/guide/index.html').read(), re.S).group(1)
css = css.replace('--accent:#004499;', '--accent:#9a4a00;')
css = css.replace('header.cover { background:#10283f;', 'header.cover { background:#2a1606;')
css = css.replace('color:#dce7f3;', 'color:#f3e3cf;')
css = css.replace('background:#00bbff; color:#10283f;', 'background:#ffb347; color:#2a1606;')
css = css.replace('h2.part { margin-top:56px; padding:12px 14px; background:#10283f;', 'h2.part { margin-top:56px; padding:12px 14px; background:#2a1606;')
css = css.replace('color:#c9d8e8;', 'color:#ead2b8;')
css += """p.cams { margin:2px 0 10px; font-size:0.92em; }
span.count { font-weight:normal; font-size:0.85em; color:#777; }
"""
body = open(HERE + '/body.html').read().replace('<!-- CAMERAS -->', appendix)
page = """<!DOCTYPE html>
<html lang="en-GB">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>LumenRaw User Guide</title>
<style>""" + css + """</style>
</head>
<body>
""" + body + """
</body>
</html>
"""
open(HERE + '/../../docs/lumenraw-guide/index.html', 'w').write(page)
print(len(page), len(order), 'makers', sum(len(v) for v in groups.values()))
