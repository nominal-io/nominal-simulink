"""Fail the build when a reference page stops quoting the source.

The reference pages quote the block's dialog labels, its messages and the relay's log messages
word for word, since a reader searches for exactly what they see. Those strings live in the
code, so on every build this reads them from there and warns about each one a page no longer
contains. `just build-docs` and CI build with -W, so a warning fails the build: change the page
in the same commit as the string.

    page             what it must quote                          read from
    ref/block.md     dialog prompts, popup options, defaults      simulink/**/make_library.m
    ref/messages.md  the block's errors and end-of-run warning    simulink/src/nominal_sink_sfun.c
                     why a stream can't open, and "Last error"    cstream/src/nominal_stream.c,
                                                                  cstream/src/nm_http.c
                     the Detect button's errors                   simulink/**/nominal_sink_autonames.m
                     the Tags field's errors                      simulink/**/nominal_sink_tags.m
                     the Execution ID field's errors              simulink/**/nominal_sink_execid.m
    ref/nmproxy.md   the relay's log messages and warnings        nmproxy/**/*.go
                     the errors that stop it                      nmproxy/proxy/config.go,
                                                                  nmproxy/proxy/profile.go,
                                                                  nmproxy/proxy/server.go
    ref/frames.md    the frame limits in FRAME_LIMITS below       cstream/src/nm_frame.h

The MATLAB files are found by name anywhere under simulink/, so moving them within it doesn't
break the check. A message with printf placeholders is compared piece by piece, between them.
"""

from __future__ import annotations

import re
from pathlib import Path
from typing import Any

from sphinx.application import Sphinx
from sphinx.util import logging

logger = logging.getLogger(__name__)

# The limits ref/frames.md and ref/block.md state, by their #define in nm_frame.h. When one
# changes there, update the pages, then this table.
FRAME_LIMITS = {
    "NM_FRAME_VERSION": 1,
    "NM_FRAME_MAX_BYTES": 1400,
    "NM_FRAME_HEADER_BYTES": 24,
    "NM_FRAME_DATA_FIXED_BYTES": 12,
    "NM_FRAME_MAX_NAME_BYTES": 256,
    "NM_FRAME_MAX_RID_BYTES": 256,
    "NM_FRAME_MAX_TAGS": 64,
}
MAX_CHANNELS = 169  # derived in nm_frame.h from the limits above
HELLO_INTERVAL_MS = 1000  # nominal_stream.c; the pages say the channel list repeats every second

MATLAB_STRING = r"'((?:[^']|'')*)'"
C_STRING = r'"((?:[^"\\]|\\.)*)"'
# printf conversions, plus Go's %q, %v and %w
PRINTF = re.compile(r"%[-+ #0]*\d*(?:\.\d+)?(?:hh|h|ll|l|z|j|t|L)?[diouxXeEfgGcspqvw%]")
# Statuses whose nm_strerror text a block user never sees. nm_open never returns the first
# two, the block checks every setting NM_ERR_ARG covers before it calls nm_open, and the
# block reports NM_ERR_TOO_LARGE in a message of its own.
UNREACHABLE_STATUSES = {"NM_OK", "NM_ERR_CLOSED", "NM_ERR_ARG", "NM_ERR_TOO_LARGE"}
# Guards against the library misusing itself, which no user can reach
UNREACHABLE_MESSAGES = {"invalid request"}
# Pieces of a message shorter than this, such as " of " between two numbers, prove nothing.
SHORTEST = 12


def _unquote(s: str) -> str:
    return s.replace("''", "'")


def _squash(s: str) -> str:
    # a page wraps its lines wherever it likes, so compare with all whitespace as single spaces
    return " ".join(s.split())


