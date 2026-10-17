# docs/

These pages build here and are published on the Nominal docs hub (nominal-io/pub-docs) at
`/matlab/simulink/`. The repo's CLAUDE.md brings in the hub's writing rules, and those apply
here in full. This file adds what is specific to the pages.

## Where the facts come from

Quote labels and messages exactly as the source has them, labels in bold.

- Dialog prompts, popup options and defaults: `simulink/**/make_library.m`.
- The block's errors and end-of-run warning: `simulink/src/nominal_sink_sfun.c`. The reason in
  `could not open the stream (...)` comes from `nm_strerror` in `cstream/src/nominal_stream.c`.
- The Detect button's errors: `simulink/**/nominal_sink_autonames.m`.
- The relay's settings, errors and log lines: `nmproxy/`.
- The frame protocol: `cstream/src/nm_frame.h`, whose grammar `ref/frames.md` includes at build
  time, and the fixtures in `cstream/tests/fixtures/frames.txt`.

`_ext/source_check.py` fails the build when a `ref/` page stops quoting one of these, so change
the page in the same commit as the string.

For anything in the Nominal app, such as the **API keys** page or creating a dataset, link the
hub's quickstart pages rather than describing the app from memory.

## Rules for these pages

- Readers know Simulink and MATLAB. Explain only what is Nominal's.
- State only what the source shows or what has been run. Simulink's own behavior, such as data
  types, buses and its menu labels, needs a test in Simulink before a page relies on it.
- Real-time targets and code generation stay out of the pages until they work.
- The app calls the credential an API key. The environment variable is still `NOMINAL_TOKEN`.
- Text values in examples keep their single quotes, since the block's text fields take MATLAB
  expressions.
- Link to other projects on the hub as `[text](site:/{lang}/{project}/{page}/)`, never with an
  absolute URL, so the hub's offline copy works.
- `% SCREENSHOT:` lines are placeholders for screenshots still to be taken.

The in-toolbox help is a separate set of pages, in `simulink/doc-src/` and built by `just docs`.
Keep the facts in both consistent.

## Building

`just build-docs` builds into `docs/_build/dirhtml`, with warnings failing the build as in CI.
Run it before calling a change done. `just serve-docs` serves a live preview on
http://127.0.0.1:8000.

`.github/workflows/docs-check.yml` builds every pull request. `deploy-docs.yml` deploys every
push to `main` to this repo's GitHub Pages, which are public, and pub-docs pulls that deploy.
