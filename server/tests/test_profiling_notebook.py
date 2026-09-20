"""Structural checks for the dedicated Colab profiling entry point."""

import json
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
NOTEBOOK_DIR = REPO_ROOT / "server" / "notebooks"


def load_notebook(name):
    with open(NOTEBOOK_DIR / name, encoding="utf-8") as handle:
        return json.load(handle)


def code_cells(notebook):
    return [
        "".join(cell.get("source", []))
        for cell in notebook["cells"]
        if cell.get("cell_type") == "code"
    ]


def cell_containing(notebook, marker):
    for source in code_cells(notebook):
        if marker in source:
            return source
    raise AssertionError(f"notebook has no code cell containing {marker!r}")


class ProfilingNotebookTests(unittest.TestCase):
    def setUp(self):
        self.production = load_notebook("nemotron_server.ipynb")
        self.profiling = load_notebook("nemotron_profiling.ipynb")

    def test_reuses_the_exact_production_backend_configuration(self):
        marker = "backend = NemotronConfig("
        self.assertEqual(
            cell_containing(self.production, marker),
            cell_containing(self.profiling, marker),
        )

    def test_enables_trace_before_creating_the_real_app(self):
        cells = code_cells(self.profiling)
        trace_index = next(i for i, source in enumerate(cells) if "SUBTITLE_TRACE" in source)
        app_index = next(i for i, source in enumerate(cells) if "app = create_app(" in source)

        self.assertLess(trace_index, app_index)
        self.assertIn("SUBTITLE_TRACE_PATH", cells[trace_index])
        self.assertIn("create_app(config, backend_state=state)", cells[app_index])

    def test_last_cell_closes_uvicorn_and_downloads_the_trace(self):
        server_cell = cell_containing(self.profiling, "app = create_app(")
        final_cell = code_cells(self.profiling)[-1]

        self.assertIn("timeout_graceful_shutdown=10", server_cell)
        self.assertIn("server.should_exit = True", final_cell)
        self.assertIn("server_thread.join", final_cell)
        self.assertIn("files.download(str(SERVER_TRACE_PATH))", final_cell)

    def test_every_code_cell_is_valid_python(self):
        for index, source in enumerate(code_cells(self.profiling)):
            with self.subTest(index=index):
                compile(source, f"nemotron_profiling.ipynb:cell-{index}", "exec")


if __name__ == "__main__":
    unittest.main()
