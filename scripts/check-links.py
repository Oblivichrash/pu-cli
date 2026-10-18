import re, os

files = ['README.md', 'CONTRIBUTING.md', 'CHANGELOG.md', 'docs/ARCHITECTURE.md',
         'docs/configuration.md', 'docs/web-api.md', 'docs/providers.md',
         '.github/pull_request_template.md']
bad = 0
for f in files:
    base = os.path.dirname(f)
    text = open(f).read()
    for m in re.finditer(r'\]\((?!https?://|#)([^)#\s]+)(#[^)]*)?\)', text):
        target = m.group(1)
        path = os.path.normpath(os.path.join(base, target))
        if not os.path.exists(path):
            print(f, 'broken ->', target)
            bad = 1
print('all links resolve' if not bad else 'broken links found')
