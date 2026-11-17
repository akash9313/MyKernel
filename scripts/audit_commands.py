import re
from collections import defaultdict

with open('kernel/shell/shell.c', 'r', encoding='utf-8') as f:
    text = f.read()

start = text.find('g_builtins[] = {')
end = text.find('};', start)
table_text = text[start:end]

entries = re.findall(r'\{\s*"([^"]+)"\s*,\s*([a-zA-Z0-9_]+)\s*\}', table_text)

print(f"Total entries in g_builtins: {len(entries)}")
unique_names = sorted(list(set(e[0] for e in entries)))
print(f"Unique command names registered: {len(unique_names)}")

h_map = defaultdict(list)
for name, h in entries:
    h_map[h].append(name)

print(f"Unique handlers: {len(h_map)}")
print("\nShared handlers (aliases):")
for h, names in h_map.items():
    if len(names) > 1:
        print(f"  Handler {h}: {names}")

print("\nAll command entries:")
for i, (name, h) in enumerate(entries):
    print(f"  {i+1:3d}. {name:<14} -> {h}")