def _mask_strings(text: str) -> list[str]:
    """The block's name, and each mask parameter's prompt, popup options and default."""
    strings = [_unquote(m) for m in re.findall(r"blockName\s*=\s*" + MATLAB_STRING, text)]
    for call in re.finditer(r"addParameter\((.*?)\);", text, re.S):
        body = call.group(1)
        options = re.search(r"'TypeOptions',\s*\{(.*?)\}", body, re.S)
        if options:
            strings += [_unquote(s) for s in re.findall(MATLAB_STRING, options.group(1))]
            body = body[: options.start()] + body[options.end() :]
        literals = [_unquote(s) for s in re.findall(MATLAB_STRING, body)]
        strings += [value for key, value in zip(literals, literals[1:]) if key in ("Prompt", "Value")]
    strings += [_unquote(s) for s in re.findall(r"\.Prompt\s*=\s*" + MATLAB_STRING, text)]
    return strings


def _sfun_messages(text: str) -> list[str]:
    """Every "Nominal Stream Sink: ..." message, joining C's adjacent string literals."""
    pattern = r'"Nominal Stream Sink: (?:[^"\\]|\\.)*"(?:\s*' + C_STRING + ")*"
    return [
        "".join(re.findall(C_STRING, m.group(0))).removeprefix("Nominal Stream Sink: ")
        for m in re.finditer(pattern, text)
    ]


def _joined(literals: str) -> str:
    return "".join(re.findall(C_STRING, literals))


def _sfun_causes(text: str) -> list[str]:
    """The end-of-run warning's explanations, written into `why`."""
    pattern = r"snprintf\(\s*why\s*,\s*sizeof why\s*,\s*((?:" + C_STRING + r"\s*)+)"
    return [_joined(m.group(1)) for m in re.finditer(pattern, text)]


def _stream_messages(text: str) -> list[str]:
    """nm_strerror's reasons a stream can't open, and the texts that end up after "Last error:"."""
    messages = [
        _joined(m.group(2))
        for m in re.finditer(r"case (NM_\w+):\s*return\s*((?:" + C_STRING + r"\s*)+);", text)
        if m.group(1) not in UNREACHABLE_STATUSES
    ]
    pattern = (
        r"snprintf\(\s*(?:s->last_error|out->error)\s*,\s*sizeof[^,]*,\s*((?:" + C_STRING + r"\s*)+)"
    )
    messages += [_joined(m.group(1)) for m in re.finditer(pattern, text)]
    return [m for m in messages if m not in UNREACHABLE_MESSAGES]


def _go_errors(text: str) -> list[str]:
    """fmt.Errorf messages, joining Go's "..." + "..." concatenation."""
    pattern = r"fmt\.Errorf\(\s*((?:" + C_STRING + r"\s*\+?\s*)+)"
    return [_joined(m.group(1)) for m in re.finditer(pattern, text)]


def _matlab_errors(text: str) -> list[str]:
    """The message of each error(id, message) call, joining a bracketed concatenation."""
    pattern = r"\berror\(\s*" + MATLAB_STRING + r"\s*,\s*(?:\.\.\.\s*)?(\[.*?\]|" + MATLAB_STRING + ")"
    return [
        "".join(_unquote(s) for s in re.findall(MATLAB_STRING, m.group(2)))
        for m in re.finditer(pattern, text, re.S)
    ]


def _go_messages(text: str) -> list[str]:
    """Log messages, and the startup warnings config.go builds by concatenation."""
    messages = re.findall(r"\.(?:Debug|Info|Warn|Error)\(\s*" + C_STRING, text)
    for m in re.finditer(r"append\(warnings,\s*((?:" + C_STRING + r"\s*\+?\s*)+)", text):
        messages.append("".join(re.findall(C_STRING, m.group(1))))
    return messages


def _pieces(message: str) -> list[str]:
    parts = [p.strip() for p in PRINTF.split(message)]
    if len(parts) == 1:
        return parts
    return [p for p in parts if len(p) >= SHORTEST]


def _defines(text: str) -> dict[str, int]:
    return {m.group(1): int(m.group(2)) for m in re.finditer(r"#define\s+(\w+)\s+(\d+)\b", text)}


