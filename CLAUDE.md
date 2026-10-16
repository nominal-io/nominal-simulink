# nominal-simulink

Everything a person reads in this repo follows the Nominal docs hub's writing rules:

@../pub-docs/CLAUDE.md

If that import comes up empty, pub-docs isn't checked out beside this repo. Read
https://github.com/nominal-io/pub-docs/blob/main/CLAUDE.md instead. Its "Building and checking"
and "Repo conventions" sections are about pub-docs itself and don't apply here.

The rules cover more than the docs in `docs/`:
- the README
- the in-toolbox help in `simulink/doc-src/`
- the block's dialog prompts and description, in `make_library.m`
- the toolbox's summary and description
- every error, warning and log message: the S-function, the mask callbacks and `nmproxy`
- the comments in `nmproxy.example.yaml`
- code comments

The user docs quote the block's labels and messages word for word, so changing one is a docs
change too. `just build-docs` fails until `docs/ref/` matches. `docs/CLAUDE.md` has the rules
for the pages themselves.
