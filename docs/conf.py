"""Sphinx configuration for the Nominal for Simulink docs.

Built with `just build-docs`. .github/workflows/deploy-docs.yml deploys the build to this
repo's GitHub Pages, and the Nominal docs hub (nominal-io/pub-docs) pulls it from there and
serves it at /matlab/simulink/.
"""

import sys
from pathlib import Path

from nominal_sphinx_theme import theme_options

HERE = Path(__file__).parent
sys.path.insert(0, str(HERE / "_ext"))

project = "Simulink"  # the name in the header, after the Nominal Docs logo
copyright = "Nominal, Inc."

extensions = [
    "myst_parser",
    "sphinx_design",
    "sphinx_copybutton",
    "nominal_sphinx_theme",
    "site_links",  # [text](site:/quickstart/...) links to other projects on the docs hub
    "source_check",  # fails the build when ref/ stops quoting the source's labels and messages
    "frame_spec",  # {frame-spec}: the frame grammar, read from cstream/src/nm_frame.h
]

myst_enable_extensions = ["colon_fence", "deflist", "attrs_inline", "attrs_block", "fieldlist"]
myst_heading_anchors = 6

# CLAUDE.md holds the rules for writing these pages; it isn't one
exclude_patterns = ["_build", "CLAUDE.md"]

html_theme = "shibuya"
html_title = "Nominal for Simulink"  # browser tabs and search results
html_copy_source = False
html_context = {
    "source_type": "github",
    "source_user": "nominal-io",
    "source_repo": "nominal-simulink",
    "source_version": "main",
    "source_docs_path": "/docs/",
}
html_theme_options = theme_options(
    github_url="https://github.com/nominal-io/nominal-simulink",
    nav_links=[
        {"title": "Guides", "url": "index"},
        {"title": "Reference", "url": "ref/index"},
    ],
)