def problems(root: Path, docs: Path) -> list[str]:
    """Everything the pages in docs/ fail to quote from the source under root."""
    found: list[str] = []
    pages = {
        name: _squash((docs / "ref" / f"{name}.md").read_text(encoding="utf-8"))
        for name in ("block", "messages", "nmproxy", "frames")
    }

    def one(pattern: str) -> Path | None:
        paths = sorted(p for p in root.glob(pattern) if p.is_file() and "_build" not in p.parts)
        if len(paths) != 1:
            found.append(f"expected one {pattern} in the repository, found {len(paths)}")
            return None
        return paths[0]

    def require(page: str, strings: list[str], source: Path) -> None:
        where = source.relative_to(root).as_posix()
        for string in strings:
            for piece in _pieces(_squash(string)):
                if piece and piece not in pages[page]:
                    found.append(f"ref/{page}.md doesn't quote {piece!r} from {where}")

    if library := one("simulink/**/make_library.m"):
        require("block", _mask_strings(library.read_text(encoding="utf-8")), library)

    sfun = root / "simulink" / "src" / "nominal_sink_sfun.c"
    sfun_text = sfun.read_text(encoding="utf-8")
    require("messages", _sfun_messages(sfun_text) + _sfun_causes(sfun_text), sfun)

    for c in ("nominal_stream.c", "nm_http.c"):
        source = root / "cstream" / "src" / c
        require("messages", _stream_messages(source.read_text(encoding="utf-8")), source)

    for go in ("config.go", "profile.go", "server.go"):
        source = root / "nmproxy" / "proxy" / go
        require("nmproxy", _go_errors(source.read_text(encoding="utf-8")), source)

    # The page leaves off the "Nominal Stream Sink: " the run-time errors start with.
    for name in ("nominal_sink_autonames.m", "nominal_sink_tags.m", "nominal_sink_execid.m"):
        if helper := one(f"simulink/**/{name}"):
            messages = _matlab_errors(helper.read_text(encoding="utf-8"))
            messages = [m.removeprefix("Nominal Stream Sink: ") for m in messages]
            require("messages", messages, helper)

    for go in sorted((root / "nmproxy").rglob("*.go")):
        if not go.name.endswith("_test.go"):
            require("nmproxy", _go_messages(go.read_text(encoding="utf-8")), go)

    defines = _defines((root / "cstream" / "src" / "nm_frame.h").read_text(encoding="utf-8"))
    for name, documented in FRAME_LIMITS.items():
        if defines.get(name) != documented:
            found.append(
                f"cstream/src/nm_frame.h has {name} = {defines.get(name)}, and the pages say "
                f"{documented}: update the pages, then FRAME_LIMITS in {Path(__file__).name}"
            )
    if all(name in defines for name in FRAME_LIMITS):
        channels = (
            defines["NM_FRAME_MAX_BYTES"]
            - defines["NM_FRAME_HEADER_BYTES"]
            - defines["NM_FRAME_DATA_FIXED_BYTES"]
            - 8
        ) // 8
        if channels != MAX_CHANNELS:
            found.append(f"the frame limits now allow {channels} channels, and the pages say {MAX_CHANNELS}")

    stream_c = root / "cstream" / "src" / "nominal_stream.c"
    interval = _defines(stream_c.read_text(encoding="utf-8")).get("NM_HELLO_INTERVAL_MS")
    if interval != HELLO_INTERVAL_MS:
        found.append(
            f"cstream/src/nominal_stream.c repeats the channel list every {interval} ms, and the "
            f"pages say every second"
        )
    return found


def _check(app: Sphinx) -> None:
    docs = Path(app.srcdir)
    for problem in problems(docs.parent, docs):
        logger.warning(f"source_check: {problem}", type="source_check")


def setup(app: Sphinx) -> dict[str, Any]:
    app.connect("builder-inited", _check)
    return {"parallel_read_safe": True, "parallel_write_safe": True}
