"""The {frame-spec} directive: the frame grammar, read from cstream/src/nm_frame.h.

The ABNF in nm_frame.h is the protocol's specification, kept beside the code it describes.
Reading it at build time means ref/frames.md always shows the grammar the code implements.
It sits in a C comment, so this strips each line's leading " * ", and replaces the constant
names it mentions (NM_FRAME_VERSION) with their values, which mean nothing to a reader
outside the C code.
"""

from __future__ import annotations

import re
import textwrap
from pathlib import Path
from typing import Any

from docutils import nodes
from sphinx.application import Sphinx
from sphinx.util.docutils import SphinxDirective

# The grammar runs from its first production to the heading that follows it.
FIRST = re.compile(r"^frame\s+=")
LAST = "CONSTRAINTS NOT EXPRESSIBLE IN THE GRAMMAR"


def grammar(header: Path) -> str:
    text = header.read_text(encoding="utf-8")
    lines = [re.sub(r"^\s*\*( |$)", "", line) for line in text.splitlines()]
    start = next(i for i, line in enumerate(lines) if FIRST.match(line.strip()))
    end = next(i for i, line in enumerate(lines) if line.strip() == LAST)
    abnf = textwrap.dedent("\n".join(lines[start:end])).strip()
    for name, value in re.findall(r"#define\s+(NM_FRAME_\w+)\s+(\d+)\b", text):
        abnf = re.sub(rf"\b{name}\b", value, abnf)
    return abnf


class FrameSpec(SphinxDirective):
    has_content = False

    def run(self) -> list[nodes.Node]:
        header = Path(self.env.srcdir).parent / "cstream" / "src" / "nm_frame.h"
        self.env.note_dependency(str(header))
        abnf = grammar(header)
        block = nodes.literal_block(abnf, abnf, language="abnf")
        self.set_source_info(block)
        return [block]


def setup(app: Sphinx) -> dict[str, Any]:
    app.add_directive("frame-spec", FrameSpec)
    return {"parallel_read_safe": True, "parallel_write_safe": True}
