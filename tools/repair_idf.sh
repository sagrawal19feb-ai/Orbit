#!/bin/bash
# repair_idf.sh <component-path> [tag] — download files missing from the local
# esp-idf tree for one component, straight from the IDF git tag.
# Usage: repair_idf.sh components/esp_netif [v5.5.4]
IDF=/home/user/esp-idf
TAG=${2:-v5.5.4}
COMP="$1"
API="https://api.github.com/repos/espressif/esp-idf/git/trees/${TAG}:${COMP}?recursive=1"
curl -s "$API" -o /tmp/tree_$$.json
COMP="$COMP" IDF="$IDF" TAG="$TAG" TREE=/tmp/tree_$$.json python3 << 'EOF'
import json, os, sys, urllib.request
comp, idf, tag, tree = os.environ['COMP'], os.environ['IDF'], os.environ['TAG'], os.environ['TREE']
d = json.load(open(tree))
if 'tree' not in d:
    print(f'{comp}: API error: {d.get("message")}'); sys.exit(1)
# NOTE: tree API paths are RELATIVE to the queried subtree
missing = [e['path'] for e in d['tree'] if e['type'] == 'blob'
           and not os.path.exists(os.path.join(idf, comp, e['path']))]
print(f'{comp}: {len(missing)} missing files')
base = f'https://raw.githubusercontent.com/espressif/esp-idf/{tag}/{comp}/'
fails = 0
for p in missing:
    local = os.path.join(idf, comp, p)
    os.makedirs(os.path.dirname(local), exist_ok=True)
    try:
        urllib.request.urlretrieve(base + p, local)
    except Exception as ex:
        print(f'  FAIL {p}: {ex}'); fails += 1
print(f'{comp}: repaired {len(missing)-fails}/{len(missing)} files')
EOF
rm -f /tmp/tree_$$.json
