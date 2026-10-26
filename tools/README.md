# Admin module tools

The `tools` directory contains standard-library Python scripts for the
13 administration modules below `src/admin*.c`. No package manager or virtual
environment is required.

| Script | Purpose |
| --- | --- |
| `module_inventory.py` | List modules, files, line counts and missing files. |
| `module_check.py` | Enforce the Alves layout and registry consistency. |
| `module_graph.py` | Render the module dependency graph as DOT or JSON. |
| `module_scaffold.py` | Generate a new admin C module skeleton. |
| `module_test.py` | Build a temporary repository and smoke-test `mesh admin`. |
| `module_docs.py` | Generate or check `docs/admin_modules.rst`. |
| `module_metrics.py` | Report line, function and column metrics. |
| `module_clean.py` | Remove admin object files and Python caches. |

Typical use:

```sh
python3 tools/module_inventory.py
python3 tools/module_check.py
python3 tools/module_metrics.py
python3 tools/module_docs.py --check
python3 tools/module_test.py
python3 tools/module_clean.py --apply
```

`module_scaffold.py` writes only when `--write` is present. A generated module
still needs a prototype in `src/core.h`, a `Makefile` entry and a record in
`tools/modules.json`; the script prints those next steps.
