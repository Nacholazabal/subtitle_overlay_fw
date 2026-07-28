"""Host-side tooling for the subtitle-overlay project.

This marker intentionally makes ``scripts`` a regular Python package.  NeMo's
Colab dependencies can install another top-level package with the same name; a
PEP 420 namespace directory would otherwise lose import resolution to that
third-party package even when the repository is first on ``sys.path``.
"""
